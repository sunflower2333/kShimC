# QUP/GENI serial engines, I2C, SPI and UART

Owner: qup. This describes the implementation behind `kshim/qup.h`,
`kshim/i2c.h`, `kshim/spi.h` and `kshim/uart.h`. The contracts are in those
headers and in `docs/io-stack.md` (sections 3.1 to 3.3). Nothing here has run
on hardware yet.

## Files

| File | Role |
| --- | --- |
| `kernel/dt/qup.c` | Pure DT discovery: wrappers, SE windows, protocols, I2C bus rates, used windows |
| `kernel/drivers/qup/qup.c` | Registry, ownership, clock/pin bring-up, SE interrupt, mappings, handoff |
| `kernel/drivers/qup/geni_engine.{c,h}` | CrDK GeniSeLib FIFO core made interrupt-driven (shared by I2C and SPI) |
| `kernel/drivers/qup/geni_i2c.c` | GENI I2C adapter (`kshim_geni_i2c_probe`) |
| `kernel/drivers/qup/geni_spi.c` | GENI SPI controller (`kshim_geni_spi_probe`) |
| `kernel/drivers/qup/geni_uart.c` | UART layer for PL011 and GENI (always built; the console uses it) |
| `kernel/drivers/geni_rx_irq.c` | GENI UART RX interrupt engine on `struct kshim_uart` |
| `kernel/drivers/i2c/core.c`, `kernel/drivers/spi/core.c` | Bus cores |
| `kernel/drivers/console.c` | The DT console on one `struct kshim_uart` |

Kconfig: `KSHIM_QUP` (selects `KSHIM_GPIO`, `KSHIM_CLK`), `KSHIM_I2C`,
`KSHIM_SPI`, and the derived `KSHIM_I2C_GENI`/`KSHIM_SPI_GENI`. Build glue is
`cmake/io/qup.cmake`. CrDK is used unmodified: `CRDK_GENI` (GeniSeLib FIFO
core) and `CRDK_UART` (DebugUartLib polled engine).

## Discovery

- Engines: mainline `qcom,geni-i2c`, `-spi`, `-uart`, `-debug-uart`; Android
  `qcom,i2c-geni`, `qcom,spi-geni`, `qcom,msm-geni-serial-hs`,
  `qcom,msm-geni-console`. The first `reg` entry is the SE window.
- Wrappers: the parent `qcom,geni-se-qup` (mainline), else the
  `qcom,wrapper-core` target (Android).
- Only windows with an available node are registered, so the many disabled
  engines of an Android DT never take a slot. Disabled nodes naming a
  registered window are added to `nodes[]`. `dt_reg()` refuses disabled
  nodes, so those are matched as siblings by their untranslated address.
  This covers SM8650 i2c4/spi4 (0xa90000), the kona SE12 console/I2C pair,
  and the andromeda `i2c@0xc80000`/`spi@c80000` pair.
- I2C rate: `clock-frequency`, then Android `qcom,clk-freq-out`, else
  100 kHz for mainline nodes (Linux `i2c-qcom-geni` default) and 400 kHz
  for Android nodes. 400 kHz is the msm driver default and was also kShimC's
  former FTM5 binding default (`kernel/dt/touch.c`). The header's
  "default 100000" is therefore family-dependent.
- `kshim_qup_map_resources()` runs before `kshim_qup_init()` and maps each
  window that has a user: an available I2C or SPI controller with an
  available child, or an available UART node. The wrapper common window is
  never accessed and is not mapped. Once a mapping pass has run, acquiring an
  unmapped window returns ERROR_NOT_SUPPORTED instead of faulting.

## Ownership and bring-up

- `kshim_qup_init()` reserves the console's window (`kshim_console_base()`)
  for `KSHIM_QUP_OWNER_CONSOLE`. It cannot be claimed or acquired. The
  console instance is opened from MMIO before QUP exists and runs on its
  inherited clocks.
- Claims are per window. A second owner, or the same owner through another
  node, gets ERROR_BUSY.
- `kshim_qup_se_acquire()` follows CrDK ClockLib `clock_poll.c`
  (CrClockPollPrepare), with clocks resolved through `kshim/clk.h`:
  1. Resolve the clocks. The SE clock is `se` (mainline) or `se-clk`
     (Android). The AHB clocks `m-ahb`/`s-ahb` come from the wrapper node
     when it names them (mainline), else from the engine node (Android).
  2. Enable the AHB clocks. A clock that already runs is not written.
  3. Read `GENI_STATUS` (+0x40) through them. An I2C/SPI engine whose M or S
     sequencer is active is refused with ERROR_BUSY and never retuned.
  4. `kshim_clk_set_rate_cxo()` selects CXO 19.2 MHz undivided: a verified
     DFS slot (whose index goes to `GENI_SE_CLK_SEL`), else the RCG. An
     equal value is not rewritten. A UART that keeps its inherited baud
     passes NULL rate pointers, and its RCG is not touched.
  5. Enable the SE clock.
  6. Check `GENI_FW_REVISION_RO[15:8]` (+0x68). A mismatch returns
     ERROR_NOT_SUPPORTED, because kShimC never loads SE firmware.
  7. Apply pinctrl `default` of the owner node. A failure is fatal on a cold
     engine and only logged on an inherited one.
  Every failure unwinds the clock enables. `inherited_active` records that
  all three clocks already ran.
