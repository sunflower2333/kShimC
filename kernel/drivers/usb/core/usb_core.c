/* SPDX-License-Identifier: MIT */
/*
 * USB host core (kshim/usb.h, kshim/usb_hcd.h; docs/io-stack.md 3.7):
 * device model, enumeration over the HCD ops, URB bookkeeping with
 * synchronous helpers, root-port handling and the class-driver registry.
 *
 * Written from the USB 2.0 specification (chapter 9 requests and descriptors,
 * 7.1.7.3 connect debounce of 100 ms, 7.1.7.5 reset recovery of 10 ms,
 * 9.2.6.3 SET_ADDRESS recovery of 2 ms) and USB 3.2 (bMaxPacketSize0 as an
 * exponent, the SuperSpeed endpoint companion descriptor). The xHCI route
 * string (4 bits of hub port per tier below the root) is xHCI 1.2 section
 * 8.9. No code from other stacks.
 *
 * Execution model (kshim/usb.h): single-threaded on the boot CPU. URB
 * completions run from kshim_usb_poll_events() through a core trampoline,
 * which restores the caller's buffer and callback, completes urb->done for
 * the synchronous helpers and then calls the caller's callback. Port work and
 * class-driver service hooks run from kshim_usb_poll().
 */
#include <kshim/usb.h>
#include <kshim/usb_hcd.h>
#include <kshim/io.h>
#include <kshim/irq.h>
#include <kshim/time.h>
#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if CONFIG_KSHIM_INPUT_INJECT
#include <kshim/input_inject.h>
#endif
#if CONFIG_KSHIM_USB_XHCI
/* Bus-master HCDs get buffers from the DMA pool (kernel/core/dma.c). */
#include <kshim/dma.h>
#define USB_DMA_BOUNCE 1
#else
#define USB_DMA_BOUNCE 0
#endif

#define USB_MAX_HCDS 4U
#define USB_MAX_URBS 64U
#define USB_MAX_CONFIGS 4U
#define USB_MAX_CONFIG_BYTES 4096U
#define USB_CONTROL_TIMEOUT_MS 1000U
#define USB_DEFAULT_TIMEOUT_MS 5000U
#define USB_CANCEL_WAIT_US 200000U
#define USB_DEBOUNCE_MS 100U
#define USB_RESET_RECOVERY_MS 10U
#define USB_SET_ADDRESS_RECOVERY_MS 2U
#define USB_ENUMERATE_ATTEMPTS 2U
/* kshim_usb_settle(): no port work pending and no enumeration activity for
 * this long (covers a hub's own 100 ms debounce of its initial ports). */
#define USB_SETTLE_QUIET_MS 300U
#define USB_DMA_ALIGN 64U

struct hcd_state {
    uint64_t connect_since[KSHIM_USB_MAX_ROOT_PORTS];
    uint32_t debounce_mask;   /* ports waiting for a stable connection */
    uint32_t failed_mask;     /* enumeration failed; retried on reconnect */
};

struct urb_record {
    struct kshim_usb_urb *urb;
    kshim_usb_complete_fn complete;
    void *original;
    void *dma;
    size_t dma_size;
    bool in_use;
};

static struct kshim_usb_hcd *Hcds;
static unsigned HcdCount;
static struct hcd_state HcdStates[USB_MAX_HCDS];
static struct kshim_usb_class_driver *Drivers;
static struct kshim_usb_device Devices[KSHIM_USB_MAX_DEVICES];
static uint32_t DeviceSeq[KSHIM_USB_MAX_DEVICES]; /* 0 = free; allocation order */
static uint32_t NextSeq;
static struct urb_record Records[USB_MAX_URBS];
static bool Quiesced, InEvents, Polling;
static uint64_t LastActivity;

static const char *speed_name(enum kshim_usb_speed speed)
{
    switch (speed) {
    case KSHIM_USB_SPEED_LOW: return "low";
    case KSHIM_USB_SPEED_FULL: return "full";
    case KSHIM_USB_SPEED_HIGH: return "high";
    case KSHIM_USB_SPEED_SUPER: return "super";
    case KSHIM_USB_SPEED_SUPER_PLUS: return "super+";
    default: return "unknown";
    }
}

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static void activity(void) { LastActivity = get_current_time(); }

static void sleep_ms(uint32_t ms) { (void)delay((uint64_t)ms * 1000U); }

/* ---- Descriptor helpers ------------------------------------------------- */

const uint8_t *kshim_usb_desc_next(const uint8_t *config, size_t length, size_t *offset)
{
    if (!config || !offset || *offset >= length) return NULL;
    const uint8_t *p = config + *offset;
    if (length - *offset < 2U || p[0] < 2U || p[0] > length - *offset) return NULL;
    *offset += p[0];
    return p;
}

const uint8_t *kshim_usb_desc_find_in_interface(const uint8_t *config, size_t length,
                                                const uint8_t *from, uint8_t type)
{
    if (!config || !from || from < config || from >= config + length) return NULL;
    size_t offset = (size_t)(from - config);
    const uint8_t *p = kshim_usb_desc_next(config, length, &offset); /* skip `from` */
    if (!p) return NULL;
    while ((p = kshim_usb_desc_next(config, length, &offset))) {
        if (p[1] == KSHIM_USB_DT_INTERFACE) return NULL;
        if (p[1] == type) return p;
    }
    return NULL;
}

/* ---- Device pool -------------------------------------------------------- */

static bool device_live(const struct kshim_usb_device *dev)
{
    if (!dev || dev < Devices || dev >= Devices + KSHIM_USB_MAX_DEVICES) return false;
    return DeviceSeq[dev - Devices] != 0;
}

