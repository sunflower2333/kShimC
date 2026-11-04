/* SPDX-License-Identifier: MIT */
#pragma once
/*
 * USB host core: descriptors, device model, transfers and class drivers
 * (CONFIG_KSHIM_USB).
 * Owner: xhci (kernel/drivers/usb/core/). Class drivers (hub, HID) are
 * owned by usbhid. Host controller drivers implement kshim/usb_hcd.h.
 * See docs/io-stack.md.
 *
 * Execution model (deferred, single-threaded):
 *  - HCD interrupt handlers only acknowledge the interrupter and set
 *    hcd->irq_pending; they never complete URBs.
 *  - kshim_usb_poll_events() runs every HCD's event processing (ops->poll);
 *    URB completion callbacks run there: thread context, boot CPU, never
 *    nested. Callbacks must not wait (no synchronous transfers); they may
 *    resubmit URBs and set flags for their driver's service hook.
 *  - kshim_usb_poll() = kshim_usb_poll_events() + root/hub port handling and
 *    class drivers' service hooks (which may do synchronous transfers, which
 *    in turn only pump kshim_usb_poll_events()). It is registered as an
 *    input source service (kshim/input_inject.h), so the menu's key and
 *    touch polls drive it; kshim_io_init() runs it during the initial
 *    bounded enumeration (kshim_usb_settle()).
 *  - Synchronous calls wait with kshim_completion_wait() pumping
 *    kshim_usb_poll_events(): interrupt-driven with a polling fallback.
 *
 * Buffers passed to URBs may be any heap or static memory: the core bounces
 * through kshim_dma_alloc() memory when a buffer is not suitable.
 * Multi-byte descriptor fields are little-endian on the wire; this header's
 * packed structs are used on little-endian AArch64 (and host tests) only.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "error_no.h"
#include <kshim/io.h>

/* ---- Chapter 9 ---------------------------------------------------------- */

#define KSHIM_USB_DT_DEVICE        0x01
#define KSHIM_USB_DT_CONFIG        0x02
#define KSHIM_USB_DT_STRING        0x03
#define KSHIM_USB_DT_INTERFACE     0x04
#define KSHIM_USB_DT_ENDPOINT      0x05
#define KSHIM_USB_DT_BOS           0x0f
#define KSHIM_USB_DT_HID           0x21
#define KSHIM_USB_DT_REPORT        0x22
#define KSHIM_USB_DT_HUB           0x29
#define KSHIM_USB_DT_SS_HUB        0x2a
#define KSHIM_USB_DT_SS_EP_COMP    0x30

#define KSHIM_USB_DIR_OUT          0x00
#define KSHIM_USB_DIR_IN           0x80
#define KSHIM_USB_TYPE_STANDARD    0x00
#define KSHIM_USB_TYPE_CLASS       0x20
#define KSHIM_USB_TYPE_VENDOR      0x40
#define KSHIM_USB_RECIP_DEVICE     0x00
#define KSHIM_USB_RECIP_INTERFACE  0x01
#define KSHIM_USB_RECIP_ENDPOINT   0x02
#define KSHIM_USB_RECIP_OTHER      0x03

#define KSHIM_USB_REQ_GET_STATUS        0x00
#define KSHIM_USB_REQ_CLEAR_FEATURE     0x01
#define KSHIM_USB_REQ_SET_FEATURE       0x03
#define KSHIM_USB_REQ_SET_ADDRESS       0x05
#define KSHIM_USB_REQ_GET_DESCRIPTOR    0x06
#define KSHIM_USB_REQ_GET_CONFIGURATION 0x08
#define KSHIM_USB_REQ_SET_CONFIGURATION 0x09
#define KSHIM_USB_REQ_SET_INTERFACE     0x0b
#define KSHIM_USB_FEATURE_ENDPOINT_HALT 0x00

#define KSHIM_USB_CLASS_PER_INTERFACE 0x00
#define KSHIM_USB_CLASS_HID           0x03
#define KSHIM_USB_CLASS_HUB           0x09
#define KSHIM_USB_CLASS_VENDOR        0xff

#define KSHIM_USB_ENDPOINT_XFER_MASK 0x03
#define KSHIM_USB_ENDPOINT_DIR_IN    0x80

struct kshim_usb_setup {
    uint8_t bmRequestType;
    uint8_t bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} __attribute__((packed));