- `kshim_qup_se_release()` drops kShimC's enables. The RCG selection and the
  pin snapshots go back later, in `kshim_clk_restore_all()` and
  `kshim_gpio_restore_all()` (`kshim_io_quiesce()` steps 7 and 9).
- SE interrupt: `interrupts[0]` of the owner node through
  `kshim_qcom_irq_request()`, level high. This handles a GIC parent (mainline
  4-cell `<GIC_SPI 357 IRQ_TYPE_LEVEL_HIGH 0>`) and the Android PDC root
  (`<0 0x175 0>`, a pass-through outside `qcom,pdc-ranges`). Without
  `QCOM_IRQCHIP` it uses `kshim_irq_from_fdt()`.

## The GENI FIFO engine (I2C and SPI)

CrDK's `CrGeniInit`/`CrGeniI2cTransfer`/`CrGeniSpiTransfer`/`CrGeniQuiesce`
run unmodified. They are made interrupt-driven through their `CrIo` hooks:

- `CrGeniInit` sets `SE_IRQ_EN` to 0 and programs `M_IRQ_EN` with the core's
  status set. The engine then sets `SE_IRQ_EN.GENI_M_EN` (bit 2) so that M
  status reaches the SE's SPI. `CrGeniQuiesce` restores `SE_IRQ_EN` from its
  own snapshot, taken before kShimC touched it.
- The handler masks the SPI at the GIC and counts. It reads no SE register,
  and the status stays latched for the FIFO core.
- `CrIo.delay_us` is the FIFO core's poll step and its protocol delay (the
  SPI CS hold). It first busy-waits the requested time. Then, while an M
  command is active and no enabled status is pending, it unmasks the SPI and
  sleeps in `kshim_wait_event()` (WFI) for at most about 1 ms. The SE
  interrupt ends the sleep at once.
- Fallback: with no GIC (or before it is up), an interrupt that does not
  resolve, or eight sleeps that run out with enabled status pending and no
  interrupt delivered in between, the engine uses the plain busy-wait step
  and frees the SPI. The interrupt is tried again on a later transfer once
  `kshim_irq_ready()` turns true.
- Errors: NACK becomes KSHIM_IO_ERR_NACK and a timeout becomes
  ERROR_TIMEOUT, after CrDK's bounded cancel/abort and STOP recovery. If
  even that fails, the FIFO core is poisoned. The next transfer then tries
  `CrGeniQuiesce` + `CrGeniInit`, and returns ERROR_DEVICE_ERROR while the
  engine cannot be stopped.
- SPI: speed, mode and chip select are `CrGeniInit` configuration. A message
  for a device with other settings reprograms the engine (quiesce, then
  init). The SE clock is CXO, so the rate is 19.2 MHz / ceil(19.2 MHz /
  `spi-max-frequency`). The GT9916's 1 MHz becomes 960 kHz.