static struct kshim_usb_device *device_new(void)
{
    for (unsigned i = 0; i < KSHIM_USB_MAX_DEVICES; ++i) {
        if (DeviceSeq[i]) continue;
        memset(&Devices[i], 0, sizeof(Devices[i]));
        DeviceSeq[i] = ++NextSeq;
        return &Devices[i];
    }
    return NULL;
}

static void device_release(struct kshim_usb_device *dev)
{
    free(dev->config);
    unsigned i = (unsigned)(dev - Devices);
    memset(dev, 0, sizeof(*dev));
    DeviceSeq[i] = 0;
}

/* Newest live device whose parent is `parent` (NULL: root devices). */
static struct kshim_usb_device *newest_child(const struct kshim_usb_device *parent,
                                             const struct kshim_usb_hcd *hcd)
{
    struct kshim_usb_device *best = NULL;
    uint32_t seq = 0;
    for (unsigned i = 0; i < KSHIM_USB_MAX_DEVICES; ++i) {
        if (!DeviceSeq[i] || Devices[i].parent != parent) continue;
        if (hcd && Devices[i].hcd != hcd) continue;
        if (DeviceSeq[i] > seq) { seq = DeviceSeq[i]; best = &Devices[i]; }
    }
    return best;
}

/* ---- URBs --------------------------------------------------------------- */

static bool urb_is_in(const struct kshim_usb_urb *urb)
{
    if (urb->endpoint->type == KSHIM_USB_XFER_CONTROL)
        return (urb->setup.bmRequestType & KSHIM_USB_DIR_IN) != 0;
    return (urb->endpoint->address & KSHIM_USB_ENDPOINT_DIR_IN) != 0;
}

static struct urb_record *record_get(void)
{
    for (unsigned i = 0; i < USB_MAX_URBS; ++i) {
        if (Records[i].in_use) continue;
        memset(&Records[i], 0, sizeof(Records[i]));
        Records[i].in_use = true;
        return &Records[i];
    }
    return NULL;
}

#if USB_DMA_BOUNCE
/* Whole cache lines below 4 GiB need no bounce: the HCD cleans and
 * invalidates the buffer itself and no neighbouring data shares a line. */
static bool dma_suitable(const void *buffer, uint32_t length)
{
    uintptr_t at = (uintptr_t)buffer;
    return !(at & (USB_DMA_ALIGN - 1U)) && !(length & (USB_DMA_ALIGN - 1U)) &&
           (uint64_t)at + length <= UINT64_C(0x100000000);
}
#endif

static void urb_giveback(struct kshim_usb_urb *urb)
{
    struct urb_record *rec = urb->bounce;
    if (!rec || rec->urb != urb) return; /* not ours or already given back */
    bool in = urb_is_in(urb);
#if USB_DMA_BOUNCE
    if (rec->dma) {
        uint32_t n = urb->actual < urb->length ? urb->actual : urb->length;
        if (in && n) memcpy(rec->original, rec->dma, n);
        kshim_dma_free(rec->dma, rec->dma_size);
    }
#endif
    urb->buffer = rec->original;
    urb->complete = rec->complete;
    urb->bounce = NULL;
    rec->in_use = false;
    if (urb->status == KSHIM_IO_ERR_PENDING) urb->status = ERROR_DEVICE_ERROR;
    if (!urb->status && in && (urb->flags & KSHIM_USB_URB_SHORT_NOT_OK) && urb->actual < urb->length)
        urb->status = KSHIM_IO_ERR_SHORT;
    if (urb->status == KSHIM_IO_ERR_STALL) urb->endpoint->halted = true;
    kshim_completion_complete(&urb->done, urb->status);
    if (urb->complete) urb->complete(urb);
}

int kshim_usb_submit_urb(struct kshim_usb_urb *urb)
{
    if (!urb || !urb->device || !urb->endpoint) return ERROR_INVALID_PARAMETER;
    struct kshim_usb_device *dev = urb->device;
    struct kshim_usb_hcd *hcd = dev->hcd;
    if (Quiesced || !hcd || hcd->quiesced) return KSHIM_IO_ERR_QUIESCED;
    if (!device_live(dev) || dev->gone) return ERROR_NOT_READY;
    if (urb->endpoint->type == KSHIM_USB_XFER_ISOC) return ERROR_NOT_SUPPORTED;
    if (urb->length && !urb->buffer) return ERROR_INVALID_PARAMETER;
    if (urb->bounce) return ERROR_BUSY;
    struct urb_record *rec = record_get();
    if (!rec) return KSHIM_IO_ERR_NO_MEMORY;
    rec->urb = urb;
    rec->complete = urb->complete;
    rec->original = urb->buffer;
#if USB_DMA_BOUNCE
    if (urb->length && !dma_suitable(urb->buffer, urb->length)) {
        uint64_t bus;
        size_t size = (urb->length + USB_DMA_ALIGN - 1U) & ~(size_t)(USB_DMA_ALIGN - 1U);
        rec->dma = kshim_dma_alloc(size, USB_DMA_ALIGN, 0, UINT32_MAX, &bus);
        if (!rec->dma) { rec->in_use = false; return KSHIM_IO_ERR_NO_MEMORY; }
        rec->dma_size = size;
        if (!urb_is_in(urb)) memcpy(rec->dma, urb->buffer, urb->length);
        urb->buffer = rec->dma;
    }
#endif
    urb->actual = 0;
    urb->status = KSHIM_IO_ERR_PENDING;
    urb->hcd_priv = NULL;
    kshim_completion_init(&urb->done);
    urb->bounce = rec;
    urb->complete = urb_giveback;
    int status = hcd->ops->submit(hcd, urb);
    if (status) {
        urb->buffer = rec->original;
        urb->complete = rec->complete;
        urb->bounce = NULL;
#if USB_DMA_BOUNCE
        if (rec->dma) kshim_dma_free(rec->dma, rec->dma_size);
#endif
        rec->in_use = false;
        urb->status = status;
    }
    return status;
}