struct kshim_usb_device_descriptor {
    uint8_t bLength, bDescriptorType;
    uint16_t bcdUSB;
    uint8_t bDeviceClass, bDeviceSubClass, bDeviceProtocol, bMaxPacketSize0;
    uint16_t idVendor, idProduct, bcdDevice;
    uint8_t iManufacturer, iProduct, iSerialNumber, bNumConfigurations;
} __attribute__((packed));

struct kshim_usb_config_descriptor {
    uint8_t bLength, bDescriptorType;
    uint16_t wTotalLength;
    uint8_t bNumInterfaces, bConfigurationValue, iConfiguration, bmAttributes, bMaxPower;
} __attribute__((packed));

struct kshim_usb_interface_descriptor {
    uint8_t bLength, bDescriptorType;
    uint8_t bInterfaceNumber, bAlternateSetting, bNumEndpoints;
    uint8_t bInterfaceClass, bInterfaceSubClass, bInterfaceProtocol, iInterface;
} __attribute__((packed));

struct kshim_usb_endpoint_descriptor {
    uint8_t bLength, bDescriptorType;
    uint8_t bEndpointAddress, bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t bInterval;
} __attribute__((packed));

struct kshim_usb_ss_ep_comp_descriptor {
    uint8_t bLength, bDescriptorType;
    uint8_t bMaxBurst, bmAttributes;
    uint16_t wBytesPerInterval;
} __attribute__((packed));

/* USB 2.0 hub descriptor (11.23.2.1), fixed part; the variable
 * DeviceRemovable/PortPwrCtrlMask bytes follow. */
struct kshim_usb_hub_descriptor {
    uint8_t bDescLength, bDescriptorType, bNbrPorts;
    uint16_t wHubCharacteristics;
    uint8_t bPwrOn2PwrGood, bHubContrCurrent;
} __attribute__((packed));

/* HID class descriptor (HID 1.11, 6.2.1) with its first entry. */
struct kshim_usb_hid_descriptor {
    uint8_t bLength, bDescriptorType;
    uint16_t bcdHID;
    uint8_t bCountryCode, bNumDescriptors;
    uint8_t bReportDescriptorType;
    uint16_t wReportDescriptorLength;
} __attribute__((packed));

/* ---- Descriptor parsing helpers ----------------------------------------- */

/* Iterate a configuration descriptor's sub-descriptors: *offset starts at 0
 * (the config header itself is returned first). Returns a pointer to the
 * next descriptor whose bLength is >= 2 and fits, or NULL at the end or on
 * a malformed length. */
const uint8_t *kshim_usb_desc_next(const uint8_t *config, size_t length, size_t *offset);
/* First descriptor of `type` between `from` (exclusive) and the next
 * interface descriptor; for class descriptors in an interface. */
const uint8_t *kshim_usb_desc_find_in_interface(const uint8_t *config, size_t length,
                                                const uint8_t *from, uint8_t type);

/* ---- Device model ------------------------------------------------------- */

enum kshim_usb_speed {
    KSHIM_USB_SPEED_UNKNOWN = 0,
    KSHIM_USB_SPEED_LOW,
    KSHIM_USB_SPEED_FULL,
    KSHIM_USB_SPEED_HIGH,
    KSHIM_USB_SPEED_SUPER,
    KSHIM_USB_SPEED_SUPER_PLUS,
};

enum kshim_usb_xfer_type {
    KSHIM_USB_XFER_CONTROL = 0,
    KSHIM_USB_XFER_ISOC = 1,
    KSHIM_USB_XFER_BULK = 2,
    KSHIM_USB_XFER_INTERRUPT = 3,
};

#define KSHIM_USB_MAX_INTERFACES 8U
#define KSHIM_USB_MAX_IF_ENDPOINTS 8U
#define KSHIM_USB_MAX_DEVICES 32U
#define KSHIM_USB_MAX_TIER 6U

struct kshim_usb_hcd;
struct kshim_usb_device;
struct kshim_usb_class_driver;

struct kshim_usb_endpoint {
    uint8_t address;        /* bEndpointAddress (0 / 0x80 for EP0) */
    uint8_t type;           /* enum kshim_usb_xfer_type */
    uint16_t max_packet;    /* wMaxPacketSize bits 10:0 */
    uint8_t mult;           /* HS high-bandwidth: bits 12:11, SS: bmAttributes */
    uint8_t max_burst;      /* SS companion */
    uint8_t interval;       /* bInterval as in the descriptor */
    bool halted;
    void *hcd_priv;         /* HCD endpoint state (xHCI transfer ring) */
};

