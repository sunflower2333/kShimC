/* SPDX-License-Identifier: MIT */
/*
 * Behavioural xHCI model for host tests (see xhci_model.h). Register
 * offsets, TRB/context layouts and completion codes follow xHCI 1.2; the
 * devices answer USB 2.0 chapter 9 and HID 1.11 requests.
 */
#include "xhci_model.h"

#include <assert.h>
#include <string.h>

struct xm Xm;

#define CAPLEN 0x20U
#define OP     0x20U
#define RT     0x600U
#define DB     0x800U
#define XECP   0x900U

#define CMD_RS    (1U << 0)
#define CMD_HCRST (1U << 1)
#define CMD_INTE  (1U << 2)
#define STS_HCH   (1U << 0)
#define STS_EINT  (1U << 3)
#define STS_PCD   (1U << 4)
#define IMAN_IP   (1U << 0)
#define IMAN_IE   (1U << 1)
#define ERDP_EHB  (1U << 3)

#define P_CCS (1U << 0)
#define P_PED (1U << 1)
#define P_PR  (1U << 4)
#define P_PP  (1U << 9)
#define P_CSC (1U << 17)
#define P_PEC (1U << 18)
#define P_WRC (1U << 19)
#define P_OCC (1U << 20)
#define P_PRC (1U << 21)
#define P_PLC (1U << 22)
#define P_CEC (1U << 23)
#define P_WPR (1U << 31)
#define P_CHANGES (P_CSC | P_PEC | P_WRC | P_OCC | P_PRC | P_PLC | P_CEC)

#define T_CYCLE (1U << 0)
#define T_TC    (1U << 1)
#define T_ISP   (1U << 2)
#define T_CH    (1U << 4)
#define T_IOC   (1U << 5)
#define T_IDT   (1U << 6)
#define T_TYPE(c) (((c) >> 10) & 0x3fU)

enum { CC_SUCCESS = 1, CC_TRANSACTION = 4, CC_TRB = 5, CC_STALL = 6, CC_NO_SLOTS = 9,
       CC_SLOT_NOT_ENABLED = 11, CC_SHORT = 13, CC_PARAMETER = 17, CC_CONTEXT_STATE = 19,
       CC_STOPPED = 26 };
enum { EP_DISABLED = 0, EP_RUNNING = 1, EP_HALTED = 2, EP_STOPPED = 3 };

static void *ptr(uint64_t a) { return (void *)(uintptr_t)a; }
static uint32_t *trb(uint64_t a) { return ptr(a); }
static bool usb3(unsigned port) { return port >= 3; }

/* ---- Events and the interrupt line ---------------------------------------- */

static void raise_irq(void)
{
    if (Xm.erdp_lo & ERDP_EHB) return; /* the handler has not consumed yet */
    Xm.iman |= IMAN_IP;
    Xm.erdp_lo |= ERDP_EHB;
    Xm.usbsts |= STS_EINT;
    if ((Xm.iman & IMAN_IE) && (Xm.usbcmd & CMD_INTE)) Xm.irq_due = true;
}

static void post(uint32_t p0, uint32_t p1, uint32_t status, uint32_t control)
{
    if (!(Xm.usbcmd & CMD_RS) || !Xm.ev_base) return;
    uint32_t *e = trb(Xm.ev_base + 16ULL * Xm.ev_enq);
    e[0] = p0;
    e[1] = p1;
    e[2] = status;
    e[3] = (control & ~T_CYCLE) | Xm.ev_cycle;
    if (++Xm.ev_enq == Xm.ev_size) {
        Xm.ev_enq = 0;
        Xm.ev_cycle ^= 1U;
    }
    ++Xm.ev_posted;
    raise_irq();
}

static void post_transfer(uint64_t at, unsigned cc, uint32_t residual, unsigned slot, unsigned dci)
{
    post((uint32_t)at, (uint32_t)(at >> 32), cc << 24 | (residual & 0xffffffU),
         32U << 10 | dci << 16 | slot << 24);
}

