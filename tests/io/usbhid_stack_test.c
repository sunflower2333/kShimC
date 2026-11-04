/* SPDX-License-Identifier: MIT */
/*
 * USB core + hub + HID/XInput class drivers over a fake HCD.
 *
 * The fake HCD implements kshim/usb_hcd.h over a model of devices that
 * answer chapter 9, HID and hub class requests from their descriptors
 * (QEMU usb-kbd/usb-mouse/usb-tablet layouts, an Xbox-style HID gamepad,
 * an XInput pad, USB 2.0 full/high-speed hubs and a SuperSpeed hub). The
 * test checks enumeration (addresses, descriptors, configuration, route
 * strings, TT), class binding, the reports reaching the menu through the
 * real consumer hooks (kshim_key_source_poll, kshim_input_inject_poll_touch
 * after kshim_input_service), boot-protocol fallback, STALL recovery,
 * hotplug below hubs and on root ports with debounce, a device that fails
 * enumeration, and the handoff order (children before parents, newest
 * first, then the HCD quiesce).
 *
 * The core is the xHCI branch's kernel/drivers/usb/core/ (core.c, desc.c,
 * enumerate.c). The fake HCD follows its contract: data moves through
 * urb->bounce when the core bounced the buffer into the DMA pool, and
 * finished URBs are handed back with kshim_usb_hcd_giveback(); the core
 * runs completions and callbacks from kshim_usb_poll_events().
 */
#include <kshim/usb.h>
#include <kshim/usb_hcd.h>
#include <kshim/usb_class.h>
#include <kshim/hid.h>
#include <kshim/input_inject.h>
#include <kshim/io.h>
#include <kshim/input.h>
#include <qcom_keys.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Fake clock and interrupt layer ------------------------------------- */

static uint64_t now_us;
static bool irq_live;
uint64_t kshim_smp_time_us(void) { return now_us; }
uint64_t get_current_time(void) { return now_us / 1000U; }
bool kshim_irq_ready(void) { return irq_live; }
void kshim_wait_event(uint64_t deadline_ms)
{
    uint64_t deadline = deadline_ms * 1000U;
    now_us = deadline > now_us ? deadline : now_us + 1U;
}
uint64_t delay(uint64_t us) { now_us += us; return us; }
unsigned long kshim_irq_save(void) { return 0; }
void kshim_irq_restore(unsigned long flags) { (void)flags; }

enum {
    KEY_UP = 103, KEY_DOWN = 108, BTN_SOUTH = 0x130, BTN_EAST = 0x131, BTN_DPAD_DOWN = 0x221,
};

/* ---- Descriptors -------------------------------------------------------- */

static const uint8_t KbdReport[] = {
    0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x75, 0x01, 0x95, 0x08, 0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7,
    0x15, 0x00, 0x25, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0x95, 0x05, 0x75, 0x01,
    0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02, 0x95, 0x01, 0x75, 0x03, 0x91, 0x01, 0x95, 0x06,
    0x75, 0x08, 0x15, 0x00, 0x25, 0xff, 0x05, 0x07, 0x19, 0x00, 0x29, 0xff, 0x81, 0x00, 0xc0,
};
static const uint8_t MouseReport[] = {
    0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x01, 0xa1, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x05,
    0x15, 0x00, 0x25, 0x01, 0x95, 0x05, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x03, 0x81, 0x01,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x03,
    0x81, 0x06, 0xc0, 0xc0,
};
static const uint8_t TabletReport[] = {
    0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x01, 0xa1, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x05,
    0x15, 0x00, 0x25, 0x01, 0x95, 0x05, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x03, 0x81, 0x01,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x00, 0x26, 0xff, 0x7f, 0x35, 0x00, 0x46, 0xff, 0x7f,
    0x75, 0x10, 0x95, 0x02, 0x81, 0x02, 0x05, 0x01, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7f, 0x35, 0x00,
    0x45, 0x00, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06, 0xc0, 0xc0,
};
/* Gamepad: report ID 1, X/Y 8-bit, hat 0..7 (null 8), 8 buttons. */
static const uint8_t PadReport[] = {
    0x05, 0x01, 0x09, 0x05, 0xa1, 0x01, 0x85, 0x01,
    0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08, 0x95, 0x02, 0x09, 0x30, 0x09, 0x31, 0x81, 0x02,
    0x25, 0x07, 0x75, 0x04, 0x95, 0x01, 0x09, 0x39, 0x81, 0x42, 0x81, 0x01,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x08, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0xc0,
};
/* Truncated: the last item misses its data byte. */
static const uint8_t BrokenReport[] = {0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x75};

struct fake_dev {
    const char *name;
    uint8_t device[18];
    uint8_t config[96];
    uint16_t config_length;
    const uint8_t *report;
    uint16_t report_length;
    enum kshim_usb_speed speed;
    bool dead;           /* never answers SET_ADDRESS */
    bool stall_set_idle;
    bool stall_next_interrupt;
    bool gone;
    /* State seen by the test. */
    uint8_t address, config_value, protocol, hub_depth;
    bool idle_set, protocol_set;
    unsigned clear_halts, alloc_attempts;
    uint8_t queue[8][32];
    uint8_t queue_length[8];
    unsigned queue_head, queue_count;
    /* Hubs */
    bool hub, hub_ss;
    uint8_t hub_desc[12], hub_desc_length, ports;
    struct fake_dev *child[8];
    uint16_t port_status[8], port_change[8];
    uint16_t bitmap;
    struct kshim_usb_device *usb;
};

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

