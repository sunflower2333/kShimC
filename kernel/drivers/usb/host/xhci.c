/* SPDX-License-Identifier: MIT */
/*
 * xHCI controller lifecycle, rings, commands and the event ring.
 *
 * Written from the xHCI 1.2 specification (see xhci.h): 4.2 host controller
 * initialization, 4.9 TRB rings and cycle bits, 4.6 commands, 4.17
 * interrupters and the event ring, 4.22 / 7.1 BIOS-to-OS handoff, 5.4 / 5.5
 * operational and runtime registers.
 *
 * Execution model (docs/io-stack.md): the interrupt handler only clears
 * IMAN.IP / USBSTS.EINT and sets hcd.irq_pending. Events are consumed in
 * thread context by xhci_process_events(), which the USB core runs through
 * ops->poll and command waits run directly. The Event Handler Busy flag is
 * cleared only after the events were consumed, so the next interrupt is
 * not raised before the thread caught up. Without a usable interrupt the
 * same path polls.
 */
#include "xhci.h"
#include <mmio.h>

#include <kshim/dma.h>
#include <kshim/smp.h>
#include <kshim/time.h>

#include <stdio.h>
#include <string.h>

#define XHCI_CNR_TIMEOUT_US      1000000U
#define XHCI_HALT_TIMEOUT_US     32000U   /* 16 ms max after R/S=0 (5.4.1), doubled */
#define XHCI_RESET_TIMEOUT_US    1000000U
#define XHCI_RUN_TIMEOUT_US      100000U
#define XHCI_BIOS_TIMEOUT_US     100000U  /* no SMM on these platforms: short, then force */
#define XHCI_COMMAND_TIMEOUT_US  5000000U /* Address Device includes SET_ADDRESS */
#define XHCI_PORT_POWER_MS       20U      /* root hub power-on to power-good */
#define XHCI_IMOD_INTERVAL       160U     /* 40 us in 250 ns units (5.5.2.2) */
#define XHCI_IRQ_STUCK_LIMIT     100U

static inline void xhci_barrier(void)
{
#if defined(__aarch64__)
    __asm__ volatile("dsb sy" ::: "memory");
#else
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}

/* ---- MMIO --------------------------------------------------------------- */

static uint32_t mmio_read32(void *context, uintptr_t address)
{
    (void)context;
    return kshim_mmio_read32((uintptr_t)address);
}

static void mmio_write32(void *context, uintptr_t address, uint32_t value)
{
    (void)context;
    kshim_mmio_write32((uintptr_t)address, value);
}

void xhci_io_mmio(struct xhci_io *io)
{
    io->context = NULL;
    io->read32 = mmio_read32;
    io->write32 = mmio_write32;
}

uint32_t xhci_read(struct xhci *x, uintptr_t offset) { return x->io.read32(x->io.context, x->base + offset); }
void xhci_write(struct xhci *x, uintptr_t offset, uint32_t value) { x->io.write32(x->io.context, x->base + offset, value); }
uint32_t xhci_op_read(struct xhci *x, uintptr_t offset) { return x->io.read32(x->io.context, x->op + offset); }
void xhci_op_write(struct xhci *x, uintptr_t offset, uint32_t value) { x->io.write32(x->io.context, x->op + offset, value); }
uint32_t xhci_rt_read(struct xhci *x, uintptr_t offset) { return x->io.read32(x->io.context, x->rt + offset); }
void xhci_rt_write(struct xhci *x, uintptr_t offset, uint32_t value) { x->io.write32(x->io.context, x->rt + offset, value); }

/* 64-bit registers as two dword writes, low dword first (5.1). */
static void op_write64(struct xhci *x, uintptr_t offset, uint64_t value)
{
    xhci_op_write(x, offset, (uint32_t)value);
    xhci_op_write(x, offset + 4, (uint32_t)(value >> 32));
}

static void rt_write64(struct xhci *x, uintptr_t offset, uint64_t value)
{
    xhci_rt_write(x, offset, (uint32_t)value);
    xhci_rt_write(x, offset + 4, (uint32_t)(value >> 32));
}

void xhci_ring_doorbell(struct xhci *x, unsigned slot, unsigned target)
{
    xhci_barrier();
    x->io.write32(x->io.context, x->db + 4U * slot, target);
}

bool xhci_wait(struct xhci *x, uintptr_t address, uint32_t mask, uint32_t want,
               uint32_t timeout_us)
{
    uint64_t start = kshim_smp_time_us();
    for (;;) {
        uint32_t value = x->io.read32(x->io.context, address);
        if (value == UINT32_MAX && mask != UINT32_MAX) return false; /* device gone */
        if ((value & mask) == want) return true;
        uint64_t elapsed = kshim_smp_time_us() - start;
        if (elapsed >= timeout_us) return false;
        (void)delay(elapsed < 1000 ? 10 : 500);
    }
}

static bool wait_op(struct xhci *x, uintptr_t offset, uint32_t mask, uint32_t want, uint32_t timeout_us)
{
    return xhci_wait(x, x->op + offset, mask, want, timeout_us);
}