static void post_port(unsigned port)
{
    post(port << 24, 0, CC_SUCCESS << 24, 34U << 10);
    Xm.usbsts |= STS_PCD;
}

static void deliver(void)
{
    if (Xm.depth || !Xm.irq_due) return;
    Xm.irq_due = false;
    ++Xm.irqs_raised;
    if (Xm.irq) Xm.irq(Xm.irq_context);
}

/* ---- Contexts --------------------------------------------------------------- */

static uint32_t *out_ctx(unsigned slot, unsigned index)
{
    uint64_t *dcbaa = ptr((uint64_t)Xm.dcbaap_hi << 32 | Xm.dcbaap_lo);
    assert(dcbaa && dcbaa[slot]);
    return (uint32_t *)((uint8_t *)ptr(dcbaa[slot]) + index * Xm.ctx_size);
}

static void set_state(unsigned slot, unsigned dci, uint8_t state)
{
    Xm.slots[slot].ep[dci].state = state;
    uint32_t *c = out_ctx(slot, dci);
    c[0] = (c[0] & ~7U) | state;
}

/* ---- Ports -------------------------------------------------------------------- */

static uint32_t port_idle(unsigned port)
{
    const struct xm_dev *d = &Xm.devs[port];
    uint32_t v = P_PP;
    if (!d->present) return v | (usb3(port) ? 5U << 5 : 0); /* RxDetect */
    v |= P_CCS | P_CSC | (uint32_t)d->speed << 10;
    if (usb3(port)) v |= P_PED; /* SuperSpeed trains to U0 by itself */
    return v;
}

void xm_attach(unsigned port, const struct xm_dev *dev)
{
    Xm.devs[port] = *dev;
    Xm.devs[port].present = true;
    Xm.portsc[port] = port_idle(port);
    post_port(port);
    deliver();
}

void xm_detach(unsigned port)
{
    Xm.devs[port].present = false;
    Xm.portsc[port] = (Xm.portsc[port] & ~(P_CCS | P_PED)) | P_CSC;
    post_port(port);
    deliver();
}

struct xm_slot *xm_slot_of_port(unsigned port)
{
    for (unsigned s = 1; s <= XM_SLOTS; ++s)
        if (Xm.slots[s].enabled && Xm.slots[s].addressed && Xm.slots[s].port == port) return &Xm.slots[s];
    return NULL;
}

static void port_write(unsigned port, uint32_t v)
{
    uint32_t old = Xm.portsc[port], nv = old;
    nv &= ~(v & P_CHANGES);
    if ((v & P_PED) && !usb3(port)) nv &= ~P_PED;
    bool reset = false;
    if ((v & P_PR) && (nv & P_CCS)) {
        nv |= P_PED | P_PRC;
        reset = true;
    }
    if ((v & P_WPR) && usb3(port) && (nv & P_CCS)) {
        nv |= P_PED | P_PRC | P_WRC;
        reset = true;
    }
    Xm.portsc[port] = nv;
    if (reset) post_port(port);
}

/* ---- Reset / run ----------------------------------------------------------------- */

static void hc_reset(void)
{
    ++Xm.hcrst_count;
    Xm.hcrst_seq = ++Xm.seq;
    Xm.usbcmd = 0;
    Xm.usbsts = STS_HCH;
    Xm.dnctrl = Xm.crcr_lo = Xm.crcr_hi = Xm.dcbaap_lo = Xm.dcbaap_hi = Xm.config = 0;
    Xm.iman = Xm.imod = Xm.erstsz = Xm.erstba_lo = Xm.erstba_hi = Xm.erdp_lo = Xm.erdp_hi = 0;
    Xm.cmd_deq = 0;
    Xm.cmd_cycle = 0;
    Xm.ev_base = 0;
    Xm.ev_enq = Xm.ev_size = 0;
    memset(Xm.slots, 0, sizeof(Xm.slots));
    for (unsigned p = 1; p <= XM_PORTS; ++p) Xm.portsc[p] = port_idle(p);
}

