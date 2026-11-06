# USB hub, HID and input injection

This is the usbhid part of the I/O stack (`docs/io-stack.md` sections 3.7 to
3.9). It turns USB keyboards, mice, tablets and gamepads into boot-menu
input. Nothing here has run on hardware.

## Pieces

| File | Role |
| --- | --- |
| `kernel/core/input_inject.c` | Key and pointer queues, source registry, service pump, pointer to touch-stream conversion (`kshim/input_inject.h`) |
| `kernel/drivers/hid/hid_parse.c` | HID 1.11 report descriptor parser (`kshim/hid.h`) |
| `kernel/drivers/hid/hid_input.c` | Report to menu-event mapping |
| `kernel/drivers/usb/class/hub.c` | Hub class driver, USB 2.0 and SuperSpeed |
| `kernel/drivers/usb/class/hid.c` | HID transport, the XInput gamepad fallback and the quirk lookup |
| `kernel/drivers/usb/class/hid_quirks.c` | Per-device quirk rows (the extension point for captured devices) |
| `kernel/drivers/usb/core/trace.c` | Bring-up trace (`CONFIG_KSHIM_USB_TRACE`), owned by the core |

## Data path

1. The HCD completes the HID interrupt-IN URB from its event ring. It does this
   from its interrupt (which only marks work pending) or from polling.
2. The URB callback maps the report and calls `kshim_input_inject_key` or
   `kshim_input_inject_pointer`. Then it resubmits the URB.
3. The menu drains the events through the consumer hooks that are already in
   the tree. These hooks run `kshim_input_service()` first, which pumps
   `kshim_usb_poll()`:
   - `kshim_key_source_poll()` for keys;
   - `kshim_input_touch_poll()` for the pointer, which needs `KSHIM_TOUCH`.

Hub port work, endpoint recovery and gamepad auto-repeat run from the class
drivers' service hooks, which are also driven by that pump.

## Mapping

Key events carry Linux codes and are edges, scoped per report ID.

| Source | UP | DOWN | SELECT | BACK |
| --- | --- | --- | --- | --- |
| Keyboard | Up arrow | Down arrow | Enter, Keypad Enter, Space | Escape, Backspace |
| Gamepad / joystick | Hat N/NE/NW, D-pad Up, stick Y < -50 % | Hat S/SE/SW, D-pad Down, stick Y > +50 % | Button 1 | Button 2 |
| Consumer | Volume Increment | Volume Decrement | | |
| Mouse wheel | Wheel + | Wheel − | | |

Gamepad details:

- Every direction source on a gamepad uses the code `BTN_DPAD_UP` or
  `BTN_DPAD_DOWN`. A hat and a stick that point the same way therefore give
  one press.
- A stick is released when it returns inside 25 %, and that band is the dead
  zone.
- The X axis is not mapped.
- A held direction repeats after 500 ms and then every 150 ms. Each repeat is
  a `PRESS` event with `pressed = 2`.
- Keyboards, buttons and wheels never repeat.

Pointer details:

- Mouse X/Y are relative and Button 1 is the press.
- Tablet and digitizer X/Y are absolute, and the press is Button 1 or Tip
  Switch.
- The pointer reaches the menu as `PRESS`, `MOVE`, `RELEASE` or `HOVER` with
  contact id 15. The LVGL port maps `BACK` to `LV_KEY_ESC` and handles
  `HOVER`. In the serial menu, `BACK` moves the selection to the default
  entry without booting it.

Boot keyboards and mice fall back to `SET_PROTOCOL(boot)` and the HID 1.11
appendix B layouts when their report descriptor does not parse or does not
map.

## XInput fallback

Some gamepads have no HID report descriptor: interface class 0xff, subclass
0x5d, protocol 0x01. These are Xbox 360-class wired pads, and many handheld
controllers copy them. Their fixed 20-byte input report is mapped as follows:

1. The report is described by an equivalent HID report descriptor (in `hid.c`).
2. The Y axes are negated to the HID convention.
3. The report goes through the same mapping as a HID gamepad: A is SELECT,
   B is BACK, and the D-pad and left stick Y are UP/DOWN.

No start, LED or rumble command is sent. Xbox One (GIP, 0xff/0x47/0xd0) pads
need a start command and are not handled.

