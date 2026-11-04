/* SPDX-License-Identifier: MIT */
/*
 * Qualcomm clock-controller leaves (kshim/clk.h): branch and vote clocks,
 * GDSC power domains, BCR resets and the CXO-only RCG rate change.
 *
 * Register facts:
 *  - Branch CBCR: CLK_ENABLE bit 0, CLK_OFF bit 31; vote clocks set a bit in
 *    an APCS vote register and report through their CBCR CLK_OFF.
 *  - GDSC: GDSCR SW_COLLAPSE bit 0 and PWR_ON bit 31; a votable GDSC is
 *    powered by clearing its bit in the collapse-vote register (SM8650
 *    pcie_1_gdsc: 0x5214c bit 1). Both power on by clearing the bit.
 *  - BCR: BLK_ARES bit 0 (or the listed bit).
 *  - RCG2: CMD_RCGR UPDATE bit 0 / ROOT_OFF bit 31; CFG_RCGR (+4) SRC_DIV
 *    4:0, SRC_SEL 10:8, MODE 13:12; CMD_DFSR (+0x14) DFS_EN bit 0 and
 *    PERF_DFSR(n) at +0x1c + 4n. CXO undivided is SRC_SEL 0, MODE 0,
 *    SRC_DIV <= 1, which is CrDK ClockLib clock_poll's selection policy.
 * The SoC rows are kshim/soc_tables.h (CrAndroidClock, sm8650_tables.c).
 *
 * Policy: an inherited running leaf is never written; disable() only undoes
 * kShimC's own enable; every changed word is snapshot first and
 * kshim_clk_restore_all() puts it back newest first.
 */
#include <kshim/clk.h>
#include <kshim/io.h>
#include <kshim/soc_tables.h>
#include "../gpio/internal.h"

#include <qcom_dt.h>
#include <stdio.h>
#include <string.h>

#define CLK_POOL 48U
#define CLK_CONTROLLERS 4U
#define CLK_OFF (1U << 31)
#define RCG_UPDATE 1U
#define RCG_CFG_FIELDS (0x1fU | (7U << 8) | (3U << 12))
#define RCG_DFS_LEVELS 8U
#define BRANCH_TIMEOUT_US 200U
#define GDSC_TIMEOUT_US 1000U
#define RCG_TIMEOUT_US 500U

struct kshim_clk {
    bool used;
    const struct kshim_soc_clock_controller *ctrl;
    const char *name;
    uint8_t space;
    uint32_t id;
    struct kshim_clk_desc desc;
    unsigned refs;
    bool ours;               /* kShimC wrote the enable bit */
    bool saved, rcg_saved, reset_touched;
    uint32_t enable_word, rcg_cfg;
    uint32_t seq, rcg_seq;
};

static struct {
    const void *fdt;
    struct { int node; uintptr_t base; size_t size; const struct kshim_soc_clock_controller *ctrl; }
        controllers[CLK_CONTROLLERS];
    unsigned count;
    uint32_t seq;
    struct kshim_clk pool[CLK_POOL];
} C;

static uint32_t rd(uintptr_t a) { return kshim_platform_read(a); }
static void wr(uintptr_t a, uint32_t v) { kshim_platform_write(a, v); }
static uint64_t now_us(void)
{
    const struct CrIo *io = kshim_platform_io();
    return io->now_us ? io->now_us(io->context) : 0;
}
static void step(void)
{
    const struct CrIo *io = kshim_platform_io();
    if (io->delay_us) io->delay_us(io->context, 1);
}
static int wait_field(uintptr_t a, uint32_t mask, uint32_t value, uint32_t timeout_us)
{
    uint64_t start = now_us();
    for (uint32_t spins = 0;; ++spins) {
        if ((rd(a) & mask) == value) return 0;
        if (now_us() - start >= timeout_us || spins > timeout_us * 4U) return ERROR_TIMEOUT;
        step();
    }
}

static const struct kshim_soc_clock_controller *controller_of(const void *f, int node)
{
    for (size_t i = 0;; ++i) {
        const struct kshim_soc_clock_controller *c = kshim_soc_clock_controller_at(i);
        if (!c) return NULL;
        if (!fdt_node_check_compatible(f, node, c->compatible)) return c;
    }
}