void xm_init(unsigned ctx_size, unsigned scratchpads, bool running)
{
    memset(&Xm, 0, sizeof(Xm));
    Xm.ctx_size = ctx_size;
    Xm.scratchpads = scratchpads;
    Xm.legsup = 1U << 16; /* BIOS owned */
    Xm.legctl = 0x00000011U | 0x20000000U; /* SMI enables + a latched event */
    Xm.bios_releases = true;
    hc_reset();
    Xm.hcrst_count = 0;
    Xm.hcrst_seq = 0;
    if (running) {
        /* The previous firmware left it running with its own rings. */
        Xm.usbcmd = CMD_RS | CMD_INTE;
        Xm.usbsts = 0;
        Xm.dcbaap_lo = 0xdead0000U;
        Xm.config = 4;
    }
}

/* ---- Devices (chapter 9 / HID) ---------------------------------------------------- */

static bool request_stalls(const struct xm_dev *d, uint8_t bm, uint8_t req, uint16_t value)
{
    if (bm == 0x80 && req == 6) return !((value >> 8) == 1 || (value >> 8) == 2);
    if (bm == 0x81 && req == 6) return (value >> 8) != 0x22 || !d->report_desc;
    if (bm == 0x80 && req == 0) return false;
    if (bm == 0x00 && req == 9) return false;
    if (bm == 0x02 && req == 1) return false;
    if (bm == 0x21 && req == 0x0a) return d->stall_set_idle;
    return true;
}

static uint32_t control_in(struct xm_dev *d, const struct xm_ep *ep, uint8_t *buf, uint32_t len)
{
    const uint8_t *src = NULL;
    size_t avail = 0;
    static const uint8_t status[2] = {0, 0};
    uint8_t type = (uint8_t)(ep->setup.value >> 8);
    if (ep->setup.req == 6 && type == 1) { src = d->dev_desc; avail = d->dev_len; }
    else if (ep->setup.req == 6 && type == 2) { src = d->cfg_desc; avail = d->cfg_len; }
    else if (ep->setup.req == 6 && type == 0x22) { src = d->report_desc; avail = d->report_len; }
    else if (ep->setup.req == 0) { src = status; avail = 2; }
    if (avail > ep->setup.length) avail = ep->setup.length;
    if (avail > len) avail = len;
    if (src && avail) memcpy(buf, src, avail);
    return (uint32_t)avail;
}

static void control_out_done(struct xm_dev *d, const struct xm_ep *ep)
{
    if (ep->setup.bm == 0x00 && ep->setup.req == 9) d->config = (uint8_t)ep->setup.value;
    if (ep->setup.bm == 0x02 && ep->setup.req == 1) ++d->clear_halts;
}

/* Advance past the rest of a chained TD after a short packet. */
static void skip_td(struct xm_ep *ep, uint32_t control)
{
    while (control & T_CH) {
        uint32_t *t = trb(ep->deq);
        if (T_TYPE(t[3]) == 6) {
            ep->deq = ((uint64_t)t[1] << 32 | t[0]) & ~0xfULL;
            if (t[3] & T_TC) ep->cycle ^= 1U;
            continue;
        }
        control = t[3];
        ep->deq += 16;
    }
}