Provenance: the interface triples and the 20-byte layout are facts from
Linux `drivers/input/joystick/xpad.c` (GPL-2.0-or-later) at
`40288c9206c17eb66a603262e06a58d300d0f279` (sha256 `92d10718...8bf200`):
`XPAD_XBOX360_VENDOR_PROTOCOL` (0xff/93/1), `XPAD_XBOXONE_VENDOR_PROTOCOL`
(0xff/71/208) and the byte and bit positions read by
`xpad360_process_packet`. No xpad code was used. kShimC feeds an equivalent
HID report descriptor to its own parser, which is unrelated to xpad's
structure.

## Capturing an unknown device

The AYANEO Pocket S2's internal gamepad sits behind pcie1 and the uPD720201
xHCI, and its USB VID:PID and report format are not published.
`CONFIG_KSHIM_USB_TRACE` (on in `configs/ayaneo_pocket_s2_defconfig`) makes
the first boot record everything needed to map it.

### How to capture

1. Build and flash the Pocket S2 profile as usual. The trace goes through
   `printf`, so it reaches the console UART and, when
   `CONFIG_KSHIM_CONSOLE_USB_ACM=y`, the USB CDC-ACM mirror on the USB-C
   port. The Pocket S2 profile does not enable the mirror; without access
   to the UART pads, add `CONFIG_KSHIM_CONSOLE_USB_ACM=y` to a local copy of
   the defconfig. Output written before the host opens the port is kept
   (16 KiB), which holds the enumeration records of a few devices.
2. On the host, open the port before or right after boot, for example
   `picocom -b 115200 /dev/ttyACM0 | tee pocket-s2-usb.log`.
3. When the menu is up, press each control once, one at a time, slowly:
   D-pad up, down, left, right, A, B, X, Y, the shoulder buttons and
   triggers, then move each stick to its four end points. Only reports that
   differ from the previous one are printed, at most
   `CONFIG_KSHIM_USB_TRACE_REPORTS` (32) per interface, so one control per
   report lets you see which bits change.
4. `grep '^usbtrace:' pocket-s2-usb.log > capture.txt` and keep the file
   with the board, firmware version and date.

### Record format

Each record is one line: `usbtrace:`, an event word, `path=` and (for
interface records) `if=`, then space-separated `key=value` fields. Values
never contain spaces, except a final quoted `reason="..."` or `text="..."`.
Hex dumps are contiguous lowercase byte pairs, 32 bytes per line, with the
byte offset in `off=` (hex) and the full length in `len=`; a dump is cut
at 1024 bytes and its last line then ends with `trunc=1`. `path` is
`<controller>-<root port>[.<hub port>...]`, as in the other USB log lines.

| Event | From | Fields |
| --- | --- | --- |
| `dev` | core, after configuration | `addr speed vid pid rel usb class(dev class/subclass/protocol) mps0 config nconfig ifaces tier route(xHCI route string, hex) tt(TT hub slot/port)` |
| `str` | core | `kind(manufacturer, product) index lang text` (non-ASCII as `?`); absent when the device has no strings |
| `if` | core, per interface | `alt class(class/subclass/protocol) eps(<address>:<ctrl,isoc,bulk,int>:<max packet>:<bInterval>,...` or `-`) |
| `cfg` | core | the configuration descriptor as read (hex dump) |
| `rdesc` | HID driver | the interface's report descriptor as read (hex dump), or `len error=` |
| `hid` | HID driver | `map(gamepad, joystick, keyboard, mouse, tablet, consumer, keys, boot-keyboard, boot-mouse, xinput) caps(keys, pointer, keys+pointer) ep len(URB bytes) report_bytes ids(report IDs used) quirk`; or `map=unbound quirk reason="..."` |
| `bind` | core, per interface | `class driver(usb-hid, xinput, usb-hub, usb-hid-quirk, none)` |
| `sniff` | core | an unbound interface's interrupt IN endpoint is read for the trace only: `ep len started`; it maps nothing |
| `report` | HID driver, sniffer | `seq len hex held`: `held` lists the mapped codes held after the report (`<Linux code hex>:<up,down,select,back>`), `-` for none, `unbound` for a sniffed interface; a final `seq=<n> limit=1` line marks the cap |
| `gone` | core, disconnect | `vid pid` |

Example from `usb_gamepad_test`, an excerpt grouped by device (a generic HID gamepad on a root port and an
Xbox 360 wired controller behind a high-speed hub):

