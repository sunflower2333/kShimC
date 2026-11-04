/* SPDX-License-Identifier: MIT */
#pragma once
/*
 * xHCI host controller driver internals (owner xhci).
 *
 * Written from the public eXtensible Host Controller Interface for USB
 * specification, revision 1.2 (Intel, May 2019). Section references below
 * are to that document. No code from Linux, EDK2, U-Boot or CrDK is used.
 *
 * The controller core (xhci.c, xhci_ops.c) is bus-independent: all MMIO
 * goes through struct xhci_io and all DMA memory comes from kshim_dma, so
 * the same code runs on hardware, in QEMU and against the host register
 * model in tests/io/xhci_test.c. xhci_pci.c binds it to PCI functions.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kshim/io.h>
#include <kshim/usb.h>
#include <kshim/usb_hcd.h>

/* ---- Capability registers (5.3) ----------------------------------------- */
#define XHCI_CAPLENGTH      0x00 /* 7:0 CAPLENGTH, 31:16 HCIVERSION */
#define XHCI_HCSPARAMS1     0x04 /* 7:0 MaxSlots, 18:8 MaxIntrs, 31:24 MaxPorts */
#define XHCI_HCSPARAMS2     0x08 /* 7:4 ERST Max, 25:21 scratchpad hi, 31:27 scratchpad lo */
#define XHCI_HCSPARAMS3     0x0c
#define XHCI_HCCPARAMS1     0x10 /* 0 AC64, 2 CSZ, 3 PPC, 31:16 xECP (dwords) */
#define XHCI_DBOFF          0x14
#define XHCI_RTSOFF         0x18
#define XHCI_HCCPARAMS2     0x1c

#define XHCI_HCC_AC64       (1U << 0)
#define XHCI_HCC_CSZ        (1U << 2)
#define XHCI_HCC_PPC        (1U << 3)
#define XHCI_HCC_XECP(v)    (((v) >> 16) & 0xffffU)

/* ---- Operational registers (5.4), offsets from CAPLENGTH ---------------- */
#define XHCI_USBCMD         0x00
#define XHCI_USBSTS         0x04
#define XHCI_PAGESIZE       0x08
#define XHCI_DNCTRL         0x14
#define XHCI_CRCR_LO        0x18
#define XHCI_CRCR_HI        0x1c
#define XHCI_DCBAAP_LO      0x30
#define XHCI_DCBAAP_HI      0x34
#define XHCI_CONFIG         0x38
#define XHCI_PORTSC(n)      (0x400 + 0x10 * ((n) - 1)) /* n = 1..MaxPorts */

#define XHCI_CMD_RS         (1U << 0)
#define XHCI_CMD_HCRST      (1U << 1)
#define XHCI_CMD_INTE       (1U << 2)
#define XHCI_CMD_HSEE       (1U << 3)

#define XHCI_STS_HCH        (1U << 0)
#define XHCI_STS_HSE        (1U << 2)  /* RW1C */
#define XHCI_STS_EINT       (1U << 3)  /* RW1C */
#define XHCI_STS_PCD        (1U << 4)  /* RW1C */
#define XHCI_STS_SRE        (1U << 10) /* RW1C */
#define XHCI_STS_CNR        (1U << 11)
#define XHCI_STS_HCE        (1U << 12)

#define XHCI_CRCR_RCS       (1U << 0)
#define XHCI_CRCR_CS        (1U << 1)
#define XHCI_CRCR_CA        (1U << 2)
#define XHCI_CRCR_CRR       (1U << 3)

