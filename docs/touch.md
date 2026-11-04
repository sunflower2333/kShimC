# Touch framework and the ST FTM4/FTM5 drivers

This is the touch-st part of the I/O stack (`docs/io-stack.md` section 3.10).
The contract is `kernel/include/kshim/touch_driver.h`; this page describes how
it is implemented, the board bindings it was checked against, and where each
protocol fact comes from. Nothing here has run on hardware.

## Files

| File | Role |
| --- | --- |
| `kernel/drivers/input/touch/core.c` | Framework: selection, DT resources, power sequence, deferred IRQ, polling fallback, transform, quiesce |
| `kernel/drivers/input/touch/registry.c` | Driver table in match order: FTM5, FTM4, GT911, GT9916 |
| `kernel/drivers/input/touch/ftm5.c` | FTM5 driver: binds the existing protocol core to the framework |
| `kernel/drivers/st_ftm5.c`, `kernel/include/st_ftm5.h` | FTM5 protocol core (logic unchanged; bus-neutral, the GENI adapters are gone) |
| `kernel/drivers/input/touch/ftm4.c` | FTM4 driver, new, written from protocol facts |
| `kernel/drivers/input/touch/st_fts.h` | Android `st,fts` family check, raw-level GPIO polarity, vendor flips |
| `kernel/dt/touch.c` | `kshim_touch_dt_selected()` (Android `qcom,i2c-touch-active` and panel selection) |
| `kernel/core/input.c` | Public touch service; forwards to the framework (`CONFIG_KSHIM_TOUCH_FRAMEWORK`) |
| `kernel/drivers/touch.c`, `kernel/include/touch.h` | Contact tracker (primary contact, flip, scale) |

## Flow

`kshim_input_touch_start()` calls `kshim_touch_framework_start(fdt, w, h,
emit, ctx)`:

1. For each driver in `kshim_touch_drivers[]`, each compatible, each available
   node in DT order, `try_node()` runs. One device becomes active.
2. DT resources without device access: `touchscreen-*` properties, the reset
   and IRQ GPIOs (driver property lists first, then `reset-gpios` /
   `irq-gpios`), the TLMM interrupt (`interrupts` / `interrupts-extended`).
3. `driver->match` sees those and may adjust or reject the node. Supplies are
   resolved only for accepted nodes; an unsupported provider is logged and
   left as inherited.
4. The interrupt and the IRQ GPIO are reconciled (one pin serves both; the
   interrupt trigger decides the GPIO's active level; a pin mismatch drops the
   interrupt).
5. The bus is bound: `kshim_i2c_adapter_get()` / `kshim_spi_controller_get()`
   on the parent (first device access), then the client from `reg`.
6. Power: reset asserted, supplies on in table order, pinctrl `default`
   (Android: `pmx_ts_active`) once the rails are up, IRQ GPIO input,
   `power_on_delay_us`, reset released, `post_reset_delay_us`. A
   `driver->power_on` override gets pinctrl first and the IRQ GPIO is made an
   input after it.
7. The tracker gets a provisional range, then `driver->probe` runs with
   `max_x/max_y` preset to the DT size (0 when absent). The tracker then
   gets the final range: the DT size, else the probe's `max_x/max_y`. A
   failed probe runs `driver->quiesce` and releases everything.
8. The TLMM interrupt is requested when `CONFIG_KSHIM_INPUT_IRQ` and the GIC
   are up (otherwise at a later poll).

Events (`kshim_touch_framework_poll()`, from `kshim_input_touch_poll()`):

- The handler masks the pin and sets `irq_pending`; it never touches the bus.
  The poll runs `driver->handle` and unmasks. A level line still asserted
  re-fires at once; an edge latched while masked is kept by the TLMM.
- Right after the interrupt is requested one handle pass runs, for anything
  latched before.
- Polling fallback: no interrupt, a sealed TLMM pin, or the IRQ GPIO at its
  active level for 100 ms without a delivery. Then each poll calls `handle`
  when `driver->pending` (or the IRQ GPIO level) says so.
- A `handle` error cancels the contact, frees the interrupt and stops event
  processing. The device is still quiesced at stop. This is the legacy FTM5
  behaviour.

Reports go through `kshim_touch_report()` or straight into `dev->tracker` (the
FTM5 core does that). The tracker is configured with the touchscreen.yaml
transform: inversion in controller space, then the swap, done as the
tracker's flip followed by a 90 degree rotation with the Y flip toggled. It
then scales to the surface and emits PRESS/MOVE/RELEASE to the UI callback.
`kshim_touch_report()` also turns a MOVE for a contact that is not down into
ENTER.

