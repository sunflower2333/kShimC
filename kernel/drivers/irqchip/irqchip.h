/* SPDX-License-Identifier: MIT */
#pragma once
/* Private helpers shared by the Qualcomm cascades. */
#include <qcom_irq.h>
#include <oskal/cr_interrupt.h>
#include <Library/cr_geni.h>
#include <Library/gpio.h>

/* TLMM interrupt layout (tlmm.c): CrDK's TLMM target RegCfg[0] and, per
 * SoC, the normal (interrupt-capable) pin count and the reserved pins.
 * irqchip_test compares both with CrDK Target/<Soc>/CrGpioTarget.c. */
#define KSHIM_TLMM_SOCS 4U
struct kshim_tlmm_soc {
    const char *compatible;
    uint16_t pins;
    bool linear; /* base + pin * 0x1000; otherwise Android binding offsets */
    uint16_t reserved[8];
    uint8_t reserved_count;
};
extern const GpioConfigRegInfo KshimTlmmLayout;
extern const struct kshim_tlmm_soc KshimTlmmSocs[KSHIM_TLMM_SOCS];

static inline bool irqchip_cr_trigger(unsigned trigger, CR_INTERRUPT_TRIGGER_TYPE *type)
{
    switch (trigger) {
    case KSHIM_IRQ_TYPE_EDGE_RISING: *type = CR_INTERRUPT_TRIGGER_EDGE_RISING; return true;
    case KSHIM_IRQ_TYPE_EDGE_FALLING: *type = CR_INTERRUPT_TRIGGER_EDGE_FALLING; return true;
    case KSHIM_IRQ_TYPE_EDGE_BOTH: *type = CR_INTERRUPT_TRIGGER_EDGE_BOTH; return true;
    case KSHIM_IRQ_TYPE_LEVEL_HIGH: *type = CR_INTERRUPT_TRIGGER_LEVEL_HIGH; return true;
    case KSHIM_IRQ_TYPE_LEVEL_LOW: *type = CR_INTERRUPT_TRIGGER_LEVEL_LOW; return true;
    default: return false;
    }
}

static inline unsigned irqchip_kshim_trigger(CR_INTERRUPT_TRIGGER_TYPE type)
{
    switch (type) {
    case CR_INTERRUPT_TRIGGER_EDGE_RISING: return KSHIM_IRQ_TYPE_EDGE_RISING;
    case CR_INTERRUPT_TRIGGER_EDGE_FALLING: return KSHIM_IRQ_TYPE_EDGE_FALLING;
    case CR_INTERRUPT_TRIGGER_EDGE_BOTH: return KSHIM_IRQ_TYPE_EDGE_BOTH;
    case CR_INTERRUPT_TRIGGER_LEVEL_HIGH: return KSHIM_IRQ_TYPE_LEVEL_HIGH;
    case CR_INTERRUPT_TRIGGER_LEVEL_LOW: return KSHIM_IRQ_TYPE_LEVEL_LOW;
    default: return KSHIM_IRQ_TYPE_NONE;
    }
}

static inline uint32_t irqchip_read(const struct CrIo *io, uintptr_t address)
{ return io->read32(io->context, address); }
static inline void irqchip_write(const struct CrIo *io, uintptr_t address, uint32_t value)
{ io->write32(io->context, address, value); }