/* ---- Rings (4.9) -------------------------------------------------------- */

int xhci_ring_init(struct xhci *x, struct xhci_ring *ring)
{
    memset(ring, 0, sizeof(*ring));
    size_t size = XHCI_RING_TRBS * sizeof(struct xhci_trb);
    /* Rings may not cross a 64 KiB boundary (Table 6-1). */
    ring->trbs = kshim_dma_alloc(size, 64, 0x10000, x->dma_limit, &ring->dma);
    if (!ring->trbs) return KSHIM_IO_ERR_NO_MEMORY;
    struct xhci_trb *link = &ring->trbs[XHCI_RING_TRBS - 1];
    link->param_lo = (uint32_t)ring->dma;
    link->param_hi = (uint32_t)(ring->dma >> 32);
    link->control = XHCI_TRB_TYPE(XHCI_TRB_LINK) | XHCI_TRB_TC; /* cycle 0: not yet owned */
    ring->cycle = 1;
    kshim_dma_sync_for_device(ring->trbs, size);
    return 0;
}

void xhci_ring_free(struct xhci_ring *ring)
{
    if (ring->trbs) kshim_dma_free(ring->trbs, XHCI_RING_TRBS * sizeof(struct xhci_trb));
    memset(ring, 0, sizeof(*ring));
}

unsigned xhci_ring_free_trbs(const struct xhci_ring *ring)
{
    /* From positions, not a count: TRBs between the software dequeue and
     * the enqueue (cancelled TDs turned into No Ops included) still belong
     * to the controller. One TRB stays spare so a full ring never looks
     * empty; the Link TRB is not a position. */
    unsigned positions = XHCI_RING_TRBS - 1U;
    unsigned in_flight = (ring->enqueue + positions - ring->dequeue) % positions;
    return in_flight >= positions - 1U ? 0 : positions - 1U - in_flight;
}

uint64_t xhci_ring_trb_dma(const struct xhci_ring *ring, unsigned index)
{
    return ring->dma + (uint64_t)index * sizeof(struct xhci_trb);
}

unsigned xhci_ring_put(struct xhci_ring *ring, const struct xhci_trb *trb, bool chain_link,
                       bool own_now)
{
    unsigned index = ring->enqueue;
    struct xhci_trb *t = &ring->trbs[index];
    t->param_lo = trb->param_lo;
    t->param_hi = trb->param_hi;
    t->status = trb->status;
    uint32_t control = trb->control & ~XHCI_TRB_CYCLE;
    if (own_now) {
        __atomic_thread_fence(__ATOMIC_RELEASE);
        t->control = control | ring->cycle;
    } else {
        t->control = control | (ring->cycle ^ 1U);
    }
    ++ring->used;
    if (++ring->enqueue == XHCI_RING_TRBS - 1U) {
        /* Hand the Link TRB over with the current cycle; it carries the
         * chain bit when the TD continues past it (4.11.5.1). */
        struct xhci_trb *link = &ring->trbs[XHCI_RING_TRBS - 1];
        uint32_t lc = XHCI_TRB_TYPE(XHCI_TRB_LINK) | XHCI_TRB_TC | (chain_link ? XHCI_TRB_CH : 0);
        __atomic_thread_fence(__ATOMIC_RELEASE);
        link->control = lc | ring->cycle;
        ring->enqueue = 0;
        ring->cycle ^= 1U;
    }
    return index;
}

/* ---- Commands (4.6) ----------------------------------------------------- */

static void cmd_pump(void *context)
{
    (void)xhci_process_events(context);
}

static void xhci_halt_dead(struct xhci *x, const char *why)
{
    if (x->dead) return;
    x->dead = true;
    printf("xhci: %s: %s; controller halted\n", x->name, why);
    uint32_t cmd = xhci_op_read(x, XHCI_USBCMD);
    xhci_op_write(x, XHCI_USBCMD, cmd & ~(XHCI_CMD_RS | XHCI_CMD_INTE));
    (void)wait_op(x, XHCI_USBSTS, XHCI_STS_HCH, XHCI_STS_HCH, XHCI_HALT_TIMEOUT_US);
    xhci_fail_all(x, ERROR_DEVICE_ERROR);
}

int xhci_command(struct xhci *x, const struct xhci_trb *trb, unsigned *cc, uint8_t *slot)
{
    if (cc) *cc = 0;
    if (slot) *slot = 0;
    if (x->dead || !x->running) return ERROR_DEVICE_ERROR;
    if (x->cmd_pending) return ERROR_BUSY;
    if (!xhci_ring_free_trbs(&x->cmd)) return ERROR_BUSY;
    kshim_completion_init(&x->cmd_done);
    x->cmd_pending = true;
    unsigned index = xhci_ring_put(&x->cmd, trb, false, true);
    x->cmd_trb_dma = xhci_ring_trb_dma(&x->cmd, index);
    kshim_dma_sync_for_device(x->cmd.trbs, XHCI_RING_TRBS * sizeof(struct xhci_trb));
    xhci_ring_doorbell(x, 0, 0);
    int status = kshim_completion_wait(&x->cmd_done, XHCI_COMMAND_TIMEOUT_US, cmd_pump, x);
    x->cmd_pending = false;
    if (status == ERROR_TIMEOUT && !kshim_completion_done(&x->cmd_done)) {
        xhci_halt_dead(x, "command timeout");
        return ERROR_TIMEOUT;
    }
    if (status) return status;
    if (cc) *cc = XHCI_EVT_CODE(x->cmd_status);
    if (slot) *slot = (uint8_t)XHCI_TRB_GET_SLOT(x->cmd_control);
    return 0;
}

