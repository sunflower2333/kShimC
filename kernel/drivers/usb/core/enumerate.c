/* SPDX-License-Identifier: MIT */
/*
 * USB enumeration, class-driver binding and root-port handling
 * (kshim/usb.h). Written from USB 2.0 chapter 9 (9.1.2 bus enumeration,
 * 9.2.6 timing, 9.4 standard requests, 9.6 descriptors), 7.1.7.3
 * (connect debounce) and 11.24.2.7 (port status bits), and USB 3.2 9.6.7
 * (SuperSpeed endpoint companion) and 10.9 (route string).
 */
#include "usb_core.h"

#include <kshim/time.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define USB_DEBOUNCE_MS 100U        /* USB 2.0 7.1.7.3 TATTDB */
#define USB_SET_ADDRESS_RECOVERY_US 2000U /* USB 2.0 9.2.6.3 */
#define USB_ENUM_ATTEMPTS 3U
#define USB_CONFIG_MAX 4096U        /* wTotalLength bound */
#define USB_CONFIGS_SCANNED 4U

/* ---- Device table and helpers ------------------------------------------- */

static struct kshim_usb_device *device_alloc(void)
{
    for (unsigned i = 0; i < KSHIM_USB_MAX_DEVICES; ++i) {
        if (UsbCore.device_used[i]) continue;
        UsbCore.device_used[i] = true;
        memset(&UsbCore.devices[i], 0, sizeof(UsbCore.devices[i]));
        return &UsbCore.devices[i];
    }
    return NULL;
}

static void device_release(struct kshim_usb_device *dev)
{
    unsigned i = (unsigned)(dev - UsbCore.devices);
    if (i >= KSHIM_USB_MAX_DEVICES) return;
    free(dev->config);
    memset(dev, 0, sizeof(*dev));
    UsbCore.device_used[i] = false;
}

void usb_core_device_path(const struct kshim_usb_device *device, char *out, size_t size)
{
    /* "<hcd>-<root port>[.<hub port>...]", like the usual bus-port path. */
    unsigned ports[KSHIM_USB_MAX_TIER + 1], n = 0;
    for (const struct kshim_usb_device *d = device; d && n <= KSHIM_USB_MAX_TIER; d = d->parent)
        ports[n++] = d->port;
    int at = snprintf(out, size, "%u-", device && device->hcd ? device->hcd->index : 0U);
    for (unsigned i = n; i > 0 && at > 0 && (size_t)at < size; --i)
        at += snprintf(out + at, size - (size_t)at, i == n ? "%u" : ".%u", ports[i - 1]);
}

void kshim_usb_device_path(const struct kshim_usb_device *device, char *out, size_t size)
{
    if (out && size) usb_core_device_path(device, out, size);
}

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static bool driver_matches(const struct kshim_usb_class_driver *drv,
                           const struct kshim_usb_device *dev,
                           const struct kshim_usb_interface *intf)
{
    if (drv->class_code >= 0 && drv->class_code != intf->class_code) return false;
    if (drv->subclass >= 0 && drv->subclass != intf->subclass) return false;
    if (drv->protocol >= 0 && drv->protocol != intf->protocol) return false;
    if (drv->vendor && drv->vendor != dev->descriptor.idVendor) return false;
    if (drv->product && drv->product != dev->descriptor.idProduct) return false;
    return true;
}

/* ---- Configuration parsing ---------------------------------------------- */

