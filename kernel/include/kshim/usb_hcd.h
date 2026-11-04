/* SPDX-License-Identifier: MIT */
#pragma once
/*
 * USB host controller driver interface (CONFIG_KSHIM_USB).
 * Owner: xhci (core side in kernel/drivers/usb/core/, xHCI in
 * kernel/drivers/usb/host/). See docs/io-stack.md and kshim/usb.h for the
 * execution model.
 *
 * The core owns topology and enumeration; the HCD owns slots, endpoint
 * rings and the schedule. All ops run in thread context on the boot CPU,
 * never concurrently (the core serialises), except that the HCD's interrupt
 * handler may run at any time: it acknowledges the interrupter, sets
 * hcd->irq_pending and returns.
 */
#include <stdbool.h>
#include <stdint.h>

#include <kshim/usb.h>

#define KSHIM_USB_MAX_ROOT_PORTS 32U

/* Port status in USB 2.0 hub terms (wPortStatus/wPortChange, 11.24.2.7),
 * which root-hub ops translate to (xHCI PORTSC). */
#define KSHIM_USB_PORT_CONNECTION   0x0001U
#define KSHIM_USB_PORT_ENABLE       0x0002U
#define KSHIM_USB_PORT_OVER_CURRENT 0x0008U
#define KSHIM_USB_PORT_RESET        0x0010U
#define KSHIM_USB_PORT_POWER        0x0100U
#define KSHIM_USB_PORT_C_CONNECTION 0x0001U /* in .change */
#define KSHIM_USB_PORT_C_ENABLE     0x0002U
#define KSHIM_USB_PORT_C_OVER_CURRENT 0x0008U
#define KSHIM_USB_PORT_C_RESET      0x0010U

struct kshim_usb_port_status {
    uint16_t status;               /* KSHIM_USB_PORT_* */
    uint16_t change;               /* KSHIM_USB_PORT_C_* */
    enum kshim_usb_speed speed;    /* valid with CONNECTION and ENABLE */
};

struct kshim_usb_hcd;
struct kshim_usb_hcd_ops {
    /* ---- Root hub ---- */
    int (*port_status)(struct kshim_usb_hcd *hcd, uint8_t port, struct kshim_usb_port_status *status);
    /* Clear change bits (KSHIM_USB_PORT_C_*). */
    int (*port_clear_change)(struct kshim_usb_hcd *hcd, uint8_t port, uint16_t change);
    /* Reset (USB2) or warm-reset-if-needed (USB3) and wait, bounded, until
     * the port is enabled; *speed receives the negotiated speed. */
    int (*port_reset)(struct kshim_usb_hcd *hcd, uint8_t port, enum kshim_usb_speed *speed);
    /* Disable a port whose device failed enumeration (optional). */
    int (*port_disable)(struct kshim_usb_hcd *hcd, uint8_t port);

    /* ---- Devices ---- (the core fills speed/route/root_port/parent/tt_* first) */
    /* Allocate the device's slot (xHCI Enable Slot) and its default control
     * endpoint state for ep0.max_packet (8/64/512 by speed). */
    int (*device_alloc)(struct kshim_usb_hcd *hcd, struct kshim_usb_device *device);
    /* Assign the address (xHCI Address Device, BSR=0); sets device->address. */
    int (*device_address)(struct kshim_usb_hcd *hcd, struct kshim_usb_device *device);
    /* ep0.max_packet changed after reading bMaxPacketSize0 (xHCI Evaluate Context). */
    int (*device_update_ep0)(struct kshim_usb_hcd *hcd, struct kshim_usb_device *device);
    /* Enable every endpoint of the selected configuration's interfaces
     * (xHCI Configure Endpoint) before SET_CONFIGURATION is sent. */
    int (*device_configure)(struct kshim_usb_hcd *hcd, struct kshim_usb_device *device);
    /* Hub class hook: mark the device as a hub with `ports` downstream ports,
     * TT think time (0..3) and multi-TT (xHCI slot context Hub/MTT/TTT). */
    int (*hub_configure)(struct kshim_usb_hcd *hcd, struct kshim_usb_device *device,
                         uint8_t ports, uint8_t tt_think_time, bool multi_tt);
    /* Reset a halted endpoint's HCD state (xHCI Reset Endpoint + Set TR Dequeue). */
    int (*endpoint_reset)(struct kshim_usb_hcd *hcd, struct kshim_usb_device *device,
                          struct kshim_usb_endpoint *endpoint);
    /* Release the slot (xHCI Disable Slot); outstanding URBs were cancelled. */
    void (*device_free)(struct kshim_usb_hcd *hcd, struct kshim_usb_device *device);

    /* ---- Transfers ---- */
    int (*submit)(struct kshim_usb_hcd *hcd, struct kshim_usb_urb *urb);
    /* Stop the endpoint and complete the URB with KSHIM_IO_ERR_CANCELLED. */
    int (*cancel)(struct kshim_usb_hcd *hcd, struct kshim_usb_urb *urb);
    /* Process the event ring: complete URBs (calling urb->complete), note
     * port changes with kshim_usb_hcd_port_changed(). Returns events handled. */
    int (*poll)(struct kshim_usb_hcd *hcd);

    /* ---- Handoff ---- */
    /* Free the interrupt, stop the schedule (R/S=0, wait HCHalted), reset
     * (HCRST) and restore what the HCD changed outside its own memory
     * (interrupter registers, legacy-support ownership, PCI-side state
     * belongs to the PCI driver quiesce). Leaves inherited firmware loaded
     * (Renesas). Idempotent. */
    int (*quiesce)(struct kshim_usb_hcd *hcd);
};

struct kshim_usb_hcd {
    const char *name;
    const struct kshim_usb_hcd_ops *ops;
    void *priv;
    uint8_t port_count;               /* root hub ports (1-based numbering) */
    bool irq_driven;                  /* interrupt live; otherwise poll() is the only path */
    volatile bool irq_pending;        /* set in IRQ context, cleared by kshim_usb_poll_events */
    /* Owned by the core. */
    struct kshim_usb_device *root[KSHIM_USB_MAX_ROOT_PORTS];
    uint32_t port_change_mask;        /* bit n-1: root port n needs handling */
    bool quiesced;
    unsigned index;
    struct kshim_usb_hcd *next;
};

/* After the controller runs: the core scans every root port at the next
 * kshim_usb_poll(). */
int kshim_usb_hcd_register(struct kshim_usb_hcd *hcd);
/* IRQ-safe: root port `port` (1-based) changed; handled by kshim_usb_poll(). */
void kshim_usb_hcd_port_changed(struct kshim_usb_hcd *hcd, uint8_t port);
/* Additive (xhci): the HCD hands a finished URB back to the core (thread
 * context: ops->poll, ops->cancel, ops->device_free, or a failed submit
 * path after it was accepted). The core records status/actual, copies a
 * bounced IN buffer back, and runs urb->complete and the completion from
 * kshim_usb_poll_events(), never nested inside an HCD op. */
void kshim_usb_hcd_giveback(struct kshim_usb_urb *urb, int status, uint32_t actual);

/* xHCI glue (CONFIG_KSHIM_USB_XHCI): registers the PCI driver for class
 * 0x0c0330 and the Renesas uPD720201/uPD720202 IDs 1912:0014/1912:0015
 * (firmware download with CONFIG_KSHIM_USB_XHCI_RENESAS). Called by
 * kshim_usb_init(). */
int kshim_xhci_pci_register(void);