int xhci_status_from_cc(unsigned cc)
{
    switch (cc) {
    case XHCI_CC_SUCCESS:
    case XHCI_CC_SHORT_PACKET: return 0;
    case XHCI_CC_STALL: return KSHIM_IO_ERR_STALL;
    case XHCI_CC_BABBLE:
    case XHCI_CC_DATA_BUFFER: return KSHIM_IO_ERR_OVERFLOW;
    case XHCI_CC_RESOURCE:
    case XHCI_CC_NO_SLOTS:
    case XHCI_CC_BANDWIDTH: return KSHIM_IO_ERR_NO_MEMORY;
    case XHCI_CC_STOPPED:
    case XHCI_CC_STOPPED_LENGTH_INVALID:
    case XHCI_CC_STOPPED_SHORT_PACKET:
    case XHCI_CC_COMMAND_ABORTED: return KSHIM_IO_ERR_CANCELLED;
    case XHCI_CC_PARAMETER:
    case XHCI_CC_TRB:
    case XHCI_CC_CONTEXT_STATE:
    case XHCI_CC_SLOT_NOT_ENABLED:
    case XHCI_CC_EP_NOT_ENABLED: return ERROR_INVALID_PARAMETER;
    default: return ERROR_DEVICE_ERROR;
    }
}

/* ---- Event ring (4.17) -------------------------------------------------- */

static void command_event(struct xhci *x, const struct xhci_trb *e)
{
    uint64_t trb = (uint64_t)e->param_hi << 32 | (e->param_lo & ~0xfU);
    if (!x->cmd_pending || trb != x->cmd_trb_dma) return;
    unsigned index = (unsigned)((trb - x->cmd.dma) / sizeof(struct xhci_trb));
    x->cmd.dequeue = index + 1U >= XHCI_RING_TRBS - 1U ? 0 : index + 1U;
    if (x->cmd.used) --x->cmd.used;
    x->cmd_status = e->status;
    x->cmd_control = e->control;
    kshim_completion_complete(&x->cmd_done, 0);
}

int xhci_process_events(struct xhci *x)
{
    if (x->processing || !x->running || !x->events) return 0;
    x->processing = true;
    if (x->irq_stuck_disabled && x->hcd.irq_driven) {
        x->hcd.irq_driven = false;
        printf("xhci: %s: interrupt stuck after acknowledge; polling\n", x->name);
    }
    uint32_t sts = xhci_op_read(x, XHCI_USBSTS);
    if (sts != UINT32_MAX && (sts & (XHCI_STS_HSE | XHCI_STS_HCE)) && !x->dead) {
        x->processing = false;
        xhci_halt_dead(x, (sts & XHCI_STS_HCE) ? "host controller error" : "host system error");
        return 0;
    }
    int handled = 0;
    for (;;) {
        struct xhci_trb *slot = &x->events[x->event_dequeue];
        kshim_dma_sync_for_cpu(slot, sizeof(*slot));
        uint32_t control = __atomic_load_n(&slot->control, __ATOMIC_ACQUIRE);
        if ((control & XHCI_TRB_CYCLE) != x->event_cycle) break;
        struct xhci_trb e = {slot->param_lo, slot->param_hi, slot->status, control};
        switch (XHCI_TRB_GET_TYPE(control)) {
        case XHCI_TRB_COMMAND_COMPLETION:
            command_event(x, &e);
            break;
        case XHCI_TRB_PORT_STATUS_CHANGE: {
            uint8_t port = (uint8_t)(e.param_lo >> 24);
            if (port) kshim_usb_hcd_port_changed(&x->hcd, port);
            break;
        }
        case XHCI_TRB_TRANSFER_EVENT:
            xhci_transfer_event(x, &e);
            break;
        case XHCI_TRB_HOST_CONTROLLER_EVENT:
            printf("xhci: %s: host controller event, code %u\n", x->name,
                   (unsigned)XHCI_EVT_CODE(e.status));
            break;
        default:
            break;
        }
        ++handled;
        if (++x->event_dequeue == XHCI_EVENT_TRBS) {
            x->event_dequeue = 0;
            x->event_cycle ^= 1U;
        }
    }
    /* EHB is set together with IP whenever the interrupter fires (4.17.2).
     * An interrupt may arrive after a poll already drained the ring, so the
     * pass that follows it finds nothing; EHB must still be cleared there,
     * or the interrupter raises nothing more until a later poll happens to
     * find an event. */
    bool ehb = handled || (x->irq_enabled && (xhci_rt_read(x, XHCI_IR(0) + XHCI_ERDP_LO) & XHCI_ERDP_EHB));
    if (handled) {
        x->event_count += (uint32_t)handled;
        x->polls_with_work++;
    }
    if (ehb) {
        /* Consumed: advance ERDP and clear Event Handler Busy (RW1C). */
        uint64_t erdp = x->events_dma + (uint64_t)x->event_dequeue * sizeof(struct xhci_trb);
        rt_write64(x, XHCI_IR(0) + XHCI_ERDP_LO, erdp | XHCI_ERDP_EHB);
    }
    if (!x->irq_enabled && (sts & (XHCI_STS_EINT | XHCI_STS_PCD))) {
        /* Polled: keep the latched status tidy. */
        xhci_op_write(x, XHCI_USBSTS, sts & (XHCI_STS_EINT | XHCI_STS_PCD));
        xhci_rt_write(x, XHCI_IR(0) + XHCI_IMAN, XHCI_IMAN_IP);
    } else if (sts & XHCI_STS_PCD) {
        xhci_op_write(x, XHCI_USBSTS, XHCI_STS_PCD);
    }
    x->processing = false;
    return handled;
}

