/* SPDX-License-Identifier: MIT */
/*
 * xHCI implementation of struct kshim_usb_hcd_ops: root ports, slots and
 * contexts, transfer TDs, transfer events and cancellation.
 *
 * Written from the xHCI 1.2 specification (see xhci.h): 4.3 USB device
 * initialization (Enable Slot, Address Device, Evaluate Context, Configure
 * Endpoint), 4.6.8-4.6.10 endpoint reset/stop/set-dequeue, 4.10-4.11 TD and
 * TRB construction (Setup/Data/Status and Normal TRBs, TD Size, ISP/IOC,
 * chaining across Link TRBs), 4.19 root hub port state and reset, 6.2 slot
 * and endpoint contexts (6.2.3.6 Interval encoding), 6.4.2 transfer events.
 */
#include "xhci.h"

#include <kshim/dma.h>
#include <kshim/time.h>

#include <stdio.h>
#include <string.h>

#define XHCI_PORT_RESET_TIMEOUT_US 500000U
#define XHCI_RESET_RECOVERY_US     10000U  /* USB 2.0 7.1.7.5 TRSTRCY */
#define XHCI_SETUP_LEN             8U

/* ---- Contexts ----------------------------------------------------------- */

static uint32_t *in_control(struct xhci *x, struct xhci_slot *s) { (void)x; return (uint32_t *)s->in_ctx; }
static uint32_t *in_slot(struct xhci *x, struct xhci_slot *s) { return (uint32_t *)((uint8_t *)s->in_ctx + x->ctx_size); }
static uint32_t *in_ep(struct xhci *x, struct xhci_slot *s, unsigned dci)
{
    return (uint32_t *)((uint8_t *)s->in_ctx + (size_t)(dci + 1U) * x->ctx_size);
}
static uint32_t *out_slot(struct xhci *x, struct xhci_slot *s) { (void)x; return (uint32_t *)s->out_ctx; }
static uint32_t *out_ep(struct xhci *x, struct xhci_slot *s, unsigned dci)
{
    return (uint32_t *)((uint8_t *)s->out_ctx + (size_t)dci * x->ctx_size);
}
static size_t in_ctx_size(struct xhci *x) { return 33U * x->ctx_size; }
static size_t out_ctx_size(struct xhci *x) { return 32U * x->ctx_size; }

static void in_ctx_reset(struct xhci *x, struct xhci_slot *s)
{
    memset(s->in_ctx, 0, in_ctx_size(x));
}

static void in_ctx_publish(struct xhci *x, struct xhci_slot *s)
{
    kshim_dma_sync_for_device(s->in_ctx, in_ctx_size(x));
}

static void out_ctx_fetch(struct xhci *x, struct xhci_slot *s)
{
    kshim_dma_sync_for_cpu(s->out_ctx, out_ctx_size(x));
}

static uint32_t speed_id(enum kshim_usb_speed speed)
{
    switch (speed) {
    case KSHIM_USB_SPEED_LOW: return XHCI_SPEED_LOW;
    case KSHIM_USB_SPEED_FULL: return XHCI_SPEED_FULL;
    case KSHIM_USB_SPEED_HIGH: return XHCI_SPEED_HIGH;
    case KSHIM_USB_SPEED_SUPER: return XHCI_SPEED_SUPER;
    case KSHIM_USB_SPEED_SUPER_PLUS: return XHCI_SPEED_SUPER_PLUS;
    default: return 0;
    }
}

static enum kshim_usb_speed speed_from_id(uint32_t id)
{
    switch (id) {
    case XHCI_SPEED_FULL: return KSHIM_USB_SPEED_FULL;
    case XHCI_SPEED_LOW: return KSHIM_USB_SPEED_LOW;
    case XHCI_SPEED_HIGH: return KSHIM_USB_SPEED_HIGH;
    case XHCI_SPEED_SUPER: return KSHIM_USB_SPEED_SUPER;
    case 0: return KSHIM_USB_SPEED_UNKNOWN;
    default: return KSHIM_USB_SPEED_SUPER_PLUS;
    }
}

static unsigned ep_dci(const struct kshim_usb_endpoint *ep)
{
    /* Device Context Index (4.5.1): EP0 and other control endpoints use the
     * IN index (2n + 1); OUT is 2n, IN is 2n + 1. */
    if (ep->type == KSHIM_USB_XFER_CONTROL) return 2U * (ep->address & 0x0fU) + 1U;
    return 2U * (ep->address & 0x0fU) + ((ep->address & KSHIM_USB_ENDPOINT_DIR_IN) ? 1U : 0U);
}

static uint32_t ep_type(const struct kshim_usb_endpoint *ep)
{
    bool in = (ep->address & KSHIM_USB_ENDPOINT_DIR_IN) != 0;
    switch (ep->type) {
    case KSHIM_USB_XFER_CONTROL: return XHCI_EP_TYPE_CONTROL;
    case KSHIM_USB_XFER_ISOC: return in ? XHCI_EP_TYPE_ISOC_IN : XHCI_EP_TYPE_ISOC_OUT;
    case KSHIM_USB_XFER_BULK: return in ? XHCI_EP_TYPE_BULK_IN : XHCI_EP_TYPE_BULK_OUT;
    default: return in ? XHCI_EP_TYPE_INT_IN : XHCI_EP_TYPE_INT_OUT;
    }
}

/* Interval field (6.2.3.6): 2^Interval * 125 us. */
static uint32_t ep_interval(enum kshim_usb_speed speed, const struct kshim_usb_endpoint *ep)
{
    if (ep->type != KSHIM_USB_XFER_INTERRUPT && ep->type != KSHIM_USB_XFER_ISOC) return 0;
    unsigned b = ep->interval;
    if (speed >= KSHIM_USB_SPEED_HIGH || ep->type == KSHIM_USB_XFER_ISOC) {
        if (b < 1) b = 1;
        if (b > 16) b = 16;
        unsigned v = b - 1U;
        if (speed < KSHIM_USB_SPEED_HIGH) v += 3U; /* FS isoch: frames */
        return v;
    }
    /* LS/FS interrupt: bInterval frames (1 ms) -> log2(frames * 8). */
    if (b < 1) b = 1;
    unsigned uframes = b * 8U, v = 0;
    while ((2U << v) <= uframes) ++v;
    if (v < 3) v = 3;
    if (v > 10) v = 10;
    return v;
}