static void run_ep(unsigned s, unsigned dci)
{
    struct xm_slot *slot = &Xm.slots[s];
    struct xm_ep *ep = &slot->ep[dci];
    struct xm_dev *d = &Xm.devs[slot->port];
    for (unsigned guard = 0; guard < 4096; ++guard) {
        if (ep->state != EP_RUNNING || !d->present) return;
        uint32_t *t = trb(ep->deq);
        if ((t[3] & T_CYCLE) != ep->cycle) return;
        uint32_t c = t[3], len = t[2] & 0x1ffffU;
        uint64_t here = ep->deq, buf = (uint64_t)t[1] << 32 | t[0];
        unsigned type = T_TYPE(c);
        if (type == 6) { /* Link */
            ep->deq = buf & ~0xfULL;
            if (c & T_TC) ep->cycle ^= 1U;
            continue;
        }
        if (type == 8) { /* No Op */
            ep->deq += 16;
            if (c & T_IOC) post_transfer(here, CC_SUCCESS, 0, s, dci);
            continue;
        }
        if (ep->type == 4) { /* control */
            if (type == 2) {
                assert((c & T_IDT) && len == 8);
                ep->setup.bm = (uint8_t)t[0];
                ep->setup.req = (uint8_t)(t[0] >> 8);
                ep->setup.value = (uint16_t)(t[0] >> 16);
                ep->setup.index = (uint16_t)t[1];
                ep->setup.length = (uint16_t)(t[1] >> 16);
                ep->stall_td = request_stalls(d, ep->setup.bm, ep->setup.req, ep->setup.value);
                ep->deq += 16;
                continue;
            }
            if (ep->stall_td) {
                post_transfer(here, CC_STALL, len, s, dci);
                set_state(s, dci, EP_HALTED);
                return;
            }
            if (type == 3 || type == 1) {
                bool in = (ep->setup.bm & 0x80) != 0;
                uint32_t n = in ? control_in(d, ep, ptr(buf), len) : len;
                ep->deq += 16;
                if (n < len) {
                    if (c & T_ISP) post_transfer(here, CC_SHORT, len - n, s, dci);
                    skip_td(ep, c);
                } else if (c & T_IOC) {
                    post_transfer(here, CC_SUCCESS, 0, s, dci);
                }
                continue;
            }
            if (type == 4) {
                control_out_done(d, ep);
                ep->deq += 16;
                if (c & T_IOC) post_transfer(here, CC_SUCCESS, 0, s, dci);
                continue;
            }
            post_transfer(here, CC_TRB, 0, s, dci);
            set_state(s, dci, EP_HALTED);
            return;
        }
        assert(type == 1);
        uint32_t n = len;
        if (ep->type == 7) { /* interrupt IN */
            if (!d->report_ready) return; /* NAK: the TD stays pending */
            n = len < 8 ? len : 8;
            memcpy(ptr(buf), d->report, n);
            d->report_ready = false;
            ++d->reports_sent;
        } else if (ep->type == 2) { /* bulk OUT */
            n = len < sizeof(d->bulk) ? len : (uint32_t)sizeof(d->bulk);
            memcpy(d->bulk, ptr(buf), n);
            d->bulk_len = n;
        } else if (ep->type == 6) { /* bulk IN: echo */
            if (!d->bulk_len) return;
            n = len < d->bulk_len ? len : d->bulk_len;
            memcpy(ptr(buf), d->bulk, n);
            d->bulk_len = 0;
        }
        ep->deq += 16;
        if (n < len) {
            if (c & T_ISP) post_transfer(here, CC_SHORT, len - n, s, dci);
            skip_td(ep, c);
        } else if (c & T_IOC) {
            post_transfer(here, CC_SUCCESS, 0, s, dci);
        }
    }
}

void xm_report(unsigned port, const uint8_t report[8])
{
    struct xm_dev *d = &Xm.devs[port];
    memcpy(d->report, report, 8);
    d->report_ready = true;
    struct xm_slot *slot = xm_slot_of_port(port);
    if (slot)
        for (unsigned dci = 2; dci < 32; ++dci)
            if (slot->ep[dci].enabled && slot->ep[dci].type == 7) run_ep((unsigned)(slot - Xm.slots), dci);
    deliver();
}

/* ---- Commands ------------------------------------------------------------------------- */

