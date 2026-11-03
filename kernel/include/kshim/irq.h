/* SPDX-License-Identifier: MIT */
#pragma once
/*
 * Kernel interrupt API (GICv3). kShimC runs interrupt-driven: the boot CPU
 * takes device and timer interrupts, and waits sleep in WFI instead of
 * spinning. Polling remains a per-driver fallback when an interrupt cannot be
 * resolved from the device tree.
 *
 * INTIDs are GIC interrupt IDs: SGI 0-15, PPI 16-31, SPI 32-1019.
 * Return values: 0 on success, otherwise a positive ERROR_* code.
 */
#include <stdbool.h>
#include <stdint.h>

#include "error_no.h"

#define KSHIM_IRQ_SGI(n) ((uint32_t)(n))
#define KSHIM_IRQ_PPI(n) (16u + (uint32_t)(n))
#define KSHIM_IRQ_SPI(n) (32u + (uint32_t)(n))

/* Devicetree "interrupts" flag values (dt-bindings/interrupt-controller/irq.h).
 * The GIC itself only implements LEVEL_HIGH and EDGE_RISING; inverted
 * polarities belong to a secondary controller (TLMM, PDC, PMIC). */
enum kshim_irq_trigger {
    KSHIM_IRQ_TYPE_NONE         = 0,
    KSHIM_IRQ_TYPE_EDGE_RISING  = 1,
    KSHIM_IRQ_TYPE_EDGE_FALLING = 2,
    KSHIM_IRQ_TYPE_EDGE_BOTH    = 3,
    KSHIM_IRQ_TYPE_LEVEL_HIGH   = 4,
    KSHIM_IRQ_TYPE_LEVEL_LOW    = 8,
};

/* Called in IRQ context on the CPU that took the interrupt, with interrupts
 * masked. Must not block. The core performs EOI after the handler returns. */
typedef void (*kshim_irq_handler_t)(uint32_t intid, void *context);

/* Boot CPU, once, after the MMU map covers the GIC: discover the GICv3 from
 * the runtime FDT ("arm,gic-v3": GICD, GICR regions, redistributor-stride),
 * save the state left by the previous bootloader, initialise the distributor,
 * this CPU's redistributor and CPU interface, install vectors and unmask IRQs. */
int kshim_irq_init(const void *fdt);
/* Each secondary CPU that should take interrupts (PPIs/SGIs), on that CPU. */
int kshim_irq_cpu_init(void);
bool kshim_irq_ready(void);

/* Register a handler. SPIs are routed to the boot CPU; SGIs/PPIs are banked
 * and apply to the calling CPU. trigger is an enum kshim_irq_trigger value
 * (NONE keeps the current configuration). The interrupt is enabled on return. */
int kshim_irq_request(uint32_t intid, unsigned trigger, kshim_irq_handler_t handler,
                      void *context);
int kshim_irq_free(uint32_t intid);
void kshim_irq_enable(uint32_t intid);
void kshim_irq_disable(uint32_t intid);

/* Resolve entry `index` of a node's "interrupts"/"interrupts-extended" when
 * its parent is the GIC. Returns ERROR_NOT_SUPPORTED when the parent is a
 * secondary controller (TLMM, PDC, SPMI); the caller then cascades through
 * that controller's driver or falls back to polling. */
int kshim_irq_from_fdt(const void *fdt, int node, int index, uint32_t *intid,
                       unsigned *trigger);

/* Local CPU interrupt masking for short critical sections shared with IRQ
 * handlers. Nestable. */
unsigned long kshim_irq_save(void);
void kshim_irq_restore(unsigned long flags);

/* Raise SGI `sgi` (0-15) on the CPU with affinity `mpidr`. */
int kshim_irq_send_sgi(uint32_t sgi, uint64_t mpidr);

/* Payload handoff, last interrupt step, after every driver has quiesced and
 * restored its own controller (TLMM, SPMI, PDC, DWC3, ...): stop the kShim
 * timer, disable, clear pending and active state of every interrupt kShimC
 * enabled, restore the saved GIC/CPU-interface state and mask DAIF.IRQ.
 * Linux and UEFI payloads start as if kShimC never touched the GIC. */
int kshim_irq_teardown(void);