static void fill_ep_ctx(struct xhci *x, uint32_t *ctx, const struct kshim_usb_device *dev,
                        const struct kshim_usb_endpoint *ep, const struct xhci_ring *ring)
{
    (void)x;
    memset(ctx, 0, 20);
    uint32_t type = ep_type(ep);
    uint32_t burst = 0, mult = 0;
    if (dev->speed >= KSHIM_USB_SPEED_SUPER) {
        burst = ep->max_burst;
        if (ep->type == KSHIM_USB_XFER_ISOC) mult = ep->mult;
    } else if (dev->speed == KSHIM_USB_SPEED_HIGH &&
               (ep->type == KSHIM_USB_XFER_INTERRUPT || ep->type == KSHIM_USB_XFER_ISOC)) {
        burst = ep->mult; /* additional transactions per microframe */
    }
    uint32_t esit = 0, avg = 8;
    if (ep->type == KSHIM_USB_XFER_INTERRUPT || ep->type == KSHIM_USB_XFER_ISOC) {
        esit = (uint32_t)ep->max_packet * (burst + 1U) * (mult + 1U);
        avg = esit < 1024U ? esit : 1024U;
    } else if (ep->type == KSHIM_USB_XFER_BULK) {
        avg = 3072U;
    }
    ctx[0] = (mult & 3U) << 8 | ep_interval(dev->speed, ep) << 16 | ((esit >> 16) & 0xffU) << 24;
    ctx[1] = (ep->type == KSHIM_USB_XFER_ISOC ? 0U : 3U) << 1 | type << 3 | (burst & 0xffU) << 8 |
             (uint32_t)ep->max_packet << 16;
    ctx[2] = (uint32_t)ring->dma | 1U; /* DCS = 1 */
    ctx[3] = (uint32_t)(ring->dma >> 32);
    ctx[4] = (avg & 0xffffU) | (esit & 0xffffU) << 16;
}

/* ---- Root hub ports (4.19, 5.4.8) ---------------------------------------- */

static bool port_valid(struct xhci *x, uint8_t port)
{
    return port && port <= x->hcd.port_count && x->running && !x->dead;
}

static bool port_is_usb3(struct xhci *x, uint8_t port, uint32_t portsc)
{
    if (x->ports[port].major) return x->ports[port].major >= 3;
    return XHCI_PORTSC_SPEED(portsc) >= XHCI_SPEED_SUPER;
}

static int op_port_status(struct kshim_usb_hcd *hcd, uint8_t port, struct kshim_usb_port_status *ps)
{
    struct xhci *x = xhci_from_hcd(hcd);
    if (!ps || !port_valid(x, port)) return ERROR_INVALID_PARAMETER;
    uint32_t v = xhci_op_read(x, XHCI_PORTSC(port));
    if (v == UINT32_MAX) return KSHIM_IO_ERR_NO_DEVICE;
    ps->status = (uint16_t)(((v & XHCI_PORTSC_CCS) ? KSHIM_USB_PORT_CONNECTION : 0) |
                            ((v & XHCI_PORTSC_PED) ? KSHIM_USB_PORT_ENABLE : 0) |
                            ((v & XHCI_PORTSC_OCA) ? KSHIM_USB_PORT_OVER_CURRENT : 0) |
                            ((v & XHCI_PORTSC_PR) ? KSHIM_USB_PORT_RESET : 0) |
                            ((v & XHCI_PORTSC_PP) ? KSHIM_USB_PORT_POWER : 0));
    ps->change = (uint16_t)(((v & XHCI_PORTSC_CSC) ? KSHIM_USB_PORT_C_CONNECTION : 0) |
                            ((v & XHCI_PORTSC_PEC) ? KSHIM_USB_PORT_C_ENABLE : 0) |
                            ((v & XHCI_PORTSC_OCC) ? KSHIM_USB_PORT_C_OVER_CURRENT : 0) |
                            ((v & (XHCI_PORTSC_PRC | XHCI_PORTSC_WRC)) ? KSHIM_USB_PORT_C_RESET : 0));
    ps->speed = (v & XHCI_PORTSC_CCS) ? speed_from_id(XHCI_PORTSC_SPEED(v)) : KSHIM_USB_SPEED_UNKNOWN;
    return 0;
}

static int op_port_clear_change(struct kshim_usb_hcd *hcd, uint8_t port, uint16_t change)
{
    struct xhci *x = xhci_from_hcd(hcd);
    if (!port_valid(x, port)) return ERROR_INVALID_PARAMETER;
    uint32_t v = xhci_op_read(x, XHCI_PORTSC(port));
    uint32_t clear = (v & (XHCI_PORTSC_PLC | XHCI_PORTSC_CEC));
    if (change & KSHIM_USB_PORT_C_CONNECTION) clear |= XHCI_PORTSC_CSC;
    if (change & KSHIM_USB_PORT_C_ENABLE) clear |= XHCI_PORTSC_PEC;
    if (change & KSHIM_USB_PORT_C_OVER_CURRENT) clear |= XHCI_PORTSC_OCC;
    if (change & KSHIM_USB_PORT_C_RESET) clear |= XHCI_PORTSC_PRC | XHCI_PORTSC_WRC;
    clear &= v & XHCI_PORTSC_CHANGES;
    if (clear) xhci_op_write(x, XHCI_PORTSC(port), (v & XHCI_PORTSC_PRESERVE) | clear);
    return 0;
}

