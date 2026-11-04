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

struct xm_dev {
    bool present;
    uint8_t speed;                 /* xHCI speed id */
    const uint8_t *dev_desc, *cfg_desc, *report_desc;
    size_t dev_len, cfg_len, report_len;
    bool stall_set_idle;
    uint8_t config;
    /* Interrupt IN: one report queued at a time. */
    uint8_t report[8];
    bool report_ready;
    unsigned reports_sent;
    /* Bulk: OUT data is echoed on IN. */
    uint8_t bulk[1024];
    uint32_t bulk_len;
    unsigned clear_halts;
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
    struct xm_dev devs[XM_PORTS + 1];
    bool ignore_commands;
    /* Interrupt line. */
    void (*irq)(void *context);
    void *irq_context;
    bool irq_due;
    unsigned depth;
    /* Statistics. */
    unsigned irqs_raised, hcrst_count, run_count, stop_count, commands[64], doorbells;
    unsigned td_trbs, last_td_trbs;   /* Normal TRBs of the last chained TD */
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
/* Queue an interrupt report and serve a pending interrupt-IN TD. */
void xm_report(unsigned port, const uint8_t report[8]);
struct xm_slot *xm_slot_of_port(unsigned port);
