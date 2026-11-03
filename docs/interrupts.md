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
| `qcom,pdc-sm8150` | `0xb220000`; maps some GPIOs/PMIC wake lines to GIC SPIs |

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
   - TLMM: the summary SPI dispatches per-GPIO handlers. Each pin's
     `INTR_CFG`/`INTR_STATUS` is configured for the apps (KPSS) target, and
     the original configuration is saved and restored.
   - PDC: GPIOs and PMIC lines that the DT routes through the PDC are mapped to
     their GIC SPI from `qcom,pdc-ranges`, and the PDC settings are restored on
     handoff.
   - SPMI PMIC arbiter: `periph_irq` dispatches per-APID handlers through
     CrDK's SPMI libraries (`SpmiLib`, `SpmiSharedCrLib` ownership and per-bit
     restore). Only kShimC's peripherals are claimed.
3. **Drivers**: PMIC PON and PMIC-GPIO keys, TLMM gpio-keys, touch
   (FTM5 IRQ GPIO), GENI UART RX, DWC3 ACM events, and UFS/SDHCI completions
   where a waiting path benefits. Each driver falls back to polling when its
   interrupt cannot be resolved.
4. **Event loops (`user/`)**: the LVGL and serial menus wait with
   `kshim_wait_event(next_deadline)` (the next LVGL timer, countdown or
   animation) and wake on input interrupts, instead of `delay(5000)` loops.

## Layer 1 as implemented

- `kernel/dt/irq.c`: the `arm,gic-v3` node (GICD, `#redistributor-regions`
  GICR regions, 1- or 2-cell `redistributor-stride`), the timer PPIs
  (positional or `interrupt-names`; on SM8150 the timer inherits the PDC as
  parent, which PPIs pass straight through to the GIC), and
  `kshim_irq_from_fdt()` over `interrupts` and `interrupts-extended`. Any
  non-GIC parent, the PDC included, is `ERROR_NOT_SUPPORTED`; on SM8150 that
  is most device nodes, since `/` names the PDC as `interrupt-parent`.
- `kernel/dt/resources.c` maps GICD and the GICR regions last, so a full
  resource table costs interrupts, never a mandatory region.
- `kernel/arch/arm64/gicv3.c`: before changing anything it saves `GICD_CTLR`
  and the SPI enables, the boot CPU's GICR SGI/PPI frame and `GICR_WAKER`,
  and its CPU interface (`ICC_SRE/CTLR/PMR/BPR1/IGRPEN1`), `VBAR` and, at EL2,
  `HCR_EL2.IMO`. Every inherited source is disabled; an SPI's group,
  priority, trigger and route are saved the first time kShimC configures it.
  Each worker saves its own frame and CPU interface in
  `kshim_irq_cpu_init()` and puts them back before PSCI `CPU_OFF`.
- `kernel/irq/irq.c`: INTID-indexed handlers; IRQ entry acknowledges,
  dispatches and EOIs (`EOImode` 0). SGI 1 is the cross-CPU wake.
- `timer.c`: the virtual timer at EL1, `CNTHP` at EL2, one-shot `CVAL`
  deadlines in `CNTPCT` units. `delay()` and `kshim_wait_event()` sleep in
  WFI with IRQs masked around the check-and-sleep; `kshim_wait_event()` ends
  on a deadline or on a device interrupt taken since the previous wait.
- `kshim_smp_wait_event()`/`kshim_smp_send_event()` replace worker `wfe`/`sev`
  hand-off with WFI and the wake SGI, and fall back to WFE/SEV without IRQs.
- `kshim_irq_teardown()` (from `kshim_platform_prepare_handoff()`, after the
  workers are off and before the MMU): stops the timer and restores its
  CTL/CVAL, disables every SPI and clears pending/active of those kShimC
  enabled, restores the saved SPI configuration, enables and `GICD_CTLR`,
  restores the frames and the CPU interface, `VBAR` and `HCR_EL2.IMO`, and
  leaves `DAIF.I` masked. `tools/test_qemu_irq.py` checks the result against
  a seeded pre-kShimC snapshot.

## Handoff contract

`prepare_boot()` order:
1. Input and touch quiesce.
2. LVGL deinit and worker shutdown.
3. Drivers restore their secondary controllers (TLMM pins, PDC, SPMI arbiter
   channels, DWC3, UFS/SDHCI).
4. `kshim_irq_teardown()`: stop the timer, disable and clear everything
   kShimC enabled, restore the saved GICD/GICR/ICC state, mask IRQs.
5. SMP/MMU teardown and the jump.

A payload must find no pending, active or enabled interrupt that kShimC
introduced.

## Verification

- QEMU `virt,gic-version=3` at EL1 (hvc) and EL2 (smc), 1-8 CPUs: timer IRQ
  delivery, WFI idle, SGI wakeups, PL011 RX interrupt, and a GIC state dump at
  handoff equal to the pre-kShimC snapshot (`tools/test_qemu_irq.py`,
  `CONFIG_KSHIM_IRQ_SELFTEST`).
- Host register models for TLMM, PDC and the SPMI arbiter dispatch and restore.
- Existing suites stay green: host ctest, `test_qemu_smp.py`, manifest QEMU,
  firmware matrix.
