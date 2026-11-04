/* SPDX-License-Identifier: MIT */
/*
 * USB hub class driver (kshim/usb_class.h, docs/io-stack.md 3.8).
 *
 * Written from USB 2.0 chapter 11 (11.23.2.1 hub descriptor, 11.24.2 hub
 * class requests and feature selectors, 11.24.2.7 wPortStatus/wPortChange,
 * 11.12.1 status change endpoint bitmap, 7.1.7.3 100 ms connect debounce,
 * 7.1.7.5 10 ms reset recovery) and USB 3.2 chapter 10 (10.15.2.1
 * SuperSpeed hub descriptor 0x2a, 10.16.2.9 SET_HUB_DEPTH, 10.16.2.6
 * SuperSpeed port status bits). Facts only; no code from other stacks.
 *
 * The status-change URB callback only records which ports changed. All
 * port work (GET_STATUS, CLEAR_FEATURE, debounce, PORT_RESET, enumeration
 * and removal) runs from the service hook in thread context. Only the
 * public USB core API is used, so any core implementing kshim/usb.h works.
 */
#include <kshim/usb.h>
#include <kshim/usb_hcd.h>
#include <kshim/usb_class.h>
#include <kshim/io.h>
#include <kshim/time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Requests and selectors (USB 2.0 tables 11-15, 11-17; USB 3.2 10-10). */
#define HUB_REQ_SET_HUB_DEPTH 12U
#define HUB_RT_IN_DEVICE  (KSHIM_USB_DIR_IN | KSHIM_USB_TYPE_CLASS | KSHIM_USB_RECIP_DEVICE)
#define HUB_RT_OUT_DEVICE (KSHIM_USB_DIR_OUT | KSHIM_USB_TYPE_CLASS | KSHIM_USB_RECIP_DEVICE)
#define HUB_RT_IN_PORT    (KSHIM_USB_DIR_IN | KSHIM_USB_TYPE_CLASS | KSHIM_USB_RECIP_OTHER)
#define HUB_RT_OUT_PORT   (KSHIM_USB_DIR_OUT | KSHIM_USB_TYPE_CLASS | KSHIM_USB_RECIP_OTHER)

#define HUB_C_HUB_LOCAL_POWER  0U
#define HUB_C_HUB_OVER_CURRENT 1U
#define PORT_FEAT_ENABLE       1U
#define PORT_FEAT_RESET        4U
#define PORT_FEAT_POWER        8U
#define PORT_FEAT_C_CONNECTION 16U
#define PORT_FEAT_C_ENABLE     17U
#define PORT_FEAT_C_SUSPEND    18U
#define PORT_FEAT_C_OVER_CURRENT 19U
#define PORT_FEAT_C_RESET      20U
#define PORT_FEAT_C_LINK_STATE 25U   /* SuperSpeed */
#define PORT_FEAT_C_CONFIG_ERROR 26U /* SuperSpeed */
#define PORT_FEAT_C_BH_RESET   29U   /* SuperSpeed */

/* wPortStatus */
#define PS_CONNECTION   0x0001U
#define PS_ENABLE       0x0002U
#define PS_OVER_CURRENT 0x0008U
#define PS_POWER_USB2   0x0100U
#define PS_LOW_SPEED    0x0200U
#define PS_HIGH_SPEED   0x0400U
#define PS_POWER_SS     0x0200U
/* wPortChange */
#define PC_CONNECTION   0x0001U
#define PC_ENABLE       0x0002U  /* USB 2.0 only */
#define PC_SUSPEND      0x0004U  /* USB 2.0 only */
#define PC_OVER_CURRENT 0x0008U
#define PC_RESET        0x0010U
#define PC_BH_RESET     0x0020U  /* SuperSpeed */
#define PC_LINK_STATE   0x0040U  /* SuperSpeed */
#define PC_CONFIG_ERROR 0x0080U  /* SuperSpeed */

/* The xHCI route string has 4 bits per tier, so 15 ports are addressable. */
#define HUB_MAX_PORTS 15U
#define HUB_DESC_MAX 72U
#define HUB_DEBOUNCE_MS 100U
#define HUB_RESET_POLL_MS 10U
#define HUB_RESET_TIMEOUT_MS 500U
#define HUB_RESET_RECOVERY_MS 10U
#define HUB_CONTROL_TIMEOUT_MS 1000U
#define HUB_MAX_URB_ERRORS 8U
#define HUB_FALLBACK_POLL_MS 250U