```
usbtrace: dev path=0-1 addr=1 speed=full vid=1209 pid=0007 rel=0100 usb=0200 class=00/00/00 mps0=8 config=1 nconfig=1 ifaces=1 tier=1 route=00000 tt=0/0
usbtrace: str path=0-1 kind=product index=2 lang=0409 text="Test Pad"
usbtrace: if path=0-1 if=0 alt=0 class=03/00/00 eps=81:int:8:4
usbtrace: cfg path=0-1 len=34 off=000 hex=09022200010100a0320904000001030000000921110100012237000705810308
usbtrace: cfg path=0-1 len=34 off=020 hex=0004
usbtrace: rdesc path=0-1 if=0 len=55 off=000 hex=05010905a101150026ff00750895020930093181022507463b01651475049501
usbtrace: rdesc path=0-1 if=0 len=55 off=020 hex=093981426500450005091901290c25017501950c8102c0
usbtrace: hid path=0-1 if=0 map=gamepad caps=keys ep=81 len=8 report_bytes=4 ids=0 quirk=none
usbtrace: bind path=0-1 if=0 class=03/00/00 driver=usb-hid
usbtrace: report path=0-1 if=0 seq=1 len=4 hex=80800800 held=-
usbtrace: report path=0-1 if=0 seq=2 len=4 hex=80800000 held=220:up
usbtrace: dev path=0-2.1 addr=3 speed=full vid=045e pid=028e rel=0114 usb=0200 class=ff/ff/ff mps0=8 config=1 nconfig=1 ifaces=4 tier=2 route=00001 tt=2/1
usbtrace: str path=0-2.1 kind=manufacturer index=1 lang=0409 text="?Microsoft Corporation"
usbtrace: str path=0-2.1 kind=product index=2 lang=0409 text="Controller"
usbtrace: if path=0-2.1 if=0 alt=0 class=ff/5d/01 eps=81:int:32:4,01:int:32:8
usbtrace: if path=0-2.1 if=1 alt=0 class=ff/5d/03 eps=82:int:32:2,02:int:32:4,83:int:32:64,03:int:32:16
usbtrace: if path=0-2.1 if=2 alt=0 class=ff/5d/02 eps=84:int:32:16
usbtrace: if path=0-2.1 if=3 alt=0 class=ff/fd/13 eps=-
usbtrace: cfg path=0-2.1 len=153 off=000 hex=09029900040100a0fa0904000002ff5d01001121000101258114000000001301
...
usbtrace: hid path=0-2.1 if=0 map=xinput caps=keys ep=81 len=32 report_bytes=20 ids=0 quirk=none
usbtrace: bind path=0-2.1 if=0 class=ff/5d/01 driver=xinput
usbtrace: bind path=0-2.1 if=1 class=ff/5d/03 driver=none
usbtrace: sniff path=0-2.1 if=1 ep=82 len=32 started=1
usbtrace: bind path=0-2.1 if=3 class=ff/fd/13 driver=none
usbtrace: report path=0-2.1 if=0 seq=1 len=3 hex=01030e held=-
usbtrace: report path=0-2.1 if=0 seq=4 len=20 hex=0014001000000000000000000000000000000000 held=130:select
usbtrace: report path=0-2.1 if=0 seq=7 len=20 hex=0014000000000000ff7f00000000000000000000 held=220:up
usbtrace: report path=0-2.1 if=1 seq=1 len=3 hex=010300 held=unbound
usbtrace: gone path=0-2.1 vid=045e pid=028e
```

To turn a dump back into bytes:
`grep '^usbtrace: rdesc path=0-1 if=0 ' capture.txt | sed 's/.*hex=//; s/ .*//' | tr -d '\n' | xxd -r -p > rdesc.bin`;
any HID descriptor decoder (for example `hidrd-convert -o spec rdesc.bin`)
then lists its items.

Costs: the trace adds GET_DESCRIPTOR(String) requests at enumeration and
reads interrupt IN endpoints no driver claimed; a report line takes about
10 ms on a 115200 baud UART. Leave it off in a release build.

## Turning a capture into a mapping

Read the records of the gamepad's path:

1. `hid ... map=gamepad|joystick|xinput caps=keys` and the menu reacts:
   nothing to do. Record the VID:PID and the layout in this document.
2. A HID interface (`class=03/..`) with `map=unbound`, or a mapping that is
   wrong (A is not Button 1, the D-pad is a vendor usage, the stick sits on
   another usage): decode `rdesc`, then write a corrected report descriptor
   for the same report bytes (the `report` lines show which bits move per
   control) and add a quirk row with it. It replaces the device's own.
3. A vendor interface (`class=ff/..`) other than `ff/5d/01` whose sniffed
   reports start `00 14` with the XInput layout: a row with
   `KSHIM_HID_QUIRK_XINPUT`.