void kshim_usb_poll_events(void)
{
    if (InEvents) return;
    InEvents = true;
    for (struct kshim_usb_hcd *hcd = Hcds; hcd; hcd = hcd->next) {
        if (hcd->quiesced || !hcd->ops->poll) continue;
        __atomic_store_n(&hcd->irq_pending, false, __ATOMIC_RELAXED);
        (void)hcd->ops->poll(hcd);
    }
    InEvents = false;
}

static void pump(void *context)
{
    (void)context;
    kshim_usb_poll_events();
}

int kshim_usb_cancel_urb(struct kshim_usb_urb *urb)
{
    if (!urb) return ERROR_INVALID_PARAMETER;
    if (!urb->bounce) return 0; /* not in flight */
    struct kshim_usb_hcd *hcd = urb->device ? urb->device->hcd : NULL;
    int status = ERROR_NOT_SUPPORTED;
    if (hcd && hcd->ops->cancel && !hcd->quiesced) status = hcd->ops->cancel(hcd, urb);
    /* The HCD completes the URB with KSHIM_IO_ERR_CANCELLED, from the cancel
     * itself or from its event ring. */
    if (urb->bounce && !InEvents)
        (void)kshim_completion_wait(&urb->done, USB_CANCEL_WAIT_US, pump, NULL);
    if (urb->bounce) {
        /* The HCD lost it (or is gone): give it back here so no caller waits
         * on a buffer the controller no longer owns. */
        urb->status = KSHIM_IO_ERR_CANCELLED;
        urb_giveback(urb);
    }
    return status == ERROR_NOT_SUPPORTED ? 0 : status;
}

/* Submit and wait. Returns the URB status; *leaked is set when the HCD never
 * gave a timed-out URB back (it must then not be freed). */
static int urb_run(struct kshim_usb_urb *urb, uint32_t timeout_ms, bool *leaked)
{
    *leaked = false;
    int status = kshim_usb_submit_urb(urb);
    if (status) return status;
    status = kshim_completion_wait(&urb->done, timeout_ms * 1000U, pump, NULL);
    if (kshim_completion_done(&urb->done)) return status;
    (void)kshim_usb_cancel_urb(urb);
    if (urb->bounce) *leaked = true;
    return ERROR_TIMEOUT;
}

static int sync_transfer(struct kshim_usb_device *dev, struct kshim_usb_endpoint *ep,
                         const struct kshim_usb_setup *setup, void *data, uint32_t length,
                         uint32_t timeout_ms, uint32_t *actual)
{
    if (actual) *actual = 0;
    if (!dev || !ep) return ERROR_INVALID_PARAMETER;
    if (InEvents) return ERROR_BUSY; /* not from URB callbacks */
    struct kshim_usb_hcd *hcd = dev->hcd;
    if (ep->type == KSHIM_USB_XFER_CONTROL && ep->halted && device_live(dev) && !dev->gone) {
        if (hcd && hcd->ops->endpoint_reset) (void)hcd->ops->endpoint_reset(hcd, dev, ep);
        ep->halted = false;
    }
    struct kshim_usb_urb *urb = calloc(1, sizeof(*urb));
    if (!urb) return KSHIM_IO_ERR_NO_MEMORY;
    urb->device = dev;
    urb->endpoint = ep;
    if (setup) urb->setup = *setup;
    urb->buffer = data;
    urb->length = length;
    bool leaked;
    int status = urb_run(urb, timeout_ms ? timeout_ms : USB_DEFAULT_TIMEOUT_MS, &leaked);
    if (actual) *actual = urb->actual;
    if (leaked) {
        printf("USB: %s request abandoned by the host controller\n", hcd ? hcd->name : "?");
        return ERROR_TIMEOUT;
    }
    free(urb);
    /* A STALLed control endpoint accepts the next SETUP on the wire, but an
     * xHCI endpoint context is halted until reset (USB 2.0 8.5.3.4). */
    if (status == KSHIM_IO_ERR_STALL && ep->type == KSHIM_USB_XFER_CONTROL && device_live(dev) &&
        !dev->gone) {
        if (hcd && hcd->ops->endpoint_reset) (void)hcd->ops->endpoint_reset(hcd, dev, ep);
        ep->halted = false;
    }
    return status;
}

int kshim_usb_control(struct kshim_usb_device *device, uint8_t request_type, uint8_t request,
                      uint16_t value, uint16_t index, void *data, uint16_t length,
                      uint32_t timeout_ms, uint32_t *actual)
{
    struct kshim_usb_setup setup = {request_type, request, value, index, length};
    if (!device) return ERROR_INVALID_PARAMETER;
    return sync_transfer(device, &device->ep0, &setup, data, length, timeout_ms, actual);
}

int kshim_usb_bulk(struct kshim_usb_device *device, struct kshim_usb_endpoint *endpoint,
                   void *data, uint32_t length, uint32_t timeout_ms, uint32_t *actual)
{
    if (!endpoint || endpoint->type != KSHIM_USB_XFER_BULK) return ERROR_INVALID_PARAMETER;
    return sync_transfer(device, endpoint, NULL, data, length, timeout_ms, actual);
}

int kshim_usb_interrupt(struct kshim_usb_device *device, struct kshim_usb_endpoint *endpoint,
                        void *data, uint32_t length, uint32_t timeout_ms, uint32_t *actual)
{
    if (!endpoint || endpoint->type != KSHIM_USB_XFER_INTERRUPT) return ERROR_INVALID_PARAMETER;
    return sync_transfer(device, endpoint, NULL, data, length, timeout_ms, actual);
}