struct hub {
    struct kshim_usb_interface *intf;
    struct kshim_usb_device *dev;
    struct kshim_usb_endpoint *ep;
    struct kshim_usb_urb urb;
    void *raw;
    uint8_t *bitmap;
    uint8_t ports;
    bool superspeed;
    bool urb_active, urb_error, stopped;
    unsigned urb_errors;
    uint64_t last_poll_ms;
    uint16_t pending;   /* bit 0: hub status; bit n: port n needs GET_STATUS */
    uint16_t debounce;  /* ports waiting for a stable connection */
    uint16_t failed;    /* enumeration failed; retried on the next connect change */
    uint64_t connect_since[HUB_MAX_PORTS + 1U];
    struct kshim_usb_device *children[HUB_MAX_PORTS + 1U];
};

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static void sleep_ms(uint32_t ms) { (void)delay((uint64_t)ms * 1000U); }

static int hub_control(struct hub *hub, uint8_t type, uint8_t request, uint16_t value, uint16_t index,
                       void *data, uint16_t length, uint32_t *actual)
{
    return kshim_usb_control(hub->dev, type, request, value, index, data, length,
                             HUB_CONTROL_TIMEOUT_MS, actual);
}

static int port_feature(struct hub *hub, bool set, uint16_t feature, uint8_t port)
{
    return hub_control(hub, HUB_RT_OUT_PORT, set ? KSHIM_USB_REQ_SET_FEATURE : KSHIM_USB_REQ_CLEAR_FEATURE,
                       feature, port, NULL, 0, NULL);
}

static int port_status(struct hub *hub, uint8_t port, uint16_t *status, uint16_t *change)
{
    uint8_t raw[4];
    uint32_t actual = 0;
    int result = hub_control(hub, HUB_RT_IN_PORT, KSHIM_USB_REQ_GET_STATUS, 0, port, raw, sizeof(raw), &actual);
    if (result) return result;
    if (actual < sizeof(raw)) return KSHIM_IO_ERR_PROTOCOL;
    *status = le16(raw);
    *change = le16(raw + 2);
    return 0;
}

/* Acknowledge every change bit so the hub reports the next one. */
static void port_clear_changes(struct hub *hub, uint8_t port, uint16_t change)
{
    static const struct { uint16_t bit, feature; } map[] = {
        {PC_CONNECTION, PORT_FEAT_C_CONNECTION}, {PC_OVER_CURRENT, PORT_FEAT_C_OVER_CURRENT},
        {PC_RESET, PORT_FEAT_C_RESET},
    };
    static const struct { uint16_t bit, feature; } usb2[] = {
        {PC_ENABLE, PORT_FEAT_C_ENABLE}, {PC_SUSPEND, PORT_FEAT_C_SUSPEND},
    };
    static const struct { uint16_t bit, feature; } ss[] = {
        {PC_BH_RESET, PORT_FEAT_C_BH_RESET}, {PC_LINK_STATE, PORT_FEAT_C_LINK_STATE},
        {PC_CONFIG_ERROR, PORT_FEAT_C_CONFIG_ERROR},
    };
    for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); ++i)
        if (change & map[i].bit) (void)port_feature(hub, false, map[i].feature, port);
    if (hub->superspeed) {
        for (unsigned i = 0; i < sizeof(ss) / sizeof(ss[0]); ++i)
            if (change & ss[i].bit) (void)port_feature(hub, false, ss[i].feature, port);
    } else {
        for (unsigned i = 0; i < sizeof(usb2) / sizeof(usb2[0]); ++i)
            if (change & usb2[i].bit) (void)port_feature(hub, false, usb2[i].feature, port);
    }
}

static int submit_status(struct hub *hub)
{
    int status = kshim_usb_submit_urb(&hub->urb);
    hub->urb_active = status == 0;
    return status;
}

/* URB callback (kshim_usb_poll_events): record the changed ports only. */
static void status_complete(struct kshim_usb_urb *urb)
{
    struct hub *hub = urb->context;
    hub->urb_active = false;
    if (hub->stopped) return;
    if (!urb->status) {
        hub->urb_errors = 0;
        for (uint32_t bit = 0; bit <= hub->ports && bit < urb->actual * 8U; ++bit)
            if (hub->bitmap[bit >> 3] & (1U << (bit & 7U))) hub->pending |= (uint16_t)(1U << bit);
        if (submit_status(hub)) hub->urb_error = true;
        return;
    }
    if (urb->status == KSHIM_IO_ERR_CANCELLED || urb->status == KSHIM_IO_ERR_QUIESCED ||
        urb->status == KSHIM_IO_ERR_NO_DEVICE || urb->status == ERROR_NOT_READY)
        return;
    hub->urb_error = true; /* the service hook recovers the endpoint */
}

