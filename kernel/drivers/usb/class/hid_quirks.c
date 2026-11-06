/* SPDX-License-Identifier: MIT */
/*
 * Per-device HID/XInput quirks (kshim/usb_class.h). This table is the
 * extension point for a device captured with CONFIG_KSHIM_USB_TRACE whose
 * reports kShimC does not map by itself; docs/usb-hid.md ("Turning a
 * capture into a mapping") shows how to write a row from the usbtrace
 * lines. Keep each row next to its provenance: the capture it came from
 * (board, firmware version, date) and what each byte of the report means.
 *
 * Example rows (not active):
 *
 *   An XInput clone whose vendor interface uses another subclass:
 *     {0x1234, 0x5678, 0, KSHIM_HID_QUIRK_XINPUT, NULL, 0, "example-xinput"},
 *
 *   A vendor-class pad with a 4-byte report (buttons, hat, X, Y) described
 *   by a report descriptor written for it:
 *     static const uint8_t ExamplePad[] = {
 *         0x05, 0x01, 0x09, 0x05, 0xa1, 0x01,       // Generic Desktop, Game Pad
 *         0x05, 0x09, 0x19, 0x01, 0x29, 0x08,       // Buttons 1..8
 *         0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
 *         0x05, 0x01, 0x09, 0x39, 0x25, 0x07, 0x75, 0x04, 0x95, 0x01,
 *         0x81, 0x42, 0x81, 0x01,                   // hat (null state), pad
 *         0x09, 0x30, 0x09, 0x31, 0x26, 0xff, 0x00, 0x75, 0x08, 0x95, 0x02,
 *         0x81, 0x02, 0xc0,                         // X, Y
 *     };
 *     {0x1234, 0x9abc, 1, 0, ExamplePad, sizeof(ExamplePad), "example-pad"},
 *
 *   An interface that must be left alone:
 *     {0x1234, 0x9abc, 2, KSHIM_HID_QUIRK_IGNORE, NULL, 0, "example-ignore"},
 */
#include <kshim/usb_class.h>

const struct kshim_hid_quirk kshim_hid_quirks[] = {
    /* Add rows above this terminator. */
    {0, 0, 0, 0, NULL, 0, NULL},
};