bool xhci_irq(struct xhci *x)
{
    /* irq_enabled is set only once the register windows are known, and
     * cleared before the controller is stopped. The acknowledge must not
     * depend on running: a level INTx that is not acknowledged is taken
     * again at once, and the GIC has no storm guard. */
    if (!x || !x->irq_enabled) return false;
    uint32_t sts = xhci_op_read(x, XHCI_USBSTS);
    if (sts == UINT32_MAX) return false;
    uint32_t iman = xhci_rt_read(x, XHCI_IR(0) + XHCI_IMAN);
    if (!(sts & XHCI_STS_EINT) && !(iman & XHCI_IMAN_IP)) {
        ++x->irqs_spurious;
        return false;
    }
    /* Acknowledge only: EINT (RW1C, other RW1C bits written as 0) and
     * IMAN.IP (RW1C) keeping IE. EHB stays set until the thread consumed
     * the events, which holds off the next interrupt (4.17.2). */
    if (sts & XHCI_STS_EINT) xhci_op_write(x, XHCI_USBSTS, XHCI_STS_EINT);
    xhci_rt_write(x, XHCI_IR(0) + XHCI_IMAN, XHCI_IMAN_IP | XHCI_IMAN_IE);
    ++x->irqs;
    /* Not running yet (xhci_run before running = true): the events stay on
     * the ring and the first poll consumes them. */
    if (x->running) x->hcd.irq_pending = true;
    /* While EHB is set nothing re-raises IP, so an IP that survives its
     * acknowledge is a level that will not drop (INTx storm). After
     * XHCI_IRQ_STUCK_LIMIT such calls in a row, stop interrupt generation;
     * the event ring is then only polled. MSI clears IP by itself. */
    if (xhci_rt_read(x, XHCI_IR(0) + XHCI_IMAN) & XHCI_IMAN_IP) {
        if (++x->irq_stuck >= XHCI_IRQ_STUCK_LIMIT) {
            xhci_op_write(x, XHCI_USBCMD, xhci_op_read(x, XHCI_USBCMD) & ~XHCI_CMD_INTE);
            xhci_rt_write(x, XHCI_IR(0) + XHCI_IMAN, XHCI_IMAN_IP);
            x->irq_enabled = false;
            x->irq_stuck_disabled = true;
        }
    } else {
        x->irq_stuck = 0;
    }
    return true;
}

/* ---- Extended capabilities ---------------------------------------------- */

static void scan_extended_caps(struct xhci *x)
{
    uint32_t offset = XHCI_HCC_XECP(x->hccparams1) * 4U;
    for (unsigned guard = 0; offset && guard < 64; ++guard) {
        uint32_t cap = xhci_read(x, offset);
        if (cap == UINT32_MAX) break;
        if (XHCI_XCAP_ID(cap) == XHCI_XCAP_LEGACY && !x->legsup_offset) {
            x->legsup_offset = offset;
        } else if (XHCI_XCAP_ID(cap) == XHCI_XCAP_PROTOCOL) {
            uint8_t major = (uint8_t)(cap >> 24);
            uint32_t ports = xhci_read(x, offset + 8);
            unsigned first = ports & 0xffU, count = (ports >> 8) & 0xffU;
            for (unsigned p = first; p && p < first + count && p <= KSHIM_USB_MAX_ROOT_PORTS; ++p)
                x->ports[p].major = major;
        }
        uint32_t next = XHCI_XCAP_NEXT(cap);
        offset = next ? offset + next * 4U : 0;
    }
}