static int port_reset(struct hub *hub, uint8_t port, enum kshim_usb_speed *speed)
{
    uint16_t status = 0, change = 0;
    int result = port_feature(hub, true, PORT_FEAT_RESET, port);
    if (result) return result;
    uint32_t waited = 0;
    for (;;) {
        sleep_ms(HUB_RESET_POLL_MS);
        waited += HUB_RESET_POLL_MS;
        result = port_status(hub, port, &status, &change);
        if (result) return result;
        if (!(status & PS_CONNECTION)) return KSHIM_IO_ERR_NO_DEVICE;
        if (change & PC_RESET) break;
        if (waited >= HUB_RESET_TIMEOUT_MS) return ERROR_TIMEOUT;
    }
    port_clear_changes(hub, port, change);
    if (!(status & PS_ENABLE)) return ERROR_DEVICE_ERROR;
    if (hub->superspeed) *speed = KSHIM_USB_SPEED_SUPER;
    else if (status & PS_LOW_SPEED) *speed = KSHIM_USB_SPEED_LOW;
    else if (status & PS_HIGH_SPEED) *speed = KSHIM_USB_SPEED_HIGH;
    else *speed = KSHIM_USB_SPEED_FULL;
    sleep_ms(HUB_RESET_RECOVERY_MS);
    return 0;
}

static void port_attach(struct hub *hub, uint8_t port)
{
    enum kshim_usb_speed speed = KSHIM_USB_SPEED_UNKNOWN;
    struct kshim_usb_device *child = NULL;
    int result = port_reset(hub, port, &speed);
    if (!result) result = kshim_usb_enumerate(hub->dev->hcd, hub->dev, port, speed, &child);
    if (!result && child) {
        hub->children[port] = child;
        return;
    }
    if (hub->stopped) return;
    hub->failed |= (uint16_t)(1U << port);
    (void)port_feature(hub, false, PORT_FEAT_ENABLE, port);
    printf("USB hub %04x:%04x port %u: device not usable (%s)\n", hub->dev->descriptor.idVendor,
           hub->dev->descriptor.idProduct, port, kshim_io_strerror(result));
}

static void port_service(struct hub *hub, uint8_t port)
{
    uint16_t bit = (uint16_t)(1U << port);
    uint16_t status, change;
    if (port_status(hub, port, &status, &change)) return;
    if (change) port_clear_changes(hub, port, change);
    bool connected = (status & PS_CONNECTION) != 0;
    if (change & PC_CONNECTION) {
        hub->failed &= (uint16_t)~bit;
        hub->connect_since[port] = 0;
    }
    if (change & PC_OVER_CURRENT) {
        printf("USB hub %04x:%04x port %u over-current\n", hub->dev->descriptor.idVendor,
               hub->dev->descriptor.idProduct, port);
        if (!(status & (hub->superspeed ? PS_POWER_SS : PS_POWER_USB2)))
            (void)port_feature(hub, true, PORT_FEAT_POWER, port);
    }
    struct kshim_usb_device *child = hub->children[port];
    if (child && (!connected || (change & PC_CONNECTION))) {
        hub->children[port] = NULL;
        kshim_usb_disconnect(child);
    }
    if (!connected) {
        hub->connect_since[port] = 0;
        hub->debounce &= (uint16_t)~bit;
        return;
    }
    if (hub->children[port] || (hub->failed & bit)) return;
    uint64_t now = get_current_time();
    /* kshim_usb_settle() cannot see a hub port's debounce: keep it polling
     * until the port is due, plus the reset (10 ms poll) that follows. */
    if (!hub->connect_since[port]) {
        hub->connect_since[port] = now ? now : 1U;
        hub->debounce |= bit;
        kshim_usb_settle_extend(HUB_DEBOUNCE_MS + HUB_RESET_POLL_MS);
        return;
    }
    if (now - hub->connect_since[port] < HUB_DEBOUNCE_MS) {
        hub->debounce |= bit;
        kshim_usb_settle_extend((uint32_t)(HUB_DEBOUNCE_MS - (now - hub->connect_since[port])) +
                                HUB_RESET_POLL_MS);
        return;
    }
    hub->debounce &= (uint16_t)~bit;
    hub->connect_since[port] = 0;
    port_attach(hub, port);
}

