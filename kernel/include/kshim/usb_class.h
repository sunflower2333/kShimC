/* SPDX-License-Identifier: MIT */
#pragma once
/*
 * USB class drivers shipped with kShimC (CONFIG_KSHIM_USB_HUB,
 * CONFIG_KSHIM_USB_HID). Owner: usbhid (kernel/drivers/usb/class/).
 * See docs/io-stack.md and kshim/usb.h for the class-driver contract.
 *
 * Hub (interface class 9): reads the hub descriptor (USB 2.0 11.23.2.1, or
 * the SuperSpeed hub descriptor 0x2a for a USB 3 hub on a SuperSpeed port),
 * reports it to the HCD (ops->hub_configure), powers the ports and waits
 * bPwrOn2PwrGood * 2 ms, then handles ports from its service hook: status
 * change endpoint (interrupt URB; the callback only records the bitmap),
 * GET_STATUS/CLEAR_FEATURE(C_*) per port, PORT_RESET, debounce (100 ms
 * connection stable), kshim_usb_enumerate() below the hub and
 * kshim_usb_disconnect() on removal. Tier limit KSHIM_USB_MAX_TIER.
 *
 * HID (interface class 3): GET_DESCRIPTOR(Report) from the interface's HID
 * descriptor length, kshim_hid_parse() + kshim_hid_input_init(); falls back
 * to SET_PROTOCOL(boot) with the fixed boot layouts for boot-subclass
 * keyboards and mice whose descriptor does not parse or map. SET_IDLE(0) is
 * sent and its STALL ignored. Reports arrive on the interrupt IN endpoint
 * (an URB resubmitted from its callback) and go to kshim_hid_input_report().
 * The HID driver registers a kshim_input_source with the capabilities it
 * found (KSHIM_INPUT_CAP_KEYS/POINTER) on its first bound interface and
 * keeps it registered. disconnect releases held keys
 * (kshim_hid_input_reset). A failing input endpoint is cleared
 * (CLEAR_FEATURE(HALT)) and resubmitted from the service hook, which also
 * runs the gamepad direction auto-repeat (kshim_hid_input_service).
 */
#include <stddef.h>
#include <stdint.h>

#include "error_no.h"

/* kshim_io_init() calls these after kshim_usb_init() and before
 * kshim_pci_init(). kshim_usb_hid_register() also registers the XInput
 * fallback below. */
int kshim_usb_hub_register(void);
int kshim_usb_hid_register(void);

/*
 * XInput fallback (usbhid addition): vendor-class gamepads with interface
 * class 0xff, subclass 0x5d, protocol 0x01 (Xbox 360-class wired
 * controllers and handheld controllers that copy them) have no HID report
 * descriptor. Their fixed 20-byte input report is described by an
 * equivalent HID report descriptor and mapped like a HID gamepad (A =
 * SELECT, B = BACK, D-pad / left stick Y = UP/DOWN). No start or LED
 * command is sent. Exposed for host tests.
 */
struct kshim_hid_input;
const uint8_t *kshim_usb_xinput_report_descriptor(size_t *length);
/* Feed one XInput message (non-input messages are ignored, returning 0). */
int kshim_usb_xinput_report(struct kshim_hid_input *input, const uint8_t *data, size_t length);

/*
 * Per-device quirks (additive, io-evidence): the extension point for a
 * device whose identity and reports were captured with
 * CONFIG_KSHIM_USB_TRACE (docs/usb-hid.md "Turning a capture into a
 * mapping"). Rows live in kernel/drivers/usb/class/hid_quirks.c, the table
 * ends with a row whose vendor is 0, and the first row matching vendor,
 * product (0 = any product of that vendor) and interface wins.
 *
 *  - KSHIM_HID_QUIRK_IGNORE: no HID/XInput binding for the interface.
 *  - KSHIM_HID_QUIRK_XINPUT: the interface (any class, for example a
 *    vendor class with a non-Xbox subclass) carries the XInput 20-byte
 *    report and is mapped like an Xbox 360 pad.
 *  - report_descriptor: a HID report descriptor written for the device.
 *    On a HID interface it replaces the device's own; on any other
 *    interface with an interrupt IN endpoint it describes that endpoint's
 *    reports, which then go through the HID mapping (kshim/hid.h).
 */
#define KSHIM_HID_QUIRK_IGNORE 0x1U
#define KSHIM_HID_QUIRK_XINPUT 0x2U
#define KSHIM_HID_QUIRK_ANY_INTERFACE (-1)

struct kshim_hid_quirk {
    uint16_t vendor, product;
    int16_t interface;                  /* bInterfaceNumber or KSHIM_HID_QUIRK_ANY_INTERFACE */
    uint16_t flags;                     /* KSHIM_HID_QUIRK_* */
    const uint8_t *report_descriptor;   /* optional */
    uint16_t report_descriptor_length;
    const char *name;                   /* quirk= in the log and the trace */
};

extern const struct kshim_hid_quirk kshim_hid_quirks[];
/* The row for one interface, or NULL. */
const struct kshim_hid_quirk *kshim_hid_quirk_find(uint16_t vendor, uint16_t product, uint8_t interface);