/* BIOS to OS handoff (4.22.1, 7.1): request ownership through the OS Owned
 * semaphore, wait a bounded time for the BIOS to release it, then take it
 * (there is no SMM on these platforms) and disable the legacy SMIs. The
 * inherited values are put back at quiesce. */
static void legacy_takeover(struct xhci *x)
{
    if (!x->legsup_offset) return;
    uintptr_t legsup = x->legsup_offset, legctl = x->legsup_offset + 4U;
    x->legsup_saved = xhci_read(x, legsup);
    x->legctl_saved = xhci_read(x, legctl);
    x->legacy_taken = true;
    if (x->legsup_saved & XHCI_LEGSUP_BIOS) {
        xhci_write(x, legsup, x->legsup_saved | XHCI_LEGSUP_OS);
        if (!xhci_wait(x, x->base + legsup, XHCI_LEGSUP_BIOS, 0, XHCI_BIOS_TIMEOUT_US)) {
            printf("xhci: %s: BIOS semaphore not released, taking ownership\n", x->name);
            xhci_write(x, legsup, (x->legsup_saved | XHCI_LEGSUP_OS) & ~XHCI_LEGSUP_BIOS);
        }
    } else if (!(x->legsup_saved & XHCI_LEGSUP_OS)) {
        xhci_write(x, legsup, x->legsup_saved | XHCI_LEGSUP_OS);
    }
    uint32_t ctl = x->legctl_saved;
    if (ctl & (XHCI_LEGCTL_SMI_ENABLES | XHCI_LEGCTL_SMI_EVENTS))
        xhci_write(x, legctl, (ctl & ~XHCI_LEGCTL_SMI_ENABLES) | (ctl & XHCI_LEGCTL_SMI_EVENTS));
}

static void legacy_restore(struct xhci *x)
{
    if (!x->legacy_taken) return;
    uintptr_t legsup = x->legsup_offset, legctl = x->legsup_offset + 4U;
    if (xhci_read(x, legctl) != x->legctl_saved)
        xhci_write(x, legctl, x->legctl_saved & ~XHCI_LEGCTL_SMI_EVENTS);
    if (xhci_read(x, legsup) != x->legsup_saved) xhci_write(x, legsup, x->legsup_saved);
    x->legacy_taken = false;
}

/* ---- Memory ------------------------------------------------------------- */

static void free_memory(struct xhci *x)
{
    for (unsigned i = 1; i <= XHCI_MAX_SLOTS; ++i)
        if (x->slots[i]) xhci_slot_release(x, x->slots[i], KSHIM_IO_ERR_CANCELLED);
    xhci_ring_free(&x->cmd);
    if (x->events) kshim_dma_free(x->events, XHCI_EVENT_TRBS * sizeof(struct xhci_trb));
    x->events = NULL;
    if (x->erst) kshim_dma_free(x->erst, 64);
    x->erst = NULL;
    for (unsigned i = 0; i < x->scratchpads && i < XHCI_MAX_SCRATCHPADS; ++i)
        if (x->scratch_pages[i]) kshim_dma_free(x->scratch_pages[i], x->page_size);
    memset(x->scratch_pages, 0, sizeof(x->scratch_pages));
    if (x->scratch_array) kshim_dma_free(x->scratch_array, x->scratchpads * sizeof(uint64_t));
    x->scratch_array = NULL;
    if (x->dcbaa) kshim_dma_free(x->dcbaa, (x->max_slots + 1U) * sizeof(uint64_t));
    x->dcbaa = NULL;
}

static int alloc_memory(struct xhci *x)
{
    x->dcbaa = kshim_dma_alloc((x->max_slots + 1U) * sizeof(uint64_t), 64, x->page_size,
                               x->dma_limit, &x->dcbaa_dma);
    if (!x->dcbaa) return KSHIM_IO_ERR_NO_MEMORY;
    if (x->scratchpads) {
        /* Scratchpad buffer array and PAGESIZE pages (4.20, Table 6-1). */
        x->scratch_array = kshim_dma_alloc(x->scratchpads * sizeof(uint64_t), 64, x->page_size,
                                           x->dma_limit, &x->scratch_array_dma);
        if (!x->scratch_array) return KSHIM_IO_ERR_NO_MEMORY;
        for (unsigned i = 0; i < x->scratchpads; ++i) {
            uint64_t bus;
            x->scratch_pages[i] = kshim_dma_alloc(x->page_size, x->page_size, x->page_size,
                                                  x->dma_limit, &bus);
            if (!x->scratch_pages[i]) return KSHIM_IO_ERR_NO_MEMORY;
            x->scratch_array[i] = bus;
        }
        kshim_dma_sync_for_device(x->scratch_array, x->scratchpads * sizeof(uint64_t));
        x->dcbaa[0] = x->scratch_array_dma;
    }
    kshim_dma_sync_for_device(x->dcbaa, (x->max_slots + 1U) * sizeof(uint64_t));
    int status = xhci_ring_init(x, &x->cmd);
    if (status) return status;
    x->events = kshim_dma_alloc(XHCI_EVENT_TRBS * sizeof(struct xhci_trb), 64, 0x10000,
                                x->dma_limit, &x->events_dma);
    x->erst = kshim_dma_alloc(64, 64, 0, x->dma_limit, &x->erst_dma);
    if (!x->events || !x->erst) return KSHIM_IO_ERR_NO_MEMORY;
    /* One Event Ring Segment Table entry (6.5): base, size. */
    x->erst[0] = (uint32_t)x->events_dma;
    x->erst[1] = (uint32_t)(x->events_dma >> 32);
    x->erst[2] = XHCI_EVENT_TRBS;
    x->erst[3] = 0;
    kshim_dma_sync_for_device(x->erst, 64);
    x->event_dequeue = 0;
    x->event_cycle = 1;
    return 0;
}