static int op_port_reset(struct kshim_usb_hcd *hcd, uint8_t port, enum kshim_usb_speed *speed)
{
    struct xhci *x = xhci_from_hcd(hcd);
    if (!speed || !port_valid(x, port)) return ERROR_INVALID_PARAMETER;
    *speed = KSHIM_USB_SPEED_UNKNOWN;
    uintptr_t reg = x->op + XHCI_PORTSC(port);
    uint32_t v = xhci_op_read(x, XHCI_PORTSC(port));
    if (!(v & XHCI_PORTSC_CCS)) return KSHIM_IO_ERR_NO_DEVICE;
    bool usb3 = port_is_usb3(x, port, v);
    if (usb3 && (v & XHCI_PORTSC_PED) && XHCI_PORTSC_PLS(v) == XHCI_PLS_U0) {
        /* A USB3 port enables itself after link training (4.19.1.2). */
        *speed = speed_from_id(XHCI_PORTSC_SPEED(v));
        return 0;
    }
    /* USB2: port reset (PR); USB3 that did not train: warm reset (WPR). */
    xhci_op_write(x, XHCI_PORTSC(port), (v & XHCI_PORTSC_PRESERVE) |
                  (usb3 ? XHCI_PORTSC_WPR : XHCI_PORTSC_PR));
    bool done = xhci_wait(x, reg, XHCI_PORTSC_PRC, XHCI_PORTSC_PRC, XHCI_PORT_RESET_TIMEOUT_US) ||
                (usb3 && xhci_wait(x, reg, XHCI_PORTSC_WRC, XHCI_PORTSC_WRC, 1000));
    v = xhci_op_read(x, XHCI_PORTSC(port));
    uint32_t clear = v & (XHCI_PORTSC_PRC | XHCI_PORTSC_WRC);
    if (clear) xhci_op_write(x, XHCI_PORTSC(port), (v & XHCI_PORTSC_PRESERVE) | clear);
    if (!done) return ERROR_TIMEOUT;
    if (!(v & XHCI_PORTSC_CCS)) return KSHIM_IO_ERR_NO_DEVICE;
    if (!(v & XHCI_PORTSC_PED)) return ERROR_DEVICE_ERROR;
    *speed = speed_from_id(XHCI_PORTSC_SPEED(v));
    if (*speed == KSHIM_USB_SPEED_UNKNOWN) return KSHIM_IO_ERR_PROTOCOL;
    if (!usb3) (void)delay(XHCI_RESET_RECOVERY_US);
    return 0;
}

static int op_port_disable(struct kshim_usb_hcd *hcd, uint8_t port)
{
    struct xhci *x = xhci_from_hcd(hcd);
    if (!port_valid(x, port)) return ERROR_INVALID_PARAMETER;
    uint32_t v = xhci_op_read(x, XHCI_PORTSC(port));
    /* PED is RW1C on USB2 ports; USB3 ports cannot be disabled this way. */
    if (!port_is_usb3(x, port, v) && (v & XHCI_PORTSC_PED))
        xhci_op_write(x, XHCI_PORTSC(port), (v & XHCI_PORTSC_PRESERVE) | XHCI_PORTSC_PED);
    return 0;
}

/* ---- Transfer descriptors ------------------------------------------------ */

static struct xhci_td *td_alloc(struct xhci *x)
{
    for (unsigned i = 0; i < XHCI_MAX_TDS; ++i) {
        if (x->tds[i].in_use) continue;
        memset(&x->tds[i], 0, sizeof(x->tds[i]));
        x->tds[i].in_use = true;
        return &x->tds[i];
    }
    return NULL;
}

static void ep_queue_remove(struct xhci_ep *ep, struct kshim_usb_urb *urb)
{
    struct kshim_usb_urb **link = &ep->head, *prev = NULL;
    while (*link && *link != urb) {
        prev = *link;
        link = &((struct xhci_td *)(*link)->hcd_priv)->next;
    }
    if (!*link) return;
    *link = ((struct xhci_td *)urb->hcd_priv)->next;
    if (ep->tail == urb) ep->tail = prev;
}

void xhci_td_giveback(struct xhci *x, struct kshim_usb_urb *urb, int status, uint32_t actual)
{
    (void)x;
    struct xhci_td *td = urb->hcd_priv;
    if (td && td->in_use) {
        struct xhci_ep *ep = td->ep;
        bool head = ep->head == urb;
        ep_queue_remove(ep, urb);
        ep->ring.used = ep->ring.used >= td->count ? ep->ring.used - td->count : 0;
        if (head) {
            unsigned next = td->last + 1U;
            ep->ring.dequeue = next >= XHCI_RING_TRBS - 1U ? 0 : next;
        }
        td->in_use = false;
    }
    urb->hcd_priv = NULL;
    kshim_usb_hcd_giveback(urb, status, actual);
}

void xhci_ep_flush(struct xhci *x, struct xhci_ep *ep, int status)
{
    while (ep->head) {
        struct kshim_usb_urb *urb = ep->head;
        struct xhci_td *td = urb->hcd_priv;
        if (!td || !td->in_use) {
            ep->head = ep->tail = NULL;
            break;
        }
        xhci_td_giveback(x, urb, status, td->transferred);
    }
}

/* Ring position order from `from` (inclusive), skipping the Link TRB. */
static unsigned ring_distance(unsigned from, unsigned to)
{
    unsigned n = XHCI_RING_TRBS - 1U;
    return (to + n - from) % n;
}

static bool td_contains(const struct xhci_td *td, unsigned index)
{
    return index < XHCI_RING_TRBS - 1U &&
           ring_distance(td->first, index) <= ring_distance(td->first, td->last);
}

/* Data bytes queued in the TD's TRBs before `index`. */
static uint32_t td_bytes_before(const struct xhci_ep *ep, const struct xhci_td *td, unsigned index)
{
    if (td->data_trb == UINT32_MAX) return 0;
    uint32_t bytes = 0;
    unsigned limit = ring_distance(td->first, td->last);
    for (unsigned i = td->data_trb; i != index;) {
        if (ring_distance(td->first, i) > limit) break;
        bytes += ep->ring.trbs[i].status & 0x1ffffU;
        if (++i == XHCI_RING_TRBS - 1U) i = 0;
    }
    return bytes;
}

static bool td_is_data(const struct xhci_td *td, unsigned index, const struct xhci_ep *ep)
{
    if (td->data_trb == UINT32_MAX) return false;
    unsigned type = XHCI_TRB_GET_TYPE(ep->ring.trbs[index].control);
    return (type == XHCI_TRB_DATA || type == XHCI_TRB_NORMAL) &&
           ring_distance(td->first, index) >= ring_distance(td->first, td->data_trb);
}

