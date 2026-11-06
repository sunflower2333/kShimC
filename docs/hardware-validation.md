# Hardware validation checklist

Everything in the I/O stack (docs/io-stack.md) has been verified in QEMU and
against host register models only. This checklist turns the first boot on
each board into a definitive result. Capture the full console log (UART, or
the USB ACM mirror with `CONFIG_KSHIM_CONSOLE_USB_ACM=y`) and grep it for the
prefixes below. All messages come from `printf`, so they appear on every
console sink.

## Build and pack

```sh
python3 tools/build_runtime.py --config configs/<board>_defconfig --out build/<board>
# build/<board>/kShimC.bin is the runtime; its 8 MiB (or 4 MiB) slot is in the
# Image header, so MultiBootKernelPatcher needs no change.
```

Point the board's MultiBootKernelPatcher config at it. For the Pocket S2 that is
`Config/Shim.AYAPS.cfg`: set `Shim=` to `build/ayaneo_pocket_s2/kShimC.bin` and
`Path=` to the Android `Image`. Pack, then boot the patched kernel the usual
way. Prefer a temporary `fastboot boot`, so the installed boot image is
untouched.

## AYANEO Pocket S2 (`ayaneo_pocket_s2_defconfig`)

| Path | Pass looks like | Failure points to |
| --- | --- | --- |
| PCIe1 link | `PCIe:` lines for pcie1 with the link up (`up: inherited` when UEFI left it live) and `PCI: 0000:01:00.0 bound to xhci` | PERST 97 / supplies 121-123 / PHY clocks (docs/pci.md) |
| uPD720201 firmware | `xhci: Renesas 0014 firmware already running` or `... downloaded (version 0x...)` | `renesas_usb_fw.mem` source (docs/usb.md): EROFS vendor partitions are unreadable; use the embed option or an ext4/F2FS/FAT partition |
| xHCI | `xhci: ...: v1.xx, N slots, N ports`, `interrupt INTID n` (or `polling`) | INTx routing through the Qualcomm host |
| Gamepad identity | `usbtrace: dev ... vid=xxxx pid=xxxx`, `usbtrace: if ...`, `usbtrace: rdesc ...` | Send these lines back: they define any quirk row (docs/usb-hid.md) |
| Gamepad mapping | `usbtrace: hid ... map=gamepad` or `map=xinput`; menu moves with D-pad/stick, A selects, B backs | `map=unbound reason=...` → add a row in `kernel/drivers/usb/class/hid_quirks.c` |
| Volume/power | volume-up (PMIC GPIO 6) and PON move/select in the menu | `keys` / SPMI cascade (docs/interrupts.md) |
| Touch (GT911) | `GT911:` probe at 0x5d with product id `911`, `Touch:` IRQ 162 bound; taps select menu entries | Reset polarity (RFC says active-high), address select, `vreg_l14b` |
| Handoff | Android boots normally afterwards, with gamepad and touch working in Android | Quiesce/restore (docs/io-stack.md handoff contract) |

## QRD8650 (`qrd8650_defconfig`)

- `QUP:`/`SPI:` lines show spi4 acquired.
- `GT9916:` probe succeeds (Berlin device info).
- `Touch:` IRQ 162 is level-low, and taps work in the menu.
- Android touch works after handoff.

## HDK8250 (`hdk8250_defconfig`)

- `I2C:` lines show the FTM4 bus.
- `Touch:` FTM4 chip id is read.
- The AVDD (l13) high-power vote is applied (`regulator:`).
- Taps work in the menu, and Android touch works after handoff.

## Mix 3 5G / andromeda (`andromeda_defconfig`)

- FTM5 now runs on the touch framework (`Touch:`). Behaviour must match the
  previous build: taps in the menu, Android touch after handoff.

## Reporting back

For each board, report the log section for each row and whether the menu
input and the Android handoff worked. For an unknown gamepad, the
`usbtrace:` lines are enough to write its quirk row.
