/* SPDX-License-Identifier: MIT */
/*
 * xHCI driver + USB core against the behavioural controller model
 * (xhci_model.c). One scenario per process (ctest runs each):
 *   irq       halted controller, interrupter 0 interrupt-driven, scratchpads,
 *             BIOS handoff, HS/FS/SS devices, deferred interrupt delivery,
 *             ring and event-ring wrap, STALL recovery, short packets,
 *             bulk with bouncing, cancel, hot unplug/replug, quiesce
 *   poll      no interrupt: everything through the polling path
 *   lostirq   interrupt enabled but never delivered: polling fallback
 *   takeover  controller left running by the previous firmware, 64-byte
 *             contexts, a BIOS that never releases its semaphore, port
 *             power control (ports off after HCRST)
 *   dead      commands never complete: bounded timeout, controller halted,
 *             handoff still resets it
 */
#include "../../kernel/drivers/usb/host/xhci.h"
#include "xhci_model.h"

#include <kshim/dma.h>
#include <kshim/smp.h>
#include <kshim/time.h>

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* ---- Fake clock and interrupt state ------------------------------------------ */

static uint64_t NowUs;
static bool IrqReady;
uint64_t kshim_smp_time_us(void) { return NowUs; }
uint64_t get_current_time(void) { return NowUs / 1000U; }
uint64_t delay(uint64_t us) { NowUs += us ? us : 1; return us; }
void kshim_wait_event(uint64_t deadline_ms)
{
    uint64_t deadline = deadline_ms * 1000U;
    NowUs = deadline > NowUs ? deadline : NowUs + 1U;
}
bool kshim_irq_ready(void) { return IrqReady; }

/* ---- Devices --------------------------------------------------------------------- */

static const uint8_t HsKbdDev[18] = {18, 1, 0x00, 0x02, 0, 0, 0, 64, 0x27, 0x06, 0x01, 0x00,
                                     0x00, 0x01, 1, 2, 0, 1};
static const uint8_t FsKbdDev[18] = {18, 1, 0x10, 0x01, 0, 0, 0, 8, 0x6d, 0x04, 0x1c, 0xc3,
                                     0x00, 0x01, 1, 2, 0, 1};
/* Config: HID boot keyboard, HID descriptor, interrupt IN 0x81 (8 bytes). */
#define KBD_CFG(interval) { \
    9, 2, 34, 0, 1, 1, 0, 0xa0, 50, \
    9, 4, 0, 0, 1, 3, 1, 1, 0, \
    9, 0x21, 0x11, 0x01, 0, 1, 0x22, 16, 0, \
    7, 5, 0x81, 3, 8, 0, (interval) }
static const uint8_t HsKbdCfg[34] = KBD_CFG(7);   /* 2^(7-1) x 125 us */
static const uint8_t FsKbdCfg[34] = KBD_CFG(10);  /* 10 frames */
static const uint8_t Report[16] = {0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x05, 0x07,
                                   0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00, 0x25, 0xc0};
static const uint8_t SsDev[18] = {18, 1, 0x00, 0x03, 0, 0, 0, 9, 0xf4, 0x46, 0x01, 0x00,
                                  0x00, 0x01, 1, 2, 0, 1};
/* Mass-storage-like interface: bulk OUT 0x02 and IN 0x81, 1024 bytes,
 * SuperSpeed companions with bMaxBurst 3. */
static const uint8_t SsCfg[44] = {
    9, 2, 44, 0, 1, 1, 0, 0x80, 50,
    9, 4, 0, 0, 2, 8, 6, 0x50, 0,
    7, 5, 0x02, 2, 0x00, 0x04, 0, 6, 0x30, 3, 0, 0, 0,
    7, 5, 0x81, 2, 0x00, 0x04, 0, 6, 0x30, 3, 0, 0, 0,
};

