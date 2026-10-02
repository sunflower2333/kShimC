# ST FTM5 protocol reference

`src/drivers/st_ftm5.c` implements the input protocol of the MIX 3 5G controller.
The reference is Xiaomi's `andromeda-p-oss` kernel, pinned in `UPSTREAM.json`.
That manifest records SHA256 hashes of the files fetched from GitHub,
including the board defconfig which selects `fts_521`, the device tree and
the controller protocol. The vendor driver is not linked into kShimC.

The phone uses I2C address 0x49 on SM8150 QUP2 SE0 (downstream SE17), with
1080 x 2340 coordinates. It is not the FTM4 protocol used by HDK8250. I2C
reads have no dummy byte; the optional SPI transport consumes one. The GPIO
and supply wiring is recorded in `configs/andromeda-provenance.json`.

After the board reset, READY is polled every 2 ms for up to 100 ms, following
`ftsTime.h`. The upstream three-reset retry limit is not a limit of three
FIFO reads. `ftsCore.c` controls the Linux host IRQ in its interrupt helpers;
it does not use the FTM4 hardware interrupt-register command. kShimC polls
the active-low GPIO after enabling scanning with `a0 00 01`.

MOTION can establish a contact when ENTER was missed, as in the vendor
handler. Hover never presses the UI. Unexpected READY or ERROR cancels
contacts and returns an error to the menu, which falls back to physical keys.
The shared UI delivers every contact transition before drawing, including a
complete tap read in one FIFO batch. Cancellation does not complete a click.

Handoff cancels contacts and requires a successful `a0 00 00` scan stop,
followed by the board's GENI/RPMh quiescence. Failed scan stops remain
retryable. Shared supply votes stay enabled. Firmware update, flash and
calibration commands are absent. Host tests and AArch64 builds do not
establish physical phone touch operation.