struct kshim_usb_interface {
    struct kshim_usb_device *device;
    uint8_t number, alternate;
    uint8_t class_code, subclass, protocol;
    uint8_t endpoint_count;
    struct kshim_usb_endpoint endpoints[KSHIM_USB_MAX_IF_ENDPOINTS];
    /* The interface descriptor inside device->config (class descriptors follow it). */
    const uint8_t *descriptor;
    const struct kshim_usb_class_driver *driver;
    void *driver_data;
};

struct kshim_usb_device {
    struct kshim_usb_hcd *hcd;
    struct kshim_usb_device *parent;  /* hub, NULL on a root port */
    uint8_t port;                     /* 1-based port on the parent (or root hub) */
    uint8_t root_port;                /* 1-based root hub port of the topology branch */
    uint8_t tier;                     /* 1 on a root port */
    uint32_t route;                   /* xHCI route string (4 bits per tier below the root) */
    enum kshim_usb_speed speed;
    uint8_t address;                  /* USB address (set by the HCD) */
    /* LS/FS behind a HS hub: the TT hub's HCD slot and port. The core
     * fills tt_port; the HCD fills tt_hub_slot in device_alloc (the core
     * does not know HCD slot ids). 0 when no TT is involved. */
    uint8_t tt_hub_slot, tt_port;
    struct kshim_usb_device_descriptor descriptor;
    uint8_t *config;                  /* full active configuration descriptor (heap) */
    uint16_t config_length;
    uint8_t config_value;
    struct kshim_usb_endpoint ep0;
    uint8_t interface_count;
    struct kshim_usb_interface interfaces[KSHIM_USB_MAX_INTERFACES];
    /* Hub devices: number of ports and the hub driver's state. */
    uint8_t hub_ports;
    void *hcd_priv;                   /* HCD device state (xHCI slot id, contexts) */
    bool configured, gone;
};

/* ---- Transfers ---------------------------------------------------------- */

#define KSHIM_USB_URB_SHORT_NOT_OK 0x1U /* a short IN transfer is KSHIM_IO_ERR_SHORT */
#define KSHIM_USB_URB_ZERO_PACKET  0x2U /* end an OUT transfer of max_packet multiple with a ZLP */

struct kshim_usb_urb;
/* Thread context (kshim_usb_poll_events), boot CPU. */
typedef void (*kshim_usb_complete_fn)(struct kshim_usb_urb *urb);

struct kshim_usb_urb {
    struct kshim_usb_device *device;
    struct kshim_usb_endpoint *endpoint;  /* &device->ep0 for control */
    struct kshim_usb_setup setup;         /* control transfers only */
    void *buffer;
    uint32_t length;
    uint32_t flags;                       /* KSHIM_USB_URB_* */
    kshim_usb_complete_fn complete;       /* may be NULL for synchronous use */
    void *context;
    /* Results, valid in complete(): */
    uint32_t actual;
    int status;   /* 0, KSHIM_IO_ERR_STALL/SHORT/OVERFLOW/CANCELLED/NO_DEVICE,
                     ERROR_TIMEOUT, ERROR_DEVICE_ERROR; KSHIM_IO_ERR_PENDING while queued */
    /* Owned by the core/HCD. */
    void *hcd_priv;
    void *bounce;
    struct kshim_completion done;
    /* Additive (xhci): core-owned link of the given-back list that
     * kshim_usb_poll_events() drains, the bounce allocation size and the
     * transfer direction captured at submit. */
    struct kshim_usb_urb *done_next;
    uint32_t bounce_size;
    bool in;
};

/* Queue an URB (control, bulk or interrupt; isochronous is not supported).
 * Interrupt URBs complete once per filled transfer; resubmit from the
 * callback to keep polling. ERROR_NOT_READY after the device is gone. */
int kshim_usb_submit_urb(struct kshim_usb_urb *urb);
/* Cancel; the URB completes with KSHIM_IO_ERR_CANCELLED unless it already
 * completed. Thread context. */
int kshim_usb_cancel_urb(struct kshim_usb_urb *urb);

/* Synchronous helpers (thread context; not from URB callbacks). */
int kshim_usb_control(struct kshim_usb_device *device, uint8_t request_type, uint8_t request,
                      uint16_t value, uint16_t index, void *data, uint16_t length,
                      uint32_t timeout_ms, uint32_t *actual);
