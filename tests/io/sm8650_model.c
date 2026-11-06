/* SPDX-License-Identifier: MIT */
/* SM8650 SoC register models on the irq_fake.c MMIO bus; see sm8650_model.h.
 * The RSC/TCS and CMD DB layouts are the ones tests/io/platform_fake.h
 * models for the platform tests (CrDK RpmhLib v3 offsets, CMD DB header and
 * 24-byte entries), here on the shared bus. */
#include "sm8650_model.h"
#include "../irq_fake.h"

#include <kshim/clk.h>
#include <kshim/soc_tables.h>

#include <assert.h>
#include <stdlib.h>
#include <string.h>

/* ---- TLMM ----------------------------------------------------------------- */

#define CFG_OE (1U << 9)
#define IO_IN 1U
#define IO_OUT 2U
#define INTR_EN 1U
#define INTR_POL 2U
#define INTR_RAW 0x10U
#define INTR_TARGET(v) (((v) >> 8) & 7U)
#define KPSS 3U

static int8_t Ext[SM_TLMM_PINS];
static bool Last[SM_TLMM_PINS];
static uint32_t IntrCfg[SM_TLMM_PINS], IntrStatus[SM_TLMM_PINS];
static unsigned WatchDepth;
void (*SmPadWatch)(unsigned pin, bool level);

static bool pad_level(unsigned pin)
{
    uint32_t cfg = fake_peek(sm_tlmm_cfg(pin));
    if (cfg & CFG_OE) return (fake_peek(sm_tlmm_cfg(pin) + 4) & IO_OUT) != 0;
    if (Ext[pin] >= 0) return Ext[pin] != 0;
    return (cfg & 3U) == 3U;
}

/* Recompute one pad: report a level change, latch edges and levels. */
static void update(unsigned pin)
{
    bool now = pad_level(pin), changed = now != Last[pin];
    uint32_t ic = IntrCfg[pin];
    unsigned det = (ic >> 2) & 3U;
    Last[pin] = now;
    if ((ic & INTR_RAW) && det && changed && ((now && (det & 1U)) || (!now && (det & 2U))))
        IntrStatus[pin] = 1;
    if ((ic & INTR_RAW) && !det && now == ((ic & INTR_POL) != 0)) IntrStatus[pin] = 1;
    if (changed && SmPadWatch && WatchDepth < 4) {
        ++WatchDepth;
        SmPadWatch(pin, now);
        --WatchDepth;
    }
}

bool sm_tlmm_level(unsigned pin) { return pad_level(pin); }
bool sm_tlmm_output(unsigned pin) { return (fake_peek(sm_tlmm_cfg(pin)) & CFG_OE) != 0; }

void sm_tlmm_drive(unsigned pin, int level)
{
    assert(pin < SM_TLMM_PINS);
    Ext[pin] = (int8_t)(level < 0 ? -1 : level != 0);
    update(pin);
}

static uint32_t tlmm_read(uintptr_t a)
{
    unsigned pin = (unsigned)((a - SM_TLMM) / 0x1000U), reg = (unsigned)(a & 0xfffU);
    if (pin >= SM_TLMM_PINS) return fake_peek(a);
    switch (reg) {
    case 4: return (fake_peek(a) & ~IO_IN) | (pad_level(pin) ? IO_IN : 0);
    case 8: return IntrCfg[pin];
    case 0xc: return IntrStatus[pin];
    default: return fake_peek(a);
    }
}

static void tlmm_write(uintptr_t a, uint32_t v)
{
    unsigned pin = (unsigned)((a - SM_TLMM) / 0x1000U), reg = (unsigned)(a & 0xfffU);
    if (pin >= SM_TLMM_PINS) { fake_poke(a, v); return; }
    switch (reg) {
    case 8: IntrCfg[pin] = v; break;
    case 0xc: IntrStatus[pin] = v & 1U; break;
    case 4: fake_poke(a, v & ~IO_IN); break;
    default: fake_poke(a, v); break;
    }
    update(pin);
}