static void attach_all(void)
{
    struct xm_dev d;
    memset(&d, 0, sizeof(d));
    d.speed = 3;
    d.dev_desc = HsKbdDev; d.dev_len = sizeof(HsKbdDev);
    d.cfg_desc = HsKbdCfg; d.cfg_len = sizeof(HsKbdCfg);
    d.report_desc = Report; d.report_len = sizeof(Report);
    xm_attach(1, &d);
    d.speed = 1;
    d.dev_desc = FsKbdDev; d.dev_len = sizeof(FsKbdDev);
    d.cfg_desc = FsKbdCfg; d.cfg_len = sizeof(FsKbdCfg);
    xm_attach(2, &d);
    memset(&d, 0, sizeof(d));
    d.speed = 4;
    d.dev_desc = SsDev; d.dev_len = sizeof(SsDev);
    d.cfg_desc = SsCfg; d.cfg_len = sizeof(SsCfg);
    xm_attach(3, &d);
}

/* ---- A HID-class test driver ----------------------------------------------------- */

struct kbd {
    struct kshim_usb_interface *intf;
    struct kshim_usb_urb urb;
    uint8_t data[8];
    unsigned reports, cancelled, probes, disconnects;
    uint8_t last[8];
};
static struct kbd Kbds[4];
static unsigned Probes, Disconnects;

static void kbd_done(struct kshim_usb_urb *urb)
{
    struct kbd *k = urb->context;
    if (urb->status == KSHIM_IO_ERR_CANCELLED) { ++k->cancelled; return; }
    assert(urb->status == 0 && urb->actual == 8);
    memcpy(k->last, k->data, 8);
    ++k->reports;
    assert(kshim_usb_submit_urb(urb) == 0);
}

static int kbd_probe(struct kshim_usb_interface *intf)
{
    struct kbd *k = &Kbds[intf->device->port];
    struct kshim_usb_endpoint *ep = kshim_usb_find_endpoint(intf, KSHIM_USB_XFER_INTERRUPT, true);
    assert(ep && ep->address == 0x81 && ep->max_packet == 8);
    memset(&k->urb, 0, sizeof(k->urb));
    k->intf = intf;
    k->urb.device = intf->device;
    k->urb.endpoint = ep;
    k->urb.buffer = k->data;       /* not pool memory: the core bounces it */
    k->urb.length = 8;
    k->urb.complete = kbd_done;
    k->urb.context = k;
    ++k->probes;
    ++Probes;
    intf->driver_data = k;
    return kshim_usb_submit_urb(&k->urb);
}

static void kbd_disconnect(struct kshim_usb_interface *intf)
{
    struct kbd *k = intf->driver_data;
    ++k->disconnects;
    ++Disconnects;
    (void)kshim_usb_cancel_urb(&k->urb);
    k->intf = NULL;
}

static struct kshim_usb_class_driver KbdDriver = {
    .name = "test-kbd", .class_code = 3, .subclass = -1, .protocol = -1,
    .probe = kbd_probe, .disconnect = kbd_disconnect,
};

/* ---- Helpers ------------------------------------------------------------------------ */

static struct xhci X;
static void line(void *context) { (void)xhci_irq(context); }

static struct xm_slot *slot_for(unsigned port)
{
    struct xm_slot *s = xm_slot_of_port(port);
    assert(s);
    return s;
}

static int start(bool irq)
{
    struct xhci_io io = {NULL, xm_read32, xm_write32};
    return xhci_start(&X, "model", XM_BASE, &io, UINT64_MAX, irq);
}

static void run_for_ms(unsigned ms)
{
    uint64_t end = NowUs + ms * 1000ULL;
    while (NowUs < end) {
        kshim_usb_poll();
        kshim_wait_event(get_current_time() + 1);
    }
}

