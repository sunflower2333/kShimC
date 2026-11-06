/* SPDX-License-Identifier: MIT */
/*
 * The quirk table usb_gamepad_test links instead of
 * kernel/drivers/usb/class/hid_quirks.c: one row per kind of entry the
 * extension point offers (kshim/usb_class.h), for devices the test plugs in.
 */
#include <kshim/usb_class.h>

/* A vendor-class pad (1209:000a, interface ff/00/00) whose 2-byte report is
 * byte 0 = buttons 1..8 (bit 0 confirm, bit 1 back), byte 1 bit 0 = D-pad
 * up, bit 1 = D-pad down. Written for it as a HID report descriptor. */
static const uint8_t VendorPadReport[] = {
    0x05, 0x01, 0x09, 0x05, 0xa1, 0x01,             /* Generic Desktop, Game Pad */
    0x05, 0x09, 0x19, 0x01, 0x29, 0x08,             /* Buttons 1..8 */
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x05, 0x01, 0x09, 0x90, 0x09, 0x91,             /* D-pad Up, D-pad Down */
    0x95, 0x02, 0x81, 0x02,
    0x95, 0x06, 0x81, 0x01,                         /* padding */
    0xc0,
};

const struct kshim_hid_quirk kshim_hid_quirks[] = {
    {0x1209, 0x000a, 0, 0, VendorPadReport, sizeof(VendorPadReport), "test-vendor-pad"},
    /* An XInput clone (1209:000c) on a vendor interface ff/ff/00 ... */
    {0x1209, 0x000c, 0, KSHIM_HID_QUIRK_XINPUT, NULL, 0, "test-xinput-clone"},
    /* ... whose second (HID consumer-control) interface is left alone. */
    {0x1209, 0x000c, 1, KSHIM_HID_QUIRK_IGNORE, NULL, 0, "test-ignore"},
    {0, 0, 0, 0, NULL, 0, NULL},
};