int kshim_clk_init(const void *fdt)
{
    if (!fdt) return ERROR_INVALID_PARAMETER;
    if (C.fdt == fdt) return 0;
    for (unsigned i = 0; i < CLK_POOL; ++i) if (C.pool[i].used) return ERROR_BUSY;
    C.count = 0;
    for (int n = fdt_next_node(fdt, -1, NULL); n >= 0; n = fdt_next_node(fdt, n, NULL)) {
        const struct kshim_soc_clock_controller *c = controller_of(fdt, n);
        struct dt_range reg;
        if (!c || !dt_available(fdt, n) || dt_reg(fdt, n, 0, &reg) || reg.base > UINTPTR_MAX) continue;
        if (C.count == CLK_CONTROLLERS) break;
        C.controllers[C.count].node = n;
        C.controllers[C.count].base = (uintptr_t)reg.base;
        C.controllers[C.count].size = (size_t)reg.size;
        C.controllers[C.count].ctrl = c;
        ++C.count;
    }
    C.fdt = fdt;
    return C.count ? 0 : ERROR_NOT_FOUND;
}

int kshim_clk_map_resources(struct kshim_resources *r)
{
    if (!r || !r->fdt) return ERROR_INVALID_PARAMETER;
    for (int n = fdt_next_node(r->fdt, -1, NULL); n >= 0; n = fdt_next_node(r->fdt, n, NULL)) {
        struct dt_range reg;
        if (!controller_of(r->fdt, n) || !dt_available(r->fdt, n)) continue;
        if (dt_reg(r->fdt, n, 0, &reg) || kshim_resource_map(r, reg.base, reg.size, KSHIM_MMU_DEVICE,
                                                           KSHIM_MMU_READ | KSHIM_MMU_WRITE))
            return ERROR_NOT_SUPPORTED;
    }
    return 0;
}

static int bound(int node)
{
    for (unsigned i = 0; i < C.count; ++i) if (C.controllers[i].node == node) return (int)i;
    return -1;
}

/* Shared handle for one leaf of one bound controller. */
static int handle(unsigned controller, const struct kshim_soc_clk *leaf, struct kshim_clk **out)
{
    struct kshim_clk *free_slot = NULL;
    const struct kshim_soc_clock_controller *ctrl = C.controllers[controller].ctrl;
    for (unsigned i = 0; i < CLK_POOL; ++i) {
        struct kshim_clk *k = &C.pool[i];
        if (k->used && k->ctrl == ctrl && k->space == leaf->space && k->id == leaf->id &&
            k->desc.enable_reg == C.controllers[controller].base + leaf->enable_reg) {
            *out = k;
            return 0;
        }
        if (!k->used && !free_slot) free_slot = k;
    }
    if (!free_slot) return KSHIM_IO_ERR_NO_MEMORY;
    uintptr_t base = C.controllers[controller].base;
    size_t size = C.controllers[controller].size;
    if (leaf->enable_reg + 4U > size || leaf->halt_reg + 4U > size || leaf->rcg + 0x5cU > size)
        return ERROR_NOT_SUPPORTED;
    *free_slot = (struct kshim_clk){.used = true, .ctrl = ctrl, .name = leaf->name,
        .space = leaf->space, .id = leaf->id,
        .desc = {.kind = leaf->kind, .halt_check = leaf->halt, .enable_reg = base + leaf->enable_reg,
                 .enable_mask = leaf->enable_mask, .halt_reg = base + leaf->halt_reg,
                 .rcg = leaf->rcg ? base + leaf->rcg : 0}};
    *out = free_slot;
    return 0;
}

static int get_spec(const void *f, int consumer, const char *list, const char *names,
                    const char *cells, const char *name, unsigned index, uint8_t space,
                    struct kshim_clk **out)
{
    struct dt_spec spec;
    if (!f || consumer < 0 || !out) return ERROR_INVALID_PARAMETER;
    if (f != C.fdt && kshim_clk_init(f) && !C.count) return ERROR_NOT_SUPPORTED;
    if (!fdt_getprop(f, consumer, list, NULL)) return ERROR_NOT_FOUND;
    if (name) {
        int i = dt_string_index(f, consumer, names, name);
        if (i < 0) return ERROR_NOT_FOUND;
        index = (unsigned)i;
    }
    if (dt_specifier(f, consumer, list, cells, index, &spec)) return ERROR_NOT_FOUND;
    int controller = bound(spec.node);
    struct kshim_soc_clk leaf;
    if (controller < 0 || spec.count != 1 ||
        !kshim_soc_clock_leaf(C.controllers[controller].ctrl, space, spec.args[0], &leaf))
        return ERROR_NOT_SUPPORTED;
    return handle((unsigned)controller, &leaf, out);
}

