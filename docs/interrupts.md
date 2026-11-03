# Interrupts

kShimC is interrupt-driven. The original runtime (e6d5386) initialised the
GICv3 and the generic timer from `kshim_main()` with QEMU constants. The
DT-driven rework (121734f) dropped those calls without a DT replacement, and
later work assumed polling. This plan restores interrupts on real hardware.
Polling stays only as a per-driver fallback.

## Hardware facts (SM8150 HDK/andromeda runtime DT)

| Node | Fact |
| --- | --- |
| `arm,gic-v3` | GICD `0x17a00000`+64K, GICR `0x17a60000`+1M, `redistributor-stride = 0x20000`, one region |
| `arm,armv8-timer` | PPIs `<1 1>,<1 2>,<1 3>,<1 0>` (secure, non-secure, virtual, hyp order), not QEMU's 29/30/27/26 |
| `qcom,spmi-pmic-arb` | `periph_irq` = SPI 481 (`<0 0x1e1 0>`); PMIC children use 4-cell specifiers |
| TLMM `qcom,sm8150-pinctrl` | summary SPI 208, `#interrupt-cells = 2`, `wakeup-parent` = PDC |
| `qcom,pdc-sm8150` | `0xb220000`+`0x400`, `#interrupt-cells = 3` (GIC form), no `qcom,pdc-ranges`; the root `interrupt-parent`, so the SPMI arbiter, TLMM summary and QUP engines are PDC children. Pins 0-93/94-124/125 feed SPIs 480/609/63 (CrDK SM8150 target) |

## Layers

1. **Core (`kernel/arch/arm64`, `kernel/irq`)**: `kernel/include/kshim/irq.h`.
   - GICv3 discovered from the DT. Save the inherited state, then set up the
     distributor, one redistributor per CPU (walk GICR frames by
     `GICR_TYPER` affinity and `redistributor-stride`) and the CPU interface.
     Group 1 NS, so it works under a hypervisor's vGIC at EL1 and natively at
     EL2.
   - The handler table is indexed by INTID, and IRQ entry acknowledges, dispatches
     and EOIs. Built on the existing `gicv3.c`/`gicv3.S`/`arm64.S`.
   - Generic timer: PPIs from DT, the virtual timer at EL1 and the EL2 physical
     timer at EL2, one-shot deadlines. `delay()` and `kshim_wait_event()` sleep
     in WFI.
   - IRQ-safe locks (`kshim_irq_save/restore`) for the console, printf and the
     allocator paths used from handlers.
   - SMP: workers call `kshim_irq_cpu_init()`. SGIs replace `sev`/`wfe` wakeups
     where workers wait for work.
   - CrDK port: `CrRegisterInterrupt`/`CrUnregisterInterrupt`/`CrInterruptIsRegistered`
     in `ports/crdk/crdk_port.c` over `kshim_irq_request`, so CrDK's
     interrupt-driven paths run in kShimC.
2. **Qualcomm cascades (`kernel/drivers/irqchip`)**:
   - TLMM (`tlmm.c`): the summary SPI dispatches per-GPIO handlers from
     `INTR_STATUS`; edge pins are acknowledged before, level pins after their
     handler. Each pin's `INTR_CFG` (detection, polarity, `RAW_STATUS_EN`, apps
     KPSS target) is saved and restored and its status latch cleared. The
     layout and reserved pins are CrDK's TLMM target data (checked against
     those tables on the host; the 13.5 KiB tables and GpioLib's context are
     too large for the runtime slot), the per-pin sequence is pinctrl-msm's.
     A summary storm with no requested pin pending seals the summary SPI and
     its clients poll.
   - PDC (`pdc.c`): GPIOs use the TLMM summary, not their PDC wake pins: one
     `INTR_CFG` word per pin is the whole state, and the shared always-on PDC
     (and the TLMM `spi_cfg` block) stay untouched. The PDC is programmed only
     for PDC-parented interrupts whose SPI it feeds (`qcom,pdc-ranges`, or
     the CrDK SM8150 table for the Android DTs): on SM8150 that is the SPMI
     `periph_irq` (pin 1, SPI 481). `IRQ_i_CFG` type and the enable bit are
     saved and restored; other PDC children are GIC pass-throughs.
   - SPMI PMIC arbiter (`spmi_pmic.c`): `periph_irq` dispatches per-peripheral
     handlers through `SpmiSharedCrLib` (one client per claimed QPNPINT bit,
     snapshot and read-back restore of `SET_TYPE`/`POLARITY`/`EN` and the APID
     PIC enable) over `SpmiLib`. The transport grants SpmiLib writes only to
     the channel and PIC words of claimed peripherals. A pending APID without a
     client seals `periph_irq` (CrDK's shared-parent policy) and its clients poll.
3. **Drivers**: PMIC PON and PMIC-GPIO keys, TLMM gpio-keys, touch
   (FTM5 IRQ GPIO), GENI UART RX, DWC3 ACM events, and UFS/SDHCI completions
   where a waiting path benefits. Each driver falls back to polling when its
   interrupt cannot be resolved.
   Key and touch handlers queue events in IRQ context (keys: sample and
   commit at once, then a 15 ms quiet window settled by one resample; touch:
   the FTM5 FIFO is drained by the level-low handler). GENI RX sets only the
   S-sequencer RX watermark/last enables, so DebugUartLib's polled TX and its
   own enable snapshot are unaffected. `CONFIG_QCOM_IRQCHIP`,
   `CONFIG_KSHIM_INPUT_IRQ` and `CONFIG_KSHIM_CONSOLE_RX_IRQ` depend on
   `CONFIG_KSHIM_IRQ`.
4. **Event loops (`user/`)**: the LVGL and serial menus wait with
   `kshim_wait_event(next_deadline)` (the next LVGL timer, countdown or
   animation) and wake on input interrupts, instead of `delay(5000)` loops.

## Handoff contract

`prepare_boot()` order:
1. Input and touch quiesce.
2. LVGL deinit and worker shutdown.
3. Drivers restore their secondary controllers (TLMM pins, SPMI arbiter
   channels, then PDC routes; DWC3, UFS/SDHCI).
4. `kshim_irq_teardown()`: stop the timer, disable and clear everything
   kShimC enabled, restore the saved GICD/GICR/ICC state, mask IRQs.
5. SMP/MMU teardown and the jump.

Steps 1, 2, 3 and 5 live in `kshim_platform_prepare_handoff()`, which
`kshim_user_select()` (and the power menu) call before `prepare_boot()` runs
it again; every step is idempotent. `kshim_irq_teardown()` therefore belongs
in its marked slot between steps 3 and 5; `prepare_boot()` repeats steps 1
and 3 before the core teardown as a guarantee.

A payload must find no pending, active or enabled interrupt that kShimC
introduced.

## Verification

- QEMU `virt,gic-version=3` at EL1 (hvc) and EL2 (smc), 1-8 CPUs: timer IRQ
  delivery, WFI idle, SGI wakeups, PL011 RX interrupt, and a GIC state dump at
  handoff equal to the pre-kShimC snapshot.
- Host register models for TLMM, PDC and the SPMI arbiter dispatch and restore.
- Existing suites stay green: host ctest, `test_qemu_smp.py`, manifest QEMU,
  firmware matrix.
