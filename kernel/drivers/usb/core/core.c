/* SPDX-License-Identifier: MIT */
/*
 * USB host core (kshim/usb.h, kshim/usb_hcd.h): HCD registry, URB
 * submission with DMA bouncing, the given-back list and its pump, the
 * synchronous helpers, class-driver registry, the poll/settle pumps and the
 * handoff steps. Enumeration and root ports are in enumerate.c.
 *
 * Execution model: HCD interrupt handlers only set hcd->irq_pending and
 * note root-port changes. HCD ops->poll runs in thread context and hands
 * finished URBs back with kshim_usb_hcd_giveback(), which only queues them;
 * usb_core_drain() then copies bounced data back, completes the URB's
 * completion and runs its callback, never nested inside an HCD op or
 * another callback.
 */
#include "usb_core.h"

#include <kshim/dma.h>
#include <kshim/irq.h>
#include <kshim/time.h>

#include <stdio.h>
#include <string.h>

#if CONFIG_KSHIM_INPUT_INJECT
#include <kshim/input_inject.h>
#endif

/* Default for synchronous helpers called with timeout_ms == 0. */
#define USB_DEFAULT_TIMEOUT_MS 5000U
/* GET_DESCRIPTOR retries on transient errors (not on STALL). */
#define USB_DESCRIPTOR_TRIES 3U
#define USB_DESCRIPTOR_TIMEOUT_MS 1000U
/* Bound for the cancel that follows a synchronous timeout. */
#define USB_CANCEL_WAIT_US 200000U
/* Earliest end of the initial enumeration after a controller registered:
 * a device has up to 100 ms after VBUS to signal attach (USB 2.0 7.1.7.3),
 * plus the 100 ms debounce. */
#define USB_SETTLE_MIN_MS 250U

struct usb_core UsbCore;

uint64_t usb_core_now_ms(void) { return get_current_time(); }

static bool urb_is_in(const struct kshim_usb_urb *urb)
{
    if (urb->endpoint && urb->endpoint->type == KSHIM_USB_XFER_CONTROL)
        return (urb->setup.bmRequestType & KSHIM_USB_DIR_IN) != 0;
    return urb->endpoint && (urb->endpoint->address & KSHIM_USB_ENDPOINT_DIR_IN);
}

/* ---- HCD registry ------------------------------------------------------- */

int kshim_usb_hcd_register(struct kshim_usb_hcd *hcd)
{
    if (!hcd || !hcd->ops || !hcd->ops->poll || !hcd->ops->submit || !hcd->ops->port_status)
        return ERROR_INVALID_PARAMETER;
    if (UsbCore.quiesced) return KSHIM_IO_ERR_QUIESCED;
    for (struct kshim_usb_hcd *h = UsbCore.hcds; h; h = h->next)
        if (h == hcd) return KSHIM_IO_ERR_EXISTS;
    if (UsbCore.hcd_count >= USB_CORE_MAX_HCDS) return KSHIM_IO_ERR_NO_MEMORY;
    if (hcd->port_count > KSHIM_USB_MAX_ROOT_PORTS) hcd->port_count = KSHIM_USB_MAX_ROOT_PORTS;
    hcd->index = UsbCore.hcd_count++;
    hcd->quiesced = false;
    hcd->next = NULL;
    memset(hcd->root, 0, sizeof(hcd->root));
    memset(UsbCore.root[hcd->index], 0, sizeof(UsbCore.root[hcd->index]));
    /* Scan every root port at the next kshim_usb_poll(). */
    hcd->port_change_mask = hcd->port_count >= 32 ? UINT32_MAX : ((1U << hcd->port_count) - 1U);
    struct kshim_usb_hcd **tail = &UsbCore.hcds;
    while (*tail) tail = &(*tail)->next;
    *tail = hcd;
    uint64_t until = usb_core_now_ms() + USB_SETTLE_MIN_MS;
    if (until > UsbCore.settle_until_ms) UsbCore.settle_until_ms = until;
    printf("USB: %s registered: %u root ports, %s\n", hcd->name ? hcd->name : "hcd",
           (unsigned)hcd->port_count, hcd->irq_driven ? "interrupt-driven" : "polled");
    return 0;
}

