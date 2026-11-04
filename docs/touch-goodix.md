# Goodix touch drivers: GT911 (I2C) and GT9916 (SPI)

Owner: touch-goodix. These are two drivers on the touch framework
(`kernel/include/kshim/touch_driver.h`, owner touch-st). See
`docs/io-stack.md` section 3.10 for the framework.

| | GT911 | GT9916 |
| --- | --- | --- |
| Kconfig | `KSHIM_TOUCH_GT911` (TOUCH && I2C && REGULATOR) | `KSHIM_TOUCH_GT9916` (TOUCH && SPI && REGULATOR) |
| Source | `kernel/drivers/input/touch/goodix_gt911.c` | `kernel/drivers/input/touch/goodix_gt9916.c` |
| Compatible | `goodix,gt911` | `goodix,gt9916` |
| Board | AYANEO Pocket S2 / S2 Pro | Qualcomm QRD8650 |
| Bus | `&i2c4` (SE 0xa90000), 400 kHz, address 0x5d | `&spi4` (SE 0xa90000), CS 0, mode 0, 1 MHz |
| IRQ | TLMM 162, falling edge | TLMM 162, level low |
| Reset | TLMM 161, `GPIO_ACTIVE_HIGH` (see below) | TLMM 161, `GPIO_ACTIVE_LOW` |
| Supplies (driver order) | `AVDD28`, `VDDIO` (both vreg_l14b_3p2) | `vddio` (absent on QRD), `avdd` (vreg_l14b_3p2) |
| Size | 1440 × 2560 | 1080 × 2400 |

`goodix_touch.h` holds what both drivers share: the frame-to-event diff
(`goodix_report_frame`: new track IDs ENTER, kept ones MOVE, missing ones
LEAVE at their last position), the bus-idle check (all 0x00 or all 0xff),
the inherited-rail check and the polling cadence (10 ms).

## Provenance

No GPL code was copied or adapted. The drivers use hardware and protocol
facts only. Each source file's header comment lists them next to where they
come from.

- GT911: the Goodix GT911 datasheet (I2C addresses, the RSTB/INT
  address-select sequence and its T2-T5 timings) and the Goodix "GT911
  Programming Guide" (register map, configuration layout and checksum,
  status handshake, point layout).
- GT9916: the Goodix Berlin host-interface facts as the mainline Linux
  `goodix_berlin_core.c` / `goodix_berlin_spi.c` drivers and Goodix's Berlin
  Android driver use them: SPI framing, register addresses, the
  boot-option handshake, the firmware-version and IC-information layouts,
  the event layout and checksums, and the power and reset delays. Those
  drivers are GPL-2.0; they were used as evidence for facts and nothing
  else.
- DT bindings `goodix.yaml` and `goodix,gt9916.yaml` give property names, the
  0x5d/0x14 address set and the required properties. The board facts come
  from the BSD-3-Clause mainline DTS files and the Pocket S2 RFC patch (see
  `tests/fixtures/io/goodix/README.md`).

## GT911

**Power-on and address select.** The controller samples INT when RSTB
(active low) rises: INT low selects 0x5d and INT high selects 0x14. The
driver overrides `power_on`:

1. RSTB low and INT driven low, then the rails (AVDD28, then VDDIO).
2. Wait 20 ms (T2 > 10 ms), then drive INT to the level for the DT address.
3. Wait 200 µs (T3 > 100 µs), then raise RSTB.
4. Wait 6 ms (T4 > 5 ms), then drive INT low for 50 ms (T5).
5. Turn INT into an input for the interrupt.

Without an IRQ GPIO the INT level is left to the board pull. Without a
reset GPIO the chip comes out of its power-on reset by itself. In both
cases `probe` falls back to the other address if the DT one does not
answer, and logs the address it found.