bool sm_tlmm_summary_line(void)
{
    for (unsigned pin = 0; pin < SM_TLMM_PINS; ++pin)
        if ((IntrCfg[pin] & INTR_EN) && IntrStatus[pin] && INTR_TARGET(IntrCfg[pin]) == KPSS) return true;
    return false;
}

/* ---- GCC ------------------------------------------------------------------ */

static const struct kshim_soc_clock_controller *Gcc;

static const struct kshim_soc_clock_controller *gcc(void)
{
    if (Gcc) return Gcc;
    for (size_t i = 0; kshim_soc_clock_controller_at(i); ++i)
        if (!strcmp(kshim_soc_clock_controller_at(i)->compatible, "qcom,sm8650-gcc"))
            Gcc = kshim_soc_clock_controller_at(i);
    assert(Gcc && Gcc->leaves && Gcc->count);
    return Gcc;
}

static bool gated(const struct kshim_soc_clk *l)
{
    return (l->kind == KSHIM_CLK_BRANCH || l->kind == KSHIM_CLK_VOTE) && l->halt == KSHIM_CLK_HALT_BIT31 &&
           l->halt_reg;
}

static void gcc_write(uintptr_t a, uint32_t v)
{
    const struct kshim_soc_clock_controller *c = gcc();
    uint32_t off = (uint32_t)(a - SM_GCC);
    for (size_t i = 0; i < *c->count; ++i) {
        const struct kshim_soc_clk *l = &c->leaves[i];
        if (l->rcg && off == l->rcg) v &= ~1U;  /* CMD_RCGR UPDATE: applied at once */
    }
    fake_poke(a, v);
    for (size_t i = 0; i < *c->count; ++i) {
        const struct kshim_soc_clk *l = &c->leaves[i];
        if (!gated(l) || l->enable_reg != off) continue;
        uintptr_t halt = SM_GCC + l->halt_reg;
        uint32_t h = fake_peek(halt);
        fake_poke(halt, (v & l->enable_mask) ? h & ~(1U << 31) : h | (1U << 31));
    }
}

static const struct kshim_soc_clk *leaf(const char *name)
{
    static struct kshim_soc_clk found;
    assert(kshim_soc_clock_leaf_by_name(gcc(), name, &found));
    return &found;
}

void sm_gcc_inherit_on(const char *name)
{
    const struct kshim_soc_clk *l = leaf(name);
    uintptr_t enable = SM_GCC + l->enable_reg;
    fake_poke(enable, fake_peek(enable) | l->enable_mask);
    fake_poke(SM_GCC + l->halt_reg, fake_peek(SM_GCC + l->halt_reg) & ~(1U << 31));
}

bool sm_gcc_on(const char *name)
{
    const struct kshim_soc_clk *l = leaf(name);
    return !(fake_peek(SM_GCC + l->halt_reg) & (1U << 31));
}

/* ---- RPMh RSC and CMD DB --------------------------------------------------- */

#define TCS_COUNT 7U
#define TCS_STRIDE 0x2a0U
static struct { uint32_t address, value; bool used; } Votes[64];
static unsigned RpmhWrites;
void (*SmRpmhWatch)(uint32_t address, uint32_t value);
static uint8_t *Db;
static size_t DbSize;

static uint32_t *vote(uint32_t address, bool create)
{
    for (unsigned i = 0; i < 64; ++i)
        if (Votes[i].used && Votes[i].address == address) return &Votes[i].value;
    if (!create) return NULL;
    for (unsigned i = 0; i < 64; ++i)
        if (!Votes[i].used) {
            Votes[i].address = address; Votes[i].value = 0; Votes[i].used = true;
            return &Votes[i].value;
        }
    assert(!"vote store full");
    return NULL;
}
void sm_rpmh_set(uint32_t address, uint32_t value) { *vote(address, true) = value; }
uint32_t sm_rpmh_get(uint32_t address) { uint32_t *v = vote(address, false); return v ? *v : 0; }
unsigned sm_rpmh_writes(void) { return RpmhWrites; }

