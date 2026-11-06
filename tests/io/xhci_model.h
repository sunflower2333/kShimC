/* SPDX-License-Identifier: MIT */
#pragma once
/*
 * A behavioural xHCI controller model for host tests (xHCI 1.2 register
 * layout and TRB/context formats): capability/operational/runtime registers,
 * HCRST and run/stop, legacy-support semaphore, Supported Protocol caps,
 * root ports (USB2 ports 1-2, USB3 ports 3-4), the command ring, one event
 * ring segment with interrupter 0 (IP/EHB/IE/INTE), doorbells and transfer
 * rings (Setup/Data/Status, Normal, Link, No Op), and simple USB devices
 * answering chapter-9 requests. DMA addresses are host pointers.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define XM_BASE 0x40000000UL
#define XM_PORTS 4U
#define XM_SLOTS 8U
/* Device table: index 1..XM_PORTS is the device on that root port, higher
 * indexes are devices behind hubs (xm_hub_attach; never reused). */
#define XM_DEVS 24U
#define XM_HUB_PORTS 7U
#define XM_REPORT_MAX 64U
#define XM_QUEUE 16U
#define XM_IFS 4U

struct xm_dev {
    bool present;
    uint8_t speed;                 /* xHCI speed id: 1 FS, 2 LS, 3 HS, 4 SS */
    const uint8_t *dev_desc, *cfg_desc, *report_desc;
    size_t dev_len, cfg_len, report_len;
    /* HID report descriptors of interfaces 1..XM_IFS-1 (report_desc is
     * interface 0) and raw string descriptors by index. */
    const uint8_t *if_report[XM_IFS];
    size_t if_report_len[XM_IFS];
    const uint8_t *strings[4];
    bool stall_set_idle;
    uint8_t config;
    unsigned set_idles, set_protocols, string_reads;
    uint8_t protocol[XM_IFS];      /* last SET_PROTOCOL value per interface */
    /* Interrupt IN reports, queued per endpoint address, served in order. */
    uint8_t queue[XM_QUEUE][XM_REPORT_MAX];
    uint8_t queue_len[XM_QUEUE], queue_ep[XM_QUEUE];
    unsigned queue_head, queue_count;
    unsigned reports_sent, interrupt_out;
    /* Bulk: OUT data is echoed on IN. */
    uint8_t bulk[1024];
    uint32_t bulk_len;
    unsigned clear_halts;
    /* USB 2.0 hub (class 9): ports, wPortStatus/wPortChange, children by
     * port (device indexes), the status-change bitmap of endpoint 0x81. */
    bool hub;
    uint8_t hub_ports, hub_desc[16], hub_desc_len;
    uint16_t port_status[XM_HUB_PORTS + 1], port_change[XM_HUB_PORTS + 1];
    uint8_t child[XM_HUB_PORTS + 1];
    uint16_t hub_bitmap;
    unsigned port_resets;
    uint8_t parent, parent_port;   /* 0 on a root port */
};

struct xm_ep {
    bool enabled;
    uint8_t state, type;
    uint16_t mps;
    uint8_t interval, max_burst;
    uint64_t deq;
    uint32_t cycle;
    struct { uint8_t bm, req; uint16_t value, index, length; } setup;
    bool stall_td;                 /* current control TD stalls */
};

struct xm_slot {
    bool enabled, addressed;
    uint8_t port, speed, address;
    uint8_t dev;                   /* device index (Xm.devs) */
    uint8_t tt_slot, tt_port;      /* slot context TT fields as programmed */
    uint32_t route;
    bool hub;
    uint8_t hub_ports;
    struct xm_ep ep[32];
};