static void make_device(struct fake_dev *f, const char *name, enum kshim_usb_speed speed, uint8_t cls,
                        uint16_t vid, uint16_t pid)
{
    memset(f, 0, sizeof(*f));
    f->name = name;
    f->speed = speed;
    f->protocol = 1;
    uint8_t mps = speed >= KSHIM_USB_SPEED_SUPER ? 9 : speed == KSHIM_USB_SPEED_HIGH ? 64 : 8;
    uint8_t d[18] = {18, 1, 0x00, 0x02, cls, 0, cls == 9 ? (speed == KSHIM_USB_SPEED_HIGH ? 1 : speed >= KSHIM_USB_SPEED_SUPER ? 3 : 0) : 0,
                     mps, 0, 0, 0, 0, 0, 0, 1, 2, 3, 1};
    put16(&d[8], vid);
    put16(&d[10], pid);
    memcpy(f->device, d, sizeof(d));
}

static uint8_t *config_begin(struct fake_dev *f)
{
    uint8_t c[9] = {9, 2, 0, 0, 1, 1, 0, 0xa0, 50};
    memcpy(f->config, c, 9);
    f->config_length = 9;
    return f->config;
}
static void config_add(struct fake_dev *f, const uint8_t *desc)
{
    memcpy(f->config + f->config_length, desc, desc[0]);
    f->config_length = (uint16_t)(f->config_length + desc[0]);
    put16(&f->config[2], f->config_length);
}

static void make_hid(struct fake_dev *f, const char *name, enum kshim_usb_speed speed, uint8_t sub, uint8_t proto,
                     const uint8_t *report, uint16_t length, uint8_t mps, uint8_t interval)
{
    make_device(f, name, speed, 0, 0x0627, 0x0001);
    config_begin(f);
    uint8_t intf[9] = {9, 4, 0, 0, 1, 3, sub, proto, 0};
    uint8_t hid[9] = {9, 0x21, 0x01, 0x01, 0, 1, 0x22, 0, 0};
    uint8_t ep[7] = {7, 5, 0x81, 3, mps, 0, interval};
    put16(&hid[7], length);
    config_add(f, intf);
    config_add(f, hid);
    config_add(f, ep);
    if (speed >= KSHIM_USB_SPEED_SUPER) {
        uint8_t comp[6] = {6, 0x30, 8, 0, 64, 0}; /* bMaxBurst 8 */
        config_add(f, comp);
    }
    f->report = report;
    f->report_length = length;
}

static void make_xinput(struct fake_dev *f)
{
    make_device(f, "xinput", KSHIM_USB_SPEED_FULL, 0xff, 0x045e, 0x028e);
    config_begin(f);
    f->config[4] = 2;
    uint8_t intf0[9] = {9, 4, 0, 0, 2, 0xff, 0x5d, 0x01, 0};
    uint8_t vendor[17] = {17, 0x21, 0x00, 0x01, 0x01, 0x25, 0x81, 0x14, 0, 0, 0, 0, 0x13, 0x01, 0x08, 0, 0};
    uint8_t in[7] = {7, 5, 0x81, 3, 32, 0, 4};
    uint8_t out[7] = {7, 5, 0x01, 3, 32, 0, 8};
    uint8_t intf1[9] = {9, 4, 1, 0, 0, 0xff, 0x5d, 0x03, 0};
    config_add(f, intf0);
    config_add(f, vendor);
    config_add(f, in);
    config_add(f, out);
    config_add(f, intf1);
}

static void make_hub(struct fake_dev *f, const char *name, enum kshim_usb_speed speed, uint8_t ports)
{
    make_device(f, name, speed, 9, 0x0409, 0x55aa);
    config_begin(f);
    f->hub = true;
    f->hub_ss = speed >= KSHIM_USB_SPEED_SUPER;
    f->ports = ports;
    uint8_t intf[9] = {9, 4, 0, 0, 1, 9, 0, speed == KSHIM_USB_SPEED_HIGH ? 1 : 0, 0};
    uint8_t ep[7] = {7, 5, 0x81, 3, 2, 0, speed == KSHIM_USB_SPEED_FULL ? 255 : 12};
    config_add(f, intf);
    config_add(f, ep);
    if (f->hub_ss) {
        uint8_t comp[6] = {6, 0x30, 0, 0, 2, 0};
        config_add(f, comp);
        uint8_t d[12] = {12, 0x2a, ports, 0x09, 0x00, 10, 0, 0, 0, 0, 0, 0};
        memcpy(f->hub_desc, d, 12);
        f->hub_desc_length = 12;
    } else {
        /* wHubCharacteristics: per-port power (01), TT think time 01. */
        uint8_t d[11] = {11, 0x29, ports, 0x21, 0x00, 50, 0, 0, 0, 0xff, 0xff};
        memcpy(f->hub_desc, d, 11);
        f->hub_desc_length = 11;
    }
}

static void queue_report(struct fake_dev *f, const uint8_t *data, uint8_t length)
{
    assert(f->queue_count < 8);
    unsigned at = (f->queue_head + f->queue_count++) % 8U;
    memcpy(f->queue[at], data, length);
    f->queue_length[at] = length;
}