int kshim_clk_get(const void *f, int consumer, const char *name, struct kshim_clk **clk)
{
    return get_spec(f, consumer, "clocks", "clock-names", "#clock-cells", name, 0,
                    KSHIM_SOC_CLK_SPACE_CLOCK, clk);
}

int kshim_clk_get_power_domain(const void *f, int consumer, unsigned index, struct kshim_clk **gdsc)
{
    return get_spec(f, consumer, "power-domains", NULL, "#power-domain-cells", NULL, index,
                    KSHIM_SOC_CLK_SPACE_GDSC, gdsc);
}

int kshim_clk_get_reset(const void *f, int consumer, const char *name, struct kshim_clk **reset)
{
    return get_spec(f, consumer, "resets", "reset-names", "#reset-cells", name, 0,
                    KSHIM_SOC_CLK_SPACE_RESET, reset);
}

int kshim_clk_get_by_name(const char *controller, const char *name, struct kshim_clk **clk)
{
    if (!controller || !name || !clk) return ERROR_INVALID_PARAMETER;
    for (unsigned i = 0; i < C.count; ++i) {
        struct kshim_soc_clk leaf;
        if (strcmp(C.controllers[i].ctrl->label, controller) ||
            !kshim_soc_clock_leaf_by_name(C.controllers[i].ctrl, name, &leaf)) continue;
        return handle(i, &leaf, clk);
    }
    return C.count ? ERROR_NOT_SUPPORTED : ERROR_NOT_READY;
}

const struct kshim_clk_desc *kshim_clk_desc(const struct kshim_clk *clk)
{
    return clk && clk->used ? &clk->desc : NULL;
}

bool kshim_clk_is_on(const struct kshim_clk *clk)
{
    if (!clk || !clk->used) return false;
    const struct kshim_clk_desc *d = &clk->desc;
    switch (d->kind) {
    case KSHIM_CLK_BRANCH:
    case KSHIM_CLK_VOTE:
        if (d->halt_check == KSHIM_CLK_HALT_BIT31) return !(rd(d->halt_reg) & CLK_OFF);
        return (rd(d->enable_reg) & d->enable_mask) != 0;
    case KSHIM_CLK_GDSC:
        return (rd(d->halt_reg) & CLK_OFF) != 0; /* PWR_ON */
    case KSHIM_CLK_RESET:
        return (rd(d->enable_reg) & d->enable_mask) != 0; /* asserted */
    default:
        return false;
    }
}

static void save_enable(struct kshim_clk *k)
{
    if (k->saved) return;
    k->enable_word = rd(k->desc.enable_reg);
    k->saved = true;
    k->seq = ++C.seq;
}

static void restore_enable(struct kshim_clk *k)
{
    if (!k->saved) return;
    uint32_t m = k->desc.enable_mask, now = rd(k->desc.enable_reg);
    uint32_t want = (now & ~m) | (k->enable_word & m);
    if (want != now) wr(k->desc.enable_reg, want);
    k->saved = false;
}

int kshim_clk_enable(struct kshim_clk *clk)
{
    if (!clk || !clk->used || clk->desc.kind == KSHIM_CLK_RESET) return ERROR_INVALID_PARAMETER;
    if (clk->refs++) return 0;
    if (kshim_clk_is_on(clk)) { clk->ours = false; return 0; } /* inherited */
    const struct kshim_clk_desc *d = &clk->desc;
    save_enable(clk);
    uint32_t value = rd(d->enable_reg);
    bool gdsc = d->kind == KSHIM_CLK_GDSC;
    wr(d->enable_reg, gdsc ? value & ~d->enable_mask : value | d->enable_mask);
    clk->ours = true;
    int status = 0;
    if (gdsc) status = wait_field(d->halt_reg, CLK_OFF, CLK_OFF, GDSC_TIMEOUT_US);
    else if (d->halt_check == KSHIM_CLK_HALT_BIT31)
        status = wait_field(d->halt_reg, CLK_OFF, 0, BRANCH_TIMEOUT_US);
    else for (unsigned i = 0; i < 10; ++i) step();
    if (status) {
        printf("clk: %s did not turn on\n", clk->name ? clk->name : "leaf");
        restore_enable(clk);
        clk->ours = false;
        clk->refs = 0;
    }
    return status;
}

