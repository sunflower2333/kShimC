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
| `kernel/drivers/usb/class/hid.c` | HID transport and the XInput gamepad fallback |

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
- `tools/test_qemu_usb_input.py` is the end-to-end test over the PCI core
  (ECAM), xHCI, a usb-hub and HID devices.