static unsigned exec_command(unsigned type, const uint32_t *t, uint8_t *slot_out)
{
    uint64_t param = (uint64_t)t[1] << 32 | t[0];
    unsigned s = t[3] >> 24, dci = (t[3] >> 16) & 0x1fU;
    struct xm_slot *slot = s && s <= XM_SLOTS ? &Xm.slots[s] : NULL;
    *slot_out = (uint8_t)s;
    unsigned cs = Xm.ctx_size / 4U;
    switch (type) {
    case 9: /* Enable Slot */
        for (unsigned i = 1; i <= XM_SLOTS && i <= (Xm.config & 0xffU); ++i) {
            if (Xm.slots[i].enabled) continue;
            memset(&Xm.slots[i], 0, sizeof(Xm.slots[i]));
            Xm.slots[i].enabled = true;
            *slot_out = (uint8_t)i;
            return CC_SUCCESS;
        }
        *slot_out = 0;
        return CC_NO_SLOTS;
    case 10: /* Disable Slot */
        if (!slot || !slot->enabled) return CC_SLOT_NOT_ENABLED;
        memset(slot, 0, sizeof(*slot));
        return CC_SUCCESS;
    case 11: { /* Address Device */
        if (!slot || !slot->enabled) return CC_SLOT_NOT_ENABLED;
        uint32_t *in = ptr(param), *sc = in + cs, *e0 = in + 2 * cs;
        if ((in[1] & 3U) != 3U) return CC_PARAMETER;
        unsigned port = (sc[1] >> 16) & 0xffU, speed = (sc[0] >> 20) & 0xfU;
        if (!port || port > XM_PORTS || !Xm.devs[port].present) return CC_TRANSACTION;
        if (speed != Xm.devs[port].speed || ((e0[1] >> 3) & 7U) != 4 || ((e0[1] >> 1) & 3U) != 3)
            return CC_PARAMETER;
        slot->port = (uint8_t)port;
        slot->speed = (uint8_t)speed;
        slot->route = sc[0] & 0xfffffU;
        slot->address = (uint8_t)s;
        slot->addressed = true;
        struct xm_ep *ep = &slot->ep[1];
        memset(ep, 0, sizeof(*ep));
        ep->enabled = true;
        ep->type = 4;
        ep->mps = (uint16_t)(e0[1] >> 16);
        ep->deq = ((uint64_t)e0[3] << 32 | e0[2]) & ~0xfULL;
        ep->cycle = e0[2] & 1U;
        memcpy(out_ctx(s, 0), sc, 16);
        out_ctx(s, 0)[3] = slot->address | 2U << 27;
        memcpy(out_ctx(s, 1), e0, 20);
        set_state(s, 1, EP_RUNNING);
        return CC_SUCCESS;
    }
    case 13: { /* Evaluate Context */
        if (!slot || !slot->addressed) return CC_SLOT_NOT_ENABLED;
        uint32_t *in = ptr(param);
        if (in[1] & 2U) {
            slot->ep[1].mps = (uint16_t)(in[2 * cs + 1] >> 16);
            out_ctx(s, 1)[1] = (out_ctx(s, 1)[1] & 0xffffU) | (uint32_t)slot->ep[1].mps << 16;
        }
        return CC_SUCCESS;
    }
    case 12: { /* Configure Endpoint */
        if (!slot || !slot->addressed) return CC_SLOT_NOT_ENABLED;
        uint32_t *in = ptr(param);
        uint32_t drop = in[0], add = in[1];
        if ((drop & 3U) || (add & 2U)) return CC_PARAMETER;
        unsigned max_dci = 1;
        for (unsigned d = 2; d < 32; ++d)
            if ((drop >> d) & 1U) {
                slot->ep[d].enabled = false;
                set_state(s, d, EP_DISABLED);
            }
        for (unsigned d = 2; d < 32; ++d) {
            if (!((add >> d) & 1U)) continue;
            uint32_t *e = in + (d + 1U) * cs;
            unsigned ty = (e[1] >> 3) & 7U;
            bool in_dir = d & 1U;
            if (!ty || ty == 4 || (in_dir != (ty >= 5))) return CC_PARAMETER;
            struct xm_ep *ep = &slot->ep[d];
            memset(ep, 0, sizeof(*ep));
            ep->enabled = true;
            ep->type = (uint8_t)ty;
            ep->mps = (uint16_t)(e[1] >> 16);
            ep->interval = (uint8_t)(e[0] >> 16);
            ep->max_burst = (uint8_t)(e[1] >> 8);
            ep->deq = ((uint64_t)e[3] << 32 | e[2]) & ~0xfULL;
            ep->cycle = e[2] & 1U;
            memcpy(out_ctx(s, d), e, 20);
            set_state(s, d, EP_RUNNING);
            if (d > max_dci) max_dci = d;
        }
        for (unsigned d = 2; d < 32; ++d)
            if (slot->ep[d].enabled && d > max_dci) max_dci = d;
        if (add & 1U) {
            uint32_t *sc = in + cs;
            if ((sc[0] >> 27) < max_dci) return CC_PARAMETER;
            slot->hub = (sc[0] >> 26) & 1U;
            slot->hub_ports = (uint8_t)(sc[1] >> 24);
            memcpy(out_ctx(s, 0), sc, 12);
        }
        return CC_SUCCESS;
    }
    case 14: /* Reset Endpoint */
        if (!slot || !slot->enabled || !dci || !slot->ep[dci].enabled) return CC_PARAMETER;
        if (slot->ep[dci].state != EP_HALTED) return CC_CONTEXT_STATE;
        set_state(s, dci, EP_STOPPED);
        return CC_SUCCESS;
    case 15: { /* Stop Endpoint */
        if (!slot || !slot->enabled || !dci || !slot->ep[dci].enabled) return CC_PARAMETER;
        struct xm_ep *ep = &slot->ep[dci];
        if (ep->state != EP_RUNNING) return CC_CONTEXT_STATE;
        uint32_t *cur = trb(ep->deq);
        if ((cur[3] & T_CYCLE) == ep->cycle && T_TYPE(cur[3]) != 6)
            post_transfer(ep->deq, CC_STOPPED, cur[2] & 0x1ffffU, s, dci);
        set_state(s, dci, EP_STOPPED);
        return CC_SUCCESS;
    }
    case 16: { /* Set TR Dequeue Pointer */
        if (!slot || !slot->enabled || !dci || !slot->ep[dci].enabled) return CC_PARAMETER;
        struct xm_ep *ep = &slot->ep[dci];
        if (ep->state != EP_STOPPED) return CC_CONTEXT_STATE;
        ep->deq = param & ~0xfULL;
        ep->cycle = (uint32_t)(param & 1U);
        return CC_SUCCESS;
    }
    case 23:
        return CC_SUCCESS;
    default:
        return CC_TRB;
    }
}