/* PORTSC (5.4.8). RW1C change bits, RW1S PR/WPR, RO CCS/OCA/speed. */
#define XHCI_PORTSC_CCS     (1U << 0)
#define XHCI_PORTSC_PED     (1U << 1)  /* RW1C: writing 1 disables */
#define XHCI_PORTSC_OCA     (1U << 3)
#define XHCI_PORTSC_PR      (1U << 4)
#define XHCI_PORTSC_PLS(v)  (((v) >> 5) & 0xfU)
#define XHCI_PORTSC_PP      (1U << 9)
#define XHCI_PORTSC_SPEED(v) (((v) >> 10) & 0xfU)
#define XHCI_PORTSC_PIC_MASK (3U << 14)
#define XHCI_PORTSC_LWS     (1U << 16)
#define XHCI_PORTSC_CSC     (1U << 17)
#define XHCI_PORTSC_PEC     (1U << 18)
#define XHCI_PORTSC_WRC     (1U << 19)
#define XHCI_PORTSC_OCC     (1U << 20)
#define XHCI_PORTSC_PRC     (1U << 21)
#define XHCI_PORTSC_PLC     (1U << 22)
#define XHCI_PORTSC_CEC     (1U << 23)
#define XHCI_PORTSC_WCE     (1U << 25)
#define XHCI_PORTSC_WDE     (1U << 26)
#define XHCI_PORTSC_WOE     (1U << 27)
#define XHCI_PORTSC_WPR     (1U << 31)
#define XHCI_PORTSC_CHANGES (XHCI_PORTSC_CSC | XHCI_PORTSC_PEC | XHCI_PORTSC_WRC | \
                             XHCI_PORTSC_OCC | XHCI_PORTSC_PRC | XHCI_PORTSC_PLC | \
                             XHCI_PORTSC_CEC)
/* Bits that keep their value when written back (RW, not RW1C/RW1S). */
#define XHCI_PORTSC_PRESERVE (XHCI_PORTSC_PP | XHCI_PORTSC_PIC_MASK | XHCI_PORTSC_WCE | \
                              XHCI_PORTSC_WDE | XHCI_PORTSC_WOE)
#define XHCI_PLS_U0         0U
#define XHCI_PLS_RXDETECT   5U
#define XHCI_PLS_POLLING    7U

/* Default protocol speed IDs (7.2.2.1.1). */
#define XHCI_SPEED_FULL     1U
#define XHCI_SPEED_LOW      2U
#define XHCI_SPEED_HIGH     3U
#define XHCI_SPEED_SUPER    4U
#define XHCI_SPEED_SUPER_PLUS 5U

/* ---- Runtime registers (5.5), offsets from RTSOFF ----------------------- */
#define XHCI_IR(n)          (0x20 + 0x20 * (n))
#define XHCI_IMAN           0x00
#define XHCI_IMOD           0x04
#define XHCI_ERSTSZ         0x08
#define XHCI_ERSTBA_LO      0x10
#define XHCI_ERSTBA_HI      0x14
#define XHCI_ERDP_LO        0x18
#define XHCI_ERDP_HI        0x1c
#define XHCI_IMAN_IP        (1U << 0)  /* RW1C */
#define XHCI_IMAN_IE        (1U << 1)
#define XHCI_ERDP_EHB       (1U << 3)  /* RW1C */

/* ---- Extended capabilities (7) ------------------------------------------ */
#define XHCI_XCAP_ID(v)     ((v) & 0xffU)
#define XHCI_XCAP_NEXT(v)   (((v) >> 8) & 0xffU)
#define XHCI_XCAP_LEGACY    1U  /* USB Legacy Support (7.1) */
#define XHCI_XCAP_PROTOCOL  2U  /* Supported Protocol (7.2) */
#define XHCI_LEGSUP_BIOS    (1U << 16)
#define XHCI_LEGSUP_OS      (1U << 24)
/* USBLEGCTLSTS (7.1.2): SMI enables 0,4,13,14,15; RW1C events 29,30,31. */
#define XHCI_LEGCTL_SMI_ENABLES 0x0000e011U
#define XHCI_LEGCTL_SMI_EVENTS  0xe0000000U

/* ---- TRBs (6.4) ---------------------------------------------------------- */
struct xhci_trb {
    uint32_t param_lo, param_hi, status, control;
};