static struct kshim_usb_urb *ep_find(struct xhci_ep *ep, unsigned index)
{
    for (struct kshim_usb_urb *u = ep->head; u; u = ((struct xhci_td *)u->hcd_priv)->next)
        if (td_contains(u->hcd_priv, index)) return u;
    return NULL;
}

void xhci_transfer_event(struct xhci *x, const struct xhci_trb *e)
{
    unsigned slot_id = XHCI_TRB_GET_SLOT(e->control), dci = XHCI_TRB_GET_EP(e->control);
    if (!slot_id || slot_id > XHCI_MAX_SLOTS || !dci) return;
    struct xhci_slot *slot = x->slots[slot_id];
    if (!slot) return;
    struct xhci_ep *ep = &slot->eps[dci];
    if (!ep->enabled || (e->control & XHCI_EVT_ED)) return;
    uint64_t trb = (uint64_t)e->param_hi << 32 | e->param_lo;
    if (trb < ep->ring.dma || trb >= ep->ring.dma + XHCI_RING_TRBS * sizeof(struct xhci_trb)) return;
    unsigned index = (unsigned)((trb - ep->ring.dma) / sizeof(struct xhci_trb));
    struct kshim_usb_urb *urb = ep_find(ep, index);
    if (!urb) return; /* stale (already given back) */
    struct xhci_td *td = urb->hcd_priv;
    unsigned cc = XHCI_EVT_CODE(e->status);
    uint32_t residual = XHCI_EVT_LEN(e->status);
    bool is_data = td_is_data(td, index, ep);
    uint32_t trb_len = ep->ring.trbs[index].status & 0x1ffffU;
    uint32_t before = td_bytes_before(ep, td, index);
    bool last = index == td->last;

    switch (cc) {
    case XHCI_CC_SUCCESS:
        if (is_data) td->transferred = before + trb_len - (residual < trb_len ? residual : trb_len);
        if (!last) return;
        xhci_td_giveback(x, urb, 0, td->short_seen || is_data ? td->transferred : td->data_len);
        return;
    case XHCI_CC_SHORT_PACKET:
        td->transferred = before + trb_len - (residual < trb_len ? residual : trb_len);
        td->short_seen = true;
        /* Control: the Status stage TD still runs; others end here (4.10.1.1). */
        if (ep->endpoint && ep->endpoint->type == KSHIM_USB_XFER_CONTROL && !last) return;
        xhci_td_giveback(x, urb, 0, td->transferred);
        return;
    case XHCI_CC_STOPPED:
    case XHCI_CC_STOPPED_SHORT_PACKET:
        if (is_data) td->transferred = before + trb_len - (residual < trb_len ? residual : trb_len);
        return; /* cancel() or the next doorbell decides */
    case XHCI_CC_STOPPED_LENGTH_INVALID:
        return;
    default: {
        int status = xhci_status_from_cc(cc);
        if (!status) status = ERROR_DEVICE_ERROR;
        if (is_data) td->transferred = before + trb_len - (residual < trb_len ? residual : trb_len);
        /* STALL, babble, transaction and similar errors halt the endpoint
         * (4.10.2): its HCD state needs Reset Endpoint + Set TR Dequeue. */
        if (cc == XHCI_CC_STALL || cc == XHCI_CC_BABBLE || cc == XHCI_CC_TRANSACTION ||
            cc == XHCI_CC_DATA_BUFFER || cc == XHCI_CC_TRB) {
            ep->needs_reset = true;
            if (ep->endpoint) ep->endpoint->halted = true;
        }
        xhci_td_giveback(x, urb, status, td->transferred);
        return;
    }
    }
}

/* ---- Endpoint recovery (4.6.8, 4.6.9, 4.6.10) ------------------------------ */

static int cmd_simple(struct xhci *x, uint32_t type, uint8_t slot, unsigned dci, uint64_t param,
                      uint32_t flags, unsigned *cc)
{
    struct xhci_trb trb = {
        (uint32_t)param, (uint32_t)(param >> 32), 0,
        XHCI_TRB_TYPE(type) | XHCI_TRB_SLOT(slot) | XHCI_TRB_EP(dci) | flags,
    };
    return xhci_command(x, &trb, cc, NULL);
}

/* Point the endpoint's dequeue at the first queued TD, or at the enqueue
 * position when nothing is queued (the endpoint must be Stopped). */
static int ep_set_dequeue(struct xhci *x, struct xhci_slot *slot, struct xhci_ep *ep)
{
    unsigned index = ep->ring.enqueue;
    uint32_t cycle = ep->ring.cycle;
    unsigned used = 0;
    if (ep->head) {
        struct xhci_td *td = ep->head->hcd_priv;
        index = td->first;
        cycle = td->first_cycle;
        for (struct kshim_usb_urb *u = ep->head; u; u = ((struct xhci_td *)u->hcd_priv)->next)
            used += ((struct xhci_td *)u->hcd_priv)->count;
    }
    unsigned cc = 0;
    int status = cmd_simple(x, XHCI_TRB_SET_TR_DEQ, slot->id, ep->dci,
                            xhci_ring_trb_dma(&ep->ring, index) | cycle, 0, &cc);
    if (status) return status;
    if (cc != XHCI_CC_SUCCESS) return xhci_status_from_cc(cc);
    ep->ring.dequeue = index;
    ep->ring.used = used;
    return 0;
}

static int ep_recover(struct xhci *x, struct xhci_slot *slot, struct xhci_ep *ep)
{
    unsigned cc = 0;
    int status = cmd_simple(x, XHCI_TRB_RESET_EP, slot->id, ep->dci, 0, 0, &cc);
    if (status) return status;
    /* Context State Error: the endpoint was not halted (already reset). */
    if (cc != XHCI_CC_SUCCESS && cc != XHCI_CC_CONTEXT_STATE) return xhci_status_from_cc(cc);
    status = ep_set_dequeue(x, slot, ep);
    if (status) return status;
    ep->needs_reset = false;
    if (ep->endpoint && ep->dci == 1) ep->endpoint->halted = false;
    if (ep->head) xhci_ring_doorbell(x, slot->id, ep->dci);
    return 0;
}

