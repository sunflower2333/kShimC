/* SPDX-License-Identifier: MIT */
#pragma once
/*
 * USB core internals shared by core.c (HCD registry, URBs, pumps, handoff)
 * and enumerate.c (device table, enumeration, class binding, root ports).
 * Not a public interface; see kshim/usb.h and kshim/usb_hcd.h.
 */
#include <config.h>
#include <kshim/usb.h>
#include <kshim/usb_hcd.h>

#define USB_CORE_MAX_HCDS 4U

/* Root-port connection state machine (USB 2.0 7.1.7.3: 100 ms attach
 * debounce before the reset; 9.2.6.3 recovery after SET_ADDRESS). */
enum usb_root_state {
    USB_ROOT_IDLE = 0,
    USB_ROOT_DEBOUNCE,  /* connected, waiting for a stable 100 ms */
    USB_ROOT_FAILED,    /* enumeration gave up until the device goes away */
};

struct usb_root_port {
    uint8_t state;      /* enum usb_root_state */
    uint8_t attempts;
    uint64_t deadline_ms;
};

struct usb_core {
    struct kshim_usb_hcd *hcds;
    unsigned hcd_count;
    struct usb_root_port root[USB_CORE_MAX_HCDS][KSHIM_USB_MAX_ROOT_PORTS];
    struct kshim_usb_class_driver *drivers;
    struct kshim_usb_device devices[KSHIM_USB_MAX_DEVICES];
    bool device_used[KSHIM_USB_MAX_DEVICES];
    struct kshim_usb_urb *done_head, *done_tail;
    bool in_events;     /* kshim_usb_poll_events() (HCD poll + callbacks) */
    bool draining;      /* given-back URBs are being finished */
    bool in_poll;       /* kshim_usb_poll() */
    bool quiesced;
    bool initialized;
    uint64_t settle_until_ms;
};

extern struct usb_core UsbCore;

/* core.c */
void usb_core_drain(void);
uint64_t usb_core_now_ms(void);

/* enumerate.c */
void usb_core_root_service(struct kshim_usb_hcd *hcd);
bool usb_core_root_busy(const struct kshim_usb_hcd *hcd);
void usb_core_bind_device(struct kshim_usb_device *device);
void usb_core_device_path(const struct kshim_usb_device *device, char *out, size_t size);

#if CONFIG_KSHIM_USB_TRACE
/* trace.c (bring-up trace, kshim/usb.h): after configuration, after class
 * binding, and in kshim_usb_disconnect() before the slot is freed. */
void usb_trace_device(struct kshim_usb_device *device);
void usb_trace_bound(struct kshim_usb_device *device);
void usb_trace_disconnect(struct kshim_usb_device *device);
#endif

#if CONFIG_KSHIM_USB_SELFTEST
/* selftest.c (QEMU self-test class driver). */
int kshim_usb_selftest_register(void);
void kshim_usb_selftest_settle(void);
#endif
