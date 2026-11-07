/* SPDX-License-Identifier: MIT */
/*
 * Handoff of the Qualcomm cascades (docs/interrupts.md step 3) and their
 * MMU resources. Children before parents: TLMM pins and their summary SPI,
 * then the SPMI arbiter (its periph_irq is a PDC route on SM8150), then any
 * PDC route left. Each step stops GIC delivery before restoring hardware.
 */
#include <qcom_irq.h>
#include <qcom_dt.h>
#if !defined(KSHIM_HOST_TEST)
#include <config.h>
#endif

int kshim_irqchip_restore(void)
{
    int first = kshim_tlmm_irq_restore();
#if CONFIG_QCOM_SPMI
    int status = kshim_spmi_irq_restore();
    if (status && !first) first = status;
#endif
    int pdc = kshim_pdc_restore();
    return first ? first : pdc;
}

void kshim_irqchip_map_resources(struct kshim_resources *r)
{
    /* Optional: without a mapping the PDC routes stay unprogrammed and the
     * affected cascades (SPMI on SM8150) fall back to polling. */
    for (int n = dt_next_node(r->fdt, -1); n >= 0; n = dt_next_node(r->fdt, n)) {
        struct dt_range reg;
        if (kshim_dt_irq_kind(r->fdt, n) != KSHIM_DT_IRQ_PDC || !dt_available(r->fdt, n) ||
            dt_reg(r->fdt, n, 0, &reg)) continue;
        if (kshim_resource_map(r, reg.base, reg.size, KSHIM_MMU_DEVICE,
                               KSHIM_MMU_READ | KSHIM_MMU_WRITE)) kshim_pdc_unmapped = true;
    }
}
