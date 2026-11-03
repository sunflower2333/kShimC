/* SPDX-License-Identifier: MIT */
/*
 * Qualcomm PDC routes. The PDC sits in front of a fixed set of GIC SPIs; for
 * those it converts falling/low to rising/high and gates forwarding with an
 * enable bit. CrDK PdcLib supplies range validation and the register layout
 * (PdcValidateInterrupt: bank enable before v3.2, IRQ_i_CFG bit 3 after);
 * the snapshot, programming and restore go through the injectable CrIo.
 *
 * Policy (docs/interrupts.md): kShimC routes TLMM GPIOs through the TLMM
 * summary SPI, not their PDC wake pins, so the PDC is programmed only for
 * interrupts whose DT parent is the PDC and whose SPI the PDC feeds - on
 * SM8150 that is the SPMI arbiter's periph_irq (PDC pin 1, SPI 481). The
 * TLMM summary (SPI 208) and the QUP engines are PDC pass-throughs and leave
 * the PDC untouched.
 */
#include "irqchip.h"
#include <dt.h>
#include <Library/pdc.h>
#include <string.h>

#define PDC_ROUTES 8U
#define PDC_TYPE_MASK 7U
/* Android pdc-<soc>.c pin tables as carried by CrDK
 * Target/Sm8150/CrTarget.c, which is UEFI-only and cannot be linked here.
 * Upstream DTs describe the same data with qcom,pdc-ranges. */
static const struct {
    const char *compatible;
    uint16_t count;
    PdcRange ranges[3];
} Builtin[] = {
    {"qcom,pdc-sm8150", 3, {{0, 480, 94}, {94, 609, 31}, {125, 63, 1}}},
};

struct pdc_route {
    bool used, programmed;
    uint32_t intid;
    uint16_t pin;
    PDC_INTERRUPT_REGISTERS regs;
    uint32_t saved_config, saved_enable;
};

bool kshim_pdc_unmapped;

static struct {
    const void *fdt;
    int node;
    bool ready;
    PdcDeviceContext ctx;
    struct pdc_route routes[PDC_ROUTES];
} Pdc = {.node = -1};

static bool routes_live(void)
{
    for (unsigned i = 0; i < PDC_ROUTES; ++i) if (Pdc.routes[i].used) return true;
    return false;
}

static int pdc_ranges(const void *f, int node, PdcDeviceContext *ctx)
{
    int len;
    const fdt32_t *p = fdt_getprop(f, node, "qcom,pdc-ranges", &len);
    if (p) {
        if (len <= 0 || len % 12 || len / 12 > PDC_RANGE_COUNT_MAX) return -1;
        ctx->PdcRangeCount = (UINT16)(len / 12);
        for (int i = 0; i < len / 12; ++i) {
            uint32_t pin = fdt32_to_cpu(p[3 * i]), spi = fdt32_to_cpu(p[3 * i + 1]),
                     count = fdt32_to_cpu(p[3 * i + 2]);
            if (pin > UINT16_MAX || spi > UINT16_MAX || count > UINT16_MAX) return -1;
            ctx->PdcRegion[i] = (PdcRange){(UINT16)pin, (UINT16)spi, (UINT16)count};
        }
        return 0;
    }
    for (unsigned i = 0; i < sizeof(Builtin) / sizeof(Builtin[0]); ++i) {
        if (fdt_node_check_compatible(f, node, Builtin[i].compatible)) continue;
        ctx->PdcRangeCount = Builtin[i].count;
        memcpy(ctx->PdcRegion, Builtin[i].ranges, Builtin[i].count * sizeof(PdcRange));
        return 0;
    }
    return -1;
}

static int pdc_init(const void *f, int node)
{
    /* Re-read the DT whenever no route depends on the current snapshot. */
    if (routes_live()) return Pdc.ready && Pdc.fdt == f && Pdc.node == node ? 0 : ERROR_DEVICE_ERROR;
    if (kshim_pdc_unmapped) return ERROR_NOT_SUPPORTED;
    struct dt_range reg;
    PdcDeviceContext ctx = {0};
    if (dt_reg(f, node, 0, &reg) || reg.base > UINTPTR_MAX || reg.size > SIZE_MAX ||
        pdc_ranges(f, node, &ctx)) return ERROR_NOT_FOUND;
    ctx.BaseAddress = (UINTN)reg.base;
    ctx.Size = (UINTN)reg.size;
    /* Never let PdcLibInit clear routes the firmware or another EE owns. The
     * version register only exists in the larger (>= v3.x) apertures. */
    ctx.PreserveInheritedInterrupts = TRUE;
    ctx.RequireVersion = reg.size > 0x1000U;
    PdcDeviceContext *p = &Pdc.ctx;
    Pdc.ctx = ctx;
    if (CR_ERROR(PdcLibInit(&p))) { Pdc.ready = false; return ERROR_NOT_SUPPORTED; }
    Pdc.fdt = f; Pdc.node = node; Pdc.ready = true;
    return 0;
}

static void route_restore(const struct CrIo *io, struct pdc_route *r)
{
    if (!r->programmed) return;
    uintptr_t cfg = r->regs.ConfigAddress, en = r->regs.EnableAddress;
    irqchip_write(io, cfg, (irqchip_read(io, cfg) & ~PDC_TYPE_MASK) | (r->saved_config & PDC_TYPE_MASK));
    irqchip_write(io, en, (irqchip_read(io, en) & ~r->regs.EnableMask) | (r->saved_enable & r->regs.EnableMask));
    r->programmed = false;
}

