/* SPDX-License-Identifier: MIT */
/*
 * GIC- or PDC-parented interrupts. The core resolves GIC parents
 * (kshim_irq_from_fdt); it reports ERROR_NOT_SUPPORTED for secondary
 * controllers, and a PDC parent is handled here. TLMM and SPMI children go
 * through their own cascades.
 */
#include <qcom_irq.h>
#include <Library/cr_geni.h>

static const struct CrIo *Io;

void kshim_irqchip_set_io(const struct CrIo *io) { Io = io; }

const struct CrIo *kshim_irqchip_io(void)
{
    if (!Io) Io = CrIoDefault();
    return CrIoValid(Io) ? Io : NULL;
}

int kshim_qcom_irq_request_spec(const void *fdt, const struct kshim_dt_irq *spec,
                                unsigned default_trigger, kshim_irq_handler_t handler,
                                void *context, uint32_t *intid)
{
    if (!fdt || !spec || !handler || !intid) return ERROR_INVALID_PARAMETER;
    switch (kshim_dt_irq_kind(fdt, spec->parent)) {
    case KSHIM_DT_IRQ_PDC:
        return kshim_pdc_request(fdt, spec, default_trigger, handler, context, intid);
    case KSHIM_DT_IRQ_GIC: {
        /* <type number flags>: SPI 0, PPI 1. Only reached for an
         * interrupts-extended entry the core does not index for us. */
        if (spec->count != 3 || spec->cells[0] > 1 ||
            spec->cells[1] > (spec->cells[0] ? 15U : 987U)) return ERROR_INVALID_PARAMETER;
        uint32_t id = spec->cells[0] ? KSHIM_IRQ_PPI(spec->cells[1]) : KSHIM_IRQ_SPI(spec->cells[1]);
        unsigned trigger = kshim_dt_irq_trigger(spec->cells[2]);
        int status = kshim_irq_request(id, trigger ? trigger : default_trigger, handler, context);
        if (!status) *intid = id;
        return status;
    }
    default:
        return ERROR_NOT_SUPPORTED;
    }
}

int kshim_qcom_irq_request(const void *fdt, int node, unsigned index,
                           unsigned default_trigger, kshim_irq_handler_t handler,
                           void *context, uint32_t *intid)
{
    if (!fdt || !handler || !intid) return ERROR_INVALID_PARAMETER;
    uint32_t id;
    unsigned trigger;
    int status = kshim_irq_from_fdt(fdt, node, (int)index, &id, &trigger);
    if (!status) {
        status = kshim_irq_request(id, trigger ? trigger : default_trigger, handler, context);
        if (!status) *intid = id;
        return status;
    }
    if (status != ERROR_NOT_SUPPORTED) return status;
    struct kshim_dt_irq spec;
    if (kshim_dt_irq(fdt, node, index, &spec)) return ERROR_NOT_FOUND;
    if (kshim_dt_irq_kind(fdt, spec.parent) != KSHIM_DT_IRQ_PDC) return ERROR_NOT_SUPPORTED;
    return kshim_pdc_request(fdt, &spec, default_trigger, handler, context, intid);
}

int kshim_qcom_irq_free(uint32_t intid)
{
    /* A PDC route (pass-through or programmed) is owned by the PDC driver. */
    int status = kshim_pdc_free(intid);
    return status == ERROR_NOT_FOUND ? kshim_irq_free(intid) : status;
}