/* ---- Fake HCD ----------------------------------------------------------- */

#define ROOT_PORTS 6U
static struct fake_dev *Root[ROOT_PORTS];
static uint16_t RootStatus[ROOT_PORTS], RootChange[ROOT_PORTS];
static struct kshim_usb_urb *Pending[128];
static unsigned PendingCount;
static uint8_t NextAddress;
static char FreeLog[64][16];
static unsigned FreeCount, ConfigureCount, UpdateEp0Count, EndpointResets, PortDisables, HcdQuiesces;
static uint8_t HubConfigured[8][3];
static unsigned HubConfigureCount;
static struct kshim_usb_hcd Hcd;

static void root_attach(uint8_t port, struct fake_dev *f)
{
    Root[port - 1] = f;
    f->gone = false;
    RootStatus[port - 1] = KSHIM_USB_PORT_CONNECTION | KSHIM_USB_PORT_POWER;
    RootChange[port - 1] |= KSHIM_USB_PORT_C_CONNECTION;
    kshim_usb_hcd_port_changed(&Hcd, port);
}
static void mark_gone(struct fake_dev *f)
{
    f->gone = true;
    for (unsigned i = 0; i < 8; ++i) if (f->child[i]) mark_gone(f->child[i]);
}
static void root_detach(uint8_t port)
{
    mark_gone(Root[port - 1]);
    Root[port - 1] = NULL;
    RootStatus[port - 1] = KSHIM_USB_PORT_POWER;
    RootChange[port - 1] |= KSHIM_USB_PORT_C_CONNECTION;
    kshim_usb_hcd_port_changed(&Hcd, port);
}
static void hub_attach(struct fake_dev *hub, uint8_t port, struct fake_dev *f)
{
    hub->child[port - 1] = f;
    f->gone = false;
    hub->port_status[port - 1] |= 0x0001;
    hub->port_change[port - 1] |= 0x0001;
    hub->bitmap |= (uint16_t)(1U << port);
}
static void hub_detach(struct fake_dev *hub, uint8_t port)
{
    mark_gone(hub->child[port - 1]);
    hub->child[port - 1] = NULL;
    hub->port_status[port - 1] &= (uint16_t)~0x0003U;
    hub->port_change[port - 1] |= 0x0001;
    hub->bitmap |= (uint16_t)(1U << port);
}

static int f_port_status(struct kshim_usb_hcd *h, uint8_t port, struct kshim_usb_port_status *s)
{
    (void)h;
    s->status = RootStatus[port - 1];
    s->change = RootChange[port - 1];
    s->speed = Root[port - 1] ? Root[port - 1]->speed : KSHIM_USB_SPEED_UNKNOWN;
    return 0;
}
static int f_port_clear(struct kshim_usb_hcd *h, uint8_t port, uint16_t change)
{
    (void)h;
    RootChange[port - 1] &= (uint16_t)~change;
    return 0;
}
static int f_port_reset(struct kshim_usb_hcd *h, uint8_t port, enum kshim_usb_speed *speed)
{
    if (!Root[port - 1]) return KSHIM_IO_ERR_NO_DEVICE;
    RootStatus[port - 1] |= KSHIM_USB_PORT_ENABLE;
    RootChange[port - 1] |= KSHIM_USB_PORT_C_RESET;
    *speed = Root[port - 1]->speed;
    kshim_usb_hcd_port_changed(h, port);
    return 0;
}
static int f_port_disable(struct kshim_usb_hcd *h, uint8_t port)
{
    (void)h;
    RootStatus[port - 1] &= (uint16_t)~KSHIM_USB_PORT_ENABLE;
    ++PortDisables;
    return 0;
}
static int f_device_alloc(struct kshim_usb_hcd *h, struct kshim_usb_device *d)
{
    (void)h;
    struct fake_dev *f = d->parent ? ((struct fake_dev *)d->parent->hcd_priv)->child[d->port - 1] : Root[d->port - 1];
    if (!f) return KSHIM_IO_ERR_NO_DEVICE;
    ++f->alloc_attempts;
    assert(d->ep0.max_packet == (d->speed >= KSHIM_USB_SPEED_SUPER ? 512 : d->speed == KSHIM_USB_SPEED_LOW ? 8 : 64));
    /* The HCD names the TT hub (kshim/usb.h): the nearest high-speed hub
     * upstream of a low/full-speed device, whose port the core put in
     * tt_port. This fake uses USB addresses as slot ids. */
    d->tt_hub_slot = 0;
    if (d->tt_port)
        for (const struct kshim_usb_device *hub = d->parent; hub; hub = hub->parent)
            if (hub->speed == KSHIM_USB_SPEED_HIGH) { d->tt_hub_slot = hub->address; break; }
    d->hcd_priv = f;
    f->usb = d;
    return 0;
}
static int f_device_address(struct kshim_usb_hcd *h, struct kshim_usb_device *d)
{
    (void)h;
    struct fake_dev *f = d->hcd_priv;
    if (f->dead) return ERROR_TIMEOUT;
    f->address = ++NextAddress;
    d->address = f->address;
    return 0;
}
static int f_update_ep0(struct kshim_usb_hcd *h, struct kshim_usb_device *d)
{
    (void)h; (void)d;
    ++UpdateEp0Count;
    return 0;
}
static int f_configure(struct kshim_usb_hcd *h, struct kshim_usb_device *d)
{
    (void)h; (void)d;
    ++ConfigureCount;
    return 0;
}
static int f_hub_configure(struct kshim_usb_hcd *h, struct kshim_usb_device *d, uint8_t ports, uint8_t think,
                           bool multi)
{
    (void)h; (void)d;
    HubConfigured[HubConfigureCount][0] = ports;
    HubConfigured[HubConfigureCount][1] = think;
    HubConfigured[HubConfigureCount++][2] = multi;
    return 0;
}
static int f_endpoint_reset(struct kshim_usb_hcd *h, struct kshim_usb_device *d, struct kshim_usb_endpoint *e)
{
    (void)h; (void)d; (void)e;
    ++EndpointResets;
    return 0;
}
static void f_device_free(struct kshim_usb_hcd *h, struct kshim_usb_device *d)
{
    (void)h;
    struct fake_dev *f = d->hcd_priv;
    for (unsigned i = 0; i < PendingCount; ++i) assert(Pending[i]->device != d); /* all cancelled */
    if (f) {
        snprintf(FreeLog[FreeCount++ % 64], 16, "%s", f->name);
        f->usb = NULL;
    }
}
static int f_submit(struct kshim_usb_hcd *h, struct kshim_usb_urb *u)
{
    (void)h;
    /* Synchronous helpers submit without a callback; the core bounces any
     * buffer outside the DMA pool. */
    assert(PendingCount < 128 && u->status == KSHIM_IO_ERR_PENDING);
    assert(!u->length || u->bounce);
    Pending[PendingCount++] = u;
    return 0;
}
static void complete(struct kshim_usb_urb *u, int status, uint32_t actual)
{
    kshim_usb_hcd_giveback(u, status, actual);
}
/* The bytes the controller would move: the core's DMA bounce buffer. */
static uint8_t *xfer_data(struct kshim_usb_urb *u) { return u->bounce ? u->bounce : u->buffer; }
static int f_cancel(struct kshim_usb_hcd *h, struct kshim_usb_urb *u)
{
    (void)h;
    for (unsigned i = 0; i < PendingCount; ++i) {
        if (Pending[i] != u) continue;
        memmove(&Pending[i], &Pending[i + 1], (PendingCount - i - 1) * sizeof(Pending[0]));
        --PendingCount;
        complete(u, KSHIM_IO_ERR_CANCELLED, 0);
        return 0;
    }
    return 0;
}
static int f_quiesce(struct kshim_usb_hcd *h)
{
    (void)h;
    ++HcdQuiesces;
    assert(PendingCount == 0);
    return 0;
}

