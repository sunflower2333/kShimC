/* SPDX-License-Identifier: MIT */
#pragma once
/*
 * Qualcomm secondary interrupt controllers behind the GICv3 core
 * (kshim/irq.h), docs/interrupts.md layer 2:
 *
 *  - TLMM: the summary SPI dispatches per-GPIO handlers from INTR_STATUS.
 *  - PDC: GIC SPIs that the PDC feeds are programmed (type, enable) and
 *    restored; other PDC-parented SPIs pass straight through to the GIC.
 *  - SPMI PMIC arbiter: periph_irq dispatches per-peripheral handlers
 *    through CrDK SpmiSharedCrLib/SpmiLib.
 *
 * Every function returns 0 or a positive ERROR_* code, like kshim/irq.h.
 * Thread-context entry points serialize against the handlers themselves with
 * kshim_irq_save()/kshim_irq_restore(); handlers run with IRQs masked.
 * All of this is CONFIG_QCOM_IRQCHIP (depends on CONFIG_KSHIM_IRQ).
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kshim/irq.h>

struct CrIo;
struct kshim_resources;

/* ---- Devicetree interrupt specifiers (kernel/dt/irq.c) ------------------ */

/* The parent follows interrupt-parent, else the tree parent, up to the first
 * node with #interrupt-cells; interrupts-extended takes precedence over
 * interrupts (Linux of_irq_find_parent/of_irq_parse_one, no interrupt-map). */
#define KSHIM_DT_IRQ_CELLS 4U
struct kshim_dt_irq {
    int parent;
    unsigned count;
    uint32_t cells[KSHIM_DT_IRQ_CELLS];
};
enum kshim_dt_irq_kind {
    KSHIM_DT_IRQ_OTHER = 0,
    KSHIM_DT_IRQ_GIC,
    KSHIM_DT_IRQ_PDC,
    KSHIM_DT_IRQ_TLMM,
    KSHIM_DT_IRQ_SPMI,
};
int kshim_dt_irq_parent(const void *fdt, int node);
int kshim_dt_irq(const void *fdt, int node, unsigned index, struct kshim_dt_irq *irq);
int kshim_dt_irq_named(const void *fdt, int node, const char *name,
                       struct kshim_dt_irq *irq);
enum kshim_dt_irq_kind kshim_dt_irq_kind(const void *fdt, int controller);
/* Node of SPMI arbiter `controller`, numbered like kshim_dt_spmi(). */
int kshim_dt_spmi_controller(const void *fdt, unsigned controller);
/* Find a node whose 4-cell <sid peripheral irq type> specifier on arbiter
 * `controller` names this interrupt; *trigger receives the type cell. */
int kshim_dt_spmi_irq_find(const void *fdt, unsigned controller, uint8_t sid,
                           uint8_t peripheral, uint8_t irq, unsigned *trigger);
/* Devicetree flags to enum kshim_irq_trigger (sense bits only). */
static inline unsigned kshim_dt_irq_trigger(uint32_t flags) { return flags & 0xfU; }

/* ---- MMIO -------------------------------------------------------------- */

/* TLMM, PDC and GENI accesses use CrIoDefault() unless a host register model
 * is injected. */
void kshim_irqchip_set_io(const struct CrIo *io);
const struct CrIo *kshim_irqchip_io(void);
/* Map the PDC registers before the MMU is built (kshim_resources_init). A
 * failed mapping sets kshim_pdc_unmapped and PDC routes are then refused. */
void kshim_irqchip_map_resources(struct kshim_resources *resources);
extern bool kshim_pdc_unmapped;

/* ---- GIC or PDC parent (kernel/drivers/irqchip/resolve.c) --------------- */

/* Request interrupt `index` of `node` when its parent is the GIC (through
 * kshim_irq_from_fdt) or the PDC. default_trigger replaces a NONE type.
 * ERROR_NOT_SUPPORTED for TLMM/SPMI parents: use their cascades. */