#define XHCI_TRB_CYCLE      (1U << 0)
#define XHCI_TRB_TC         (1U << 1)  /* Link: toggle cycle */
#define XHCI_TRB_ENT        (1U << 1)
#define XHCI_TRB_ISP        (1U << 2)
#define XHCI_TRB_CH         (1U << 4)
#define XHCI_TRB_IOC        (1U << 5)
#define XHCI_TRB_IDT        (1U << 6)
#define XHCI_TRB_BSR        (1U << 9)  /* Address Device */
#define XHCI_TRB_DC         (1U << 9)  /* Configure Endpoint: deconfigure */
#define XHCI_TRB_DIR_IN     (1U << 16) /* Data/Status stage */
#define XHCI_TRB_TYPE(t)    ((uint32_t)(t) << 10)
#define XHCI_TRB_GET_TYPE(c) (((c) >> 10) & 0x3fU)
#define XHCI_TRB_SLOT(s)    ((uint32_t)(s) << 24)
#define XHCI_TRB_GET_SLOT(c) (((c) >> 24) & 0xffU)
#define XHCI_TRB_EP(e)      ((uint32_t)(e) << 16)
#define XHCI_TRB_GET_EP(c)  (((c) >> 16) & 0x1fU)
#define XHCI_TRB_TRT_NONE   (0U << 16) /* Setup stage transfer type */
#define XHCI_TRB_TRT_OUT    (2U << 16)
#define XHCI_TRB_TRT_IN     (3U << 16)
#define XHCI_TRB_LEN(l)     ((uint32_t)(l) & 0x1ffffU)
#define XHCI_TRB_TD_SIZE(n) ((uint32_t)((n) > 31 ? 31 : (n)) << 17)
#define XHCI_EVT_CODE(s)    (((s) >> 24) & 0xffU)
#define XHCI_EVT_LEN(s)     ((s) & 0xffffffU)
#define XHCI_EVT_ED         (1U << 2)

enum {
    XHCI_TRB_NORMAL = 1, XHCI_TRB_SETUP = 2, XHCI_TRB_DATA = 3, XHCI_TRB_STATUS = 4,
    XHCI_TRB_LINK = 6, XHCI_TRB_NOOP = 8,
    XHCI_TRB_ENABLE_SLOT = 9, XHCI_TRB_DISABLE_SLOT = 10, XHCI_TRB_ADDRESS_DEVICE = 11,
    XHCI_TRB_CONFIGURE_EP = 12, XHCI_TRB_EVALUATE_CTX = 13, XHCI_TRB_RESET_EP = 14,
    XHCI_TRB_STOP_EP = 15, XHCI_TRB_SET_TR_DEQ = 16, XHCI_TRB_NOOP_CMD = 23,
    XHCI_TRB_TRANSFER_EVENT = 32, XHCI_TRB_COMMAND_COMPLETION = 33,
    XHCI_TRB_PORT_STATUS_CHANGE = 34, XHCI_TRB_HOST_CONTROLLER_EVENT = 37,
};

/* Completion codes (6.4.5). */
enum {
    XHCI_CC_SUCCESS = 1, XHCI_CC_DATA_BUFFER = 2, XHCI_CC_BABBLE = 3,
    XHCI_CC_TRANSACTION = 4, XHCI_CC_TRB = 5, XHCI_CC_STALL = 6, XHCI_CC_RESOURCE = 7,
    XHCI_CC_BANDWIDTH = 8, XHCI_CC_NO_SLOTS = 9, XHCI_CC_SLOT_NOT_ENABLED = 11,
    XHCI_CC_EP_NOT_ENABLED = 12, XHCI_CC_SHORT_PACKET = 13, XHCI_CC_PARAMETER = 17,
    XHCI_CC_CONTEXT_STATE = 19, XHCI_CC_EVENT_RING_FULL = 21,
    XHCI_CC_COMMAND_RING_STOPPED = 24, XHCI_CC_COMMAND_ABORTED = 25,
    XHCI_CC_STOPPED = 26, XHCI_CC_STOPPED_LENGTH_INVALID = 27,
    XHCI_CC_STOPPED_SHORT_PACKET = 28,
};

/* ---- Contexts (6.2) ------------------------------------------------------ */
#define XHCI_EP_TYPE_ISOC_OUT 1U
#define XHCI_EP_TYPE_BULK_OUT 2U
#define XHCI_EP_TYPE_INT_OUT  3U
#define XHCI_EP_TYPE_CONTROL  4U
#define XHCI_EP_TYPE_ISOC_IN  5U
#define XHCI_EP_TYPE_BULK_IN  6U
#define XHCI_EP_TYPE_INT_IN   7U
#define XHCI_EP_STATE(v)      ((v) & 7U)
#define XHCI_EP_STATE_DISABLED 0U
#define XHCI_EP_STATE_RUNNING  1U
#define XHCI_EP_STATE_HALTED   2U
#define XHCI_EP_STATE_STOPPED  3U
#define XHCI_EP_STATE_ERROR    4U