static uint32_t copy_out(void *dst, uint32_t want, const void *src, uint32_t have)
{
    uint32_t n = want < have ? want : have;
    memcpy(dst, src, n);
    return n;
}

/* Chapter 9, HID and hub requests. Returns 0 or KSHIM_IO_ERR_STALL. */
static int device_request(struct fake_dev *f, const struct kshim_usb_setup *s, uint8_t *data, uint32_t *actual)
{
    uint8_t type = (uint8_t)(s->wValue >> 8), port = (uint8_t)s->wIndex;
    *actual = 0;
    switch ((s->bmRequestType << 8) | s->bRequest) {
    case 0x8006:
        if (type == 1) { *actual = copy_out(data, s->wLength, f->device, 18); return 0; }
        if (type == 2 && (s->wValue & 0xff) == 0) { *actual = copy_out(data, s->wLength, f->config, f->config_length); return 0; }
        return KSHIM_IO_ERR_STALL;
    case 0x0009: f->config_value = (uint8_t)s->wValue; return 0;
    case 0x8106:
        if (type != 0x22 || !f->report) return KSHIM_IO_ERR_STALL;
        *actual = copy_out(data, s->wLength, f->report, f->report_length);
        return 0;
    case 0x210a:
        if (f->stall_set_idle) return KSHIM_IO_ERR_STALL;
        f->idle_set = true;
        return 0;
    case 0x210b: f->protocol = (uint8_t)s->wValue; f->protocol_set = true; return 0;
    case 0x0201: ++f->clear_halts; return 0;
    default: break;
    }
    if (!f->hub) return KSHIM_IO_ERR_STALL;
    switch ((s->bmRequestType << 8) | s->bRequest) {
    case 0xa006:
        if (type != f->hub_desc[1]) return KSHIM_IO_ERR_STALL;
        *actual = copy_out(data, s->wLength, f->hub_desc, f->hub_desc_length);
        return 0;
    case 0x200c: f->hub_depth = (uint8_t)s->wValue; return 0;
    case 0xa000: { uint8_t z[4] = {0}; *actual = copy_out(data, s->wLength, z, 4); return 0; }
    case 0xa300: {
        if (!port || port > f->ports) return KSHIM_IO_ERR_STALL;
        uint8_t st[4];
        put16(st, f->port_status[port - 1]);
        put16(st + 2, f->port_change[port - 1]);
        *actual = copy_out(data, s->wLength, st, 4);
        return 0;
    }
    case 0x2303:
        if (!port || port > f->ports) return KSHIM_IO_ERR_STALL;
        if (s->wValue == 8) f->port_status[port - 1] |= f->hub_ss ? 0x0200 : 0x0100;
        if (s->wValue == 4 && (f->port_status[port - 1] & 1U)) {
            struct fake_dev *c = f->child[port - 1];
            f->port_status[port - 1] |= 0x0002;
            if (!f->hub_ss && c->speed == KSHIM_USB_SPEED_LOW) f->port_status[port - 1] |= 0x0200;
            if (!f->hub_ss && c->speed == KSHIM_USB_SPEED_HIGH) f->port_status[port - 1] |= 0x0400;
            f->port_change[port - 1] |= 0x0010;
        }
        return 0;
    case 0x2301:
        if (!port || port > f->ports) return KSHIM_IO_ERR_STALL;
        if (s->wValue == 1) f->port_status[port - 1] &= (uint16_t)~0x0602U;
        if (s->wValue >= 16 && s->wValue <= 20) f->port_change[port - 1] &= (uint16_t)~(1U << (s->wValue - 16));
        if (s->wValue == 29) f->port_change[port - 1] &= (uint16_t)~0x20U;
        return 0;
    default:
        return KSHIM_IO_ERR_STALL;
    }
}