static void check_enumerated(void)
{
    struct kshim_usb_device *hs = X.hcd.root[0], *fs = X.hcd.root[1], *ss = X.hcd.root[2];
    assert(hs && fs && ss && !X.hcd.root[3]);
    assert(hs->speed == KSHIM_USB_SPEED_HIGH && hs->configured && hs->config_value == 1);
    assert(hs->descriptor.idVendor == 0x0627 && hs->descriptor.idProduct == 0x0001);
    assert(hs->address == slot_for(1)->address && hs->ep0.max_packet == 64);
    assert(hs->interface_count == 1 && hs->interfaces[0].class_code == 3);
    assert(Xm.devs[1].config == 1 && Xm.devs[2].config == 1 && Xm.devs[3].config == 1);
    /* HS interrupt: Interval = bInterval - 1; FS: log2(10 frames x 8). */
    assert(slot_for(1)->ep[3].enabled && slot_for(1)->ep[3].type == 7);
    assert(slot_for(1)->ep[3].interval == 6 && slot_for(1)->ep[3].mps == 8);
    assert(slot_for(2)->ep[3].interval == 6);
    /* FS default pipe: 64 first, then bMaxPacketSize0 = 8 via Evaluate Context. */
    assert(fs->speed == KSHIM_USB_SPEED_FULL && fs->ep0.max_packet == 8);
    assert(slot_for(2)->ep[1].mps == 8 && Xm.commands[13] >= 1);
    /* SS: 512-byte EP0 (2^9), bulk endpoints with the companion burst. */
    assert(ss->speed == KSHIM_USB_SPEED_SUPER && ss->ep0.max_packet == 512);
    assert(slot_for(3)->ep[4].type == 2 && slot_for(3)->ep[4].max_burst == 3 && slot_for(3)->ep[4].mps == 1024);
    assert(slot_for(3)->ep[3].type == 6);
    /* No route string or TT on root ports; root hub port numbers. */
    assert(slot_for(1)->route == 0 && slot_for(3)->port == 3);
    assert(Kbds[1].probes == 1 && Kbds[2].probes == 1);
}

static void check_reports(unsigned port, unsigned count)
{
    struct kbd *k = &Kbds[port];
    unsigned before = k->reports;
    for (unsigned i = 0; i < count; ++i) {
        uint8_t r[8] = {0, 0, (uint8_t)(4 + i % 26), 0, 0, 0, 0, (uint8_t)i};
        xm_report(port, r);
        if (X.irq_enabled && Xm.irq) {
            /* Deferred model: the handler only marked the work pending. */
            assert(X.hcd.irq_pending && k->reports == before + i);
        }
        kshim_usb_poll();
        assert(!X.hcd.irq_pending);
        assert(k->reports == before + i + 1 && !memcmp(k->last, r, 8));
    }
}