int kshim_usb_get_descriptor(struct kshim_usb_device *device, uint8_t type, uint8_t index,
                             uint16_t language, void *data, uint16_t length, uint32_t *actual)
{
    return kshim_usb_control(device, KSHIM_USB_DIR_IN | KSHIM_USB_TYPE_STANDARD | KSHIM_USB_RECIP_DEVICE,
                             KSHIM_USB_REQ_GET_DESCRIPTOR, (uint16_t)((type << 8) | index), language,
                             data, length, USB_CONTROL_TIMEOUT_MS, actual);
}

int kshim_usb_clear_halt(struct kshim_usb_device *device, struct kshim_usb_endpoint *endpoint)
{
    if (!device || !endpoint) return ERROR_INVALID_PARAMETER;
    int status = kshim_usb_control(device, KSHIM_USB_DIR_OUT | KSHIM_USB_TYPE_STANDARD | KSHIM_USB_RECIP_ENDPOINT,
                                   KSHIM_USB_REQ_CLEAR_FEATURE, KSHIM_USB_FEATURE_ENDPOINT_HALT,
                                   endpoint->address, NULL, 0, USB_CONTROL_TIMEOUT_MS, NULL);
    /* The device's data toggle restarts with CLEAR_FEATURE; the HCD's
     * endpoint state must restart with it (USB 2.0 9.4.5). */
    struct kshim_usb_hcd *hcd = device->hcd;
    if (hcd && hcd->ops->endpoint_reset && device_live(device) && !device->gone) {
        int reset = hcd->ops->endpoint_reset(hcd, device, endpoint);
        if (!status) status = reset;
    }
    if (!status) endpoint->halted = false;
    return status;
}

struct kshim_usb_endpoint *kshim_usb_find_endpoint(struct kshim_usb_interface *interface,
                                                   enum kshim_usb_xfer_type type, bool in)
{
    if (!interface) return NULL;
    for (unsigned i = 0; i < interface->endpoint_count && i < KSHIM_USB_MAX_IF_ENDPOINTS; ++i) {
        struct kshim_usb_endpoint *ep = &interface->endpoints[i];
        if (ep->type == type && ((ep->address & KSHIM_USB_ENDPOINT_DIR_IN) != 0) == in) return ep;
    }
    return NULL;
}

/* ---- Class drivers ------------------------------------------------------ */

static bool driver_matches(const struct kshim_usb_class_driver *driver,
                           const struct kshim_usb_device *dev, uint8_t cls, uint8_t sub, uint8_t proto)
{
    if (driver->class_code >= 0 && driver->class_code != cls) return false;
    if (driver->subclass >= 0 && driver->subclass != sub) return false;
    if (driver->protocol >= 0 && driver->protocol != proto) return false;
    if (driver->vendor && driver->vendor != dev->descriptor.idVendor) return false;
    if (driver->product && driver->product != dev->descriptor.idProduct) return false;
    return true;
}

static void bind_interface(struct kshim_usb_interface *intf)
{
    struct kshim_usb_device *dev = intf->device;
    for (struct kshim_usb_class_driver *d = Drivers; d && !intf->driver; d = d->next) {
        if (!driver_matches(d, dev, intf->class_code, intf->subclass, intf->protocol)) continue;
        intf->driver = d;
        int status = d->probe ? d->probe(intf) : ERROR_NOT_SUPPORTED;
        if (!device_live(dev) || dev->gone) return;
        if (status) {
            intf->driver = NULL;
            intf->driver_data = NULL;
            printf("USB: %04x:%04x interface %u: %s not bound (%s)\n", dev->descriptor.idVendor,
                   dev->descriptor.idProduct, intf->number, d->name, kshim_io_strerror(status));
            continue;
        }
        printf("USB: %04x:%04x interface %u bound to %s\n", dev->descriptor.idVendor,
               dev->descriptor.idProduct, intf->number, d->name);
    }
}

static void bind_device(struct kshim_usb_device *dev)
{
    for (unsigned i = 0; i < dev->interface_count; ++i) {
        if (!device_live(dev) || dev->gone) return;
        if (!dev->interfaces[i].driver) bind_interface(&dev->interfaces[i]);
    }
}

int kshim_usb_class_register(struct kshim_usb_class_driver *driver)
{
    if (!driver) return ERROR_INVALID_PARAMETER;
    struct kshim_usb_class_driver **tail = &Drivers;
    for (; *tail; tail = &(*tail)->next)
        if (*tail == driver) return KSHIM_IO_ERR_EXISTS;
    driver->next = NULL;
    *tail = driver;
    /* Offer the new driver every unbound interface. */
    if (!InEvents && !Quiesced)
        for (unsigned i = 0; i < KSHIM_USB_MAX_DEVICES; ++i)
            if (DeviceSeq[i] && Devices[i].configured && !Devices[i].gone) bind_device(&Devices[i]);
    return 0;
}

/* ---- Enumeration -------------------------------------------------------- */

static bool config_has_driver(const struct kshim_usb_device *dev, const uint8_t *config, size_t length)
{
    size_t offset = 0;
    const uint8_t *p;
    while ((p = kshim_usb_desc_next(config, length, &offset))) {
        if (p[1] != KSHIM_USB_DT_INTERFACE || p[0] < 9 || p[3] != 0) continue;
        for (const struct kshim_usb_class_driver *d = Drivers; d; d = d->next)
            if (driver_matches(d, dev, p[5], p[6], p[7])) return true;
    }
    return false;
}