/* Interfaces at alternate setting 0 (each number once) and their endpoints. */
static int parse_config(struct kshim_usb_device *dev)
{
    size_t offset = 0;
    const uint8_t *d;
    struct kshim_usb_interface *intf = NULL;
    struct kshim_usb_endpoint *last_ep = NULL;
    dev->interface_count = 0;
    while ((d = kshim_usb_desc_next(dev->config, dev->config_length, &offset))) {
        if (d[1] == KSHIM_USB_DT_INTERFACE && d[0] >= 9) {
            intf = NULL;
            last_ep = NULL;
            if (d[3] != 0) continue; /* alternate settings are not selected */
            bool seen = false;
            for (unsigned i = 0; i < dev->interface_count; ++i)
                if (dev->interfaces[i].number == d[2]) seen = true;
            if (seen || dev->interface_count >= KSHIM_USB_MAX_INTERFACES) continue;
            intf = &dev->interfaces[dev->interface_count++];
            intf->device = dev;
            intf->number = d[2];
            intf->alternate = d[3];
            intf->class_code = d[5];
            intf->subclass = d[6];
            intf->protocol = d[7];
            intf->descriptor = d;
        } else if (d[1] == KSHIM_USB_DT_ENDPOINT && d[0] >= 7 && intf) {
            last_ep = NULL;
            if (intf->endpoint_count >= KSHIM_USB_MAX_IF_ENDPOINTS) continue;
            uint8_t address = d[2];
            if (!(address & 0x0f)) continue; /* endpoint 0 is not described here */
            struct kshim_usb_endpoint *ep = &intf->endpoints[intf->endpoint_count++];
            uint16_t mps = le16(d + 4);
            ep->address = address;
            ep->type = d[3] & KSHIM_USB_ENDPOINT_XFER_MASK;
            ep->max_packet = mps & 0x7ffU;
            /* High-speed high-bandwidth periodic: additional transactions. */
            ep->mult = dev->speed == KSHIM_USB_SPEED_HIGH &&
                (ep->type == KSHIM_USB_XFER_INTERRUPT || ep->type == KSHIM_USB_XFER_ISOC)
                ? (uint8_t)((mps >> 11) & 3U) : 0;
            ep->interval = d[6];
            last_ep = ep;
        } else if (d[1] == KSHIM_USB_DT_SS_EP_COMP && d[0] >= 6 && last_ep) {
            last_ep->max_burst = d[2];
            if (last_ep->type == KSHIM_USB_XFER_ISOC) last_ep->mult = d[3] & 3U;
            last_ep = NULL;
        }
    }
    return dev->interface_count ? 0 : KSHIM_IO_ERR_PROTOCOL;
}

static bool config_supported(const struct kshim_usb_device *dev, const uint8_t *config,
                             size_t length)
{
    size_t offset = 0;
    const uint8_t *d;
    while ((d = kshim_usb_desc_next(config, length, &offset))) {
        if (d[1] != KSHIM_USB_DT_INTERFACE || d[0] < 9 || d[3] != 0) continue;
        struct kshim_usb_interface probe = {
            .class_code = d[5], .subclass = d[6], .protocol = d[7],
        };
        for (const struct kshim_usb_class_driver *drv = UsbCore.drivers; drv; drv = drv->next)
            if (driver_matches(drv, dev, &probe)) return true;
    }
    return false;
}

static int read_config(struct kshim_usb_device *dev, uint8_t index, uint8_t **out,
                       uint16_t *length)
{
    uint8_t header[9];
    uint32_t actual = 0;
    int status = kshim_usb_get_descriptor(dev, KSHIM_USB_DT_CONFIG, index, 0, header,
                                          sizeof(header), &actual);
    if (status) return status;
    if (actual < sizeof(header) || header[1] != KSHIM_USB_DT_CONFIG) return KSHIM_IO_ERR_PROTOCOL;
    uint16_t total = le16(header + 2);
    if (total < sizeof(header)) return KSHIM_IO_ERR_PROTOCOL;
    if (total > USB_CONFIG_MAX) total = USB_CONFIG_MAX;
    uint8_t *config = malloc(total);
    if (!config) return KSHIM_IO_ERR_NO_MEMORY;
    status = kshim_usb_get_descriptor(dev, KSHIM_USB_DT_CONFIG, index, 0, config, total, &actual);
    if (!status && (actual < sizeof(header) || config[1] != KSHIM_USB_DT_CONFIG))
        status = KSHIM_IO_ERR_PROTOCOL;
    if (status) {
        free(config);
        return status;
    }
    *out = config;
    *length = (uint16_t)actual;
    return 0;
}

/* The first configuration with an interface a registered driver supports,
 * else the first one. */
static int select_config(struct kshim_usb_device *dev)
{
    unsigned count = dev->descriptor.bNumConfigurations;
    if (!count) return KSHIM_IO_ERR_PROTOCOL;
    if (count > USB_CONFIGS_SCANNED) count = USB_CONFIGS_SCANNED;
    int first_status = 0;
    for (unsigned i = 0; i < count; ++i) {
        uint8_t *config = NULL;
        uint16_t length = 0;
        int status = read_config(dev, (uint8_t)i, &config, &length);
        if (status) {
            if (!i) first_status = status;
            if (status == KSHIM_IO_ERR_NO_MEMORY || status == ERROR_TIMEOUT) break;
            continue;
        }
        bool supported = config_supported(dev, config, length);
        if (!dev->config || supported) {
            free(dev->config);
            dev->config = config;
            dev->config_length = length;
            if (supported) break;
        } else {
            free(config);
        }
    }
    if (!dev->config) return first_status ? first_status : KSHIM_IO_ERR_PROTOCOL;
    dev->config_value = dev->config[5];
    return parse_config(dev);
}