void kshim_usb_hcd_port_changed(struct kshim_usb_hcd *hcd, uint8_t port)
{
    if (!hcd || !port || port > KSHIM_USB_MAX_ROOT_PORTS) return;
    __atomic_fetch_or(&hcd->port_change_mask, 1U << (port - 1U), __ATOMIC_RELAXED);
}

/* ---- URBs --------------------------------------------------------------- */

void kshim_usb_hcd_giveback(struct kshim_usb_urb *urb, int status, uint32_t actual)
{
    if (!urb || urb->status != KSHIM_IO_ERR_PENDING) return;
    if (actual > urb->length) actual = urb->length;
    if (!status && urb->in && (urb->flags & KSHIM_USB_URB_SHORT_NOT_OK) && actual < urb->length)
        status = KSHIM_IO_ERR_SHORT;
    urb->actual = actual;
    urb->status = status;
    urb->done_next = NULL;
    if (UsbCore.done_tail) UsbCore.done_tail->done_next = urb;
    else UsbCore.done_head = urb;
    UsbCore.done_tail = urb;
}

static void urb_finish(struct kshim_usb_urb *urb)
{
    if (urb->bounce) {
        if (urb->in && urb->actual) {
            kshim_dma_sync_for_cpu(urb->bounce, urb->bounce_size);
            memcpy(urb->buffer, urb->bounce, urb->actual);
        }
        kshim_dma_free(urb->bounce, urb->bounce_size);
        urb->bounce = NULL;
        urb->bounce_size = 0;
    } else if (urb->in && urb->actual) {
        kshim_dma_sync_for_cpu(urb->buffer, urb->length);
    }
}

void usb_core_drain(void)
{
    if (UsbCore.draining) return;
    UsbCore.draining = true;
    struct kshim_usb_urb *urb;
    while ((urb = UsbCore.done_head)) {
        UsbCore.done_head = urb->done_next;
        if (!UsbCore.done_head) UsbCore.done_tail = NULL;
        urb->done_next = NULL;
        urb_finish(urb);
        kshim_usb_complete_fn complete = urb->complete;
        /* The completion first: a callback may resubmit (which re-arms it). */
        kshim_completion_complete(&urb->done, urb->status);
        if (complete) complete(urb);
    }
    UsbCore.draining = false;
}

int kshim_usb_submit_urb(struct kshim_usb_urb *urb)
{
    if (!urb || !urb->device || !urb->endpoint || !urb->device->hcd) return ERROR_INVALID_PARAMETER;
    struct kshim_usb_device *dev = urb->device;
    struct kshim_usb_hcd *hcd = dev->hcd;
    if (!kshim_io_on_boot_cpu()) return ERROR_BUSY;
    if (UsbCore.quiesced || hcd->quiesced) return KSHIM_IO_ERR_QUIESCED;
    if (dev->gone) return ERROR_NOT_READY;
    if (urb->length && !urb->buffer) return ERROR_INVALID_PARAMETER;
    if (urb->endpoint->type == KSHIM_USB_XFER_ISOC) return ERROR_NOT_SUPPORTED;
    if (urb->status == KSHIM_IO_ERR_PENDING) return ERROR_BUSY;
    urb->in = urb_is_in(urb);
    urb->actual = 0;
    urb->done_next = NULL;
    urb->hcd_priv = NULL;
    kshim_completion_init(&urb->done);
    if (urb->length && !kshim_dma_owns(urb->buffer, urb->length)) {
        uint64_t bus;
        size_t boundary = urb->length <= 0x10000U ? 0x10000U : 0;
        void *bounce = kshim_dma_alloc(urb->length, 64, boundary, UINT64_MAX, &bus);
        if (!bounce) return KSHIM_IO_ERR_NO_MEMORY;
        if (!urb->in) memcpy(bounce, urb->buffer, urb->length);
        urb->bounce = bounce;
        urb->bounce_size = urb->length;
    } else {
        urb->bounce = NULL;
        urb->bounce_size = 0;
    }
    urb->status = KSHIM_IO_ERR_PENDING;
    int status = hcd->ops->submit(hcd, urb);
    if (status) {
        /* Not accepted: nothing was queued, no callback runs. */
        if (urb->bounce) kshim_dma_free(urb->bounce, urb->bounce_size);
        urb->bounce = NULL;
        urb->bounce_size = 0;
        urb->status = status;
    }
    return status;
}