static int parse_config(struct kshim_usb_device *dev)
{
    const uint8_t *config = dev->config;
    size_t length = dev->config_length, offset = 0;
    struct kshim_usb_interface *intf = NULL;
    struct kshim_usb_endpoint *last = NULL;
    const uint8_t *p = kshim_usb_desc_next(config, length, &offset);
    if (!p || p[1] != KSHIM_USB_DT_CONFIG || p[0] < 9) return KSHIM_IO_ERR_PROTOCOL;
    dev->config_value = p[5];
    while ((p = kshim_usb_desc_next(config, length, &offset))) {
        switch (p[1]) {
        case KSHIM_USB_DT_INTERFACE:
            last = NULL;
            intf = NULL;
            if (p[0] < 9 || p[3] != 0) break; /* alternate settings stay unused */
            if (dev->interface_count == KSHIM_USB_MAX_INTERFACES) break;
            intf = &dev->interfaces[dev->interface_count++];
            intf->device = dev;
            intf->number = p[2];
            intf->alternate = p[3];
            intf->class_code = p[5];
            intf->subclass = p[6];
            intf->protocol = p[7];
            intf->descriptor = p;
            break;
        case KSHIM_USB_DT_ENDPOINT: {
            last = NULL;
            if (!intf || p[0] < 7 || intf->endpoint_count == KSHIM_USB_MAX_IF_ENDPOINTS) break;
            uint16_t mps = le16(&p[4]);
            last = &intf->endpoints[intf->endpoint_count++];
            *last = (struct kshim_usb_endpoint){
                .address = p[2],
                .type = (uint8_t)(p[3] & KSHIM_USB_ENDPOINT_XFER_MASK),
                .max_packet = (uint16_t)(mps & 0x7ffU),
                .mult = (uint8_t)((mps >> 11) & 3U),
                .interval = p[6],
            };
            break;
        }
        case KSHIM_USB_DT_SS_EP_COMP:
            if (!last || p[0] < 6) break;
            last->max_burst = p[2];
            if (last->type == KSHIM_USB_XFER_ISOC) last->mult = (uint8_t)(p[3] & 3U);
            break;
        default:
            break;
        }
    }
    return 0;
}

static int read_config(struct kshim_usb_device *dev, uint8_t index, uint8_t **out, uint16_t *out_length)
{
    uint8_t head[9];
    uint32_t actual;
    *out = NULL;
    int status = kshim_usb_get_descriptor(dev, KSHIM_USB_DT_CONFIG, index, 0, head, sizeof(head), &actual);
    if (status) return status;
    if (actual < 9 || head[1] != KSHIM_USB_DT_CONFIG) return KSHIM_IO_ERR_PROTOCOL;
    uint16_t total = le16(&head[2]);
    if (total < 9) return KSHIM_IO_ERR_PROTOCOL;
    if (total > USB_MAX_CONFIG_BYTES) total = USB_MAX_CONFIG_BYTES;
    uint8_t *buffer = malloc(total);
    if (!buffer) return KSHIM_IO_ERR_NO_MEMORY;
    status = kshim_usb_get_descriptor(dev, KSHIM_USB_DT_CONFIG, index, 0, buffer, total, &actual);
    if (!status && actual < 9) status = KSHIM_IO_ERR_PROTOCOL;
    if (status) { free(buffer); return status; }
    *out = buffer;
    *out_length = (uint16_t)actual;
    return 0;
}

static uint16_t default_ep0_size(enum kshim_usb_speed speed)
{
    switch (speed) {
    case KSHIM_USB_SPEED_LOW: return 8;
    case KSHIM_USB_SPEED_SUPER: case KSHIM_USB_SPEED_SUPER_PLUS: return 512;
    default: return 64;
    }
}

/* bMaxPacketSize0: LS 8, FS 8/16/32/64, HS 64, SS 2^9 (USB 2.0 5.5.3, 9.6.1;
 * USB 3.2 9.6.1). */
static int ep0_size(enum kshim_usb_speed speed, uint8_t value, uint16_t *size)
{
    if (speed >= KSHIM_USB_SPEED_SUPER) {
        if (value != 9) return KSHIM_IO_ERR_PROTOCOL;
        *size = 512;
        return 0;
    }
    if (value != 8 && value != 16 && value != 32 && value != 64) return KSHIM_IO_ERR_PROTOCOL;
    if (speed == KSHIM_USB_SPEED_LOW && value != 8) return KSHIM_IO_ERR_PROTOCOL;
    *size = value;
    return 0;
}