int kshim_clk_disable(struct kshim_clk *clk)
{
    if (!clk || !clk->used || clk->desc.kind == KSHIM_CLK_RESET || !clk->refs)
        return ERROR_INVALID_PARAMETER;
    if (--clk->refs) return 0;
    if (clk->ours) restore_enable(clk);
    clk->ours = false;
    return 0;
}

int kshim_clk_reset(struct kshim_clk *reset, bool asserted)
{
    if (!reset || !reset->used || reset->desc.kind != KSHIM_CLK_RESET) return ERROR_INVALID_PARAMETER;
    const struct kshim_clk_desc *d = &reset->desc;
    if (!reset->reset_touched) { reset->reset_touched = true; reset->seq = ++C.seq; }
    uint32_t now = rd(d->enable_reg), want = asserted ? now | d->enable_mask : now & ~d->enable_mask;
    if (want != now) wr(d->enable_reg, want);
    return 0;
}

int kshim_clk_set_rate_cxo(struct kshim_clk *clk, uint8_t *dfs_index)
{
    if (!clk || !clk->used || !dfs_index) return ERROR_INVALID_PARAMETER;
    uintptr_t rcg = clk->desc.rcg;
    if (!rcg) return ERROR_NOT_SUPPORTED;
    /* Never retune a branch someone else left running. */
    if (kshim_clk_is_on(clk) && !clk->ours) return ERROR_BUSY;
    if (rd(rcg + 0x14U) & 1U) {
        for (uint8_t i = 0; i < RCG_DFS_LEVELS; ++i) {
            uint32_t cfg = rd(rcg + 0x1cU + i * 4U);
            if (!(cfg & ((7U << 8) | (3U << 12))) && (cfg & 0x1fU) <= 1U) { *dfs_index = i; return 0; }
        }
        return ERROR_NOT_SUPPORTED;
    }
    uint32_t cfg = rd(rcg + 4U), next = (cfg & ~RCG_CFG_FIELDS) | 1U;
    *dfs_index = 0;
    if (next == cfg) return 0;
    if (!clk->rcg_saved) { clk->rcg_saved = true; clk->rcg_cfg = cfg; clk->rcg_seq = ++C.seq; }
    wr(rcg + 4U, next);
    wr(rcg, rd(rcg) | RCG_UPDATE);
    return wait_field(rcg, RCG_UPDATE, 0, RCG_TIMEOUT_US);
}

static int restore_rcg(struct kshim_clk *k)
{
    if (!k->rcg_saved) return 0;
    uintptr_t rcg = k->desc.rcg;
    k->rcg_saved = false;
    if (rd(rcg + 4U) == k->rcg_cfg) return 0;
    wr(rcg + 4U, k->rcg_cfg);
    wr(rcg, rd(rcg) | RCG_UPDATE);
    return wait_field(rcg, RCG_UPDATE, 0, RCG_TIMEOUT_US);
}

int kshim_clk_restore_all(void)
{
    int result = 0;
    for (;;) {
        /* Newest change first: enable words, RCG configurations, resets. */
        struct kshim_clk *newest = NULL;
        uint32_t best = 0;
        int what = 0;
        for (unsigned i = 0; i < CLK_POOL; ++i) {
            struct kshim_clk *k = &C.pool[i];
            if (!k->used) continue;
            if (k->saved && k->seq >= best) { newest = k; best = k->seq; what = 1; }
            if (k->rcg_saved && k->rcg_seq >= best) { newest = k; best = k->rcg_seq; what = 2; }
            if (k->reset_touched && k->seq >= best) { newest = k; best = k->seq; what = 3; }
        }
        if (!newest) break;
        int status = 0;
        if (what == 1) restore_enable(newest);
        else if (what == 2) status = restore_rcg(newest);
        else {
            /* Resets are left deasserted. */
            uint32_t now = rd(newest->desc.enable_reg);
            if (now & newest->desc.enable_mask) wr(newest->desc.enable_reg, now & ~newest->desc.enable_mask);
            newest->reset_touched = false;
        }
        if (status && !result) result = status;
    }
    for (unsigned i = 0; i < CLK_POOL; ++i) { C.pool[i].refs = 0; C.pool[i].ours = false; }
    return result;
}