int kshim_usb_cancel_urb(struct kshim_usb_urb *urb)
{
    if (!urb || !urb->device || !urb->device->hcd) return ERROR_INVALID_PARAMETER;
    if (urb->status != KSHIM_IO_ERR_PENDING) return 0; /* finished (or never queued) */
    struct kshim_usb_hcd *hcd = urb->device->hcd;
    int status = hcd->ops->cancel ? hcd->ops->cancel(hcd, urb) : ERROR_NOT_SUPPORTED;
    /* Run the CANCELLED callback before the caller frees its state, unless
     * this is a callback (the running drain picks it up next). */
    usb_core_drain();
    return status;
}

/* ---- Pumps -------------------------------------------------------------- */

void kshim_usb_poll_events(void)
{
    if (!kshim_io_on_boot_cpu() || UsbCore.in_events || UsbCore.draining) return;
    UsbCore.in_events = true;
    for (struct kshim_usb_hcd *hcd = UsbCore.hcds; hcd; hcd = hcd->next) {
        if (hcd->quiesced) continue;
        hcd->irq_pending = false;
        (void)hcd->ops->poll(hcd);
    }
    usb_core_drain();
    UsbCore.in_events = false;
}

static void pump_events(void *context)
{
    (void)context;
    kshim_usb_poll_events();
}

void kshim_usb_poll(void)
{
    if (!kshim_io_on_boot_cpu() || UsbCore.in_poll || UsbCore.in_events || UsbCore.draining)
        return;
    UsbCore.in_poll = true;
    kshim_usb_poll_events();
    if (!UsbCore.quiesced) {
        for (struct kshim_usb_hcd *hcd = UsbCore.hcds; hcd; hcd = hcd->next)
            if (!hcd->quiesced) usb_core_root_service(hcd);
        /* Class drivers' deferred work. Services may enumerate or disconnect
         * (hub driver), so the table is re-checked at every step. */
        for (unsigned i = 0; i < KSHIM_USB_MAX_DEVICES; ++i) {
            struct kshim_usb_device *dev = &UsbCore.devices[i];
            for (unsigned j = 0; j < KSHIM_USB_MAX_INTERFACES; ++j) {
                if (!UsbCore.device_used[i] || !dev->configured || dev->gone) break;
                if (j >= dev->interface_count) break;
                struct kshim_usb_interface *intf = &dev->interfaces[j];
                if (intf->driver && intf->driver->service) intf->driver->service(intf);
            }
        }
    }
    UsbCore.in_poll = false;
}

void kshim_usb_settle_extend(uint32_t ms)
{
    uint64_t until = usb_core_now_ms() + ms;
    if (until > UsbCore.settle_until_ms) UsbCore.settle_until_ms = until;
}

static bool usb_busy(void)
{
    if (usb_core_now_ms() < UsbCore.settle_until_ms) return true;
    for (struct kshim_usb_hcd *hcd = UsbCore.hcds; hcd; hcd = hcd->next)
        if (!hcd->quiesced && usb_core_root_busy(hcd)) return true;
    return false;
}

static void usb_sleep_ms(void)
{
    if (kshim_irq_ready()) kshim_wait_event(get_current_time() + 1);
    else (void)delay(1000);
}

int kshim_usb_settle(uint32_t timeout_ms)
{
    if (!UsbCore.hcds) return 0;
    uint64_t start = usb_core_now_ms();
    int status = 0;
    for (;;) {
        kshim_usb_poll();
        if (!usb_busy()) break;
        if (usb_core_now_ms() - start >= timeout_ms) {
            status = ERROR_TIMEOUT;
            break;
        }
        usb_sleep_ms();
    }
#if CONFIG_KSHIM_USB_SELFTEST
    kshim_usb_selftest_settle();
#endif
    return status;
}

/* ---- Synchronous helpers ------------------------------------------------ */