/* ---- Lifecycle ---------------------------------------------------------- */

void xhci_fail_all(struct xhci *x, int status)
{
    for (unsigned s = 1; s <= XHCI_MAX_SLOTS; ++s) {
        struct xhci_slot *slot = x->slots[s];
        if (!slot) continue;
        for (unsigned d = 1; d < 32; ++d) xhci_ep_flush(x, &slot->eps[d], status);
    }
    if (x->cmd_pending) kshim_completion_complete(&x->cmd_done, status);
}

/* Renesas uPD720201/uPD720202: a reset while 64-bit addresses are
 * programmed keeps their upper halves internally, so the next owner's
 * addresses would be combined with the old ones. While halted, clear every
 * 64-bit address register an earlier owner may have left (DCBAAP, CRCR,
 * each interrupter's ERSTBA and ERDP) before HCRST. Only done when an upper
 * half is visibly non-zero. */
static void zero_64b_registers(struct xhci *x)
{
    unsigned intrs = (x->hcsparams1 >> 8) & 0x7ffU;
    if (intrs > 1024) intrs = 1024;
    bool high = xhci_op_read(x, XHCI_DCBAAP_HI) != 0;
    for (unsigned i = 0; i < intrs && !high; ++i)
        high = xhci_rt_read(x, XHCI_IR(i) + XHCI_ERSTBA_HI) || xhci_rt_read(x, XHCI_IR(i) + XHCI_ERDP_HI);
    if (!high) return;
    printf("xhci: %s: clearing inherited 64-bit addresses before reset\n", x->name);
    op_write64(x, XHCI_DCBAAP_LO, 0);
    op_write64(x, XHCI_CRCR_LO, 0);
    for (unsigned i = 0; i < intrs; ++i) {
        rt_write64(x, XHCI_IR(i) + XHCI_ERSTBA_LO, 0);
        rt_write64(x, XHCI_IR(i) + XHCI_ERDP_LO, 0);
    }
}

static int reset_controller(struct xhci *x)
{
    xhci_op_write(x, XHCI_USBCMD, XHCI_CMD_HCRST);
    /* Some controllers stall register reads right after HCRST. */
    (void)delay(1000);
    if (!wait_op(x, XHCI_USBCMD, XHCI_CMD_HCRST, 0, XHCI_RESET_TIMEOUT_US) ||
        !wait_op(x, XHCI_USBSTS, XHCI_STS_CNR, 0, XHCI_RESET_TIMEOUT_US))
        return ERROR_TIMEOUT;
    return 0;
}