static void run_commands(void)
{
    for (unsigned guard = 0; guard < 1024 && Xm.cmd_deq; ++guard) {
        uint32_t *t = trb(Xm.cmd_deq);
        if ((t[3] & T_CYCLE) != Xm.cmd_cycle) return;
        unsigned type = T_TYPE(t[3]);
        if (type == 6) {
            Xm.cmd_deq = ((uint64_t)t[1] << 32 | t[0]) & ~0xfULL;
            if (t[3] & T_TC) Xm.cmd_cycle ^= 1U;
            continue;
        }
        if (Xm.ignore_commands) return;
        uint64_t here = Xm.cmd_deq;
        Xm.cmd_deq += 16;
        ++Xm.commands[type];
        uint8_t slot = 0;
        unsigned cc = exec_command(type, t, &slot);
        post((uint32_t)here, (uint32_t)(here >> 32), cc << 24, 33U << 10 | (uint32_t)slot << 24);
    }
}

static void doorbell(unsigned slot, uint32_t value)
{
    ++Xm.doorbells;
    if (!(Xm.usbcmd & CMD_RS)) return;
    if (!slot) {
        run_commands();
        return;
    }
    unsigned dci = value & 0xffU;
    if (slot > XM_SLOTS || !Xm.slots[slot].enabled || dci < 1 || dci > 31) return;
    struct xm_ep *ep = &Xm.slots[slot].ep[dci];
    if (!ep->enabled || ep->state == EP_HALTED) return;
    if (ep->state == EP_STOPPED) set_state(slot, dci, EP_RUNNING);
    run_ep(slot, dci);
}