static int usb_sync(struct kshim_usb_urb *urb, uint32_t timeout_ms)
{
    /* Callbacks must not wait; nested waits would never see their event. */
    if (UsbCore.draining || UsbCore.in_events) return ERROR_BUSY;
    int status = kshim_usb_submit_urb(urb);
    if (status) return status;
    uint32_t timeout_us = (timeout_ms ? timeout_ms : USB_DEFAULT_TIMEOUT_MS) * 1000U;
    status = kshim_completion_wait(&urb->done, timeout_us, pump_events, NULL);
    if (status == ERROR_TIMEOUT && !kshim_completion_done(&urb->done)) {
        (void)kshim_usb_cancel_urb(urb);
        (void)kshim_completion_wait(&urb->done, USB_CANCEL_WAIT_US, pump_events, NULL);
        return ERROR_TIMEOUT;
    }
    return status;
}

int kshim_usb_control(struct kshim_usb_device *device, uint8_t request_type, uint8_t request,
                      uint16_t value, uint16_t index, void *data, uint16_t length,
                      uint32_t timeout_ms, uint32_t *actual)
{
    if (actual) *actual = 0;
    if (!device) return ERROR_INVALID_PARAMETER;
    struct kshim_usb_urb urb;
    memset(&urb, 0, sizeof(urb));
    urb.device = device;
    urb.endpoint = &device->ep0;
    urb.setup.bmRequestType = request_type;
    urb.setup.bRequest = request;
    urb.setup.wValue = value;
    urb.setup.wIndex = index;
    urb.setup.wLength = length;
    urb.buffer = length ? data : NULL;
    urb.length = length;
    int status = usb_sync(&urb, timeout_ms);
    if (actual) *actual = urb.actual;
    return status;
}

static int usb_sync_xfer(struct kshim_usb_device *device, struct kshim_usb_endpoint *endpoint,
                         enum kshim_usb_xfer_type type, void *data, uint32_t length,
                         uint32_t timeout_ms, uint32_t *actual)
{
    if (actual) *actual = 0;
    if (!device || !endpoint || endpoint->type != type) return ERROR_INVALID_PARAMETER;
    struct kshim_usb_urb urb;
    memset(&urb, 0, sizeof(urb));
    urb.device = device;
    urb.endpoint = endpoint;
    urb.buffer = length ? data : NULL;
    urb.length = length;
    int status = usb_sync(&urb, timeout_ms);
    if (actual) *actual = urb.actual;
    return status;
}

int kshim_usb_bulk(struct kshim_usb_device *device, struct kshim_usb_endpoint *endpoint,
                   void *data, uint32_t length, uint32_t timeout_ms, uint32_t *actual)
{
    return usb_sync_xfer(device, endpoint, KSHIM_USB_XFER_BULK, data, length, timeout_ms, actual);
}

int kshim_usb_interrupt(struct kshim_usb_device *device, struct kshim_usb_endpoint *endpoint,
                        void *data, uint32_t length, uint32_t timeout_ms, uint32_t *actual)
{
    return usb_sync_xfer(device, endpoint, KSHIM_USB_XFER_INTERRUPT, data, length, timeout_ms,
                         actual);
}

int kshim_usb_get_descriptor(struct kshim_usb_device *device, uint8_t type, uint8_t index,
                             uint16_t language, void *data, uint16_t length, uint32_t *actual)
{
    int status = ERROR_DEVICE_ERROR;
    for (unsigned attempt = 0; attempt < USB_DESCRIPTOR_TRIES; ++attempt) {
        status = kshim_usb_control(device, KSHIM_USB_DIR_IN | KSHIM_USB_TYPE_STANDARD |
                                   KSHIM_USB_RECIP_DEVICE, KSHIM_USB_REQ_GET_DESCRIPTOR,
                                   (uint16_t)(type << 8 | index), language, data, length,
                                   USB_DESCRIPTOR_TIMEOUT_MS, actual);
        if (!status || status == KSHIM_IO_ERR_STALL || status == ERROR_BUSY ||
            status == KSHIM_IO_ERR_QUIESCED || status == ERROR_NOT_READY ||
            status == KSHIM_IO_ERR_NO_MEMORY || status == ERROR_INVALID_PARAMETER)
            break;
    }
    return status;
}