- Limits (the FIFO core's): I2C 7-bit addresses, `len` >= 1, at most 32
  messages between two STOPs, and SCL rates of 100 kHz, 400 kHz or 1 MHz.
  SPI supports CS 0-3, modes 0-3 with 8-bit MSB-first words, and at most 32
  segments. Timeouts are clamped to 1 s.

## UART and the console

- `kshim_uart_open_mmio()` uses the inherited configuration
  (`CrDebugUartPollInit`). `kshim_uart_open_dt()` opens a GENI node through
  its engine, with a claim and a clock acquire at the inherited rate. It
  also opens PL011 nodes. Only baud 0 is supported.
- TX is polled under `kshim_irq_save`. `kshim_uart_read_byte()` returns
  queued bytes first, then the RX interrupt queue (draining the FIFO when
  the queue is empty), then the polled FIFO.
- RX interrupt (`geni_rx_irq.c`): it sets only `SE_IRQ_EN.GENI_S` and
  `S_IRQ_EN.{RX_FIFO_WATERMARK, RX_FIFO_LAST}`. The handler drains through
  `CrDebugUartPollRead` into the 256-byte queue, where the newest bytes win.
  It never takes the console lock. Stop restores kShimC's bits first, then
  frees the SPI.
- The console behaves as before. It is opened from MMIO and its RX
  interrupt starts on the first read after the GIC is up (with
  `CONFIG_KSHIM_CONSOLE_RX_IRQ`). `kshim_console_rx_irq_stop()` is handoff
  step 1, and `kshim_console_quiesce()` (`CrDebugUartPollQuiesce`) is the
  last step of `prepare_boot`.
- DT instances still open at handoff are quiesced by
  `kshim_uart_quiesce_all()`, which `kshim_qup_quiesce_all()` runs first.

## Handoff

`kshim_io_quiesce()` runs `kshim_i2c_quiesce_all()` and
`kshim_spi_quiesce_all()` (each adapter: mask `SE_IRQ_EN.GENI_M`, free the
SPI, `CrGeniQuiesce`, release, unclaim). It then runs
`kshim_qup_quiesce_all()` (DT UARTs, then any engine still acquired, newest
first, skipping the console). Later transfers return KSHIM_IO_ERR_QUIESCED.
A `CrGeniQuiesce` timeout keeps the engine acquired and fails the step, so
handoff is refused.

## Board paths (DT-verified in `qup_dt_test`)

| Board | Bus | Engine | Child | IRQ |
| --- | --- | --- | --- | --- |
| andromeda (SM8150) | `i2c@0xc80000`, Android, 400 kHz | 0xc80000, wrapper 0xcc0000 | `fts@49` (FTM5) | `<0 0x175 0>` via PDC root, SPI 373 |
| hdk8250 (SM8250) | `qupv3_se13_i2c` `i2c@a94000`, Android, 400 kHz | 0xa94000, wrapper 0xac0000 | `st_fts@49` (FTM4) | SPI 358 |
| qrd8650 (SM8650) | `&spi4`, mainline | 0xa90000 (shared with i2c4), wrapper 0xac0000 | `touchscreen@0` GT9916, CS 0, mode 0, 1 MHz | SPI 357 |
| Pocket S2 (SM8650) | `&i2c4` (RFC), mainline, 400 kHz | 0xa90000 (shared with spi4) | `touchscreen@5d` GT911 | SPI 357 |

For andromeda, the old FTM5 binding (`kshim_dt_touch`, CrDK
`CrQupResources`) and the new layer agree on the engine window, the address
and the bus rate. The clock policy is the same (`clock_poll.c`), and the pins
come from the same `pinctrl-0` of the bus node. What changes is that
completion is interrupt-driven, with a fallback.

## Provenance

- GENI register offsets and fields: Linux `include/linux/soc/qcom/geni-se.h`
  layout as mirrored in CrDK `Include/Library/cr_geni_regs.h` (MIT).
- Clock policy: CrDK `Library/ClockLib/clock_poll.c` (MIT).
- Bindings: Linux `Documentation/devicetree/bindings/soc/qcom/qcom,geni-se.yaml`
  and the DTs in `tests/fixtures` (see `tests/fixtures/io/qup/README.md`).
- No GPL code was used. Linux `qcom-geni-se.c`, `i2c-qcom-geni.c` and
  `spi-geni-qcom.c` were not consulted for code.

## Tests

`tests/io/qup.cmake`: `qup_dt_test`, `qup_core_test`, `qup_i2c_test`,
`qup_spi_test` and `qup_uart_test`, built over `tests/io/qup_fakes.c` (fake
clocks and pinctrl, time, and a GENI register model with its SE line on the
fake GIC of `tests/irq_fake.c`). The console section of `input_irq_test`
covers the RX interrupt engine, and `usb_acm_test` covers the console on the
UART layer.

The GENI model answers either with canned read data or through modelled bus
devices (`geni_model_i2c_target()`, `geni_model_spi_target()`). An I2C
target sees each bus phase as the M sequencer runs it: START or repeated
START (a previous command with STOP_STRETCH), the address with R/W (it may
NACK), every TX FIFO byte (it may NACK), every read byte before the
command's CMD_DONE, and STOP (a command without STOP_STRETCH, or
STOP_ON_BUS). An SPI device sees CS_ASSERT and CS_DEASSERT with the
programmed mode, and exchanges one MISO byte per MOSI byte (RX-only clocks
MOSI low). `Geni.bus_log` records the I2C side as text, for example
`S 5d/W A 81 40 Sr 5d/R A [11] P `. `qup_i2c_test` checks a register
read with repeated START, a data NACK (the NACKed byte clocked, nothing
after it, STOP from the recovery) and an address NACK against such a device;
`qup_spi_test` checks full-duplex and RX-only exchanges under one CS.

`touch_goodix_bus_test` (`tests/io/touch_goodix.cmake`) runs this layer
without its clock and pinctrl fakes (`QUP_REAL_PLATFORM=1`): the QUP core,
the GENI I2C/SPI controllers and engine with the CrDK FIFO core, the real
clock driver, pinctrl, regulators and the TLMM irqchip, with the GT911 and
GT9916 as bus devices (docs/touch-goodix.md).

## Open points

- The clock, GPIO and pinctrl implementations come from the sm8650 owner.
  The QUP unit tests use fakes of their headers; `touch_goodix_bus_test`
  runs the QUP core on the real ones over SM8650 GCC and TLMM register
  models (`tests/io/sm8650_model.c`).
- The QUP core clock (`qup-core` interconnect) and the CX domain are
  assumed to be inherited. A cold engine behind a gated QUP core cannot be
  revived.
- The interrupt-driven waits depend on the SE line really being level-high
  while M status is pending, and on `SE_IRQ_EN.GENI_M_EN` gating it. This
  must be confirmed on hardware.