/* ---- Driver limits -------------------------------------------------------- */
#define XHCI_MAX_SLOTS        32U   /* CONFIG.MaxSlotsEn (KSHIM_USB_MAX_DEVICES) */
#define XHCI_RING_TRBS        64U   /* per transfer/command ring, last is the Link */
#define XHCI_EVENT_TRBS       256U  /* one event ring segment (4 KiB) */
#define XHCI_MAX_SCRATCHPADS  256U
#define XHCI_TRB_MAX_BYTES    0x10000U /* one TRB never crosses 64 KiB (4.11.7.1) */

/* MMIO access; read32/write32 must be IRQ-safe (plain loads/stores). */
struct xhci_io {
    void *context;
    uint32_t (*read32)(void *context, uintptr_t address);
    void (*write32)(void *context, uintptr_t address, uint32_t value);
};

struct xhci_ring {
    struct xhci_trb *trbs;
    uint64_t dma;
    unsigned enqueue;   /* next TRB the producer writes */
    unsigned dequeue;   /* consumer position known to software */
    uint32_t cycle;     /* producer cycle state */
    unsigned used;      /* TRBs owned by the controller */
};

struct xhci_slot;
struct xhci_ep {
    struct xhci_ring ring;
    struct kshim_usb_endpoint *endpoint;
    struct kshim_usb_urb *head, *tail;  /* in-flight URBs, submission order */
    uint8_t dci;
    bool enabled;
    bool needs_reset;                   /* halted: Reset Endpoint before the next TD */
};

/* Per-URB TD bookkeeping (urb->hcd_priv points at it). */
struct xhci_td {
    struct xhci_ep *ep;
    struct kshim_usb_urb *next;
    unsigned first, last, count;        /* ring indices of the TD, TRBs used */
    uint32_t first_cycle;               /* cycle the first TRB was written with */
    uint32_t data_trb;                  /* index of the (first) data TRB, or UINT32_MAX */
    uint32_t data_len;
    uint32_t transferred;               /* bytes reported so far (short/stop events) */
    bool short_seen;
    bool in_use;
};

struct xhci_slot {
    struct kshim_usb_device *device;
    uint8_t id;
    void *out_ctx, *in_ctx;
    uint64_t out_dma, in_dma;
    struct xhci_ep eps[32];             /* by DCI (1..31) */
};

struct xhci_port_info {
    uint8_t major;                      /* 2 or 3 (Supported Protocol), 0 unknown */
};

#define XHCI_MAX_TDS 128U

struct xhci {
    struct kshim_usb_hcd hcd;           /* first: container_of by cast */
    const char *name;
    struct xhci_io io;
    uintptr_t base, op, rt, db;
    uint32_t hcsparams1, hcsparams2, hccparams1;
    uint16_t version;
    uint8_t max_slots, max_ports, ctx_size;
    bool ac64, ppc;
    uint32_t page_size;
    unsigned scratchpads;
    uint64_t dma_limit;
    /* DMA: device context base array, scratchpads, rings, ERST. */
    uint64_t *dcbaa;
    uint64_t dcbaa_dma;
    uint64_t *scratch_array;
    uint64_t scratch_array_dma;
    void *scratch_pages[XHCI_MAX_SCRATCHPADS];
    struct xhci_ring cmd;
    struct xhci_trb *events;
    uint64_t events_dma;
    unsigned event_dequeue;
    uint32_t event_cycle;
    uint32_t *erst;
    uint64_t erst_dma;
    /* One outstanding command at a time (thread context). */
    struct kshim_completion cmd_done;
    uint64_t cmd_trb_dma;
    uint32_t cmd_status, cmd_control;
    bool cmd_pending;
    struct xhci_slot *slots[XHCI_MAX_SLOTS + 1];
    struct xhci_port_info ports[KSHIM_USB_MAX_ROOT_PORTS + 1];
    struct xhci_td tds[XHCI_MAX_TDS];
    /* Legacy support (7.1) ownership, restored at quiesce. */
    uint32_t legsup_offset;             /* byte offset from base, 0 = none */
    uint32_t legsup_saved, legctl_saved;
    bool legacy_taken;
    /* State. */
    bool running, dead, quiesced, inherited_running, processing;
    bool irq_enabled;
    bool irq_stuck_disabled;            /* set in IRQ context: polled from then on */
    uint32_t irq_stuck;
    /* Statistics for logs and tests. */
    uint32_t irqs, irqs_spurious, event_count, polls_with_work;
};