static void hub_status_service(struct hub *hub)
{
    uint8_t raw[4];
    uint32_t actual = 0;
    if (hub_control(hub, HUB_RT_IN_DEVICE, KSHIM_USB_REQ_GET_STATUS, 0, 0, raw, sizeof(raw), &actual) ||
        actual < sizeof(raw))
        return;
    uint16_t change = le16(raw + 2);
    if (change & 1U) (void)hub_control(hub, HUB_RT_OUT_DEVICE, KSHIM_USB_REQ_CLEAR_FEATURE,
                                        HUB_C_HUB_LOCAL_POWER, 0, NULL, 0, NULL);
    if (change & 2U) {
        printf("USB hub %04x:%04x over-current\n", hub->dev->descriptor.idVendor,
               hub->dev->descriptor.idProduct);
        (void)hub_control(hub, HUB_RT_OUT_DEVICE, KSHIM_USB_REQ_CLEAR_FEATURE, HUB_C_HUB_OVER_CURRENT, 0,
                          NULL, 0, NULL);
    }
}

static void hub_service(struct kshim_usb_interface *intf)
{
    struct hub *hub = intf->driver_data;
    if (!hub || hub->stopped) return;
    uint64_t now = get_current_time();
    if (hub->urb_error && !hub->urb_active) {
        hub->urb_error = false;
        (void)kshim_usb_clear_halt(hub->dev, hub->ep);
        if (++hub->urb_errors > HUB_MAX_URB_ERRORS)
            printf("USB hub %04x:%04x: status endpoint failing; ports are polled\n",
                   hub->dev->descriptor.idVendor, hub->dev->descriptor.idProduct);
        else if (submit_status(hub))
            hub->urb_error = true;
    }
    /* Without a working status endpoint every port is polled (fallback). */
    if (hub->urb_errors > HUB_MAX_URB_ERRORS && now - hub->last_poll_ms >= HUB_FALLBACK_POLL_MS) {
        hub->last_poll_ms = now;
        hub->pending |= (uint16_t)(((1U << hub->ports) - 1U) << 1);
    }
    if (hub->pending & 1U) {
        hub->pending &= (uint16_t)~1U;
        hub_status_service(hub);
    }
    for (uint8_t port = 1; port <= hub->ports && !hub->stopped; ++port) {
        uint16_t bit = (uint16_t)(1U << port);
        bool due = (hub->pending & bit) != 0;
        /* A debouncing port needs no I/O until its 100 ms have passed. */
        if (!due && (hub->debounce & bit) && now - hub->connect_since[port] >= HUB_DEBOUNCE_MS) due = true;
        if (!due) continue;
        hub->pending &= (uint16_t)~bit;
        port_service(hub, port);
    }
}

static void *alloc_aligned(size_t size, void **raw)
{
    *raw = calloc(1, size + 63U);
    if (!*raw) return NULL;
    return (void *)(((uintptr_t)*raw + 63U) & ~(uintptr_t)63U);
}