/* ---- Slots ---------------------------------------------------------------- */

static struct xhci_slot *slot_of(struct kshim_usb_device *dev)
{
    return dev ? (struct xhci_slot *)dev->hcd_priv : NULL;
}

void xhci_slot_release(struct xhci *x, struct xhci_slot *slot, int status)
{
    for (unsigned d = 1; d < 32; ++d) {
        struct xhci_ep *ep = &slot->eps[d];
        xhci_ep_flush(x, ep, status);
        if (ep->endpoint) ep->endpoint->hcd_priv = NULL;
        xhci_ring_free(&ep->ring);
        ep->enabled = false;
    }
    if (slot->id && slot->id <= XHCI_MAX_SLOTS) {
        if (x->dcbaa) {
            x->dcbaa[slot->id] = 0;
            kshim_dma_sync_for_device(&x->dcbaa[slot->id], sizeof(uint64_t));
        }
        x->slots[slot->id] = NULL;
    }
    if (slot->device) slot->device->hcd_priv = NULL;
    if (slot->out_ctx) kshim_dma_free(slot->out_ctx, out_ctx_size(x));
    if (slot->in_ctx) kshim_dma_free(slot->in_ctx, in_ctx_size(x));
    kshim_dma_free(slot, sizeof(*slot));
}

static int op_device_alloc(struct kshim_usb_hcd *hcd, struct kshim_usb_device *dev)
{
    struct xhci *x = xhci_from_hcd(hcd);
    if (x->dead || !x->running) return ERROR_DEVICE_ERROR;
    unsigned cc = 0;
    uint8_t id = 0;
    struct xhci_trb trb = {0, 0, 0, XHCI_TRB_TYPE(XHCI_TRB_ENABLE_SLOT)};
    int status = xhci_command(x, &trb, &cc, &id);
    if (status) return status;
    if (cc != XHCI_CC_SUCCESS) return cc == XHCI_CC_NO_SLOTS ? KSHIM_IO_ERR_NO_MEMORY : xhci_status_from_cc(cc);
    if (!id || id > x->max_slots || x->slots[id]) {
        (void)cmd_simple(x, XHCI_TRB_DISABLE_SLOT, id, 0, 0, 0, &cc);
        return KSHIM_IO_ERR_PROTOCOL;
    }
    uint64_t bus;
    struct xhci_slot *slot = kshim_dma_alloc(sizeof(*slot), 64, 0, UINT64_MAX, &bus);
    if (!slot) goto nomem;
    slot->id = id;
    slot->device = dev;
    x->slots[id] = slot;
    slot->out_ctx = kshim_dma_alloc(out_ctx_size(x), 64, x->page_size, x->dma_limit, &slot->out_dma);
    slot->in_ctx = kshim_dma_alloc(in_ctx_size(x), 64, x->page_size, x->dma_limit, &slot->in_dma);
    if (!slot->out_ctx || !slot->in_ctx) goto nomem;
    struct xhci_ep *ep0 = &slot->eps[1];
    if (xhci_ring_init(x, &ep0->ring)) goto nomem;
    ep0->dci = 1;
    ep0->endpoint = &dev->ep0;
    ep0->enabled = true;
    dev->ep0.hcd_priv = ep0;
    dev->hcd_priv = slot;
    x->dcbaa[id] = slot->out_dma;
    kshim_dma_sync_for_device(&x->dcbaa[id], sizeof(uint64_t));
    /* TT hub: the nearest high-speed hub upstream of a LS/FS device. */
    dev->tt_hub_slot = 0;
    if (dev->tt_port) {
        for (struct kshim_usb_device *hub = dev->parent; hub; hub = hub->parent) {
            if (hub->speed != KSHIM_USB_SPEED_HIGH) continue;
            struct xhci_slot *hs = slot_of(hub);
            dev->tt_hub_slot = hs ? hs->id : 0;
            break;
        }
    }
    return 0;

nomem:
    (void)cmd_simple(x, XHCI_TRB_DISABLE_SLOT, id, 0, 0, 0, &cc);
    if (slot) {
        slot->device = NULL;
        xhci_slot_release(x, slot, KSHIM_IO_ERR_CANCELLED);
    }
    x->slots[id] = NULL;
    dev->hcd_priv = NULL;
    dev->ep0.hcd_priv = NULL;
    return KSHIM_IO_ERR_NO_MEMORY;
}

static void fill_slot_ctx(struct xhci *x, struct xhci_slot *slot, const struct kshim_usb_device *dev,
                          unsigned entries)
{
    uint32_t *s = in_slot(x, slot);
    s[0] = (dev->route & 0xfffffU) | speed_id(dev->speed) << 20 | (uint32_t)entries << 27;
    s[1] = (uint32_t)dev->root_port << 16;
    s[2] = 0;
    if (dev->tt_hub_slot && (dev->speed == KSHIM_USB_SPEED_LOW || dev->speed == KSHIM_USB_SPEED_FULL))
        s[2] = dev->tt_hub_slot | (uint32_t)dev->tt_port << 8;
    s[3] = 0;
}

static int op_device_address(struct kshim_usb_hcd *hcd, struct kshim_usb_device *dev)
{
    struct xhci *x = xhci_from_hcd(hcd);
    struct xhci_slot *slot = slot_of(dev);
    if (!slot) return ERROR_INVALID_PARAMETER;
    in_ctx_reset(x, slot);
    in_control(x, slot)[1] = 1U << 0 | 1U << 1; /* A0 (slot), A1 (EP0) */
    fill_slot_ctx(x, slot, dev, 1);
    fill_ep_ctx(x, in_ep(x, slot, 1), dev, &dev->ep0, &slot->eps[1].ring);
    in_ctx_publish(x, slot);
    unsigned cc = 0;
    int status = cmd_simple(x, XHCI_TRB_ADDRESS_DEVICE, slot->id, 0, slot->in_dma, 0, &cc);
    if (status) return status;
    if (cc != XHCI_CC_SUCCESS)
        return cc == XHCI_CC_TRANSACTION ? KSHIM_IO_ERR_NO_DEVICE : xhci_status_from_cc(cc);
    out_ctx_fetch(x, slot);
    dev->address = (uint8_t)(out_slot(x, slot)[3] & 0xffU);
    return 0;
}