int xhci_prepare(struct xhci *x, const char *name, uintptr_t base, const struct xhci_io *io,
                 uint64_t dma_limit)
{
    x->name = name ? name : "xhci";
    x->io = *io;
    x->base = base;
    uint32_t cap = xhci_read(x, XHCI_CAPLENGTH);
    uint8_t caplength = (uint8_t)cap;
    if (cap == UINT32_MAX || caplength < 0x20) {
        printf("xhci: %s: no capability registers (0x%08x)\n", x->name, (unsigned)cap);
        return KSHIM_IO_ERR_NO_DEVICE;
    }
    x->version = (uint16_t)(cap >> 16);
    x->op = base + caplength;
    x->rt = base + (xhci_read(x, XHCI_RTSOFF) & ~0x1fU);
    x->db = base + (xhci_read(x, XHCI_DBOFF) & ~0x3U);
    x->hcsparams1 = xhci_read(x, XHCI_HCSPARAMS1);
    x->hcsparams2 = xhci_read(x, XHCI_HCSPARAMS2);
    x->hccparams1 = xhci_read(x, XHCI_HCCPARAMS1);
    unsigned max_slots = x->hcsparams1 & 0xffU;
    x->max_slots = (uint8_t)(max_slots > XHCI_MAX_SLOTS ? XHCI_MAX_SLOTS : max_slots);
    x->max_ports = (uint8_t)(x->hcsparams1 >> 24);
    x->ctx_size = (x->hccparams1 & XHCI_HCC_CSZ) ? 64 : 32;
    x->ac64 = (x->hccparams1 & XHCI_HCC_AC64) != 0;
    x->ppc = (x->hccparams1 & XHCI_HCC_PPC) != 0;
    x->scratchpads = ((x->hcsparams2 >> 21) & 0x1fU) << 5 | ((x->hcsparams2 >> 27) & 0x1fU);
    x->dma_limit = x->ac64 ? dma_limit : (dma_limit < UINT32_MAX ? dma_limit : UINT32_MAX);
    if (!x->max_slots || !x->max_ports || x->scratchpads > XHCI_MAX_SCRATCHPADS) {
        printf("xhci: %s: unsupported parameters (slots %u ports %u scratchpads %u)\n",
               x->name, max_slots, x->max_ports, x->scratchpads);
        return ERROR_NOT_SUPPORTED;
    }
    if (!wait_op(x, XHCI_USBSTS, XHCI_STS_CNR, 0, XHCI_CNR_TIMEOUT_US)) return ERROR_NOT_READY;

    scan_extended_caps(x);
    /* Take over a controller the previous firmware left running (UEFI
     * XhciDxe): stop the schedule first; its rings are not ours. */
    uint32_t cmd = xhci_op_read(x, XHCI_USBCMD), sts = xhci_op_read(x, XHCI_USBSTS);
    if ((cmd & XHCI_CMD_RS) || !(sts & XHCI_STS_HCH)) {
        x->inherited_running = true;
        printf("xhci: %s: taking over a running controller (USBCMD 0x%08x)\n", x->name, (unsigned)cmd);
        xhci_op_write(x, XHCI_USBCMD, cmd & ~(XHCI_CMD_RS | XHCI_CMD_INTE));
        if (!wait_op(x, XHCI_USBSTS, XHCI_STS_HCH, XHCI_STS_HCH, XHCI_HALT_TIMEOUT_US)) {
            printf("xhci: %s: controller does not halt\n", x->name);
            return ERROR_TIMEOUT;
        }
    }
    legacy_takeover(x);
    if (x->quirk_zero_64b) zero_64b_registers(x);
    int status = reset_controller(x);
    if (status) {
        printf("xhci: %s: reset timeout\n", x->name);
        legacy_restore(x);
        return status;
    }
    /* HCRST cleared USBCMD.INTE and IMAN: the function cannot raise an
     * interrupt until xhci_run enables one. */
    x->prepared = true;
    return 0;
}

int xhci_run(struct xhci *x, bool irq)
{
    if (!x->prepared) return ERROR_NOT_READY;
    x->prepared = false;
    int status;
    uint32_t pagesize = xhci_op_read(x, XHCI_PAGESIZE) & 0xffffU;
    x->page_size = 4096;
    for (unsigned bit = 0; bit < 16; ++bit) {
        if (pagesize & (1U << bit)) {
            x->page_size = 4096U << bit;
            break;
        }
    }
    if (x->page_size > 65536) x->page_size = 65536;
    if ((status = alloc_memory(x))) {
        printf("xhci: %s: out of DMA memory\n", x->name);
        free_memory(x);
        legacy_restore(x);
        return status;
    }

    /* 4.2: MaxSlotsEn, DCBAAP, CRCR, interrupter 0, then run. */
    uint32_t config = xhci_op_read(x, XHCI_CONFIG);
    xhci_op_write(x, XHCI_CONFIG, (config & ~0xffU) | x->max_slots);
    op_write64(x, XHCI_DCBAAP_LO, x->dcbaa_dma);
    op_write64(x, XHCI_CRCR_LO, x->cmd.dma | XHCI_CRCR_RCS);
    xhci_rt_write(x, XHCI_IR(0) + XHCI_ERSTSZ, 1);
    rt_write64(x, XHCI_IR(0) + XHCI_ERDP_LO, x->events_dma);
    rt_write64(x, XHCI_IR(0) + XHCI_ERSTBA_LO, x->erst_dma);
    xhci_rt_write(x, XHCI_IR(0) + XHCI_IMOD, XHCI_IMOD_INTERVAL);
    x->irq_enabled = irq;
    xhci_rt_write(x, XHCI_IR(0) + XHCI_IMAN, XHCI_IMAN_IP | (irq ? XHCI_IMAN_IE : 0));
    xhci_barrier();
    /* Run with USBCMD.INTE still clear: events posted as the schedule
     * starts (a Port Status Change for a port that is already connected)
     * stay on the ring and cannot assert the line before running is set.
     * INTE follows once the controller runs (5.4.1: INTE gates the
     * interrupter's IP onto the bus). */
    xhci_op_write(x, XHCI_USBCMD, XHCI_CMD_RS);
    if (!wait_op(x, XHCI_USBSTS, XHCI_STS_HCH, 0, XHCI_RUN_TIMEOUT_US)) {
        printf("xhci: %s: controller does not run\n", x->name);
        x->irq_enabled = false;
        xhci_op_write(x, XHCI_USBCMD, 0);
        (void)wait_op(x, XHCI_USBSTS, XHCI_STS_HCH, XHCI_STS_HCH, XHCI_HALT_TIMEOUT_US);
        (void)reset_controller(x);
        free_memory(x);
        legacy_restore(x);
        return ERROR_TIMEOUT;
    }
    x->running = true;
    x->quiesced = false;
    if (irq) {
        xhci_barrier();
        xhci_op_write(x, XHCI_USBCMD, XHCI_CMD_RS | XHCI_CMD_INTE);
    }
    /* Port power (4.19.1.1): with PPC the ports start unpowered after HCRST. */
    if (x->ppc) {
        bool powered = false;
        for (unsigned p = 1; p <= x->max_ports && p <= KSHIM_USB_MAX_ROOT_PORTS; ++p) {
            uint32_t portsc = xhci_op_read(x, XHCI_PORTSC(p));
            if (portsc & XHCI_PORTSC_PP) continue;
            xhci_op_write(x, XHCI_PORTSC(p), (portsc & XHCI_PORTSC_PRESERVE) | XHCI_PORTSC_PP);
            powered = true;
        }
        if (powered) (void)delay(XHCI_PORT_POWER_MS * 1000U);
    }
    printf("xhci: %s: v%x.%02x, %u slots, %u ports, %u-byte contexts, %u scratchpads, %s%s\n",
           x->name, x->version >> 8, x->version & 0xffU, x->max_slots, x->max_ports,
           x->ctx_size, x->scratchpads, x->ac64 ? "64-bit" : "32-bit",
           x->legsup_offset ? ", legacy support" : "");
    x->hcd.name = x->name;
    x->hcd.ops = &xhci_hcd_ops;
    x->hcd.priv = x;
    x->hcd.port_count = x->max_ports > KSHIM_USB_MAX_ROOT_PORTS ? KSHIM_USB_MAX_ROOT_PORTS : x->max_ports;
    x->hcd.irq_driven = irq;
    status = kshim_usb_hcd_register(&x->hcd);
    if (status) {
        (void)xhci_stop(x);
        return status;
    }
    return 0;
}