/* ---- Binding ------------------------------------------------------------ */

void usb_core_bind_device(struct kshim_usb_device *dev)
{
    char path[32];
    usb_core_device_path(dev, path, sizeof(path));
    for (unsigned i = 0; i < dev->interface_count; ++i) {
        struct kshim_usb_interface *intf = &dev->interfaces[i];
        if (intf->driver) continue;
        for (struct kshim_usb_class_driver *drv = UsbCore.drivers; drv; drv = drv->next) {
            if (!driver_matches(drv, dev, intf)) continue;
            intf->driver = drv;
            intf->driver_data = NULL;
            int status = drv->probe(intf);
            if (dev->gone) return;
            if (!status) {
                printf("USB: %s if%u bound to %s\n", path, intf->number, drv->name ? drv->name : "?");
                break;
            }
            intf->driver = NULL;
            intf->driver_data = NULL;
        }
    }
#if CONFIG_KSHIM_USB_TRACE
    usb_trace_bound(dev);
#endif
}

/* ---- Enumeration -------------------------------------------------------- */

static uint16_t ep0_default(enum kshim_usb_speed speed)
{
    switch (speed) {
    case KSHIM_USB_SPEED_LOW: return 8;
    case KSHIM_USB_SPEED_SUPER:
    case KSHIM_USB_SPEED_SUPER_PLUS: return 512;
    default: return 64;
    }
}

static void log_device(const struct kshim_usb_device *dev)
{
    char path[32];
    usb_core_device_path(dev, path, sizeof(path));
    const struct kshim_usb_device_descriptor *d = &dev->descriptor;
    printf("USB: %s addr %u %s-speed %04x:%04x usb %x.%02x class %u/%u/%u ep0 %u, config %u of %u (%u bytes)\n",
           path, dev->address, kshim_usb_speed_name(dev->speed), d->idVendor, d->idProduct,
           d->bcdUSB >> 8, d->bcdUSB & 0xffU, d->bDeviceClass, d->bDeviceSubClass,
           d->bDeviceProtocol, dev->ep0.max_packet, dev->config_value,
           d->bNumConfigurations, dev->config_length);
    for (unsigned i = 0; i < dev->interface_count; ++i) {
        const struct kshim_usb_interface *intf = &dev->interfaces[i];
        printf("USB: %s if%u class %u/%u/%u, %u endpoints", path, intf->number,
               intf->class_code, intf->subclass, intf->protocol, intf->endpoint_count);
        static const char *const types[] = {"ctrl", "isoc", "bulk", "int"};
        for (unsigned j = 0; j < intf->endpoint_count; ++j) {
            const struct kshim_usb_endpoint *ep = &intf->endpoints[j];
            printf(" %02x:%s/%u/%u", ep->address, types[ep->type & 3U], ep->max_packet, ep->interval);
        }
        printf("\n");
    }
}

int kshim_usb_enumerate(struct kshim_usb_hcd *hcd, struct kshim_usb_device *parent,
                        uint8_t port, enum kshim_usb_speed speed,
                        struct kshim_usb_device **device)
{
    if (device) *device = NULL;
    if (!hcd || !hcd->ops || !port || speed == KSHIM_USB_SPEED_UNKNOWN) return ERROR_INVALID_PARAMETER;
    if (!parent && port > hcd->port_count) return ERROR_INVALID_PARAMETER;
    if (parent && (parent->hcd != hcd || parent->gone)) return ERROR_INVALID_PARAMETER;
    if (UsbCore.quiesced || hcd->quiesced) return KSHIM_IO_ERR_QUIESCED;
    if (!kshim_io_on_boot_cpu() || UsbCore.in_events || UsbCore.draining) return ERROR_BUSY;
    uint8_t tier = parent ? (uint8_t)(parent->tier + 1U) : 1U;
    if (tier > KSHIM_USB_MAX_TIER) return ERROR_NOT_SUPPORTED;
    struct kshim_usb_device *dev = device_alloc();
    if (!dev) return KSHIM_IO_ERR_NO_MEMORY;

