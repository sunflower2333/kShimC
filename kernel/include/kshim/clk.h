/* SPDX-License-Identifier: MIT */
#pragma once
/*
 * Qualcomm clock-controller leaf facts: branch clocks (CBCR or APCS vote
 * bits), their RCGs, GDSC power domains and BCR resets (CONFIG_KSHIM_CLK,
 * selected by KSHIM_QUP and KSHIM_PCI_QCOM).
 * Owner: sm8650 (kernel/drivers/clk/; SoC tables in kernel/soc/). See
 * docs/io-stack.md.
 *
 * Lookups are table driven per clock-controller compatible:
 *  - SM8150/SM8250 Android DTs: the CrDK Android tables (CrAndroidClock,
 *    kshim_dt_soc) keyed by Android clock IDs;
 *  - SM8650 mainline DTs: a kShimC table keyed by the dt-bindings IDs of
 *    qcom,sm8650-gcc / qcom,sm8650-tcsr, with offsets taken as hardware
 *    facts (provenance recorded next to the table);
 *  - by name ("gcc", "gcc_pcie_1_aux_clk") for CrDK PcieTargetClock /
 *    PcieTargetReset entries.
 *
 * Policy: prefer the inherited state. enable() writes nothing when the
 * branch already runs (CLK_OFF clear); disable() only undoes kShimC's own
 * enable. Rates: only kshim_clk_set_rate_cxo() (select the 19.2 MHz CXO
 * undivided source, DFS-table aware, the CrDK ClockLib clock_poll policy) is
 * offered; an RCG that is already running is never retuned.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "error_no.h"

enum kshim_clk_kind {
    KSHIM_CLK_BRANCH = 1,  /* CBCR: enable bit 0, CLK_OFF bit 31 */
    KSHIM_CLK_VOTE,        /* APCS vote register bit; halt = the branch CBCR */
    KSHIM_CLK_GDSC,        /* GDSCR: SW_COLLAPSE bit 0, PWR_ON bit 31 */
    KSHIM_CLK_RESET,       /* BCR: BLK_ARES bit 0 */
};

enum kshim_clk_halt {
    KSHIM_CLK_HALT_NONE = 0,  /* no status bit; delay only */
    KSHIM_CLK_HALT_BIT31,     /* CLK_OFF / PWR_ON at bit 31 of halt_reg */
};

/* Facts for one leaf, absolute addresses after lookup. */
struct kshim_clk_desc {
    uint8_t kind;          /* enum kshim_clk_kind */
    uint8_t halt_check;    /* enum kshim_clk_halt */
    uintptr_t enable_reg;  /* CBCR, vote register, GDSCR or BCR */
    uint32_t enable_mask;
    uintptr_t halt_reg;    /* CBCR for vote clocks; == enable_reg for CBCR/GDSC */
    uintptr_t rcg;         /* CMD_RCGR of the source RCG, 0 when none/shared */
};

/* A reference-counted handle from the clock core's pool. */
struct kshim_clk;

/* Bind clock controllers present in the FDT to their SoC tables. */
int kshim_clk_init(const void *fdt);
struct kshim_resources;
/* Map the "reg" window of every clock controller referenced by an enabled
 * consumer (gcc, tcsr; device, RW). */
int kshim_clk_map_resources(struct kshim_resources *resources);

/* Resolve `name` in the consumer's "clock-names" (index 0 when name is
 * NULL), or the `index`-th entry of "power-domains"/"resets" for the
 * GDSC/reset variants. ERROR_NOT_FOUND when absent, ERROR_NOT_SUPPORTED for
 * a provider or ID without facts. Repeated gets of one leaf share a handle. */
int kshim_clk_get(const void *fdt, int consumer, const char *name, struct kshim_clk **clk);
int kshim_clk_get_power_domain(const void *fdt, int consumer, unsigned index,
                               struct kshim_clk **gdsc);
int kshim_clk_get_reset(const void *fdt, int consumer, const char *name, struct kshim_clk **reset);
/* By controller label and Linux clock/reset/GDSC name, for CrDK target data. */
int kshim_clk_get_by_name(const char *controller, const char *name, struct kshim_clk **clk);

const struct kshim_clk_desc *kshim_clk_desc(const struct kshim_clk *clk);
bool kshim_clk_is_on(const struct kshim_clk *clk);

/* Thread context. Branch/vote/GDSC: refcounted on/off, inherited-on
 * branches are not written. Reset: assert/deassert the BCR (not counted). */
int kshim_clk_enable(struct kshim_clk *clk);
int kshim_clk_disable(struct kshim_clk *clk);
int kshim_clk_reset(struct kshim_clk *reset, bool asserted);
/* Select CXO (19.2 MHz) undivided on the branch's RCG: a verified DFS entry
 * when DFS is enabled (*dfs_index set), else CFG_RCGR SRC_SEL=0, divider 1.
 * ERROR_BUSY when the RCG feeds an active engine. Restored at handoff. */
int kshim_clk_set_rate_cxo(struct kshim_clk *clk, uint8_t *dfs_index);

/* Handoff: every enable bit, RCG configuration and GDSC kShimC changed goes
 * back to its snapshot, newest first. Resets are left deasserted. */
int kshim_clk_restore_all(void);
