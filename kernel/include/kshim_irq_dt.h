/* SPDX-License-Identifier: MIT */
#pragma once
/*
 * Devicetree side of the interrupt core: the GICv3 node, the generic timer
 * PPIs and interrupt specifiers. No hardware access; host-tested against the
 * real SM8150 runtime DTBs. kshim_irq_from_fdt() (kshim/irq.h) is built on
 * these helpers.
 */
#include <stdint.h>

#include <dt.h>

#define KSHIM_GIC_MAX_REDIST_REGIONS 4U
#define KSHIM_GIC_FRAME_SIZE UINT64_C(0x10000)

struct kshim_gic_desc {
    int node;
    struct dt_range dist;
    struct dt_range redist[KSHIM_GIC_MAX_REDIST_REGIONS];
    unsigned redist_count;
    /* 0: derive per frame from GICR_TYPER.VLPIS (2 or 4 64K frames). */
    uint64_t redist_stride;
};

/* Generic timer interrupts in binding order; 0 when absent. */
enum {
    KSHIM_TIMER_SEC_PHYS,
    KSHIM_TIMER_PHYS,
    KSHIM_TIMER_VIRT,
    KSHIM_TIMER_HYP_PHYS,
    KSHIM_TIMER_HYP_VIRT,
    KSHIM_TIMER_COUNT,
};
struct kshim_timer_desc {
    int node;
    uint32_t intid[KSHIM_TIMER_COUNT];
};

/* The available "arm,gic-v3" node: GICD, #redistributor-regions GICR
 * regions and redistributor-stride. 0 or a positive ERROR_* code. */
int kshim_dt_gic(const void *fdt, struct kshim_gic_desc *gic);

/* The "arm,armv8-timer" PPIs. Qualcomm trees inherit the PDC as the root
 * interrupt-parent; PPIs never pass through it, so a 3-cell parent whose
 * interrupt-parent chain ends at the GIC is accepted for PPIs here. */
int kshim_dt_arch_timer(const void *fdt, struct kshim_timer_desc *timer);