static int op_device_update_ep0(struct kshim_usb_hcd *hcd, struct kshim_usb_device *dev)
{
    struct xhci *x = xhci_from_hcd(hcd);
    struct xhci_slot *slot = slot_of(dev);
    if (!slot) return ERROR_INVALID_PARAMETER;
    in_ctx_reset(x, slot);
    in_control(x, slot)[1] = 1U << 1; /* A1: EP0 Max Packet Size (4.6.7) */
    fill_ep_ctx(x, in_ep(x, slot, 1), dev, &dev->ep0, &slot->eps[1].ring);
    in_ctx_publish(x, slot);
    unsigned cc = 0;
    int status = cmd_simple(x, XHCI_TRB_EVALUATE_CTX, slot->id, 0, slot->in_dma, 0, &cc);
    if (status) return status;
    return cc == XHCI_CC_SUCCESS ? 0 : xhci_status_from_cc(cc);
}

static void copy_slot_ctx(struct xhci *x, struct xhci_slot *slot)
{
    out_ctx_fetch(x, slot);
    memcpy(in_slot(x, slot), out_slot(x, slot), 12);
    in_slot(x, slot)[3] = 0;
}

static int op_device_configure(struct kshim_usb_hcd *hcd, struct kshim_usb_device *dev)
{
    struct xhci *x = xhci_from_hcd(hcd);
    struct xhci_slot *slot = slot_of(dev);
    if (!slot) return ERROR_INVALID_PARAMETER;
    in_ctx_reset(x, slot);
    uint32_t add = 1U << 0, drop = 0;
    unsigned max_dci = 1;
    /* Reconfiguration drops what an earlier configuration enabled. */
    for (unsigned d = 2; d < 32; ++d) {
        if (!slot->eps[d].enabled) continue;
        drop |= 1U << d;
    }
    struct xhci_ep *added[32] = {0};
    for (unsigned i = 0; i < dev->interface_count; ++i) {
        struct kshim_usb_interface *intf = &dev->interfaces[i];
        for (unsigned j = 0; j < intf->endpoint_count; ++j) {
            struct kshim_usb_endpoint *e = &intf->endpoints[j];
            if (e->type == KSHIM_USB_XFER_ISOC || e->type == KSHIM_USB_XFER_CONTROL) continue;
            unsigned dci = ep_dci(e);
            if (dci < 2 || dci > 31 || added[dci]) continue;
            struct xhci_ep *ep = &slot->eps[dci];
            if (ep->enabled) {
                /* Dropped above and re-added: keep a fresh ring. */
                xhci_ep_flush(x, ep, KSHIM_IO_ERR_CANCELLED);
                xhci_ring_free(&ep->ring);
                ep->enabled = false;
            }
            if (xhci_ring_init(x, &ep->ring)) {
                for (unsigned k = 2; k < 32; ++k)
                    if (added[k]) xhci_ring_free(&added[k]->ring);
                return KSHIM_IO_ERR_NO_MEMORY;
            }
            ep->dci = (uint8_t)dci;
            ep->endpoint = e;
            added[dci] = ep;
            add |= 1U << dci;
            if (dci > max_dci) max_dci = dci;
            fill_ep_ctx(x, in_ep(x, slot, dci), dev, e, &ep->ring);
        }
    }
    for (unsigned d = 2; d < 32; ++d)
        if ((drop >> d) & 1U && !added[d]) {
            struct xhci_ep *ep = &slot->eps[d];
            xhci_ep_flush(x, ep, KSHIM_IO_ERR_CANCELLED);
            if (ep->endpoint) ep->endpoint->hcd_priv = NULL;
            xhci_ring_free(&ep->ring);
            ep->enabled = false;
        }
    in_control(x, slot)[0] = drop;
    in_control(x, slot)[1] = add;
    in_control(x, slot)[7] = dev->config_value;
    copy_slot_ctx(x, slot);
    uint32_t *s = in_slot(x, slot);
    s[0] = (s[0] & ~(0x1fU << 27)) | (uint32_t)max_dci << 27;
    in_ctx_publish(x, slot);
    unsigned cc = 0;
    int status = cmd_simple(x, XHCI_TRB_CONFIGURE_EP, slot->id, 0, slot->in_dma, 0, &cc);
    if (!status && cc != XHCI_CC_SUCCESS) status = xhci_status_from_cc(cc);
    for (unsigned d = 2; d < 32; ++d) {
        struct xhci_ep *ep = added[d];
        if (!ep) continue;
        if (status) {
            xhci_ring_free(&ep->ring);
            ep->endpoint = NULL;
            continue;
        }
        ep->enabled = true;
        ep->needs_reset = false;
        ep->endpoint->hcd_priv = ep;
        ep->endpoint->halted = false;
    }
    return status;
}

static int op_hub_configure(struct kshim_usb_hcd *hcd, struct kshim_usb_device *dev, uint8_t ports,
                            uint8_t tt_think_time, bool multi_tt)
{
    struct xhci *x = xhci_from_hcd(hcd);
    struct xhci_slot *slot = slot_of(dev);
    if (!slot) return ERROR_INVALID_PARAMETER;
    in_ctx_reset(x, slot);
    in_control(x, slot)[1] = 1U << 0; /* A0: slot context only */
    copy_slot_ctx(x, slot);
    uint32_t *s = in_slot(x, slot);
    s[0] |= 1U << 26 | (multi_tt && dev->speed == KSHIM_USB_SPEED_HIGH ? 1U << 25 : 0); /* Hub, MTT */
    s[1] = (s[1] & 0x00ffffffU) | (uint32_t)ports << 24;
    if (dev->speed == KSHIM_USB_SPEED_HIGH) s[2] = (s[2] & ~(3U << 16)) | (uint32_t)(tt_think_time & 3U) << 16;
    in_ctx_publish(x, slot);
    unsigned cc = 0;
    int status = cmd_simple(x, XHCI_TRB_CONFIGURE_EP, slot->id, 0, slot->in_dma, 0, &cc);
    if (status) return status;
    return cc == XHCI_CC_SUCCESS ? 0 : xhci_status_from_cc(cc);
}