static unsigned Polls, ControlRequests;
static int f_poll(struct kshim_usb_hcd *h)
{
    (void)h;
    ++Polls;
    struct kshim_usb_urb *batch[128];
    unsigned count = PendingCount, handled = 0;
    memcpy(batch, Pending, count * sizeof(batch[0]));
    PendingCount = 0;
    for (unsigned i = 0; i < count; ++i) {
        struct kshim_usb_urb *u = batch[i];
        struct fake_dev *f = u->device->hcd_priv;
        if (f->gone) { complete(u, KSHIM_IO_ERR_NO_DEVICE, 0); ++handled; continue; }
        if (u->endpoint->type == KSHIM_USB_XFER_CONTROL) {
            uint32_t actual;
            ++ControlRequests;
            int status = device_request(f, &u->setup, xfer_data(u), &actual);
            complete(u, status, actual);
            ++handled;
            continue;
        }
        if (f->stall_next_interrupt) {
            f->stall_next_interrupt = false;
            complete(u, KSHIM_IO_ERR_STALL, 0);
            ++handled;
            continue;
        }
        if (f->hub && f->bitmap) {
            uint8_t bits[2];
            put16(bits, f->bitmap);
            f->bitmap = 0;
            complete(u, 0, copy_out(xfer_data(u), u->length, bits, (f->ports + 8U) / 8U));
            ++handled;
            continue;
        }
        if (!f->hub && f->queue_count) {
            unsigned at = f->queue_head;
            f->queue_head = (f->queue_head + 1U) % 8U;
            --f->queue_count;
            complete(u, 0, copy_out(xfer_data(u), u->length, f->queue[at], f->queue_length[at]));
            ++handled;
            continue;
        }
        Pending[PendingCount++] = u; /* still waiting for data (NAK) */
    }
    return (int)handled;
}

static const struct kshim_usb_hcd_ops FakeOps = {
    .port_status = f_port_status, .port_clear_change = f_port_clear, .port_reset = f_port_reset,
    .port_disable = f_port_disable, .device_alloc = f_device_alloc, .device_address = f_device_address,
    .device_update_ep0 = f_update_ep0, .device_configure = f_configure, .hub_configure = f_hub_configure,
    .endpoint_reset = f_endpoint_reset, .device_free = f_device_free, .submit = f_submit,
    .cancel = f_cancel, .poll = f_poll, .quiesce = f_quiesce,
};

/* ---- Helpers ------------------------------------------------------------ */

static QcomKeys PlatformKeys; /* none configured: the QEMU case */

static bool next_key(kshim_key_event_t *e)
{
    return kshim_key_source_poll(QcomKeysSource(&PlatformKeys), e) == KSHIM_KEYS_EVENT;
}
static void expect_key(int type, int action, uint32_t code)
{
    kshim_key_event_t e;
    for (unsigned i = 0; i < 4 && !next_key(&e); ++i) (void)0;
    if (e.type != (kshim_key_event_type_t)type || e.action != (kshim_key_action_t)action || e.code != code) {
        fprintf(stderr, "key %d/%d/%u, expected %d/%d/%u\n", e.type, e.action, (unsigned)e.code, type, action,
                (unsigned)code);
        assert(0);
    }
}
static void expect_no_key(void)
{
    kshim_key_event_t e;
    for (unsigned i = 0; i < 3; ++i) assert(!next_key(&e));
}
/* Menu loop stand-in: pump input for `ms` milliseconds of fake time. */
static void run_ms(unsigned ms)
{
    uint64_t end = now_us + (uint64_t)ms * 1000U;
    while (now_us < end) {
        kshim_input_service();
        now_us += 1000U;
    }
}
static unsigned live_devices(struct fake_dev *const *devs, unsigned n)
{
    unsigned live = 0;
    for (unsigned i = 0; i < n; ++i) live += devs[i]->usb != NULL;
    return live;
}
static int free_index(const char *name)
{
    for (unsigned i = 0; i < FreeCount; ++i) if (!strcmp(FreeLog[i], name)) return (int)i;
    return -1;
}

static struct fake_dev Kbd, Hub, Mouse, Tablet, Hub2, Pad, XPad, HsHub, LsKbd, SsHub, SsPad, Dead, Kbd2;