int kshim_pdc_request(const void *fdt, const struct kshim_dt_irq *spec,
                      unsigned default_trigger, kshim_irq_handler_t handler,
                      void *context, uint32_t *intid)
{
    if (!fdt || !spec || !handler || !intid) return ERROR_INVALID_PARAMETER;
    int status = pdc_init(fdt, spec->parent);
    if (status) return status;
    UINT16 pin = 0, gic = 0;
    unsigned trigger;
    bool routed;
    if (spec->count == 3) {          /* Android: <0 spi flags> in GIC form. */
        if (spec->cells[0] != 0 || spec->cells[1] > 987U) return ERROR_INVALID_PARAMETER;
        gic = (UINT16)KSHIM_IRQ_SPI(spec->cells[1]);
        CR_STATUS s = PdcGetPdcPinFromGicIrq(&Pdc.ctx, gic, &pin);
        if (s != CR_SUCCESS && s != CR_NOT_FOUND) return ERROR_INVALID_PARAMETER;
        routed = s == CR_SUCCESS;
        trigger = kshim_dt_irq_trigger(spec->cells[2]);
    } else if (spec->count == 2) {   /* Upstream: <pin flags>. */
        if (spec->cells[0] > UINT16_MAX) return ERROR_INVALID_PARAMETER;
        pin = (UINT16)spec->cells[0];
        if (PdcGetGicIrqFromPdcPin(&Pdc.ctx, pin, &gic) != CR_SUCCESS) return ERROR_NOT_FOUND;
        routed = true;
        trigger = kshim_dt_irq_trigger(spec->cells[1]);
    } else return ERROR_INVALID_PARAMETER;
    if (!trigger) trigger = default_trigger;

    struct pdc_route *r = NULL;
    for (unsigned i = 0; i < PDC_ROUTES; ++i) {
        if (Pdc.routes[i].used && Pdc.routes[i].intid == gic) return ERROR_DEVICE_ERROR;
        if (!Pdc.routes[i].used && !r) r = &Pdc.routes[i];
    }
    if (!r) return ERROR_NOT_SUPPORTED;

    if (!routed) {
        /* The GIC alone cannot invert a falling or low line. */
        if (trigger == KSHIM_IRQ_TYPE_EDGE_FALLING || trigger == KSHIM_IRQ_TYPE_LEVEL_LOW ||
            trigger == KSHIM_IRQ_TYPE_EDGE_BOTH) return ERROR_NOT_SUPPORTED;
        status = kshim_irq_request(gic, trigger, handler, context);
        if (status) return status;
        *r = (struct pdc_route){.used = true, .intid = gic};
        *intid = gic;
        return 0;
    }

    const struct CrIo *io = kshim_irqchip_io();
    CR_INTERRUPT_TRIGGER_TYPE type;
    if (!io) return ERROR_DEVICE_ERROR;
    PDC_INTERRUPT_REGISTERS regs;
    if (trigger) {
        if (!irqchip_cr_trigger(trigger, &type)) return ERROR_INVALID_PARAMETER;
        if (PdcValidateInterrupt(&Pdc.ctx, pin, type, &regs) != CR_SUCCESS) return ERROR_INVALID_PARAMETER;
    } else {
        /* No type anywhere: keep the inherited PDC type and give the GIC the
         * matching sense (the PDC already converts low/falling). */
        if (PdcValidateInterrupt(&Pdc.ctx, pin, CR_INTERRUPT_TRIGGER_LEVEL_HIGH, &regs) != CR_SUCCESS)
            return ERROR_INVALID_PARAMETER;
        uint32_t current = irqchip_read(io, regs.ConfigAddress) & PDC_TYPE_MASK;
        regs.PdcType = current;
        regs.GicType = (current & 2U) ? CR_INTERRUPT_TRIGGER_EDGE_RISING : CR_INTERRUPT_TRIGGER_LEVEL_HIGH;
    }
    unsigned long flags = kshim_irq_save();
    *r = (struct pdc_route){.used = true, .programmed = true, .intid = gic, .pin = pin, .regs = regs,
        .saved_config = irqchip_read(io, regs.ConfigAddress),
        .saved_enable = irqchip_read(io, regs.EnableAddress)};
    irqchip_write(io, regs.ConfigAddress,
                  (r->saved_config & ~PDC_TYPE_MASK) | (regs.PdcType & PDC_TYPE_MASK));
    irqchip_write(io, regs.EnableAddress, irqchip_read(io, regs.EnableAddress) | regs.EnableMask);
    kshim_irq_restore(flags);
    status = kshim_irq_request(gic, irqchip_kshim_trigger(regs.GicType), handler, context);
    if (status) {
        flags = kshim_irq_save();
        route_restore(io, r);
        *r = (struct pdc_route){0};
        kshim_irq_restore(flags);
        return status;
    }
    *intid = gic;
    return 0;
}

int kshim_pdc_free(uint32_t intid)
{
    for (unsigned i = 0; i < PDC_ROUTES; ++i) {
        struct pdc_route *r = &Pdc.routes[i];
        if (!r->used || r->intid != intid) continue;
        /* Stop delivery before the PDC stops forwarding, so the GIC is not
         * left with a pending state the payload never asked for. */
        int status = kshim_irq_free(intid);
        if (status) return status;
        if (r->programmed) {
            const struct CrIo *io = kshim_irqchip_io();
            if (!io) return ERROR_DEVICE_ERROR;
            unsigned long flags = kshim_irq_save();
            route_restore(io, r);
            kshim_irq_restore(flags);
        }
        *r = (struct pdc_route){0};
        return 0;
    }
    return ERROR_NOT_FOUND;
}

int kshim_pdc_restore(void)
{
    int first = 0;
    for (unsigned i = 0; i < PDC_ROUTES; ++i) {
        if (!Pdc.routes[i].used) continue;
        int status = kshim_pdc_free(Pdc.routes[i].intid);
        if (status && !first) first = status;
    }
    if (!first) Pdc.ready = false;
    return first;
}