int xhci_start(struct xhci *x, const char *name, uintptr_t base, const struct xhci_io *io,
               uint64_t dma_limit, bool irq)
{
    int status = xhci_prepare(x, name, base, io, dma_limit);
    return status ? status : xhci_run(x, irq);
}

/* A function that reads all-ones has left the bus (surprise removal, link
 * down, power lost): it can no longer master DMA, so there is nothing to
 * halt or reset. */
static bool xhci_gone(struct xhci *x)
{
    return xhci_op_read(x, XHCI_USBCMD) == UINT32_MAX && xhci_op_read(x, XHCI_USBSTS) == UINT32_MAX;
}

int xhci_stop(struct xhci *x)
{
    if (!x || x->quiesced) return 0;
    if (!x->base) {
        x->quiesced = true;
        return 0;
    }
    x->prepared = false;
    if (xhci_gone(x)) {
        /* The rings are no longer reachable by the controller: give every
         * URB back, release the memory and report success, so the handoff
         * is not refused for a function that vanished. The PCI core still
         * clears its bus-master enable and replays the config journal. */
        x->irq_enabled = false;
        x->running = false;
        xhci_fail_all(x, KSHIM_IO_ERR_CANCELLED);
        free_memory(x);
        x->legacy_taken = false;
        x->quiesced = true;
        printf("xhci: %s: controller gone (registers read all-ones); nothing to reset\n", x->name);
        return 0;
    }
    int result = 0;
    uint32_t cmd = xhci_op_read(x, XHCI_USBCMD);
    if (cmd != UINT32_MAX) {
        /* Interrupts off first, then stop the schedule (5.4.1). */
        xhci_op_write(x, XHCI_USBCMD, cmd & ~(XHCI_CMD_RS | XHCI_CMD_INTE));
        xhci_rt_write(x, XHCI_IR(0) + XHCI_IMAN, XHCI_IMAN_IP);
        x->irq_enabled = false;
        if (!wait_op(x, XHCI_USBSTS, XHCI_STS_HCH, XHCI_STS_HCH, XHCI_HALT_TIMEOUT_US)) {
            printf("xhci: %s: does not halt; handoff refused\n", x->name);
            result = ERROR_TIMEOUT;
        }
    }
    x->running = false;
    /* Halted: nothing touches the rings any more. */
    xhci_fail_all(x, KSHIM_IO_ERR_CANCELLED);
    if (!result) {
        if (reset_controller(x)) {
            printf("xhci: %s: reset timeout at handoff\n", x->name);
            result = ERROR_TIMEOUT;
        }
    }
    legacy_restore(x);
    if (!result) {
        /* HCRST cleared DCBAAP, CRCR, CONFIG and the interrupter. The
         * memory may go back to the pool. */
        free_memory(x);
        x->quiesced = true;
        uint32_t sts = xhci_op_read(x, XHCI_USBSTS);
        printf("xhci: %s: stopped (USBCMD 0x%08x USBSTS 0x%08x, %u interrupts)\n", x->name,
               (unsigned)xhci_op_read(x, XHCI_USBCMD), (unsigned)sts, (unsigned)x->irqs);
    }
    return result;
}