int kshim_usb_bulk(struct kshim_usb_device *device, struct kshim_usb_endpoint *endpoint,
                   void *data, uint32_t length, uint32_t timeout_ms, uint32_t *actual);
int kshim_usb_interrupt(struct kshim_usb_device *device, struct kshim_usb_endpoint *endpoint,
                        void *data, uint32_t length, uint32_t timeout_ms, uint32_t *actual);
int kshim_usb_get_descriptor(struct kshim_usb_device *device, uint8_t type, uint8_t index,
                             uint16_t language, void *data, uint16_t length, uint32_t *actual);
/* CLEAR_FEATURE(ENDPOINT_HALT) plus the HCD's endpoint reset. */
int kshim_usb_clear_halt(struct kshim_usb_device *device, struct kshim_usb_endpoint *endpoint);
/* Look up an endpoint of an interface by type and direction (IN = true). */
struct kshim_usb_endpoint *kshim_usb_find_endpoint(struct kshim_usb_interface *interface,
                                                   enum kshim_usb_xfer_type type, bool in);

/* ---- Enumeration (root ports and the hub class driver) ------------------ */

/* Enumerate a device on `port` of `parent` (NULL = root hub of `hcd`), whose
 * port was reset and enabled at `speed`: HCD slot, address, device and
 * configuration descriptors, the first configuration with a supported
 * interface (else configuration 1), endpoint setup, then class driver
 * binding per interface. Thread context. On failure everything is undone. */
int kshim_usb_enumerate(struct kshim_usb_hcd *hcd, struct kshim_usb_device *parent,
                        uint8_t port, enum kshim_usb_speed speed,
                        struct kshim_usb_device **device);
/* Unbind class drivers (children first, recursively for hubs), cancel URBs,
 * free the HCD slot and the device. Thread context. */
void kshim_usb_disconnect(struct kshim_usb_device *device);

/* ---- Class drivers ------------------------------------------------------ */

#define KSHIM_USB_MATCH_ANY (-1)

struct kshim_usb_class_driver {
    const char *name;
    /* Interface match; -1 (KSHIM_USB_MATCH_ANY) is a wildcard. vendor and
     * product 0 match any device. */
    int16_t class_code, subclass, protocol;
    uint16_t vendor, product;
    /* Thread context; synchronous transfers allowed. 0 binds the interface. */
    int (*probe)(struct kshim_usb_interface *interface);
    /* Device gone or handoff: cancel URBs, free state. No transfers to a
     * gone device. */
    void (*disconnect)(struct kshim_usb_interface *interface);
    /* Deferred work from kshim_usb_poll() (thread context, may transfer);
     * optional. */
    void (*service)(struct kshim_usb_interface *interface);
    struct kshim_usb_class_driver *next; /* owned by the core */
};

/* Register before kshim_pci_init() (kshim_io_init orders this); a driver
 * registered later is offered every unbound interface. First match in
 * registration order wins. */
int kshim_usb_class_register(struct kshim_usb_class_driver *driver);

/* ---- Lifecycle (kernel/core/io.c) ---------------------------------------- */

/* Core state, the xHCI PCI driver registration (CONFIG_KSHIM_USB_XHCI) and
 * the input-source service registration. */
int kshim_usb_init(const void *fdt);
/* Run kshim_usb_poll() until no port change is pending and every port has
 * been handled, or until timeout_ms (initial enumeration). */
int kshim_usb_settle(uint32_t timeout_ms);
void kshim_usb_poll_events(void);
void kshim_usb_poll(void);
/* Additive (xhci): a class driver with port work still in progress (the hub
 * driver's connect debounce) keeps kshim_usb_settle() polling for at least
 * `ms` more milliseconds. Thread context. */
void kshim_usb_settle_extend(uint32_t ms);
/* Additive (xhci): "low", "full", "high", "super", "super+" or "unknown". */
const char *kshim_usb_speed_name(enum kshim_usb_speed speed);
/* Handoff step "USB": class drivers' disconnect and device slots freed,
 * newest topology first, on every HCD. Idempotent. */
int kshim_usb_quiesce(void);
/* Handoff step "xHCI": every HCD's ops->quiesce. Idempotent. */
int kshim_usb_hcd_quiesce_all(void);