static int op_endpoint_reset(struct kshim_usb_hcd *hcd, struct kshim_usb_device *dev,
                             struct kshim_usb_endpoint *endpoint)
{
    struct xhci *x = xhci_from_hcd(hcd);
    struct xhci_slot *slot = slot_of(dev);
    struct xhci_ep *ep = endpoint ? endpoint->hcd_priv : NULL;
    if (!slot || !ep || !ep->enabled) return ERROR_INVALID_PARAMETER;
    return ep_recover(x, slot, ep);
}

static void op_device_free(struct kshim_usb_hcd *hcd, struct kshim_usb_device *dev)
{
    struct xhci *x = xhci_from_hcd(hcd);
    struct xhci_slot *slot = slot_of(dev);
    if (!slot) return;
    /* Disable Slot stops every endpoint before the buffers are released. */
    if (x->running && !x->dead) {
        unsigned cc = 0;
        (void)cmd_simple(x, XHCI_TRB_DISABLE_SLOT, slot->id, 0, 0, 0, &cc);
    }
    xhci_slot_release(x, slot, KSHIM_IO_ERR_CANCELLED);
    dev->ep0.hcd_priv = NULL;
}

/* ---- Submission (4.11) ----------------------------------------------------- */

/* Number of TRBs for a buffer: one per 64 KiB-bounded chunk, at least one. */
static unsigned data_trbs(uint64_t dma, uint32_t length)
{
    if (!length) return 1;
    unsigned n = 0;
    while (length) {
        uint32_t chunk = XHCI_TRB_MAX_BYTES - (uint32_t)(dma & (XHCI_TRB_MAX_BYTES - 1U));
        if (chunk > length) chunk = length;
        dma += chunk;
        length -= chunk;
        ++n;
    }
    return n;
}

/* Queue the data TRBs of one TD. type is DATA or NORMAL for the first. */
static unsigned put_data(struct xhci_ring *ring, uint32_t first_type, bool in, uint64_t dma,
                         uint32_t length, uint16_t mps, uint32_t last_flags, bool *owned_first,
                         unsigned *first_index)
{
    unsigned index = 0;
    uint32_t done = 0;
    bool first = true;
    do {
        uint32_t chunk = XHCI_TRB_MAX_BYTES - (uint32_t)(dma & (XHCI_TRB_MAX_BYTES - 1U));
        if (chunk > length - done) chunk = length - done;
        bool last = done + chunk >= length;
        uint32_t remaining = length - done - chunk;
        uint32_t packets = last || !mps ? 0 : (remaining + mps - 1U) / mps;
        struct xhci_trb t = {
            (uint32_t)dma, (uint32_t)(dma >> 32),
            XHCI_TRB_LEN(chunk) | XHCI_TRB_TD_SIZE(packets),
            XHCI_TRB_TYPE(first ? first_type : XHCI_TRB_NORMAL) | (in ? XHCI_TRB_ISP : 0) |
                (last ? last_flags : XHCI_TRB_CH) |
                (first && first_type == XHCI_TRB_DATA && in ? XHCI_TRB_DIR_IN : 0),
        };
        bool own = !(first && owned_first && !*owned_first);
        index = xhci_ring_put(ring, &t, !last, own);
        if (first && first_index) *first_index = index;
        if (first && owned_first && !*owned_first) *owned_first = true;
        first = false;
        dma += chunk;
        done += chunk;
    } while (done < length);
    return index;
}