/* xhci.c: controller lifecycle. */
void xhci_io_mmio(struct xhci_io *io);
/* Halt a controller the previous firmware left running, take ownership
 * from the BIOS semaphore, reset, allocate the DCBAA/scratchpads/rings,
 * program the event ring and run. irq selects interrupter 0's IE/INTE.
 * Registers the HCD with the USB core. */
int xhci_start(struct xhci *x, const char *name, uintptr_t base, const struct xhci_io *io,
               uint64_t dma_limit, bool irq);
/* IRQ context: acknowledge interrupter 0 and mark the HCD pending.
 * Returns true when the controller had an event interrupt pending. */
bool xhci_irq(struct xhci *x);
/* Stop interrupts and the schedule, HCRST, give back every URB, release
 * memory and the BIOS semaphore state. Idempotent. */
int xhci_stop(struct xhci *x);

/* xhci.c internals shared with xhci_ops.c. */
uint32_t xhci_read(struct xhci *x, uintptr_t offset);
void xhci_write(struct xhci *x, uintptr_t offset, uint32_t value);
uint32_t xhci_op_read(struct xhci *x, uintptr_t offset);
void xhci_op_write(struct xhci *x, uintptr_t offset, uint32_t value);
void xhci_ring_doorbell(struct xhci *x, unsigned slot, unsigned target);
void xhci_rt_write(struct xhci *x, uintptr_t offset, uint32_t value);
uint32_t xhci_rt_read(struct xhci *x, uintptr_t offset);
int xhci_ring_init(struct xhci *x, struct xhci_ring *ring);
void xhci_ring_free(struct xhci_ring *ring);
/* Write one TRB at the enqueue position (cycle bit last unless defer),
 * following the Link TRB. Returns the index used. */
unsigned xhci_ring_put(struct xhci_ring *ring, const struct xhci_trb *trb, bool chain_link,
                       bool own_now);
unsigned xhci_ring_free_trbs(const struct xhci_ring *ring);
uint64_t xhci_ring_trb_dma(const struct xhci_ring *ring, unsigned index);
/* Issue a command and wait for its completion event (thread context).
 * Returns 0 when a completion event arrived (*cc = its completion code,
 * *slot = its slot id) or a kShimC error (timeout: controller declared
 * dead and halted). */
int xhci_command(struct xhci *x, const struct xhci_trb *trb, unsigned *cc, uint8_t *slot);
/* Bounded register wait on an absolute MMIO address. */
bool xhci_wait(struct xhci *x, uintptr_t address, uint32_t mask, uint32_t want,
               uint32_t timeout_us);
/* Give back an URB and release its TD (thread context). */
void xhci_td_giveback(struct xhci *x, struct kshim_usb_urb *urb, int status, uint32_t actual);
/* Give back every URB queued on an endpoint. */
void xhci_ep_flush(struct xhci *x, struct xhci_ep *ep, int status);
/* Process the event ring (thread context, not re-entered). */
int xhci_process_events(struct xhci *x);
int xhci_status_from_cc(unsigned cc);
void xhci_fail_all(struct xhci *x, int status);
/* Process a transfer event (xhci_ops.c). */
void xhci_transfer_event(struct xhci *x, const struct xhci_trb *event);

/* xhci_ops.c: the struct kshim_usb_hcd_ops implementation. */
extern const struct kshim_usb_hcd_ops xhci_hcd_ops;
void xhci_slot_release(struct xhci *x, struct xhci_slot *slot, int status);

static inline struct xhci *xhci_from_hcd(struct kshim_usb_hcd *hcd)
{
    return (struct xhci *)hcd;
}