4. A vendor interface with any other report format: work out each control
   from the sniffed `report` lines, describe the report as a HID report
   descriptor (as `hid.c` does for XInput) and add a row with it. The
   reports then go through the normal HID mapping.
5. An interface that disturbs the menu (a mouse-like touch pad, a keyboard
   interface that repeats): `KSHIM_HID_QUIRK_IGNORE`.

A device that only reports after a vendor start command (Xbox One GIP,
`ff/47/d0`) cannot be described by a row; it needs code in `hid.c`.

The rows live in `kernel/drivers/usb/class/hid_quirks.c`
(`struct kshim_hid_quirk`, `kshim/usb_class.h`): vendor, product (0 = any),
interface (`KSHIM_HID_QUIRK_ANY_INTERFACE` = all), flags, an optional
report descriptor and a name that appears as `quirk=` in the trace and as
`, quirk <name>` in the HID log line. The first matching row wins; the
table ends with a row whose vendor is 0. Rows with the XInput flag or a
descriptor register a class driver (`usb-hid-quirk`) for that VID:PID that
is tried before the generic ones, so a vendor-class interface binds and the
core counts the configuration as supported. The file's header shows one
example of each kind; `tests/io/usb_gamepad_quirks.c` has working rows for
all three, exercised end to end by `usb_gamepad_test`. Keep the capture
(board, firmware version, date) next to a row as its provenance.

## Core ownership

The USB core is the xhci implementer's `kernel/drivers/usb/core/` (`core.c`,
`desc.c`, `enumerate.c`; see `docs/usb.md`). The usbhid branch carried a
stand-in core (`usb_core.c`) until the merge; the integration removed it and
moved the host tests onto the xhci core.

The class drivers use only the public `kshim/usb.h` API. They assume the core
contract on these points:

- the core disconnects children before their hub, and it calls the class
  driver's `disconnect` hook;
- URB callbacks never run nested;
- `kshim_usb_settle()` cannot see a hub port's 100 ms connect debounce, so
  the hub driver calls `kshim_usb_settle_extend()` while a port debounces;
- the HCD, not the core, fills `tt_hub_slot` in `device_alloc` (the core
  fills `tt_port`).

## Tests

- `usbhid_hid_test` covers the parser, the mapping, the XInput adapter, the
  injection queue and the `kshim_key_source_poll()` hook. Its descriptors are
  the QEMU usb-kbd, usb-mouse and usb-tablet report descriptors, plus written
  Xbox-style, generic joystick and consumer descriptors.
- `usbhid_stack_test` runs the xhci core, the hub and HID over a fake HCD
  that hands URBs back with `kshim_usb_hcd_giveback()`. It covers
  nested hubs (full-speed, high-speed with TT, SuperSpeed), boot fallback,
  STALL recovery, hotplug with debounce, a dead device and the handoff order.
- `usb_gamepad_test` (scenarios irq, poll, takeover, nohalt) runs the real
  PCI core and ECAM host, `xhci_pci.c` with the Renesas uPD720201 path
  (firmware already running), the xHCI driver, the USB core with the
  trace, hub, HID, XInput, the quirk table, injection and
  `kshim_key_source_poll()`, through `kshim_io_init()` and
  `kshim_io_quiesce()`, over the PCI configuration model and the xHCI
  model (`tests/io/xhci_model.c`, extended with hubs, route strings, TT
  checks, per-endpoint report queues, interrupt OUT, HID class requests,
  string descriptors and a bus-master gate). Topology: a generic HID
  gamepad on a root port; a high-speed hub with an Xbox 360 wired
  controller (045e:028e, the full four-interface configuration), a report-ID
  joystick, a low-speed keyboard, a vendor pad mapped by a quirk descriptor
  and an XInput clone bound by the XInput quirk. It checks route strings and
  TT slot/port, intervals, every trace record, the menu actions (hat, stick
  dead zone and hysteresis, buttons, repeat, XInput buttons/D-pad/stick and
  its status messages, report IDs, quirk layouts), unplug and re-plug below
  the hub and on a root port, removal of the whole hub with a key held, and
  the handoff: xHCI halted then reset, interrupt freed, the PCI
  configuration exactly as found (bus mastering and decode off after a
  cold start, UEFI's values after a takeover), nothing moving afterwards;
  a controller that never halts refuses the handoff with bus mastering off.
- `tools/test_qemu_usb_input.py` is the end-to-end test over the PCI core
  (ECAM), xHCI, a usb-hub and HID devices.
