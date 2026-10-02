# ST FTM4 protocol reference

`src/drivers/st_ftm4.c` implements the volatile input protocol from the downstream ST
driver pinned in `UPSTREAM.json`. The header snapshots and source hashes make
the chip identity, opcodes and event layout traceable to that revision.

The controller must report READY within 500 ms, using the vendor's 10 ms
polling interval. Hardware reads include a dummy response byte. The driver
accepts chip ID bytes `36 70`, reads at most 64 FIFO events per poll and applies
the HDK8250 panel's two axis flips. Native coordinates are 1440 x 2880;
the shared touch layer scales them to the selected framebuffer.

Handoff requires successful SENSE_OFF and IRQ-disable commands before board
resources are restored. A failure cancels the active contact without completing
a UI click and remains retryable. FIFO events are delivered individually to
LVGL, preserving taps whose press and release arrive in one poll. The board
layer also waits for pending RPMh acknowledgements and
drains GENI before releasing its register state. It leaves shared supply votes
enabled. See `configs/hdk8250-touch-provenance.json` for the independent HDK
pin, clock, supply and display evidence.

This driver excludes firmware updates, flash operations and calibration. Host
models and AArch64 compilation validate the implementation; physical touch
and production firmware access remain untested.