static void rsc_write(uintptr_t a, uint32_t v)
{
    if (a == SM_RSC_TCS + 8) { /* IRQ_CLEAR */
        fake_poke(SM_RSC_TCS + 4, fake_peek(SM_RSC_TCS + 4) & ~v);
        return;
    }
    fake_poke(a, v);
    if (a < SM_RSC_TCS + 0x10 || a >= SM_RSC_TCS + TCS_COUNT * TCS_STRIDE) return;
    unsigned slot = (unsigned)((a - SM_RSC_TCS) / TCS_STRIDE);
    uintptr_t base = SM_RSC_TCS + slot * TCS_STRIDE;
    if (a != base + 0x24 || !(v & (1U << 24))) return; /* CONTROL trigger */
    uint32_t msgid = fake_peek(base + 0x34), address = fake_peek(base + 0x38), data = fake_peek(base + 0x3c);
    if (msgid & (1U << 16)) {
        sm_rpmh_set(address, data);
        ++RpmhWrites;
        if (SmRpmhWatch) SmRpmhWatch(address, data);
    }
    else fake_poke(base + 0x44, sm_rpmh_get(address));
    fake_poke(base + 0x40, (1U << 8) | (1U << 16)); /* issued, completed */
    fake_poke(SM_RSC_TCS + 4, fake_peek(SM_RSC_TCS + 4) | (1U << slot));
}

static uint32_t db_read(uintptr_t a)
{
    uint32_t v = 0;
    if (Db && a + 4 <= SM_CMD_DB + DbSize) memcpy(&v, Db + (a - SM_CMD_DB), 4);
    return v;
}

void sm_cmd_db(const char *const *names, const uint32_t *addresses, unsigned count)
{
    free(Db);
    DbSize = 144 + count * 24 + 64;
    Db = calloc(1, DbSize);
    assert(Db);
    uint32_t magic = 0x0c0330db;
    memcpy(Db + 4, &magic, 4);
    uint16_t header[4] = {4, 0, 0, (uint16_t)count}; /* slave 4 (VRM) */
    memcpy(Db + 8, header, sizeof(header));
    for (unsigned i = 0; i < count; ++i) {
        uint8_t *e = Db + 144 + i * 24;
        strncpy((char *)e, names[i], 8);
        memcpy(e + 16, &addresses[i], 4);
    }
    fake_mmio_region(SM_CMD_DB, DbSize, db_read, NULL);
}

/* ---- Setup ----------------------------------------------------------------- */

void sm_model_reset(void)
{
    memset(Ext, -1, sizeof(Ext));
    memset(Last, 0, sizeof(Last));
    memset(IntrCfg, 0, sizeof(IntrCfg));
    memset(IntrStatus, 0, sizeof(IntrStatus));
    SmPadWatch = NULL;
    WatchDepth = 0;
    fake_mmio_region(SM_TLMM, SM_TLMM_SIZE, tlmm_read, tlmm_write);

    const struct kshim_soc_clock_controller *c = gcc();
    for (size_t i = 0; i < *c->count; ++i)
        if (gated(&c->leaves[i])) fake_poke(SM_GCC + c->leaves[i].halt_reg, 1U << 31);
    fake_mmio_region(SM_GCC, SM_GCC_SIZE, NULL, gcc_write);

    memset(Votes, 0, sizeof(Votes));
    RpmhWrites = 0;
    SmRpmhWatch = NULL;
    /* An idle v3 DRV-2: DRV_ID version 3.0, 16 commands per TCS, 7 TCSes. */
    fake_poke(SM_RSC_DRV2, 3U << 16);
    fake_poke(SM_RSC_DRV2 + 0xc, (16U << 27) | (TCS_COUNT << (6 * 2)));
    for (unsigned s = 0; s < TCS_COUNT; ++s) fake_poke(SM_RSC_TCS + s * TCS_STRIDE + 0x28, 1);
    fake_mmio_region(SM_RSC_DRV2, 0x10000, NULL, rsc_write);
}