int kshim_qcom_irq_request(const void *fdt, int node, unsigned index,
                           unsigned default_trigger, kshim_irq_handler_t handler,
                           void *context, uint32_t *intid);
int kshim_qcom_irq_request_spec(const void *fdt, const struct kshim_dt_irq *spec,
                                unsigned default_trigger, kshim_irq_handler_t handler,
                                void *context, uint32_t *intid);
int kshim_qcom_irq_free(uint32_t intid);

/* ---- PDC (kernel/drivers/irqchip/pdc.c) -------------------------------- */

/* PDC-parented specifier: <pin type> with qcom,pdc-ranges, or the Android
 * GIC form <0 spi type>. An SPI outside the PDC ranges is a pass-through.
 * Only routes the PDC actually feeds are programmed (IRQ_i_CFG type and the
 * enable bit); both are saved first and restored by kshim_pdc_free(). */
int kshim_pdc_request(const void *fdt, const struct kshim_dt_irq *spec,
                      unsigned default_trigger, kshim_irq_handler_t handler,
                      void *context, uint32_t *intid);
int kshim_pdc_free(uint32_t intid);
/* Free every remaining route; their PDC fields are back to the snapshot. */
int kshim_pdc_restore(void);

/* ---- TLMM (kernel/drivers/irqchip/tlmm.c) ------------------------------ */

/* Runs in IRQ context; edge pins are acknowledged before, level pins after
 * the handler, so the handler must deassert a level source (or mask it). */
typedef void (*kshim_gpio_irq_handler_t)(uint32_t pin, void *context);
/* Bind the runtime FDT's TLMM and its register layout (CrDK target data). */
int kshim_tlmm_irq_init(const void *fdt);
/* TLMM pin number of a pin's control-register address. */
int kshim_tlmm_pin(uintptr_t control, uint32_t *pin);
/* Save INTR_CFG, set detection/polarity for `trigger`, route to the apps
 * (KPSS) target, clear stale status and enable. The summary SPI is requested
 * with the first pin. */
int kshim_gpio_irq_request(uint32_t pin, unsigned trigger,
                           kshim_gpio_irq_handler_t handler, void *context);
/* Disable, restore the saved INTR_CFG fields and clear the status latch. */
int kshim_gpio_irq_free(uint32_t pin);
/* Mask/unmask one requested pin (e.g. a level source that cannot deassert). */
void kshim_gpio_irq_mask(uint32_t pin, bool masked);
/* Requested, unmasked and the summary not sealed by a spurious storm. */
bool kshim_gpio_irq_live(uint32_t pin);
int kshim_tlmm_irq_restore(void);

/* ---- SPMI PMIC arbiter (kernel/drivers/irqchip/spmi_pmic.c) ------------ */

/* Runs in IRQ context after the arbiter and PMIC latch were acknowledged;
 * SPMI reads of the peripheral are allowed here. */
typedef void (*kshim_spmi_irq_handler_t)(void *context);
/* Claim one QPNPINT bit of one peripheral (sid, peripheral) on arbiter
 * `controller`. The first claim takes periph_irq; SET_TYPE/POLARITY/EN bits
 * and the arbiter's per-APID PIC enable are snapshot by SpmiSharedCrLib. */
int kshim_spmi_irq_request(const void *fdt, unsigned controller, uint8_t sid,
                           uint8_t peripheral, uint8_t irq, unsigned trigger,
                           kshim_spmi_irq_handler_t handler, void *context);
int kshim_spmi_irq_free(unsigned controller, uint8_t sid, uint8_t peripheral, uint8_t irq);
/* Claimed and the arbiter cascade still delivering (not sealed). */
bool kshim_spmi_irq_live(unsigned controller);
int kshim_spmi_irq_restore(void);

/* ---- Handoff (kernel/drivers/irqchip/handoff.c) ------------------------ */

/* docs/interrupts.md step 3: TLMM pins, the SPMI arbiter (its periph_irq is
 * a PDC route on SM8150), then the PDC. Idempotent. */
int kshim_irqchip_restore(void);