static int hub_probe(struct kshim_usb_interface *intf)
{
    struct kshim_usb_device *dev = intf->device;
    if (dev->tier >= KSHIM_USB_MAX_TIER) {
        printf("USB hub %04x:%04x: tier %u, no room below it\n", dev->descriptor.idVendor,
               dev->descriptor.idProduct, dev->tier);
        return ERROR_NOT_SUPPORTED;
    }
    struct kshim_usb_endpoint *ep = kshim_usb_find_endpoint(intf, KSHIM_USB_XFER_INTERRUPT, true);
    if (!ep) return ERROR_NOT_SUPPORTED;
    struct hub *hub = calloc(1, sizeof(*hub));
    if (!hub) return KSHIM_IO_ERR_NO_MEMORY;
    hub->intf = intf;
    hub->dev = dev;
    hub->ep = ep;
    hub->superspeed = dev->speed >= KSHIM_USB_SPEED_SUPER;
    int status = 0;
    /* USB 3.2 10.16.2.9: the hub depth (0 below a root port) selects the
     * route string nibble the hub decodes; required before the ports work. */
    if (hub->superspeed)
        status = hub_control(hub, HUB_RT_OUT_DEVICE, HUB_REQ_SET_HUB_DEPTH, (uint16_t)(dev->tier - 1U), 0,
                             NULL, 0, NULL);
    uint8_t desc[HUB_DESC_MAX];
    uint32_t actual = 0;
    uint8_t type = hub->superspeed ? KSHIM_USB_DT_SS_HUB : KSHIM_USB_DT_HUB;
    if (!status)
        status = hub_control(hub, HUB_RT_IN_DEVICE, KSHIM_USB_REQ_GET_DESCRIPTOR, (uint16_t)(type << 8), 0, desc,
                             sizeof(desc) - 1U, &actual);
    if (!status && (actual < 7U || desc[1] != type || !desc[2])) status = KSHIM_IO_ERR_PROTOCOL;
    if (status) {
        free(hub);
        return status;
    }
    uint8_t ports = desc[2];
    if (ports > HUB_MAX_PORTS) {
        printf("USB hub %04x:%04x: using %u of %u ports\n", dev->descriptor.idVendor,
               dev->descriptor.idProduct, HUB_MAX_PORTS, ports);
        ports = HUB_MAX_PORTS;
    }
    uint16_t characteristics = le16(&desc[3]);
    uint32_t power_good_ms = desc[5] * 2U;
    /* wHubCharacteristics 6:5: TT think time (high-speed hubs). Interface
     * protocol 2 is the multi-TT setting (USB 2.0 11.23.1); alternate 0 is
     * used, which is single-TT unless its protocol says otherwise. */
    uint8_t think = dev->speed == KSHIM_USB_SPEED_HIGH ? (uint8_t)((characteristics >> 5) & 3U) : 0U;
    bool multi_tt = dev->speed == KSHIM_USB_SPEED_HIGH && intf->protocol == 2U;
    struct kshim_usb_hcd *hcd = dev->hcd;
    if (hcd->ops->hub_configure) {
        status = hcd->ops->hub_configure(hcd, dev, ports, think, multi_tt);
        if (status) {
            free(hub);
            return status;
        }
    }
    hub->ports = ports;
    dev->hub_ports = ports;
    for (uint8_t port = 1; port <= ports; ++port) (void)port_feature(hub, true, PORT_FEAT_POWER, port);
    /* bPwrOn2PwrGood: 2 ms units until power is good on a powered port. */
    sleep_ms(power_good_ms);
    uint32_t length = (ports + 1U + 7U) / 8U;
    hub->bitmap = alloc_aligned(64U, &hub->raw);
    if (!hub->bitmap) {
        free(hub);
        return KSHIM_IO_ERR_NO_MEMORY;
    }
    hub->urb = (struct kshim_usb_urb){.device = dev, .endpoint = ep, .buffer = hub->bitmap,
                                      .length = length, .complete = status_complete, .context = hub};
    /* Every port is looked at once: devices present at power-on may not
     * show up as a change. */
    hub->pending = (uint16_t)((((1U << ports) - 1U) << 1) | 1U);
    intf->driver_data = hub;
    if (submit_status(hub)) hub->urb_error = true;
    printf("USB hub %04x:%04x: %u ports, %s, power good %u ms\n", dev->descriptor.idVendor,
           dev->descriptor.idProduct, ports, hub->superspeed ? "SuperSpeed" : "USB 2.0",
           (unsigned)power_good_ms);
    return 0;
}

static void hub_disconnect(struct kshim_usb_interface *intf)
{
    struct hub *hub = intf->driver_data;
    if (!hub) return;
    hub->stopped = true;
    if (hub->urb_active) (void)kshim_usb_cancel_urb(&hub->urb);
    /* The core disconnected the children before calling this. */
    intf->driver_data = NULL;
    free(hub->raw);
    free(hub);
}

static struct kshim_usb_class_driver HubDriver = {
    .name = "usb-hub",
    .class_code = KSHIM_USB_CLASS_HUB,
    .subclass = KSHIM_USB_MATCH_ANY,
    .protocol = KSHIM_USB_MATCH_ANY,
    .probe = hub_probe,
    .disconnect = hub_disconnect,
    .service = hub_service,
};

int kshim_usb_hub_register(void)
{
    int status = kshim_usb_class_register(&HubDriver);
    return status == KSHIM_IO_ERR_EXISTS ? 0 : status;
}