static int enumerate_steps(struct kshim_usb_device *dev)
{
    struct kshim_usb_hcd *hcd = dev->hcd;
    uint8_t head[8];
    uint32_t actual = 0;
    int status = hcd->ops->device_address(hcd, dev);
    if (status) return status;
    sleep_ms(USB_SET_ADDRESS_RECOVERY_MS);

    /* The first 8 bytes carry bMaxPacketSize0 whatever the current ep0 size. */
    status = kshim_usb_get_descriptor(dev, KSHIM_USB_DT_DEVICE, 0, 0, head, sizeof(head), &actual);
    if (status) return status;
    if (actual < 8 || head[1] != KSHIM_USB_DT_DEVICE) return KSHIM_IO_ERR_PROTOCOL;
    uint16_t mps;
    status = ep0_size(dev->speed, head[7], &mps);
    if (status) return status;
    if (mps != dev->ep0.max_packet) {
        dev->ep0.max_packet = mps;
        if (hcd->ops->device_update_ep0) {
            status = hcd->ops->device_update_ep0(hcd, dev);
            if (status) return status;
        }
    }

    uint8_t raw[18];
    status = kshim_usb_get_descriptor(dev, KSHIM_USB_DT_DEVICE, 0, 0, raw, sizeof(raw), &actual);
    if (status) return status;
    if (actual < sizeof(raw) || raw[0] < sizeof(raw) || raw[1] != KSHIM_USB_DT_DEVICE)
        return KSHIM_IO_ERR_PROTOCOL;
    memcpy(&dev->descriptor, raw, sizeof(raw));
    uint8_t configs = raw[17];
    if (!configs) return KSHIM_IO_ERR_PROTOCOL;
    if (configs > USB_MAX_CONFIGS) configs = USB_MAX_CONFIGS;

    /* The first configuration with an interface some class driver takes,
     * else the first one. */
    uint8_t *chosen = NULL;
    uint16_t chosen_length = 0;
    for (uint8_t i = 0; i < configs; ++i) {
        uint8_t *config;
        uint16_t length;
        status = read_config(dev, i, &config, &length);
        if (status) {
            if (!chosen && !i) return status;
            continue;
        }
        if (config_has_driver(dev, config, length)) {
            free(chosen);
            chosen = config;
            chosen_length = length;
            break;
        }
        if (!chosen) { chosen = config; chosen_length = length; }
        else free(config);
    }
    dev->config = chosen;
    dev->config_length = chosen_length;
    status = parse_config(dev);
    if (status) return status;
    if (hcd->ops->device_configure) {
        status = hcd->ops->device_configure(hcd, dev);
        if (status) return status;
    }
    status = kshim_usb_control(dev, KSHIM_USB_DIR_OUT | KSHIM_USB_TYPE_STANDARD | KSHIM_USB_RECIP_DEVICE,
                               KSHIM_USB_REQ_SET_CONFIGURATION, dev->config_value, 0, NULL, 0,
                               USB_CONTROL_TIMEOUT_MS, NULL);
    if (status) return status;
    dev->configured = true;
    return 0;
}

int kshim_usb_enumerate(struct kshim_usb_hcd *hcd, struct kshim_usb_device *parent,
                        uint8_t port, enum kshim_usb_speed speed,
                        struct kshim_usb_device **device)
{
    if (device) *device = NULL;
    if (!hcd || !port || speed == KSHIM_USB_SPEED_UNKNOWN) return ERROR_INVALID_PARAMETER;
    if (Quiesced || hcd->quiesced) return KSHIM_IO_ERR_QUIESCED;
    if (InEvents) return ERROR_BUSY;
    if (parent && (!device_live(parent) || parent->gone || parent->hcd != hcd)) return ERROR_INVALID_PARAMETER;
    if (!parent && port > hcd->port_count) return ERROR_INVALID_PARAMETER;
    uint8_t tier = parent ? (uint8_t)(parent->tier + 1U) : 1U;
    if (tier > KSHIM_USB_MAX_TIER) return ERROR_NOT_SUPPORTED;
    struct kshim_usb_device *dev = device_new();
    if (!dev) return KSHIM_IO_ERR_NO_MEMORY;
    activity();
    dev->hcd = hcd;
    dev->parent = parent;
    dev->port = port;
    dev->root_port = parent ? parent->root_port : port;
    dev->tier = tier;
    dev->speed = speed;
    /* xHCI 1.2 8.9: one nibble per hub tier, the hub port (15 at most). */
    dev->route = parent ? parent->route | ((uint32_t)(port > 15U ? 15U : port) << (4U * (parent->tier - 1U))) : 0;
    /* A low/full-speed device below a high-speed hub uses that hub's
     * transaction translator; tt_port is the TT hub's port leading to it.
     * tt_hub_slot carries the TT hub's USB address: the HCD maps it to its
     * slot (xHCI) from the parent chain when they differ. */
    if (speed == KSHIM_USB_SPEED_LOW || speed == KSHIM_USB_SPEED_FULL) {
        struct kshim_usb_device *child = dev;
        for (struct kshim_usb_device *hub = parent; hub; child = hub, hub = hub->parent) {
            if (hub->speed != KSHIM_USB_SPEED_HIGH) continue;
            dev->tt_hub_slot = hub->address;
            dev->tt_port = child->port;
            break;
        }
    }
    dev->ep0 = (struct kshim_usb_endpoint){.address = 0, .type = KSHIM_USB_XFER_CONTROL,
                                           .max_packet = default_ep0_size(speed)};
    int status = hcd->ops->device_alloc(hcd, dev);
    if (status) {
        device_release(dev);
        return status;
    }
    if (!parent) hcd->root[port - 1U] = dev;
    status = enumerate_steps(dev);
    if (status) {
        if (parent)
            printf("USB: %s port %u tier %u hub port %u: enumeration failed: %s\n", hcd->name,
                   dev->root_port, tier, port, kshim_io_strerror(status));
        else
            printf("USB: %s port %u: enumeration failed: %s\n", hcd->name, port, kshim_io_strerror(status));
        if (hcd->ops->device_free) hcd->ops->device_free(hcd, dev);
        if (!parent && hcd->root[port - 1U] == dev) hcd->root[port - 1U] = NULL;
        device_release(dev);
        activity();
        return status;
    }
    printf("USB: %s port %u tier %u route %05x: %04x:%04x class %02x, %s speed, %u interface(s)\n",
           hcd->name, dev->root_port, dev->tier, (unsigned)dev->route, dev->descriptor.idVendor,
           dev->descriptor.idProduct, dev->descriptor.bDeviceClass, speed_name(speed),
           dev->interface_count);
    bind_device(dev);
    activity();
    if (!device_live(dev) || dev->gone) return ERROR_NOT_READY;
    if (device) *device = dev;
    return 0;
}