/* ---- MMIO -------------------------------------------------------------------------------- */

uint32_t xm_read32(void *context, uintptr_t address)
{
    (void)context;
    uintptr_t off = address - XM_BASE;
    if (off < CAPLEN) {
        switch (off) {
        case 0x00: return CAPLEN | 0x0110U << 16;
        case 0x04: return XM_SLOTS | 1U << 8 | XM_PORTS << 24;
        case 0x08: return 1U << 4 | ((Xm.scratchpads >> 5) & 0x1fU) << 21 | (Xm.scratchpads & 0x1fU) << 27;
        case 0x10: return 1U | (Xm.ctx_size == 64 ? 4U : 0U) | (XECP / 4U) << 16;
        case 0x14: return DB;
        case 0x18: return RT;
        default: return 0;
        }
    }
    if (off >= OP && off < OP + 0x400) {
        switch (off - OP) {
        case 0x00: return Xm.usbcmd;
        case 0x04: return Xm.usbsts;
        case 0x08: return 1;
        case 0x14: return Xm.dnctrl;
        case 0x18: return 0; /* CRCR reads as 0 apart from CRR */
        case 0x1c: return 0;
        case 0x30: return Xm.dcbaap_lo;
        case 0x34: return Xm.dcbaap_hi;
        case 0x38: return Xm.config;
        default: return 0;
        }
    }
    if (off >= OP + 0x400 && off < OP + 0x400 + 0x10 * XM_PORTS) {
        unsigned port = (unsigned)((off - OP - 0x400) / 0x10) + 1U;
        return (off & 0xf) == 0 ? Xm.portsc[port] : 0;
    }
    if (off >= RT && off < RT + 0x40) {
        switch (off - RT) {
        case 0x20: return Xm.iman;
        case 0x24: return Xm.imod;
        case 0x28: return Xm.erstsz;
        case 0x30: return Xm.erstba_lo;
        case 0x34: return Xm.erstba_hi;
        case 0x38: return Xm.erdp_lo;
        case 0x3c: return Xm.erdp_hi;
        default: return 0;
        }
    }
    switch (off) {
    case XECP + 0x00: return 1U | (0x10U / 4U) << 8 | Xm.legsup;
    case XECP + 0x04: return Xm.legctl;
    case XECP + 0x10: return 2U | (0x10U / 4U) << 8 | 2U << 24;
    case XECP + 0x14: return 0x20425355U;
    case XECP + 0x18: return 1U | 2U << 8;
    case XECP + 0x20: return 2U | 3U << 24;
    case XECP + 0x24: return 0x20425355U;
    case XECP + 0x28: return 3U | 2U << 8;
    default: return 0;
    }
}