static void check_control_and_bulk(void)
{
    struct kshim_usb_device *hs = X.hcd.root[0], *ss = X.hcd.root[2];
    /* SET_IDLE stalls: the default pipe recovers by itself. */
    Xm.devs[1].stall_set_idle = true;
    unsigned resets = Xm.commands[14], deqs = Xm.commands[16];
    assert(kshim_usb_control(hs, 0x21, 0x0a, 0, 0, NULL, 0, 100, NULL) == KSHIM_IO_ERR_STALL);
    uint8_t status[2] = {0xff, 0xff};
    uint32_t actual = 0;
    assert(kshim_usb_control(hs, 0x80, 0, 0, 0, status, 2, 100, &actual) == 0);
    assert(actual == 2 && !status[0] && !status[1]);
    assert(Xm.commands[14] == resets + 1 && Xm.commands[16] == deqs + 1);
    /* Unknown descriptor type: STALL in the data stage, also recovered. */
    uint8_t buf[300];
    assert(kshim_usb_get_descriptor(hs, 0x0f, 0, 0, buf, 5, &actual) == KSHIM_IO_ERR_STALL);
    /* Short packet in a control IN data stage. */
    assert(kshim_usb_get_descriptor(hs, 2, 0, 0, buf, 255, &actual) == 0 && actual == 34);
    assert(!memcmp(buf, HsKbdCfg, 34));
    /* Interface request with the report descriptor. */
    assert(kshim_usb_control(hs, 0x81, 6, 0x2200, 0, buf, 16, 100, &actual) == 0 && actual == 16);

    /* SuperSpeed bulk OUT then IN (echo), through bounce buffers. */
    uint8_t out[1024], in[1024];
    for (unsigned i = 0; i < sizeof(out); ++i) out[i] = (uint8_t)(i * 13U + 1U);
    struct kshim_usb_endpoint *bo = kshim_usb_find_endpoint(&ss->interfaces[0], KSHIM_USB_XFER_BULK, false);
    struct kshim_usb_endpoint *bi = kshim_usb_find_endpoint(&ss->interfaces[0], KSHIM_USB_XFER_BULK, true);
    assert(bo && bi && bo->max_burst == 3);
    assert(kshim_usb_bulk(ss, bo, out, 1024, 100, &actual) == 0 && actual == 1024);
    memset(in, 0, sizeof(in));
    assert(kshim_usb_bulk(ss, bi, in, 1024, 100, &actual) == 0 && actual == 1024 && !memcmp(in, out, 1024));
    /* Short bulk IN. */
    assert(kshim_usb_bulk(ss, bo, out, 100, 100, &actual) == 0 && actual == 100);
    assert(kshim_usb_bulk(ss, bi, in, 1024, 100, &actual) == 0 && actual == 100);
    /* A bulk IN with nothing to send times out and is cancelled. */
    unsigned stops = Xm.commands[15];
    assert(kshim_usb_bulk(ss, bi, in, 64, 20, &actual) == ERROR_TIMEOUT);
    assert(Xm.commands[15] == stops + 1);
    /* The ring is usable after the cancel. */
    assert(kshim_usb_bulk(ss, bo, out, 64, 100, &actual) == 0);
    assert(kshim_usb_bulk(ss, bi, in, 64, 100, &actual) == 0 && actual == 64);
    /* Two queued bulk IN URBs; cancelling the second (not the controller's
     * current TD) turns it into No Ops, the first still completes, and the
     * ring keeps working across several wraps afterwards. */
    static struct kshim_usb_urb a, b;
    static uint8_t abuf[64], bbuf[64];
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    a.device = b.device = ss;
    a.endpoint = b.endpoint = bi;
    a.buffer = abuf; b.buffer = bbuf;
    a.length = b.length = 64;
    assert(kshim_usb_submit_urb(&a) == 0 && kshim_usb_submit_urb(&b) == 0);
    stops = Xm.commands[15];
    assert(kshim_usb_cancel_urb(&b) == 0 && b.status == KSHIM_IO_ERR_CANCELLED);
    assert(Xm.commands[15] == stops + 1 && a.status == KSHIM_IO_ERR_PENDING);
    assert(kshim_usb_bulk(ss, bo, out, 64, 100, &actual) == 0);
    kshim_usb_poll_events();
    assert(a.status == 0 && a.actual == 64 && !memcmp(abuf, out, 64));
    for (unsigned i = 0; i < 150; ++i) {
        out[0] = (uint8_t)i;
        assert(kshim_usb_bulk(ss, bo, out, 32, 100, &actual) == 0);
        assert(kshim_usb_bulk(ss, bi, in, 64, 100, &actual) == 0 && actual == 32 && in[0] == (uint8_t)i);
    }
    /* 100 KiB in one URB: the bounce buffer crosses 64 KiB boundaries, so
     * the TD is several chained Normal TRBs; a short IN packet in the first
     * TRB of such a TD ends it with the bytes received. */
    static uint8_t big[100 * 1024];
    memset(big, 0x5a, sizeof(big));
    assert(kshim_usb_bulk(ss, bo, big, sizeof(big), 100, &actual) == 0 && actual == sizeof(big));
    assert(Xm.last_td_trbs >= 2);
    assert(kshim_usb_bulk(ss, bo, out, 100, 100, &actual) == 0);
    memset(big, 0, sizeof(big));
    assert(kshim_usb_bulk(ss, bi, big, sizeof(big), 100, &actual) == 0 && actual == 100);
    assert(!memcmp(big, out, 100) && Xm.last_td_trbs == 1);
    /* The interrupt endpoint refuses a bulk helper. */
    assert(kshim_usb_bulk(hs, &hs->interfaces[0].endpoints[0], in, 8, 10, NULL) == ERROR_INVALID_PARAMETER);
}