void kshim_usb_disconnect(struct kshim_usb_device *device)
{
    if (!device_live(device)) return;
    struct kshim_usb_hcd *hcd = device->hcd;
    /* Children first, newest first (hubs below hubs recurse). */
    for (struct kshim_usb_device *child; (child = newest_child(device, NULL));)
        kshim_usb_disconnect(child);
    device->gone = true;
    activity();
    for (unsigned i = 0; i < device->interface_count; ++i) {
        struct kshim_usb_interface *intf = &device->interfaces[i];
        if (!intf->driver) continue;
        if (intf->driver->disconnect) intf->driver->disconnect(intf);
        intf->driver = NULL;
        intf->driver_data = NULL;
    }
    /* Whatever the class drivers left in flight. */
    for (unsigned i = 0; i < USB_MAX_URBS; ++i)
        if (Records[i].in_use && Records[i].urb->device == device) (void)kshim_usb_cancel_urb(Records[i].urb);
    if (hcd && hcd->ops->device_free && !hcd->quiesced) hcd->ops->device_free(hcd, device);
    if (hcd && !device->parent && device->port && device->port <= KSHIM_USB_MAX_ROOT_PORTS &&
        hcd->root[device->port - 1U] == device)
        hcd->root[device->port - 1U] = NULL;
    printf("USB: %04x:%04x on %s port %u tier %u removed\n", device->descriptor.idVendor,
           device->descriptor.idProduct, hcd ? hcd->name : "?", device->root_port, device->tier);
    device_release(device);
}

/* ---- Host controllers and root ports ------------------------------------ */

int kshim_usb_hcd_register(struct kshim_usb_hcd *hcd)
{
    if (!hcd || !hcd->ops || !hcd->ops->port_status || !hcd->ops->port_reset ||
        !hcd->ops->device_alloc || !hcd->ops->device_address || !hcd->ops->submit)
        return ERROR_INVALID_PARAMETER;
    if (Quiesced) return KSHIM_IO_ERR_QUIESCED;
    struct kshim_usb_hcd **tail = &Hcds;
    for (; *tail; tail = &(*tail)->next)
        if (*tail == hcd) return KSHIM_IO_ERR_EXISTS;
    if (HcdCount == USB_MAX_HCDS) return KSHIM_IO_ERR_NO_MEMORY;
    if (hcd->port_count > KSHIM_USB_MAX_ROOT_PORTS) hcd->port_count = KSHIM_USB_MAX_ROOT_PORTS;
    hcd->index = HcdCount++;
    memset(hcd->root, 0, sizeof(hcd->root));
    memset(&HcdStates[hcd->index], 0, sizeof(HcdStates[hcd->index]));
    hcd->quiesced = false;
    hcd->next = NULL;
    /* Scan every root port at the next kshim_usb_poll(). */
    __atomic_store_n(&hcd->port_change_mask,
                     hcd->port_count == 32U ? UINT32_MAX : (1U << hcd->port_count) - 1U,
                     __ATOMIC_RELAXED);
    *tail = hcd;
    activity();
    printf("USB: host %s, %u root ports, %s\n", hcd->name ? hcd->name : "?", hcd->port_count,
           hcd->irq_driven ? "interrupt-driven" : "polled");
    return 0;
}

void kshim_usb_hcd_port_changed(struct kshim_usb_hcd *hcd, uint8_t port)
{
    if (!hcd || !port || port > KSHIM_USB_MAX_ROOT_PORTS) return;
    __atomic_fetch_or(&hcd->port_change_mask, 1U << (port - 1U), __ATOMIC_RELEASE);
}

static void root_port_enumerate(struct kshim_usb_hcd *hcd, uint8_t port, enum kshim_usb_speed speed)
{
    struct hcd_state *st = &HcdStates[hcd->index];
    uint32_t bit = 1U << (port - 1U);
    int status = ERROR_DEVICE_ERROR;
    for (unsigned attempt = 0; attempt < USB_ENUMERATE_ATTEMPTS; ++attempt) {
        enum kshim_usb_speed negotiated = speed;
        status = hcd->ops->port_reset(hcd, port, &negotiated);
        if (status) continue;
        sleep_ms(USB_RESET_RECOVERY_MS);
        status = kshim_usb_enumerate(hcd, NULL, port, negotiated, NULL);
        if (!status || status == KSHIM_IO_ERR_QUIESCED) break;
    }
    if (!status) return;
    st->failed_mask |= bit;
    if (hcd->ops->port_disable) (void)hcd->ops->port_disable(hcd, port);
    printf("USB: %s port %u: device not usable (%s)\n", hcd->name, port, kshim_io_strerror(status));
}

static void root_ports(struct kshim_usb_hcd *hcd)
{
    struct hcd_state *st = &HcdStates[hcd->index];
    uint32_t mask = __atomic_exchange_n(&hcd->port_change_mask, 0U, __ATOMIC_ACQUIRE) | st->debounce_mask;
    st->debounce_mask = 0;
    for (uint8_t port = 1; port <= hcd->port_count && mask; ++port) {
        uint32_t bit = 1U << (port - 1U);
        if (!(mask & bit)) continue;
        mask &= ~bit;
        struct kshim_usb_port_status ps = {0};
        if (hcd->ops->port_status(hcd, port, &ps)) continue;
        if (ps.change && hcd->ops->port_clear_change) (void)hcd->ops->port_clear_change(hcd, port, ps.change);
        bool connected = (ps.status & KSHIM_USB_PORT_CONNECTION) != 0;
        struct kshim_usb_device *dev = hcd->root[port - 1U];
        if (ps.change & KSHIM_USB_PORT_C_CONNECTION) {
            st->failed_mask &= ~bit;
            st->connect_since[port - 1U] = 0;
        }
        if (ps.change & KSHIM_USB_PORT_C_OVER_CURRENT)
            printf("USB: %s port %u over-current\n", hcd->name, port);
        if (dev && (!connected || (ps.change & KSHIM_USB_PORT_C_CONNECTION))) {
            kshim_usb_disconnect(dev);
            dev = NULL;
        }
        if (!connected) { st->connect_since[port - 1U] = 0; continue; }
        if (dev || (st->failed_mask & bit)) continue;
        /* USB 2.0 7.1.7.3: a connection must be stable for 100 ms before
         * the port is reset. */
        uint64_t now = get_current_time();
        if (!st->connect_since[port - 1U]) {
            st->connect_since[port - 1U] = now ? now : 1U;
            st->debounce_mask |= bit;
            activity();
            continue;
        }
        if (now - st->connect_since[port - 1U] < USB_DEBOUNCE_MS) {
            st->debounce_mask |= bit;
            continue;
        }
        st->connect_since[port - 1U] = 0;
        root_port_enumerate(hcd, port, ps.speed);
    }
}