    dev->hcd = hcd;
    dev->parent = parent;
    dev->port = port;
    dev->root_port = parent ? parent->root_port : port;
    dev->tier = tier;
    dev->speed = speed;
    /* Route string: one 4-bit hub port per tier below the root port. */
    dev->route = parent ? parent->route | ((uint32_t)(port > 15 ? 15 : port) << (4U * (parent->tier - 1U))) : 0;
    if (speed == KSHIM_USB_SPEED_LOW || speed == KSHIM_USB_SPEED_FULL) {
        /* The transaction translator is the nearest high-speed hub upstream. */
        const struct kshim_usb_device *child = dev;
        for (const struct kshim_usb_device *hub = parent; hub; child = hub, hub = hub->parent) {
            if (hub->speed == KSHIM_USB_SPEED_HIGH) {
                dev->tt_port = child->port;
                break;
            }
        }
    }
    dev->ep0.address = 0;
    dev->ep0.type = KSHIM_USB_XFER_CONTROL;
    dev->ep0.max_packet = ep0_default(speed);

    const struct kshim_usb_hcd_ops *ops = hcd->ops;
    bool slot = false;
    int status = ops->device_alloc(hcd, dev);
    if (status) goto fail;
    slot = true;
    status = ops->device_address(hcd, dev);
    if (status) goto fail;
    (void)delay(USB_SET_ADDRESS_RECOVERY_US);

    /* bMaxPacketSize0 first (USB 2.0 9.1.2 / 5.5.3): an 8-byte read fits
     * any full-speed default pipe. */
    uint8_t head[8];
    uint32_t actual = 0;
    status = kshim_usb_get_descriptor(dev, KSHIM_USB_DT_DEVICE, 0, 0, head, sizeof(head), &actual);
    if (!status && (actual < sizeof(head) || head[1] != KSHIM_USB_DT_DEVICE)) status = KSHIM_IO_ERR_PROTOCOL;
    if (status) goto fail;
    uint16_t mps0 = speed >= KSHIM_USB_SPEED_SUPER ? (uint16_t)(head[7] < 16 ? 1U << head[7] : 0) : head[7];
    if (mps0 != 8 && mps0 != 16 && mps0 != 32 && mps0 != 64 && mps0 != 512) {
        status = KSHIM_IO_ERR_PROTOCOL;
        goto fail;
    }
    if (mps0 != dev->ep0.max_packet) {
        dev->ep0.max_packet = mps0;
        if (ops->device_update_ep0 && (status = ops->device_update_ep0(hcd, dev))) goto fail;
    }
    uint8_t raw[sizeof(dev->descriptor)];
    status = kshim_usb_get_descriptor(dev, KSHIM_USB_DT_DEVICE, 0, 0, raw, sizeof(raw), &actual);
    if (!status && (actual < sizeof(raw) || raw[1] != KSHIM_USB_DT_DEVICE)) status = KSHIM_IO_ERR_PROTOCOL;
    if (status) goto fail;
    memcpy(&dev->descriptor, raw, sizeof(raw));

    if ((status = select_config(dev))) goto fail;
    if ((status = ops->device_configure(hcd, dev))) goto fail;
    status = kshim_usb_control(dev, KSHIM_USB_DIR_OUT | KSHIM_USB_TYPE_STANDARD |
                               KSHIM_USB_RECIP_DEVICE, KSHIM_USB_REQ_SET_CONFIGURATION,
                               dev->config_value, 0, NULL, 0, 1000, NULL);
    if (status) goto fail;
    dev->configured = true;
    if (!parent) hcd->root[port - 1U] = dev;
    log_device(dev);
#if CONFIG_KSHIM_USB_TRACE
    usb_trace_device(dev);
#endif
    usb_core_bind_device(dev);
    if (device) *device = dev;
    return 0;

fail:
    {
        char path[32];
        usb_core_device_path(dev, path, sizeof(path));
        printf("USB: %s enumeration failed: %s (%d)\n", path, kshim_io_strerror(status), status);
    }
    dev->gone = true;
    if (slot && ops->device_free) ops->device_free(hcd, dev);
    usb_core_drain();
    device_release(dev);
    return status;
}