static void check_cancel_and_hotplug(void)
{
    /* Cancel the pending interrupt URB: Stop Endpoint, CANCELLED callback,
     * then resubmission resumes on the same ring. */
    struct kbd *k = &Kbds[1];
    unsigned stops = Xm.commands[15];
    assert(kshim_usb_cancel_urb(&k->urb) == 0 && k->cancelled == 1);
    assert(Xm.commands[15] == stops + 1 && k->urb.status == KSHIM_IO_ERR_CANCELLED);
    assert(kshim_usb_submit_urb(&k->urb) == 0);
    check_reports(1, 3);

    /* Unplug the FS keyboard: disconnect hook, Disable Slot, root port free. */
    unsigned disables = Xm.commands[10];
    xm_detach(2);
    run_for_ms(5);
    assert(!X.hcd.root[1] && Kbds[2].disconnects == 1 && Kbds[2].cancelled == 1);
    assert(Xm.commands[10] == disables + 1);
    /* Replug: 100 ms debounce, then enumerated again. */
    struct xm_dev d;
    memset(&d, 0, sizeof(d));
    d.speed = 1;
    d.dev_desc = FsKbdDev; d.dev_len = sizeof(FsKbdDev);
    d.cfg_desc = FsKbdCfg; d.cfg_len = sizeof(FsKbdCfg);
    xm_attach(2, &d);
    run_for_ms(50);
    assert(!X.hcd.root[1]); /* still debouncing */
    run_for_ms(100);
    assert(X.hcd.root[1] && Kbds[2].probes == 2);
    check_reports(2, 2);
}

static void check_quiesce(uint32_t legsup_before, uint32_t legctl_before)
{
    assert(kshim_usb_quiesce() == 0);
    assert(Disconnects == Probes);
    for (unsigned p = 1; p <= XM_PORTS; ++p) assert(!X.hcd.root[p - 1] && !xm_slot_of_port(p));
    unsigned hcrst = Xm.hcrst_count;
    assert(kshim_usb_hcd_quiesce_all() == 0);
    /* Interrupts off, R/S cleared, halted, then HCRST: registers at reset. */
    assert(Xm.hcrst_count == hcrst + 1 && Xm.rs_clear_seq < Xm.hcrst_seq);
    assert(Xm.usbcmd == 0 && (Xm.usbsts & 1U) && !Xm.dcbaap_lo && !Xm.dcbaap_hi && !Xm.config);
    assert(!Xm.iman && !Xm.erstsz && !Xm.erstba_lo && !Xm.erdp_lo);
    /* BIOS semaphore and SMI enables back as inherited (latched events are
     * not re-created). */
    assert(Xm.legsup == legsup_before);
    assert(Xm.legctl == (legctl_before & ~0xe0000000U));
    assert(kshim_dma_in_use() == 0);
    /* Idempotent, and the core refuses new work. */
    assert(kshim_usb_quiesce() == 0 && kshim_usb_hcd_quiesce_all() == 0 && Xm.hcrst_count == hcrst + 1);
    struct kshim_usb_urb urb;
    memset(&urb, 0, sizeof(urb));
    assert(kshim_usb_submit_urb(&urb) == ERROR_INVALID_PARAMETER);
}

/* ---- Scenarios ------------------------------------------------------------------------ */

static void scenario_irq(void)
{
    xm_init(32, 2, false);
    Xm.irq = line;
    Xm.irq_context = &X;
    IrqReady = true;
    uint32_t legsup = Xm.legsup, legctl = Xm.legctl;
    attach_all();
    assert(kshim_usb_class_register(&KbdDriver) == 0);
    assert(start(true) == 0);
    /* 4.2 programming, scratchpads, interrupter 0, BIOS handoff. */
    assert(Xm.config == XM_SLOTS && Xm.dcbaap_lo && !(Xm.dcbaap_lo & 63U) && Xm.scratch_seen == 2);
    assert(Xm.erstsz == 1 && (Xm.iman & 2U) && Xm.usbcmd == 5U && Xm.imod == 160);
    assert((Xm.legsup & (1U << 24)) && !(Xm.legsup & (1U << 16)) && !(Xm.legctl & 0xe0000011U));
    assert(X.hcd.port_count == 4 && X.ports[1].major == 2 && X.ports[3].major == 3);
    assert(!X.inherited_running && Xm.hcrst_count == 1);
    assert(kshim_usb_settle(3000) == 0);
    check_enumerated();
    assert(Xm.irqs_raised > 0 && X.irqs > 0);
    check_reports(1, 300); /* > 4 transfer-ring and > 1 event-ring wraps */
    assert(Xm.ev_posted > 256);
    check_reports(2, 5);
    check_control_and_bulk();
    check_cancel_and_hotplug();
    check_quiesce(legsup, legctl);
}