#if CONFIG_KSHIM_INPUT_INJECT
static void usb_service(void *context)
{
    (void)context;
    kshim_usb_poll();
}
static struct kshim_input_source UsbSource = {.name = "usb", .caps = 0, .service = usb_service};
#endif

int kshim_usb_init(const void *fdt)
{
    (void)fdt;
    int status = 0;
#if CONFIG_KSHIM_INPUT_INJECT
    /* The menu's key and touch polls pump hotplug and the polling fallback. */
    int source = kshim_input_source_register(&UsbSource);
    if (source && source != KSHIM_IO_ERR_EXISTS) status = source;
#endif
#if CONFIG_KSHIM_USB_XHCI
    int xhci = kshim_xhci_pci_register();
    if (xhci && !status) status = xhci;
#endif
    return status;
}

void kshim_usb_poll(void)
{
    if (Quiesced || Polling || InEvents || !kshim_io_on_boot_cpu()) return;
    Polling = true;
    kshim_usb_poll_events();
    for (struct kshim_usb_hcd *hcd = Hcds; hcd; hcd = hcd->next)
        if (!hcd->quiesced) root_ports(hcd);
    /* Class drivers' deferred work, oldest device first (hubs before the
     * devices below them). Service hooks may enumerate or remove devices,
     * so liveness is checked against the sequence number per call. */
    unsigned order[KSHIM_USB_MAX_DEVICES], count = 0;
    uint32_t seqs[KSHIM_USB_MAX_DEVICES];
    for (unsigned i = 0; i < KSHIM_USB_MAX_DEVICES; ++i) {
        if (!DeviceSeq[i]) continue;
        unsigned at = count++;
        while (at && seqs[at - 1U] > DeviceSeq[i]) {
            seqs[at] = seqs[at - 1U];
            order[at] = order[at - 1U];
            --at;
        }
        seqs[at] = DeviceSeq[i];
        order[at] = i;
    }
    for (unsigned k = 0; k < count; ++k) {
        struct kshim_usb_device *dev = &Devices[order[k]];
        for (unsigned n = 0; n < dev->interface_count; ++n) {
            if (DeviceSeq[order[k]] != seqs[k] || dev->gone || !dev->configured) break;
            struct kshim_usb_interface *intf = &dev->interfaces[n];
            if (intf->driver && intf->driver->service) intf->driver->service(intf);
        }
    }
    Polling = false;
}

static bool port_work_pending(void)
{
    for (struct kshim_usb_hcd *hcd = Hcds; hcd; hcd = hcd->next) {
        if (hcd->quiesced) continue;
        if (__atomic_load_n(&hcd->port_change_mask, __ATOMIC_ACQUIRE) || HcdStates[hcd->index].debounce_mask)
            return true;
    }
    return false;
}

int kshim_usb_settle(uint32_t timeout_ms)
{
    if (!Hcds || Quiesced) return 0;
    uint64_t start = get_current_time();
    for (;;) {
        kshim_usb_poll();
        uint64_t now = get_current_time();
        if (!port_work_pending() && now - LastActivity >= USB_SETTLE_QUIET_MS) break;
        if (now - start >= timeout_ms) {
            printf("USB: enumeration still busy after %u ms; continuing in the background\n",
                   (unsigned)timeout_ms);
            return ERROR_TIMEOUT;
        }
        if (kshim_irq_ready()) kshim_wait_event(now + 1U);
        else sleep_ms(1);
    }
    unsigned count = 0;
    for (unsigned i = 0; i < KSHIM_USB_MAX_DEVICES; ++i) count += DeviceSeq[i] != 0;
    printf("USB: %u device(s) after %u ms\n", count, (unsigned)(get_current_time() - start));
    return 0;
}

int kshim_usb_quiesce(void)
{
    if (Quiesced) return 0;
    /* Newest topology first: the newest root device, its children first. */
    for (struct kshim_usb_device *dev; (dev = newest_child(NULL, NULL));) kshim_usb_disconnect(dev);
    /* Orphans cannot exist (children go before parents); be thorough. */
    for (unsigned i = 0; i < KSHIM_USB_MAX_DEVICES; ++i)
        if (DeviceSeq[i]) kshim_usb_disconnect(&Devices[i]);
    unsigned left = 0;
    for (unsigned i = 0; i < USB_MAX_URBS; ++i)
        if (Records[i].in_use) {
            ++left;
            (void)kshim_usb_cancel_urb(Records[i].urb);
        }
    if (left) printf("USB: %u transfer(s) cancelled at handoff\n", left);
    Quiesced = true;
    return 0;
}

int kshim_usb_hcd_quiesce_all(void)
{
    int result = 0;
    for (struct kshim_usb_hcd *hcd = Hcds; hcd; hcd = hcd->next) {
        if (hcd->quiesced) continue;
        int status = hcd->ops->quiesce ? hcd->ops->quiesce(hcd) : 0;
        if (status) {
            printf("USB: %s quiesce failed: %s\n", hcd->name, kshim_io_strerror(status));
            if (!result) result = status;
            continue;
        }
        hcd->quiesced = true;
    }
    return result;
}