int kshim_usb_clear_halt(struct kshim_usb_device *device, struct kshim_usb_endpoint *endpoint)
{
    if (!device || !endpoint) return ERROR_INVALID_PARAMETER;
    int status = kshim_usb_control(device, KSHIM_USB_DIR_OUT | KSHIM_USB_TYPE_STANDARD |
                                   KSHIM_USB_RECIP_ENDPOINT, KSHIM_USB_REQ_CLEAR_FEATURE,
                                   KSHIM_USB_FEATURE_ENDPOINT_HALT, endpoint->address, NULL, 0,
                                   USB_DESCRIPTOR_TIMEOUT_MS, NULL);
    if (status) return status;
    struct kshim_usb_hcd *hcd = device->hcd;
    if (endpoint != &device->ep0 && hcd->ops->endpoint_reset)
        status = hcd->ops->endpoint_reset(hcd, device, endpoint);
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

int kshim_usb_class_register(struct kshim_usb_class_driver *driver)
{
    if (!driver || !driver->probe) return ERROR_INVALID_PARAMETER;
    struct kshim_usb_class_driver **tail = &UsbCore.drivers;
    for (; *tail; tail = &(*tail)->next)
        if (*tail == driver) return KSHIM_IO_ERR_EXISTS;
    driver->next = NULL;
    *tail = driver;
    /* Offer the unbound interfaces of devices already configured. */
    for (unsigned i = 0; i < KSHIM_USB_MAX_DEVICES; ++i)
        if (UsbCore.device_used[i] && UsbCore.devices[i].configured && !UsbCore.devices[i].gone)
            usb_core_bind_device(&UsbCore.devices[i]);
    return 0;
}

/* ---- Lifecycle ---------------------------------------------------------- */

#if CONFIG_KSHIM_INPUT_INJECT
static void usb_input_service(void *context)
{
    (void)context;
    kshim_usb_poll();
}
static struct kshim_input_source UsbInputSource = {
    .name = "usb", .caps = 0, .service = usb_input_service,
};
#endif

int kshim_usb_init(const void *fdt)
{
    (void)fdt;
    if (UsbCore.initialized) return 0;
    UsbCore.initialized = true;
    int status = 0;
#if CONFIG_KSHIM_USB_XHCI
    status = kshim_xhci_pci_register();
#endif
#if CONFIG_KSHIM_USB_SELFTEST
    int st = kshim_usb_selftest_register();
    if (st && !status) status = st;
#endif
#if CONFIG_KSHIM_INPUT_INJECT
    int source = kshim_input_source_register(&UsbInputSource);
    if (source && source != KSHIM_IO_ERR_EXISTS && !status) status = source;
#endif
    return status;
}

int kshim_usb_quiesce(void)
{
    if (UsbCore.quiesced) return 0;
    /* Newest topology first: kshim_usb_disconnect() removes children before
     * their hub, and root ports are walked from the highest. */
    for (struct kshim_usb_hcd *hcd = UsbCore.hcds; hcd; hcd = hcd->next) {
        if (hcd->quiesced) continue;
        for (unsigned port = hcd->port_count; port > 0; --port)
            if (hcd->root[port - 1]) kshim_usb_disconnect(hcd->root[port - 1]);
    }
    /* Devices enumerated without a root link (none today) go last. */
    for (unsigned i = KSHIM_USB_MAX_DEVICES; i > 0; --i)
        if (UsbCore.device_used[i - 1]) kshim_usb_disconnect(&UsbCore.devices[i - 1]);
    UsbCore.quiesced = true;
    usb_core_drain();
    return 0;
}

int kshim_usb_hcd_quiesce_all(void)
{
    int result = 0;
    UsbCore.quiesced = true;
    for (struct kshim_usb_hcd *hcd = UsbCore.hcds; hcd; hcd = hcd->next) {
        int status = hcd->ops->quiesce ? hcd->ops->quiesce(hcd) : 0;
        if (status) {
            printf("USB: %s quiesce failed (%d)\n", hcd->name ? hcd->name : "hcd", status);
            if (!result) result = status;
        }
        hcd->quiesced = true;
    }
    usb_core_drain();
    return result;
}