static void op_write(uintptr_t reg, uint32_t v)
{
    switch (reg) {
    case 0x00: {
        uint32_t old = Xm.usbcmd;
        if (v & CMD_HCRST) {
            /* Software must halt first (5.4.1.1). */
            assert(Xm.usbsts & STS_HCH);
            hc_reset();
            return;
        }
        Xm.usbcmd = v & (CMD_RS | CMD_INTE | (1U << 3));
        if (!(old & CMD_RS) && (v & CMD_RS)) {
            ++Xm.run_count;
            Xm.usbsts &= ~STS_HCH;
            uint64_t *dcbaa = ptr((uint64_t)Xm.dcbaap_hi << 32 | Xm.dcbaap_lo);
            if (Xm.scratchpads) {
                uint64_t *array = ptr(dcbaa[0]);
                for (unsigned i = 0; i < Xm.scratchpads; ++i) assert(array[i] && !(array[i] & 0xfffU));
                Xm.scratch_seen = Xm.scratchpads;
            }
        }
        if ((old & CMD_RS) && !(v & CMD_RS)) {
            ++Xm.stop_count;
            Xm.rs_clear_seq = ++Xm.seq;
            Xm.usbsts |= STS_HCH;
        }
        return;
    }
    case 0x04: Xm.usbsts &= ~(v & ((1U << 2) | STS_EINT | STS_PCD | (1U << 10))); return;
    case 0x14: Xm.dnctrl = v; return;
    case 0x18: Xm.crcr_lo = v; return;
    case 0x1c:
        Xm.crcr_hi = v;
        Xm.cmd_deq = ((uint64_t)Xm.crcr_hi << 32 | Xm.crcr_lo) & ~0x3fULL;
        Xm.cmd_cycle = Xm.crcr_lo & 1U;
        return;
    case 0x30: Xm.dcbaap_lo = v; return;
    case 0x34: Xm.dcbaap_hi = v; return;
    case 0x38: Xm.config = v; return;
    default: return;
    }
}

static void rt_write(uintptr_t reg, uint32_t v)
{
    switch (reg) {
    case 0x20:
        Xm.iman = (Xm.iman & ~(v & IMAN_IP)) & ~IMAN_IE;
        Xm.iman |= v & IMAN_IE;
        return;
    case 0x24: Xm.imod = v; return;
    case 0x28: Xm.erstsz = v; return;
    case 0x30: Xm.erstba_lo = v; return;
    case 0x34: {
        Xm.erstba_hi = v;
        uint32_t *erst = ptr((uint64_t)Xm.erstba_hi << 32 | Xm.erstba_lo);
        assert(Xm.erstsz == 1 && erst);
        Xm.ev_base = ((uint64_t)erst[1] << 32 | erst[0]) & ~0x3fULL;
        Xm.ev_size = erst[2] & 0xffffU;
        Xm.ev_enq = 0;
        Xm.ev_cycle = 1;
        return;
    }
    case 0x38:
        /* EHB is RW1C; the pointer and DESI are plain. */
        Xm.erdp_lo = (v & ~ERDP_EHB) | ((Xm.erdp_lo & ERDP_EHB) & ~(v & ERDP_EHB));
        return;
    case 0x3c: {
        Xm.erdp_hi = v;
        uint64_t deq = ((uint64_t)Xm.erdp_hi << 32 | Xm.erdp_lo) & ~0xfULL;
        if (Xm.ev_base && deq != Xm.ev_base + 16ULL * Xm.ev_enq) raise_irq();
        return;
    }
    default: return;
    }
}

void xm_write32(void *context, uintptr_t address, uint32_t value)
{
    (void)context;
    uintptr_t off = address - XM_BASE;
    ++Xm.depth;
    if (off >= OP + 0x400 && off < OP + 0x400 + 0x10 * XM_PORTS) {
        if ((off & 0xf) == 0) port_write((unsigned)((off - OP - 0x400) / 0x10) + 1U, value);
    } else if (off >= OP && off < OP + 0x400) {
        op_write(off - OP, value);
    } else if (off >= RT && off < RT + 0x40) {
        rt_write(off - RT, value);
    } else if (off >= DB && off < DB + 4 * (XM_SLOTS + 1)) {
        doorbell((unsigned)(off - DB) / 4U, value);
    } else if (off == XECP) {
        Xm.legsup = value & ((1U << 16) | (1U << 24));
        if (Xm.bios_releases && (Xm.legsup & (1U << 24))) Xm.legsup &= ~(1U << 16);
    } else if (off == XECP + 4) {
        Xm.legctl = (value & ~0xe0000000U) | (Xm.legctl & 0xe0000000U & ~value);
    }
    --Xm.depth;
    deliver();
}