int main(void)
{
    kshim_touch_event_t t;
    now_us = 1000000;

    make_hid(&Kbd, "kbd", KSHIM_USB_SPEED_FULL, 1, 1, KbdReport, sizeof(KbdReport), 8, 10);
    make_hid(&Mouse, "mouse", KSHIM_USB_SPEED_FULL, 1, 2, MouseReport, sizeof(MouseReport), 4, 10);
    make_hid(&Tablet, "tablet", KSHIM_USB_SPEED_FULL, 0, 0, TabletReport, sizeof(TabletReport), 8, 10);
    make_hid(&Pad, "pad", KSHIM_USB_SPEED_FULL, 0, 0, PadReport, sizeof(PadReport), 64, 4);
    Pad.stall_set_idle = true;
    make_hid(&LsKbd, "lskbd", KSHIM_USB_SPEED_LOW, 1, 1, BrokenReport, sizeof(BrokenReport), 8, 10);
    make_hid(&SsPad, "sspad", KSHIM_USB_SPEED_SUPER, 0, 0, PadReport, sizeof(PadReport), 64, 4);
    make_hid(&Dead, "dead", KSHIM_USB_SPEED_FULL, 1, 1, KbdReport, sizeof(KbdReport), 8, 10);
    Dead.dead = true;
    make_hid(&Kbd2, "kbd2", KSHIM_USB_SPEED_HIGH, 1, 1, KbdReport, sizeof(KbdReport), 8, 4);
    make_xinput(&XPad);
    make_hub(&Hub, "hub", KSHIM_USB_SPEED_FULL, 8);
    make_hub(&Hub2, "hub2", KSHIM_USB_SPEED_FULL, 4);
    make_hub(&HsHub, "hshub", KSHIM_USB_SPEED_HIGH, 4);
    make_hub(&SsHub, "sshub", KSHIM_USB_SPEED_SUPER, 4);

    /* Root 1 kbd; root 2 hub -> {1 mouse, 2 tablet, 3 hub2 -> {1 pad}};
     * root 3 XInput; root 4 high-speed hub -> {2 low-speed boot keyboard};
     * root 5 a device that never takes an address; root 6 SuperSpeed hub ->
     * {1 SuperSpeed gamepad}. */
    hub_attach(&Hub, 1, &Mouse);
    hub_attach(&Hub, 2, &Tablet);
    hub_attach(&Hub, 3, &Hub2);
    hub_attach(&Hub2, 1, &Pad);
    hub_attach(&HsHub, 2, &LsKbd);
    hub_attach(&SsHub, 1, &SsPad);
    /* Devices present at power-on: no change bits until the scan. */
    for (unsigned i = 0; i < 8; ++i) Hub.port_change[i] = Hub2.port_change[i] = HsHub.port_change[i] = SsHub.port_change[i] = 0;
    Hub.bitmap = Hub2.bitmap = HsHub.bitmap = SsHub.bitmap = 0;

    assert(kshim_usb_init(NULL) == 0);
    assert(kshim_usb_hub_register() == 0 && kshim_usb_hid_register() == 0);
    assert(kshim_usb_hub_register() == 0); /* idempotent */
    /* Before any controller: settle is immediate. */
    assert(kshim_usb_settle(1500) == 0);

    Hcd = (struct kshim_usb_hcd){.name = "fake-xhci", .ops = &FakeOps, .port_count = ROOT_PORTS};
    root_attach(1, &Kbd);
    root_attach(2, &Hub);
    root_attach(3, &XPad);
    root_attach(4, &HsHub);
    root_attach(5, &Dead);
    root_attach(6, &SsHub);
    for (unsigned i = 0; i < ROOT_PORTS; ++i) RootChange[i] = 0; /* present at controller start */
    assert(kshim_usb_hcd_register(&Hcd) == 0);
    assert(kshim_usb_hcd_register(&Hcd) == KSHIM_IO_ERR_EXISTS);

    uint64_t start = now_us;
    assert(kshim_usb_settle(1500) == 0);
    printf("settled in %u ms\n", (unsigned)((now_us - start) / 1000U));
    assert(now_us - start < 1500000U);

    struct fake_dev *const all[] = {&Kbd, &Hub, &Mouse, &Tablet, &Hub2, &Pad, &XPad, &HsHub, &LsKbd, &SsHub, &SsPad};
    assert(live_devices(all, 11) == 11);
    /* The core tries a root port 3 times (USB_ENUM_ATTEMPTS), then disables it. */
    assert(!Dead.usb && Dead.alloc_attempts == 3 && PortDisables == 1);
    for (unsigned i = 0; i < 11; ++i) assert(all[i]->config_value == 1 && all[i]->address);
    assert(ConfigureCount == 11);
    /* bMaxPacketSize0 8 differs from the 64-byte full-speed default. */
    assert(UpdateEp0Count >= 7);

    /* Topology: route strings, tiers, TT. */
    assert(Pad.usb->tier == 3 && Pad.usb->route == 0x13 && Pad.usb->root_port == 2);
    assert(Mouse.usb->route == 0x1 && Mouse.usb->tier == 2 && !Mouse.usb->tt_port);
    assert(LsKbd.usb->tt_port == 2 && LsKbd.usb->tt_hub_slot == HsHub.address);
    assert(SsPad.usb->speed == KSHIM_USB_SPEED_SUPER && SsPad.usb->ep0.max_packet == 512);
    assert(SsPad.usb->interfaces[0].endpoints[0].max_burst == 8);
    assert(HubConfigureCount == 4);
    bool hs_seen = false;
    for (unsigned i = 0; i < HubConfigureCount; ++i)
        if (HubConfigured[i][0] == 4 && HubConfigured[i][1] == 1) hs_seen = true;
    assert(hs_seen);
    assert(SsHub.hub_depth == 0 && Hub.usb->hub_ports == 8);
    assert(Hub.port_status[0] & 0x0100); /* powered */
    assert(SsHub.port_status[0] & 0x0200);

    /* Class binding and HID setup. */
    assert(Kbd.idle_set && !Kbd.protocol_set);
    assert(Pad.usb->interfaces[0].driver && !Pad.idle_set); /* SET_IDLE STALL ignored */
    assert(LsKbd.protocol_set && LsKbd.protocol == 0);       /* boot fallback */
    assert(XPad.usb->interfaces[0].driver && !XPad.usb->interfaces[1].driver);
    assert(!strcmp(XPad.usb->interfaces[0].driver->name, "xinput"));
    assert(kshim_input_inject_has(KSHIM_INPUT_CAP_KEYS | KSHIM_INPUT_CAP_POINTER));

    /* ---- Menu input through the real consumer hooks ---- */
    kshim_input_inject_set_surface(320, 240);
    expect_no_key();
    uint8_t kr[8] = {0, 0, 0x51, 0, 0, 0, 0, 0};
    queue_report(&Kbd, kr, 8);
    expect_key(KSHIM_KEY_EVENT_PRESS, KSHIM_KEY_ACTION_DOWN, KEY_DOWN);
    kr[2] = 0;
    queue_report(&Kbd, kr, 8);
    expect_key(KSHIM_KEY_EVENT_RELEASE, KSHIM_KEY_ACTION_DOWN, KEY_DOWN);

    uint8_t boot[8] = {0, 0, 0x52, 0, 0, 0, 0, 0}; /* boot keyboard below the HS hub */
    queue_report(&LsKbd, boot, 8);
    expect_key(KSHIM_KEY_EVENT_PRESS, KSHIM_KEY_ACTION_UP, KEY_UP);
    boot[2] = 0;
    queue_report(&LsKbd, boot, 8);
    expect_key(KSHIM_KEY_EVENT_RELEASE, KSHIM_KEY_ACTION_UP, KEY_UP);

    uint8_t pr[5] = {1, 0x80, 0x80, 0x04, 0x00}; /* gamepad: hat S */
    queue_report(&Pad, pr, 5);
    expect_key(KSHIM_KEY_EVENT_PRESS, KSHIM_KEY_ACTION_DOWN, BTN_DPAD_DOWN);
    pr[3] = 0x08; pr[4] = 0x01; /* centred, button 1 */
    queue_report(&Pad, pr, 5);
    expect_key(KSHIM_KEY_EVENT_RELEASE, KSHIM_KEY_ACTION_DOWN, BTN_DPAD_DOWN);
    expect_key(KSHIM_KEY_EVENT_PRESS, KSHIM_KEY_ACTION_SELECT, BTN_SOUTH);
    pr[4] = 0x02;
    queue_report(&Pad, pr, 5);
    expect_key(KSHIM_KEY_EVENT_RELEASE, KSHIM_KEY_ACTION_SELECT, BTN_SOUTH);
    expect_key(KSHIM_KEY_EVENT_PRESS, KSHIM_KEY_ACTION_BACK, BTN_EAST);
    pr[4] = 0;
    queue_report(&Pad, pr, 5);
    expect_key(KSHIM_KEY_EVENT_RELEASE, KSHIM_KEY_ACTION_BACK, BTN_EAST);

    /* Held stick repeats from the HID service hook. */
    pr[2] = 0xff;
    queue_report(&Pad, pr, 5);
    expect_key(KSHIM_KEY_EVENT_PRESS, KSHIM_KEY_ACTION_DOWN, BTN_DPAD_DOWN);
    run_ms(520);
    expect_key(KSHIM_KEY_EVENT_PRESS, KSHIM_KEY_ACTION_DOWN, BTN_DPAD_DOWN);
    pr[2] = 0x80;
    queue_report(&Pad, pr, 5);
    expect_key(KSHIM_KEY_EVENT_RELEASE, KSHIM_KEY_ACTION_DOWN, BTN_DPAD_DOWN);

    uint8_t sp[5] = {1, 0x80, 0x80, 0x08, 0x01}; /* SuperSpeed pad */
    queue_report(&SsPad, sp, 5);
    expect_key(KSHIM_KEY_EVENT_PRESS, KSHIM_KEY_ACTION_SELECT, BTN_SOUTH);
    sp[4] = 0;
    queue_report(&SsPad, sp, 5);
    expect_key(KSHIM_KEY_EVENT_RELEASE, KSHIM_KEY_ACTION_SELECT, BTN_SOUTH);

    uint8_t xr[20] = {0x00, 0x14, 0x00, 0x10};  /* XInput A */
    queue_report(&XPad, xr, 20);
    expect_key(KSHIM_KEY_EVENT_PRESS, KSHIM_KEY_ACTION_SELECT, BTN_SOUTH);
    xr[3] = 0;
    queue_report(&XPad, xr, 20);
    expect_key(KSHIM_KEY_EVENT_RELEASE, KSHIM_KEY_ACTION_SELECT, BTN_SOUTH);

    uint8_t mr[4] = {0, 20, 10, 0}; /* mouse below the hub */
    queue_report(&Mouse, mr, 4);
    kshim_input_service();
    assert(kshim_input_inject_poll_touch(&t) && t.type == KSHIM_TOUCH_EVENT_HOVER && t.x == 180 && t.y == 130);
    uint8_t tr[6] = {1, 0x00, 0x00, 0xff, 0x7f, 0}; /* tablet click bottom-left */
    queue_report(&Tablet, tr, 6);
    tr[0] = 0;
    queue_report(&Tablet, tr, 6);
    run_ms(3);
    assert(kshim_input_inject_poll_touch(&t) && t.type == KSHIM_TOUCH_EVENT_PRESS && t.x == 0 && t.y == 239);
    assert(kshim_input_inject_poll_touch(&t) && t.type == KSHIM_TOUCH_EVENT_RELEASE);
    assert(!kshim_input_inject_poll_touch(&t));

    /* ---- STALL on the keyboard's interrupt endpoint: clear and resume ---- */
    unsigned resets = EndpointResets;
    Kbd.stall_next_interrupt = true;
    run_ms(3);
    assert(Kbd.clear_halts == 1 && EndpointResets == resets + 1);
    kr[2] = 0x28;
    queue_report(&Kbd, kr, 8);
    expect_key(KSHIM_KEY_EVENT_PRESS, KSHIM_KEY_ACTION_SELECT, 28);

    /* ---- Unplug a keyboard with a key held: RELEASE is delivered ---- */
    irq_live = true; /* the rest runs with sleeping waits */
    root_detach(1);
    run_ms(2);
    expect_key(KSHIM_KEY_EVENT_RELEASE, KSHIM_KEY_ACTION_SELECT, 28);
    assert(!Kbd.usb && free_index("kbd") >= 0);

    /* ---- Hotplug below a hub, with debounce ---- */
    hub_detach(&Hub, 1);
    run_ms(5);
    assert(!Mouse.usb && free_index("mouse") >= 0);
    hub_attach(&Hub, 1, &Mouse);
    run_ms(50);
    assert(!Mouse.usb); /* still debouncing */
    run_ms(120);
    assert(Mouse.usb && Mouse.usb->route == 0x1);
    mr[1] = 30; mr[2] = 0; /* the pointer is where the tablet left it */
    queue_report(&Mouse, mr, 4);
    run_ms(2);
    assert(kshim_input_inject_poll_touch(&t) && t.type == KSHIM_TOUCH_EVENT_HOVER && t.x == 30 && t.y == 239);

    /* ---- The device that failed is retried only on a new connection ---- */
    unsigned attempts = Dead.alloc_attempts;
    run_ms(300);
    assert(Dead.alloc_attempts == attempts);
    root_detach(5);
    run_ms(5);
    root_attach(5, &Kbd2);
    run_ms(150);
    assert(Kbd2.usb && Kbd2.usb->speed == KSHIM_USB_SPEED_HIGH && Kbd2.idle_set);

    /* ---- Removing a hub removes its subtree, children first ---- */
    unsigned before = FreeCount;
    root_detach(2);
    run_ms(5);
    assert(!Hub.usb && !Hub2.usb && !Pad.usb && !Tablet.usb && !Mouse.usb);
    assert(FreeCount == before + 5);
    assert(free_index("pad") < free_index("hub2") && free_index("hub2") < free_index("hub"));
    assert(strcmp(FreeLog[FreeCount - 1], "hub") == 0);

    /* ---- Handoff ---- */
    before = FreeCount;
    assert(kshim_usb_quiesce() == 0);
    assert(PendingCount == 0);
    /* Children before their hub; the core walks root ports from the
     * highest: port 6 (SuperSpeed hub subtree), 5 (kbd2), 4 (high-speed hub
     * subtree), 3 (the XInput pad) last. */
    assert(FreeCount == before + 6);
    assert(!strcmp(FreeLog[before], "sspad"));
    assert(free_index("sspad") < free_index("sshub") && free_index("sshub") < free_index("kbd2"));
    assert(free_index("kbd2") < free_index("lskbd") && free_index("lskbd") < free_index("hshub"));
    assert(!strcmp(FreeLog[FreeCount - 1], "xinput"));
    assert(kshim_usb_quiesce() == 0 && FreeCount == before + 6);
    assert(kshim_usb_hcd_quiesce_all() == 0 && HcdQuiesces == 1);
    /* ops->quiesce is idempotent by contract (kshim/usb_hcd.h); the core
     * may call it again on a repeated handoff attempt. */
    assert(kshim_usb_hcd_quiesce_all() == 0 && HcdQuiesces >= 1);
    unsigned polls = Polls;
    kshim_usb_poll();
    assert(Polls == polls);
    root_attach(1, &Kbd);
    run_ms(200);
    assert(!Kbd.usb);
    struct kshim_usb_device fake = {.hcd = &Hcd};
    struct kshim_usb_urb urb = {.device = &fake, .endpoint = &fake.ep0};
    assert(kshim_usb_submit_urb(&urb) == KSHIM_IO_ERR_QUIESCED);
    assert(kshim_usb_hcd_register(&Hcd) == KSHIM_IO_ERR_QUIESCED);
    printf("usbhid_stack_test: OK (%u control requests)\n", ControlRequests);
    return 0;
}