void kshim_usb_disconnect(struct kshim_usb_device *device)
{
    if (!device) return;
    unsigned index = (unsigned)(device - UsbCore.devices);
    if (index >= KSHIM_USB_MAX_DEVICES || !UsbCore.device_used[index]) return;
    /* Children first (hubs), recursively. */
    for (unsigned i = 0; i < KSHIM_USB_MAX_DEVICES; ++i)
        if (UsbCore.device_used[i] && UsbCore.devices[i].parent == device)
            kshim_usb_disconnect(&UsbCore.devices[i]);
    char path[32];
    usb_core_device_path(device, path, sizeof(path));
    device->gone = true;
    for (unsigned i = 0; i < device->interface_count; ++i) {
        struct kshim_usb_interface *intf = &device->interfaces[i];
        if (intf->driver && intf->driver->disconnect) intf->driver->disconnect(intf);
        intf->driver = NULL;
        intf->driver_data = NULL;
    }
#if CONFIG_KSHIM_USB_TRACE
    usb_trace_disconnect(device);
#endif
    struct kshim_usb_hcd *hcd = device->hcd;
    if (hcd && hcd->ops->device_free) hcd->ops->device_free(hcd, device);
    usb_core_drain();
    if (hcd && !device->parent && device->port && device->port <= KSHIM_USB_MAX_ROOT_PORTS &&
        hcd->root[device->port - 1U] == device)
        hcd->root[device->port - 1U] = NULL;
    printf("USB: %s disconnected\n", path);
    device_release(device);
}

/* ---- Root ports --------------------------------------------------------- */

bool usb_core_root_busy(const struct kshim_usb_hcd *hcd)
{
    if (hcd->port_change_mask) return true;
    for (unsigned p = 0; p < hcd->port_count; ++p)
        if (UsbCore.root[hcd->index][p].state == USB_ROOT_DEBOUNCE) return true;
    return false;
}

static void root_port(struct kshim_usb_hcd *hcd, uint8_t port, bool changed)
{
    struct usb_root_port *rp = &UsbCore.root[hcd->index][port - 1U];
    if (!changed && rp->state != USB_ROOT_DEBOUNCE) return;
    struct kshim_usb_port_status ps = {0};
    if (hcd->ops->port_status(hcd, port, &ps)) return;
    if (ps.change && hcd->ops->port_clear_change) hcd->ops->port_clear_change(hcd, port, ps.change);
    bool connected = (ps.status & KSHIM_USB_PORT_CONNECTION) != 0;
    struct kshim_usb_device *dev = hcd->root[port - 1U];
    if (dev && (!connected || ((ps.change & KSHIM_USB_PORT_C_CONNECTION) &&
                               !(ps.status & KSHIM_USB_PORT_ENABLE)))) {
        kshim_usb_disconnect(dev);
        dev = NULL;
    }
    if (!connected) {
        rp->state = USB_ROOT_IDLE;
        rp->attempts = 0;
        return;
    }
    if (dev || rp->state == USB_ROOT_FAILED) return;
    uint64_t now = usb_core_now_ms();
    if (rp->state != USB_ROOT_DEBOUNCE || (ps.change & KSHIM_USB_PORT_C_CONNECTION)) {
        rp->state = USB_ROOT_DEBOUNCE;
        rp->deadline_ms = now + USB_DEBOUNCE_MS;
        return;
    }
    if (now < rp->deadline_ms) return;
    rp->state = USB_ROOT_IDLE;
    enum kshim_usb_speed speed = KSHIM_USB_SPEED_UNKNOWN;
    int status = hcd->ops->port_reset ? hcd->ops->port_reset(hcd, port, &speed) : ERROR_NOT_SUPPORTED;
    if (!status) status = kshim_usb_enumerate(hcd, NULL, port, speed, &dev);
    else printf("USB: %u-%u port reset failed: %s (%d)\n", hcd->index, port,
                kshim_io_strerror(status), status);
    if (!status) {
        rp->attempts = 0;
        return;
    }
    if (++rp->attempts < USB_ENUM_ATTEMPTS && status != KSHIM_IO_ERR_QUIESCED) {
        /* Retry after another debounce period. */
        rp->state = USB_ROOT_DEBOUNCE;
        rp->deadline_ms = usb_core_now_ms() + USB_DEBOUNCE_MS;
        return;
    }
    rp->state = USB_ROOT_FAILED;
    if (hcd->ops->port_disable) (void)hcd->ops->port_disable(hcd, port);
    printf("USB: %u-%u giving up after %u attempts\n", hcd->index, port, (unsigned)rp->attempts);
}

void usb_core_root_service(struct kshim_usb_hcd *hcd)
{
    uint32_t mask = __atomic_exchange_n(&hcd->port_change_mask, 0U, __ATOMIC_RELAXED);
    for (uint8_t port = 1; port <= hcd->port_count && !hcd->quiesced; ++port)
        root_port(hcd, port, (mask >> (port - 1U)) & 1U);
}
