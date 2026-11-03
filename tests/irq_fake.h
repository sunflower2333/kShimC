/* SPDX-License-Identifier: MIT */
/*
 * Host fakes for the Qualcomm interrupt cascades: a GIC core behind the
 * kshim/irq.h API (kshim_irq_from_fdt() is the core's own, from
 * kernel/dt/irq.c), a sparse MMIO bus behind struct CrIo, and
 * register models of the SM8150 TLMM, PDC and SPMI PMIC arbiter v5 + PMIC.
 * Addresses are the physical ones from the real andromeda/hdk8150 DTs.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kshim/irq.h>
#include <Library/cr_geni.h>
#include <Library/spmi.h>

/* ---- GIC core --------------------------------------------------------- */
struct fake_gic_irq {
    bool requested, enabled;
    unsigned trigger, requests, frees, disables;
    kshim_irq_handler_t handler;
    void *context;
    bool (*line)(void);
    unsigned delivered;
};
extern struct fake_gic_irq FakeGic[1020];
extern bool FakeIrqReady;
extern int FakeIrqMasked;      /* kshim_irq_save nesting depth */
extern bool FakeInHandler;
/* Level line of an INTID, sampled by fake_gic_run(). */
void fake_gic_line(uint32_t intid, bool (*line)(void));
/* Deliver every asserted, enabled line until idle; -1 after a storm. */
int fake_gic_run(void);

/* ---- MMIO and time ---------------------------------------------------- */
typedef uint32_t (*fake_read_fn)(uintptr_t address);
typedef void (*fake_write_fn)(uintptr_t address, uint32_t value);
/* Hooks for [base, base + size); without one the backing store is used. */
void fake_mmio_region(uintptr_t base, size_t size, fake_read_fn read, fake_write_fn write);
uint32_t fake_peek(uintptr_t address);
void fake_poke(uintptr_t address, uint32_t value);
unsigned fake_writes_in(uintptr_t base, size_t size);
extern const struct CrIo FakeIo;
extern uint64_t FakeNowUs;
void fake_reset(void);

/* ---- SM8150 TLMM (andromeda/hdk8150 DT: 0x03000000) -------------------- */
#define FAKE_TLMM_SUMMARY KSHIM_IRQ_SPI(208)
/* Model a pin whose control register is at `control`; level is the input. */
void fake_tlmm_pin(uintptr_t control, bool level);
void fake_tlmm_set(uintptr_t control, bool level);
bool fake_tlmm_line(void);

/* ---- SM8150 PDC (0x0b220000 + 0x400) ----------------------------------- */
#define FAKE_PDC_BASE 0x0b220000U
#define FAKE_PDC_CFG(pin) (FAKE_PDC_BASE + 0x110U + 4U * (pin))
#define FAKE_PDC_ENABLE(pin) (FAKE_PDC_BASE + 0x10U + 4U * ((pin) / 32U))

/* ---- SPMI arbiter v5 + PMIC ------------------------------------------- */
#define FAKE_SPMI_PERIPH_IRQ KSHIM_IRQ_SPI(481)
#define FAKE_SPMI_EE 0U
/* Bring up the arbiter from the DT's controller 0 regions with a fake Io;
 * kshim_qcom_spmi_context(0) returns it. APID n is ppids[n]; EE 0 owns all.
 * Writes are refused (FakeSpmiBadWrites) outside granted peripherals. */
SpmiDeviceContext *fake_spmi_init(const void *fdt, const uint16_t *ppids, unsigned count);
uint8_t *fake_pmic(uint8_t sid, uint16_t address);
/* Drive RT_STS bit `irq` of (sid, peripheral) and run PMIC detection. */
void fake_pmic_rt(uint8_t sid, uint8_t peripheral, uint8_t irq, bool level);
uint32_t fake_spmi_acc_enable(uint16_t apid);
void fake_spmi_set_acc_enable(uint16_t apid, uint32_t value);
void fake_spmi_raise(uint16_t apid, uint8_t bits); /* arbiter-side pending */
bool fake_spmi_line(void);
extern unsigned FakeSpmiGrantRefs, FakeSpmiBadWrites;