struct xm {
    unsigned ctx_size;             /* 32 or 64 (HCCPARAMS1.CSZ) */
    bool ppc;                      /* port power control: ports unpowered after HCRST */
    unsigned scratchpads;
    uint32_t usbcmd, usbsts, dnctrl, crcr_lo, crcr_hi, dcbaap_lo, dcbaap_hi, config;
    uint32_t portsc[XM_PORTS + 1];
    uint32_t iman, imod, erstsz, erstba_lo, erstba_hi, erdp_lo, erdp_hi;
    uint32_t legsup, legctl;
    bool bios_releases;
    uint64_t cmd_deq;
    uint32_t cmd_cycle;
    uint64_t ev_base;
    uint32_t ev_size, ev_enq, ev_cycle, ev_posted, ev_pending;
    struct xm_slot slots[XM_SLOTS + 1];
    struct xm_dev devs[XM_DEVS];
    unsigned dev_count;            /* highest index handed out */
    bool ignore_commands;
    bool level_stuck;              /* IMAN.IP cannot be cleared: the INTx level stays */
    bool psc_at_run;               /* R/S 0->1 posts a Port Status Change per connected port */
    bool gone;                     /* function left the bus: reads all-ones, writes dropped */
    /* PCI glue: returns false while the function may not master DMA
     * (COMMAND.BME clear); every ring, context or buffer access is then
     * refused and counted. NULL: always allowed. */
    bool (*dma_allowed)(void);
    unsigned dma_blocked;
    /* Address Device for a LS/FS device behind a HS hub with the wrong TT
     * hub slot or port (it would not reach the device). */
    unsigned tt_errors;
    bool never_halts;              /* R/S cleared but HCHalted never set */
    /* Interrupt line. */
    void (*irq)(void *context);
    void *irq_context;
    bool irq_due;
    unsigned depth;
    /* Statistics. */
    unsigned irqs_raised, hcrst_count, run_count, stop_count, commands[64], doorbells;
    /* Deliveries after which the handler left the level asserted although
     * it could have been acknowledged (not level_stuck): an INTx storm. */
    unsigned unacked;
    unsigned td_trbs, last_td_trbs;   /* Normal TRBs of the last chained TD */
    uint32_t hi_at_reset;             /* DCBAAP/ERSTBA/ERDP upper halves at the last HCRST */
    unsigned rs_clear_before_hcrst; /* a takeover cleared R/S before HCRST */
    uint64_t scratch_seen;
    unsigned last_write_seq, rs_clear_seq, hcrst_seq, seq;
};

extern struct xm Xm;

void xm_init(unsigned ctx_size, unsigned scratchpads, bool running);
uint32_t xm_read32(void *context, uintptr_t address);
void xm_write32(void *context, uintptr_t address, uint32_t value);
void xm_attach(unsigned port, const struct xm_dev *dev);
void xm_detach(unsigned port);
/* Raise interrupter 0 now with no new event, as an IMOD-delayed interrupt
 * for events a poll already consumed does. */
void xm_raise_irq(void);
/* The INTx level: IMAN.IP with IMAN.IE and USBCMD.INTE (before the PCI
 * function's INTx Disable, which the PCI glue applies). */
bool xm_irq_line(void);
/* Queue an 8-byte report on endpoint 0x81 of the root-port device. */
void xm_report(unsigned port, const uint8_t report[8]);
/* Queue a report (1..XM_REPORT_MAX bytes) on interrupt IN endpoint `ep` of
 * device `index` and serve a pending TD. */
void xm_queue_report(unsigned index, uint8_t ep, const uint8_t *data, size_t length);
struct xm_slot *xm_slot_of_port(unsigned port);
/* The addressed slot of device `index`, or NULL. */
struct xm_slot *xm_slot_of(unsigned index);
/* Plug a device into port `port` of the hub at index `hub`; returns the new
 * device index. The hub reports the connection once the port is powered. */
unsigned xm_hub_attach(unsigned hub, unsigned port, const struct xm_dev *dev);
/* Unplug the device (and anything below it) from a hub port. */
void xm_hub_detach(unsigned hub, unsigned port);
/* A USB 2.0 hub device: descriptors for `ports` downstream ports at `speed`
 * (3 HS single-TT, 1 FS), per-port power switching. */
void xm_make_hub(struct xm_dev *dev, uint8_t speed, uint8_t ports);