static int op_submit(struct kshim_usb_hcd *hcd, struct kshim_usb_urb *urb)
{
    struct xhci *x = xhci_from_hcd(hcd);
    struct kshim_usb_device *dev = urb->device;
    struct xhci_slot *slot = slot_of(dev);
    struct xhci_ep *ep = urb->endpoint->hcd_priv;
    if (x->dead || !x->running) return ERROR_DEVICE_ERROR;
    if (!slot || !ep || !ep->enabled) return ERROR_NOT_READY;
    if (ep->needs_reset) {
        int status = ep_recover(x, slot, ep);
        if (status) return status;
    }
    void *buffer = urb->bounce ? urb->bounce : urb->buffer;
    uint64_t dma = (uint64_t)(uintptr_t)buffer;
    uint32_t length = urb->length;
    if (length && (dma + length - 1U > x->dma_limit)) return ERROR_NOT_SUPPORTED;
    bool control = urb->endpoint->type == KSHIM_USB_XFER_CONTROL;
    bool in = urb->in;
    bool zlp = !control && !in && (urb->flags & KSHIM_USB_URB_ZERO_PACKET) && length &&
               urb->endpoint->max_packet && !(length % urb->endpoint->max_packet);
    unsigned need = control ? 2U + (length ? data_trbs(dma, length) : 0U)
                            : data_trbs(dma, length) + (zlp ? 1U : 0U);
    if (xhci_ring_free_trbs(&ep->ring) < need) return ERROR_BUSY;
    struct xhci_td *td = td_alloc(x);
    if (!td) return ERROR_BUSY;
    if (length) kshim_dma_sync_for_device(buffer, length);

    struct xhci_ring *ring = &ep->ring;
    td->ep = ep;
    td->first_cycle = ring->cycle;
    td->first = ring->enqueue;
    td->data_trb = UINT32_MAX;
    td->data_len = length;
    unsigned used_before = ring->used;
    bool owned_first = false;
    if (control) {
        /* Setup Stage: the 8 setup bytes as immediate data (6.4.1.2.1). */
        const struct kshim_usb_setup *s = &urb->setup;
        uint32_t trt = !length ? XHCI_TRB_TRT_NONE : in ? XHCI_TRB_TRT_IN : XHCI_TRB_TRT_OUT;
        struct xhci_trb setup = {
            (uint32_t)s->bmRequestType | (uint32_t)s->bRequest << 8 | (uint32_t)s->wValue << 16,
            (uint32_t)s->wIndex | (uint32_t)s->wLength << 16,
            XHCI_TRB_LEN(XHCI_SETUP_LEN),
            XHCI_TRB_TYPE(XHCI_TRB_SETUP) | XHCI_TRB_IDT | trt,
        };
        (void)xhci_ring_put(ring, &setup, false, false);
        owned_first = true;
        if (length) {
            unsigned first_data = 0;
            (void)put_data(ring, XHCI_TRB_DATA, in, dma, length, urb->endpoint->max_packet, 0,
                           NULL, &first_data);
            td->data_trb = first_data;
        }
        /* Status Stage: opposite direction of the data, IN without data. */
        struct xhci_trb status = {
            0, 0, 0,
            XHCI_TRB_TYPE(XHCI_TRB_STATUS) | XHCI_TRB_IOC |
                ((!length || !in) ? XHCI_TRB_DIR_IN : 0),
        };
        td->last = xhci_ring_put(ring, &status, false, true);
    } else {
        unsigned first_data = 0;
        td->last = put_data(ring, XHCI_TRB_NORMAL, in, dma, length, urb->endpoint->max_packet,
                            zlp ? 0 : XHCI_TRB_IOC, &owned_first, &first_data);
        td->data_trb = first_data;
        if (zlp) {
            struct xhci_trb t = {0, 0, 0, XHCI_TRB_TYPE(XHCI_TRB_NORMAL) | XHCI_TRB_IOC};
            td->last = xhci_ring_put(ring, &t, false, true);
        }
    }
    td->count = ring->used - used_before;
    td->next = NULL;
    urb->hcd_priv = td;
    if (ep->tail) ((struct xhci_td *)ep->tail->hcd_priv)->next = urb;
    else ep->head = urb;
    ep->tail = urb;
    /* Publish the TD, then give the controller its first TRB (4.11.1). */
    kshim_dma_sync_for_device(ring->trbs, XHCI_RING_TRBS * sizeof(struct xhci_trb));
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    ring->trbs[td->first].control ^= XHCI_TRB_CYCLE;
    kshim_dma_sync_for_device(&ring->trbs[td->first], sizeof(struct xhci_trb));
    xhci_ring_doorbell(x, slot->id, ep->dci);
    return 0;
}

static int op_cancel(struct kshim_usb_hcd *hcd, struct kshim_usb_urb *urb)
{
    struct xhci *x = xhci_from_hcd(hcd);
    struct xhci_td *td = urb->hcd_priv;
    if (!td || !td->in_use) return 0;
    struct xhci_ep *ep = td->ep;
    struct xhci_slot *slot = slot_of(urb->device);
    if (!slot || !x->running || x->dead) {
        xhci_td_giveback(x, urb, KSHIM_IO_ERR_CANCELLED, td->transferred);
        return 0;
    }
    /* Stop the endpoint unless the controller already stopped it. */
    out_ctx_fetch(x, slot);
    uint32_t state = XHCI_EP_STATE(out_ep(x, slot, ep->dci)[0]);
    unsigned cc = 0;
    if (state == XHCI_EP_STATE_RUNNING) {
        int status = cmd_simple(x, XHCI_TRB_STOP_EP, slot->id, ep->dci, 0, 0, &cc);
        if (status) {
            /* Command path failed: the controller is halted (dead). */
            if (urb->hcd_priv) xhci_td_giveback(x, urb, KSHIM_IO_ERR_CANCELLED, td->transferred);
            return status;
        }
    }
    /* The URB may have completed while stopping. */
    if (urb->hcd_priv != td || !td->in_use) return 0;
    bool head = ep->head == urb;
    uint32_t transferred = td->transferred;
    if (!head) {
        /* Not the controller's current TD: turn it into No Ops (4.11.7). */
        unsigned i = td->first;
        for (;;) {
            struct xhci_trb *t = &ep->ring.trbs[i];
            t->control = (t->control & (XHCI_TRB_CYCLE | XHCI_TRB_CH)) | XHCI_TRB_TYPE(XHCI_TRB_NOOP);
            if (i == td->last) break;
            if (++i == XHCI_RING_TRBS - 1U) i = 0;
        }
        kshim_dma_sync_for_device(ep->ring.trbs, XHCI_RING_TRBS * sizeof(struct xhci_trb));
        xhci_td_giveback(x, urb, KSHIM_IO_ERR_CANCELLED, transferred);
    } else {
        ep_queue_remove(ep, urb);
        td->in_use = false;
        urb->hcd_priv = NULL;
        /* Skip the cancelled TD: dequeue to the next TD or the enqueue. */
        if (state != XHCI_EP_STATE_HALTED) (void)ep_set_dequeue(x, slot, ep);
        else ep->needs_reset = true;
        kshim_usb_hcd_giveback(urb, KSHIM_IO_ERR_CANCELLED, transferred);
    }
    if (ep->head && !ep->needs_reset) xhci_ring_doorbell(x, slot->id, ep->dci);
    return 0;
}

static int op_poll(struct kshim_usb_hcd *hcd)
{
    return xhci_process_events(xhci_from_hcd(hcd));
}

static int op_quiesce(struct kshim_usb_hcd *hcd)
{
    return xhci_stop(xhci_from_hcd(hcd));
}

const struct kshim_usb_hcd_ops xhci_hcd_ops = {
    .port_status = op_port_status,
    .port_clear_change = op_port_clear_change,
    .port_reset = op_port_reset,
    .port_disable = op_port_disable,
    .device_alloc = op_device_alloc,
    .device_address = op_device_address,
    .device_update_ep0 = op_device_update_ep0,
    .device_configure = op_device_configure,
    .hub_configure = op_hub_configure,
    .endpoint_reset = op_endpoint_reset,
    .device_free = op_device_free,
    .submit = op_submit,
    .cancel = op_cancel,
    .poll = op_poll,
    .quiesce = op_quiesce,
};