**Reset polarity (RFC check).** The Pocket S2 RFC writes
`reset-gpios = <&tlmm 161 GPIO_ACTIVE_HIGH>`. For the `goodix,gt9xx`
binding the reset GPIO's logical value is the RSTB level: 0 holds the chip
in reset. The Linux goodix driver drives it that way, and all 18
`goodix,gt911` nodes in mainline arm/arm64 (38 of the binding's 40 nodes)
use `GPIO_ACTIVE_HIGH`. The RFC also pulls GPIO161 down, which holds the
chip in reset while the pad is an input. The RFC is therefore consistent
with the GT911's active-low RSTB wired straight to the SoC. The driver
calls `kshim_touch_set_reset(dev, rstb_high)` and keeps RSTB driven high
while in use, because that pull-down would reset the chip if the pad were
released. This is not verified on hardware.

**Inherit first.** If every supply already reports on, RSTB reads high and
the product ID answers at the DT address, the controller is used as it is.
The driver takes the rail references (inherited-on rails are not written)
and drives RSTB high explicitly. Otherwise it runs the cold sequence.

**Probe.** It reads the product block at 0x8140 (ID "911", firmware
version, resolution) with three attempts, then the address fallback.
Next it reads the configuration at 0x8047-0x80FF. With a valid checksum,
the driver takes X/Y max and Touch_Number from it, plus the INT trigger
(Module_Switch1 [1:0]). A trigger that differs from the DT replaces
`dev->irq_trigger`, because the chip's configuration decides the waveform.
Without a valid checksum it uses the product resolution and 5 points. A DT
`touchscreen-size-*` wins over the chip's resolution, and a mismatch is
logged. Finally the driver clears a frame latched before it listened.

**Events.** `handle` reads the status (0x814E) and the first point (0x814F)
in one transfer. If more points are present it reads them, reports the
frame, and always writes 0 to 0x814E. A count above the configured limit
is reported as `KSHIM_IO_ERR_PROTOCOL` (the buffer is still released).
`pending` reads the status at a 10 ms cadence. Sampling the pin would miss
the short INT pulse.

**Quiesce.** The driver releases the report buffer and forgets its
contacts. It sends no sleep command. A controller kShimC powered goes back
to its inherited rail and reset state through the framework and GPIO
restore, and one that was inherited running stays running for the payload.
Quiesce always returns 0, because nothing it does needs restoring.

## GT9916 (Berlin-D)

**Wire format.** SPI mode 0 with 8-bit words (the driver sets both, as the
binding fixes them). Each access is one chip-select assertion. A write is
`0xF0, addr[31:0] big-endian, data`. A read is
`0xF1, addr, 0xFF 0xFF 0xFF`, and the data follows while the host clocks
zeros.

**Power-on.** The cold path asserts reset, enables the rails (vddio, then
avdd), waits 18 ms (vddio 3 ms plus avdd 15 ms), releases reset and waits
4 ms. The driver does this in `power_on` and also lists the delays in
`power_on_delay_us`/`post_reset_delay_us`. The inherit path is taken when
the supplies are on, reset is not asserted and a checksummed firmware
version reads back. It skips the reset and the handshake.

**Probe.** `touchscreen-size-x/y` are required, because Berlin has no
resolution register. After a cold power-on the driver runs the boot-option
handshake: it writes eight 0xAA bytes to 0x10000 and reads them back, with
three attempts 5 ms apart. It then waits 100 ms. Next it reads the 28-byte
firmware version at 0x10014 (checksum, not the bus-idle pattern) and the IC
information at 0x10070. The IC information is an LE16 length below 1024,
fixed blocks, five counted LE16 lists, then the misc block whose LE32 at +44
is the touch data address, and a checksum at the end. Each read gets three
attempts. Last, the driver releases any pending event.

**Events.** `handle` reads 26 bytes from the touch data address: the
8-byte header, two point slots and their checksum. It returns early when
the status byte is 0. The header checksum (LE16 sum of bytes 0-5) is
checked. On a touch event (status bit 7) the count is byte 2 [3:0], at most
10, and points beyond two take a second read. The point checksum covers
count × 8 bytes, or two slots when the first point is a stylus. Stylus
points (type 1 or 3) are skipped, and finger IDs are bits [7:4]. The event
is always released by writing one 0 byte at the touch data address, which
deasserts the level IRQ. Request code 3 (status bit 6) is served after the
release: all contacts are lifted, reset is asserted for 2 ms, then the
driver waits 100 ms.

**Polling and quiesce.** `pending` is true when the IRQ GPIO is at its
active level, else every 10 ms. Quiesce works as for the GT911.

## Assumptions about the framework (touch-st)

The framework is not on this branch, so the tests fake it
(`tests/io/touch_goodix_fake.c`). The drivers rely on these points of the
`touch_driver.h` contract:

1. `kshim_touch_set_reset(dev, v)` drives the reset GPIO's *logical* value
   `v` through its DT flags, where "asserted" is the DT active level. The
   GT911 passes the RSTB level.
2. `kshim_touch_irq_gpio_output(dev, level_high)` drives a *physical* level.
3. `power_on` replaces the whole common sequence. The framework requests
   the TLMM interrupt after `probe`, using `dev->irq_trigger` as `probe`
   left it (the GT911 may correct it).
4. `kshim_touch_i2c_write_read` addresses `dev->i2c.addr` at call time (the
   GT911 address fallback changes it in `probe`).
5. `dev->priv` is zeroed before `match`/`power_on`. `dev->props` is filled
   before `probe`. `dev->probed` is true only after a successful `probe`.
   `dev->max_x/max_y` already hold the DT size when it is given.
6. `quiesce` may run more than once and after a failed probe.

## Tests

- `touch_goodix_gt911_test`: an I2C register model that latches its
  address from INT at the RSTB edge and flags every T2-T5 violation. It
  covers both addresses, the product and config reads (valid, bad checksum,
  unreadable), the trigger correction, DT size precedence, one-, two- and
  three-read frames, ENTER/MOVE/LEAVE diffing, the status handshake,
  protocol and bus errors, inherited and mis-addressed controllers, no
  IRQ/reset GPIO fallbacks, polling cadence and quiesce.
- `touch_goodix_gt9916_test`: an SPI model of the Berlin-D map. It checks
  the framing and dummy bytes, the rail and reset timing, the handshake,
  version and IC-info checksums and list walking (corrupt, truncated, idle
  bus), events with one and two reads, stylus slots, header/point checksum
  errors, more than 10 points, bus errors, the level IRQ deasserting on
  release, reset requests, inheritance, polling and quiesce.
- `touch_goodix_dt_test`: resolves both boards from the real mainline DTBs.
  The Pocket S2 RFC node is applied as an overlay with libfdt. The test
  checks the SE window, the bus parameters, the TLMM interrupt through
  `kshim_dt_irq`, the reset specifiers, the driver supply tables against
  the case-sensitive properties, the RPMh rail behind them ("ldob14"),
  sizes, pinctrl states and the GT911 `match`.

Nothing here has run on hardware.

## Board notes

- QRD8650 mainline DT: the pinctrl state labelled `ts_irq` configures
  GPIO161 (the reset line) and `ts_reset` configures GPIO162 (the
  interrupt). Both pull up at 8 mA, so applying both states is harmless.
  `ts_irq` also carries `output-disable`, which the driver then overrides
  for reset.
- Pocket S2: i2c4 and spi4 share SE 0xa90000. Only i2c4 is enabled, by the
  RFC overlay.