static void scenario_poll(bool lost_irq)
{
    xm_init(32, 0, false);
    IrqReady = lost_irq;
    if (lost_irq) {
        /* The line is never delivered (sealed/unresolved interrupt). */
        Xm.irq = NULL;
    }
    uint32_t legsup = Xm.legsup, legctl = Xm.legctl;
    attach_all();
    assert(kshim_usb_class_register(&KbdDriver) == 0);
    assert(start(lost_irq) == 0);
    assert(lost_irq ? (Xm.iman & 2U) && (Xm.usbcmd & 4U) : !(Xm.iman & 2U) && !(Xm.usbcmd & 4U));
    assert(kshim_usb_settle(3000) == 0);
    check_enumerated();
    assert(X.irqs == 0);
    if (!lost_irq) assert(Xm.irqs_raised == 0);
    check_reports(1, 70);
    if (!lost_irq) assert(!(Xm.usbsts & (1U << 3))); /* EINT kept tidy */
    check_control_and_bulk();
    check_quiesce(legsup, legctl);
}

static void scenario_takeover(void)
{
    xm_init(64, 1, true);
    Xm.bios_releases = false;
    Xm.ppc = true;
    Xm.irq = line;
    Xm.irq_context = &X;
    IrqReady = true;
    uint32_t legsup = Xm.legsup, legctl = Xm.legctl;
    attach_all();
    assert(kshim_usb_class_register(&KbdDriver) == 0);
    assert(start(true) == 0);
    /* Halted first (R/S cleared), then reset, then our own rings. */
    assert(X.inherited_running && Xm.stop_count == 1 && Xm.rs_clear_seq < Xm.hcrst_seq);
    assert(Xm.dcbaap_lo != 0xdead0000U && X.ctx_size == 64);
    /* PPC: every port was powered after the reset (4.19.1.1). */
    for (unsigned p = 1; p <= XM_PORTS; ++p) assert(Xm.portsc[p] & (1U << 9));
    /* The BIOS never answered: ownership taken. */
    assert((Xm.legsup & (1U << 24)) && !(Xm.legsup & (1U << 16)));
    assert(kshim_usb_settle(3000) == 0);
    check_enumerated();
    check_reports(1, 10);
    check_control_and_bulk();
    check_quiesce(legsup, legctl);
}

static void scenario_dead(void)
{
    xm_init(32, 0, false);
    IrqReady = false;
    attach_all();
    assert(start(false) == 0);
    Xm.ignore_commands = true;
    uint64_t t0 = NowUs;
    assert(kshim_usb_settle(15000) == 0 || true);
    /* Enable Slot timed out (bounded), the controller was halted and every
     * port gave up. */
    assert(X.dead && (Xm.usbsts & 1U) && !(Xm.usbcmd & 1U));
    assert(NowUs - t0 < 20000000ULL);
    for (unsigned p = 0; p < 4; ++p) assert(!X.hcd.root[p]);
    assert(kshim_usb_quiesce() == 0 && kshim_usb_hcd_quiesce_all() == 0);
    assert(Xm.usbcmd == 0 && !Xm.dcbaap_lo && kshim_dma_in_use() == 0);
}

int main(int argc, char **argv)
{
    const char *which = argc > 1 ? argv[1] : "irq";
    if (!strcmp(which, "irq")) scenario_irq();
    else if (!strcmp(which, "poll")) scenario_poll(false);
    else if (!strcmp(which, "lostirq")) scenario_poll(true);
    else if (!strcmp(which, "takeover")) scenario_takeover();
    else if (!strcmp(which, "dead")) scenario_dead();
    else return 2;
    printf("usb_xhci_test %s: ok\n", which);
    return 0;
}