Stop (`kshim_input_touch_stop()`, `kshim_touch_quiesce_all()`): free the
interrupt, `driver->quiesce`. If that fails and a reset GPIO exists, reset is
held for `power_on_delay_us` instead, which stops the controller. Without a
reset GPIO the error is returned (handoff refused) and the next call retries.
Then the contact is cancelled, reset is asserted while the supplies are
disabled newest first, and both GPIOs are freed so the registry restores their
snapshots. The call is idempotent.

## Boards

### Xiaomi Mi MIX 3 5G "andromeda" (SM8150): FTM5

From `andromeda-fdt.dtb` (`tests/fixtures`): `fts@49` on `i2c@0xc80000`
(`qcom,i2c-geni`), `interrupts = <122 0x2008>` on the TLMM,
`fts,irq-gpio = <&tlmm 122 0x2008>`, `fts,reset-gpio = <&tlmm 54 0>`,
`avdd-supply` = `touch_vddio_vreg` (regulator-fixed GPIO37, 4 ms,
`regulator-boot-on`), `vdd-supply` = `pm8150l_l1` (RPMh XOB `ldoc1`),
`fts,x-max/y-max` 1080 x 2340, `pinctrl-names = "pmx_ts_active", ...`.

- The Android properties carry raw levels. The reset pin is pulsed low
  whatever its flags cell says, and the IRQ flags cell `0x2008` is
  IRQF_ONESHOT | IRQ_TYPE_LEVEL_LOW, so the IRQ GPIO is active low.
- Sequence: reset low, gate, `ldoc1`, `pmx_ts_active`, 10 ms, reset high.
  Then the core waits for the READY event, reads chip ID 36 39 and the
  system info, and enables scanning. An I2C NACK while the controller boots
  is retried within the core's own 102 ms ready budget. This is new; the
  legacy path failed on the first NACK.

### Qualcomm SM8250 HDK (Kona): FTM4

From the OnePlus `kona-qrd.dtsi`/`kona-hdk.dtsi` (pinned in
`configs/hdk8250-touch-provenance.json`) and the Android `android-hdk8250.dtb`:
`st_fts@49` on `i2c@a94000` (`qupv3_se13_i2c`), `interrupts = <39 0x2008>`,
`st,irq-gpio = <&tlmm 39 0x2008>`, `st,reset-gpio = <&tlmm 38 0>`,
`avdd-supply` = `pm8150_l13` (`ldoa13`, 3.008 V) before `vdd-supply` =
`pm8150a_l1` (`ldoc1`, 1.8 V), `st,x-flip`, `st,y-flip`, `pmx_ts_active`.

- The stock HDK DT sets `qcom,i2c-touch-active = ""` and lists SW43404
  panels. `kshim_touch_dt_selected()` therefore selects nothing on a stock
  HDK, as before. The paired SW43404/ST configuration (selection `"st,fts"`
  and an active matching DSI panel) is required.
- No size property: the controller range is 1440 x 2880 (`fts.h`), flipped on
  both axes.

### FTM4 protocol facts

The driver is kShimC code. The facts come from the pinned OnePlus GPL-2.0
sources, used as evidence only (hashes in the provenance JSON), under
`drivers/input/touchscreen/st/`:

