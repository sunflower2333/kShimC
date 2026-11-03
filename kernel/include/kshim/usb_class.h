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
 * (kshim_hid_input_reset).
 */
#include "error_no.h"

/* kshim_io_init() calls these after kshim_usb_init() and before
 * kshim_pci_init(). */
int kshim_usb_hub_register(void);
int kshim_usb_hid_register(void);
