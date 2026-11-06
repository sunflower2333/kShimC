/* SPDX-License-Identifier: MIT */
#pragma once
/*
 * SM8650 SoC register models on the irq_fake.c MMIO bus (tests/irq_fake.h),
 * for host tests that run the real platform drivers (kernel/drivers/gpio,
 * clk, regulator), the real QUP/GENI drivers and the real TLMM irqchip
 * together, so every one of them sees the same registers:
 *
 *  - TLMM at 0xf100000, pad n at n * 0x1000 (pinctrl-sm8650 layout, CrDK
 *    Target/Sm8650): CFG +0 (pull 1:0, func 5:2, drive 8:6, OE 9), IO +4
 *    (IN bit 0 read from the pad, OUT bit 1), INTR_CFG +8 (EN 0, POL 1,
 *    DET 3:2 with 1 rising / 2 falling / 3 both / 0 level, RAW_STATUS_EN 4,
 *    apps target 10:8 = 3), INTR_STATUS +0xc (bit 0, written as read & ~1).
 *    A pad's level is its output while OE is set, else what a device model
 *    drives (sm_tlmm_drive), else its bias (pull-up 3, pull-down 1, none
 *    low). Edges latch on a level change with RAW_STATUS_EN set, levels
 *    relatch while asserted; the summary interrupt (SPI 208) is any pad with
 *    EN, a latched status and the apps target.
 *  - GCC at 0x100000: every SM8650 branch/vote leaf of kernel/soc's table
 *    reports CLK_OFF (halt register bit 31) from its enable bit; an RCG's
 *    UPDATE bit self-clears.
 *  - The apps RSC DRV-2 at 0x17a20000 (v3, TCS window +0xd00, the DT's
 *    tcs-config: 7 TCSes, ACTIVE at 0..2) executing triggered AMC writes
 *    and reads against a per-address vote store, and a CMD DB at
 *    0x81c60000 with VRM entries.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SM_TLMM 0xf100000UL
#define SM_TLMM_SIZE 0x300000UL
#define SM_TLMM_PINS 210U
#define SM_TLMM_SUMMARY_SPI 208U
#define SM_GCC 0x100000UL
#define SM_GCC_SIZE 0x1f4200UL
#define SM_RSC_DRV2 0x17a20000UL
#define SM_RSC_TCS (SM_RSC_DRV2 + 0xd00UL)
#define SM_CMD_DB 0x81c60000UL

/* Register the regions; call after fake_reset(). Pads start as inputs with
 * no bias and no device drive; every GCC leaf starts off. */
void sm_model_reset(void);

/* ---- TLMM ----------------------------------------------------------------- */
static inline uintptr_t sm_tlmm_cfg(unsigned pin) { return SM_TLMM + pin * 0x1000UL; }
bool sm_tlmm_level(unsigned pin);
/* The SoC drives the pad (OE set). */
bool sm_tlmm_output(unsigned pin);
/* A device drives the pad: 0, 1, or -1 to let go (bias decides). */
void sm_tlmm_drive(unsigned pin, int level);
/* Called on every pad level change (device models watch reset lines). */
extern void (*SmPadWatch)(unsigned pin, bool level);
/* Summary output towards the GIC (fake_gic_line for SPI 208). */
bool sm_tlmm_summary_line(void);

/* ---- GCC ------------------------------------------------------------------ */
/* Set a leaf on (enable bit set, CLK_OFF clear) as the boot chain left it. */
void sm_gcc_inherit_on(const char *name);
bool sm_gcc_on(const char *name);

/* ---- RPMh ----------------------------------------------------------------- */
void sm_cmd_db(const char *const *names, const uint32_t *addresses, unsigned count);
void sm_rpmh_set(uint32_t address, uint32_t value);
uint32_t sm_rpmh_get(uint32_t address);
unsigned sm_rpmh_writes(void);
/* Called after every executed AMC write (rail state changes). */
extern void (*SmRpmhWatch)(uint32_t address, uint32_t value);