| Fact | Value | Source |
| --- | --- | --- |
| I2C address | 0x49 | `ftsHardware.h` I2C_SAD; DT `reg` |
| HW register op code | 0xB6, address big-endian, one dummy byte before read data | `ftsHardware.h` FTS_CMD_HW_REG_R/W, DUMMY_HW_REG; `ftsIO.c` readCmdU16 |
| Chip ID | 0x36 0x70 at 0x0004 | `ftsHardware.h` DCHIP_ID_0/1, DCHIP_ID_ADDR (non-FTM3) |
| Firmware version | 2 bytes at 0x0008 (logged only) | `ftsHardware.h` DCHIP_FW_VER_ADDR/BYTE |
| System reset | 0x80 at 0x0028 | `ftsHardware.h` SYSTEM_RESET_ADDRESS/VALUE |
| Interrupt enable | 0x41 / 0x00 at 0x002C | `ftsHardware.h` IER_ADDR, IER_ENABLE/DISABLE |
| FIFO | 8-byte events, depth 64, read one 0x85, flush 0xA1 | `ftsHardware.h` FIFO_* |
| Sense | on 0x93, off 0x92 | `ftsSoftware.h` FTS_CMD_MS_MT_SENSE_ON/OFF |
| Event IDs | none 0x00, enter 0x03, leave 0x04, motion 0x05, error 0x0F, controller ready 0x10, echo 0xEC | `ftsSoftware.h` EVENTID_* |
| Pointer layout | ID = b1[3:0]; X = b2 << 4 \| b4[7:4]; Y = b3 << 4 \| b4[3:0] | `fts.c` enter/leave handlers |
| Reset | reset GPIO low 20 ms, high, wait for controller ready (500 ms at 10 ms); without a GPIO, the system-reset write | `ftsTool.c` fts_system_reset, `ftsTool.h` timeouts |
| Touch enable | sense on, then interrupt enable | `ftsTool.c` cleanUp |
| Interrupt work | read single events until ID 0x00, at most the FIFO depth | `fts.c` fts_event_handler |
| Range | 1440 x 2880, 10 touch IDs | `fts.h` X/Y_AXIS_MAX, TOUCH_ID_MAX |

Choices made in kShimC that are not from those sources:

- The echo event after sense on is not waited for. It is skipped when it is
  read; whether echo is enabled is a firmware setting this subset does not
  read.
- A controller-ready event while running releases the contacts and enables
  sensing again, at most 3 times. FTM5 instead disables touch (legacy).
- Quiesce: sense off, interrupt disable, then FIFO flush so the level line
  deasserts.

## Migration from the legacy path

The legacy FTM5/QUP path is retired. It had the `kshim_dt_touch()` resource
prober, the `resources.c` touch windows, the CrDK `CrQup*` bring-up in
`kernel/core/input.c`, the IRQ-context FIFO drain with its event queue in
`input_irq.c`, and the FTM5 GENI adapters. `CONFIG_KSHIM_TOUCH` without a
`KSHIM_TOUCH_<chip>` now only delivers injected pointer events.
`configs/andromeda_defconfig` selects FTM5 and `configs/hdk8250_defconfig`
selects FTM4, each with `KSHIM_I2C`, `KSHIM_QUP` and `KSHIM_REGULATOR`. Their
implementations come from the qup and sm8650 branches, so this commit
links only once those are merged.

## Tests

- `touch_framework_test`: the framework over fakes and
  `tests/fixtures/io/touch-framework.dts`. It covers selection and match
  rejection, I2C vs SPI binding, resource resolution, power order (common and
  overridden), the deferred IRQ, the stuck-level and sealed fallbacks, a late
  interrupt start, the transform, handle errors, failed candidates and the
  quiesce fallback.
- `touch_st_test`: FTM5 on the real andromeda DTB, and FTM4 on the kona
  fixture and on the real Android HDK DTB when `-DKSHIM_HDK8250_DTB=<path>`
  exists (the DTB is not committed; it is GPL-2.0 vendor DT). Each runs
  against a byte-level I2C model of the controller.
- `touch_st_irq_test`: the andromeda FTM5 case through the real TLMM cascade
  (`tlmm.c`) over the `irq_fake.c` register model.
- Existing, migrated: `st_ftm5_test` (direct transports instead of the
  removed GENI adapters), `dt_test` and `irqchip_test`
  (`kshim_touch_dt_selected()` and the touch node instead of
  `kshim_dt_touch()`), and `input_irq_test` (keys and console; the FTM5
  IRQ-context drain is gone, and `touch_st_irq_test` covers the deferred
  path). Unchanged: `touch_test`, `qcom_touch_platform_test` and
  `hdk8250_touch_platform_test`, which test the CrDK `CrQup` resource
  library that kShimC no longer uses for touch.

## Risks (hardware not tested)

- The FTM4 protocol has never run against the chip. The HW-register read
  framing and the chip-ID position are inferred from the constants and the
  generic read routine, because the vendor `readChipInfo` is not in the pinned
  file set.
- HDK `ldoa13` needed HPM (mode 7) in the legacy table for the 20 mA AVDD
  load. `kshim/regulator.h` has no load or mode call, so the rail keeps
  whatever mode RPMh aggregates.
- Android `qcom,i2c-geni` nodes without `clock-frequency` run at the I2C
  core's default. The vendor default is 400 kHz.
- The touch quiesce now restores the reset pin and returns rails to their
  inherited state. The legacy path left the rails on and reset released.
