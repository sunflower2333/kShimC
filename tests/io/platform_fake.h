/* SPDX-License-Identifier: MIT */
#pragma once
/*
 * Shared host models for the sm8650 platform tests: fixture loading, a
 * sparse 32-bit register file behind a CrIo (TLMM, GCC, RSC), an optional
 * CMD DB byte window, an RPMh RSC/TCS model that executes AMC writes and
 * reads against a per-address vote store, fake time, and the stubs the
 * drivers need from the rest of the kernel.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Library/cr_geni.h>
#include <qcom_dt.h>

/* ---- fixtures --------------------------------------------------------- */

static inline void *fake_fixture(const char *name)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", FIXTURES, name);
    FILE *fp = fopen(path, "rb");
    assert(fp);
    assert(!fseek(fp, 0, SEEK_END));
    long size = ftell(fp);
    assert(size > 0);
    rewind(fp);
    void *data = calloc(1, (size_t)size + 16384);
    assert(data);
    assert(fread(data, 1, (size_t)size, fp) == (size_t)size);
    fclose(fp);
    assert(!dt_validate(data, (size_t)size));
    assert(!fdt_open_into(data, data, (int)size + 16384));
    return data;
}

static inline int fake_path(const void *f, const char *path)
{
    int n = fdt_path_offset(f, path);
    if (n < 0) fprintf(stderr, "missing node %s\n", path);
    assert(n >= 0);
    return n;
}

/* ---- register file ---------------------------------------------------- */

#define FAKE_REGS 4096
struct fake_reg { uintptr_t address; uint32_t value; };
static struct fake_reg FakeRegs[FAKE_REGS];
static unsigned FakeRegCount;
static uint64_t FakeNow;
static unsigned FakeWrites;
/* Write log for ordering checks. */
#define FAKE_LOG 4096
static struct fake_reg FakeLog[FAKE_LOG];
static unsigned FakeLogCount;

static struct fake_reg *fake_slot(uintptr_t a, bool create)
{
    for (unsigned i = 0; i < FakeRegCount; ++i) if (FakeRegs[i].address == a) return &FakeRegs[i];
    if (!create) return NULL;
    assert(FakeRegCount < FAKE_REGS);
    FakeRegs[FakeRegCount] = (struct fake_reg){a, 0};
    return &FakeRegs[FakeRegCount++];
}
static inline uint32_t fake_peek(uintptr_t a) { struct fake_reg *r = fake_slot(a, false); return r ? r->value : 0; }
static inline void fake_poke(uintptr_t a, uint32_t v) { fake_slot(a, true)->value = v; }

/* CMD DB byte window. */
static uint8_t *FakeDb;
static uintptr_t FakeDbBase;
static size_t FakeDbSize;

/* RSC model: DRV base, TCS window, v3 layout. */
struct fake_rsc {
    bool on;
    uintptr_t base, tcs;     /* DRV base and base + tcs-offset */
    unsigned slots;          /* TCS count of this DRV */
    unsigned active_offset, active_count;
    struct { uint32_t address, value; bool used; } store[64];
    unsigned writes, reads;
    bool hang;               /* never complete (timeout path) */
    bool read_unsupported;   /* reads complete without the COMPLETED status */
};
static struct fake_rsc FakeRsc;

static uint32_t *fake_vote(uint32_t address, bool create)
{
    for (unsigned i = 0; i < 64; ++i)
        if (FakeRsc.store[i].used && FakeRsc.store[i].address == address) return &FakeRsc.store[i].value;
    if (!create) return NULL;
    for (unsigned i = 0; i < 64; ++i)
        if (!FakeRsc.store[i].used) {
            FakeRsc.store[i].used = true;
            FakeRsc.store[i].address = address;
            FakeRsc.store[i].value = 0;
            return &FakeRsc.store[i].value;
        }
    assert(0);
    return NULL;
}
static inline void fake_rpmh_set(uint32_t address, uint32_t value) { *fake_vote(address, true) = value; }
static inline uint32_t fake_rpmh_get(uint32_t address) { uint32_t *v = fake_vote(address, false); return v ? *v : 0; }

static void fake_rsc_write(uintptr_t a, uint32_t v)
{
    /* TCS slot control registers (v3: control +0x24, CMD0 +0x34). */
    if (!FakeRsc.on || a < FakeRsc.tcs + 0x10 || a >= FakeRsc.tcs + FakeRsc.slots * 0x2a0U) return;
    uintptr_t off = a - FakeRsc.tcs;
    unsigned slot = (unsigned)(off / 0x2a0U);
    uintptr_t base = FakeRsc.tcs + slot * 0x2a0U;
    if (a != base + 0x24 || !(v & (1U << 24))) return; /* trigger */
    if (FakeRsc.hang) return;
    uint32_t msgid = fake_peek(base + 0x34), address = fake_peek(base + 0x38), data = fake_peek(base + 0x3c);
    if (msgid & (1U << 16)) { fake_rpmh_set(address, data); ++FakeRsc.writes; }
    else { fake_poke(base + 0x44, fake_rpmh_get(address)); ++FakeRsc.reads; }
    fake_poke(base + 0x40, FakeRsc.read_unsupported && !(msgid & (1U << 16)) ? (1U << 8) : (1U << 8) | (1U << 16));
    fake_poke(FakeRsc.tcs + 4, fake_peek(FakeRsc.tcs + 4) | (1U << slot));
}

static uint32_t fake_read32(void *c, uintptr_t a)
{
    (void)c;
    if (FakeDb && a >= FakeDbBase && a + 4 <= FakeDbBase + FakeDbSize) {
        uint32_t v;
        memcpy(&v, FakeDb + (a - FakeDbBase), 4);
        return v;
    }
    return fake_peek(a);
}
/* Optional hardware behaviour after a stored write (clock status, ...). */
static void (*FakeHook)(uintptr_t a, uint32_t v);

static void fake_write32(void *c, uintptr_t a, uint32_t v)
{
    (void)c;
    ++FakeWrites;
    if (FakeLogCount < FAKE_LOG) FakeLog[FakeLogCount++] = (struct fake_reg){a, v};
    if (FakeRsc.on && a == FakeRsc.tcs + 8) { /* IRQ_CLEAR */
        fake_poke(FakeRsc.tcs + 4, fake_peek(FakeRsc.tcs + 4) & ~v);
        return;
    }
    fake_poke(a, v);
    fake_rsc_write(a, v);
    if (FakeHook) FakeHook(a, v);
}
static uint64_t fake_now(void *c) { (void)c; return FakeNow; }
static void fake_delay(void *c, uint32_t us) { (void)c; FakeNow += us ? us : 1; }
static const struct CrIo FakeIo = {NULL, fake_read32, fake_write32, fake_now, fake_delay};

static inline void fake_reset(void)
{
    memset(FakeRegs, 0, sizeof(FakeRegs));
    FakeRegCount = FakeWrites = FakeLogCount = 0;
    memset(&FakeRsc, 0, sizeof(FakeRsc));
    FakeDb = NULL;
}

/* An idle v3 apps DRV (drv-2) with tcs-config ACTIVE 3, SLEEP 2, WAKE 2. */
static inline void fake_rsc_setup(uintptr_t drv_base, uint32_t tcs_offset, unsigned drv_id,
                                  unsigned active_offset, unsigned active_count, unsigned total)
{
    FakeRsc.on = true;
    FakeRsc.base = drv_base;
    FakeRsc.tcs = drv_base + tcs_offset;
    FakeRsc.slots = total;
    FakeRsc.active_offset = active_offset;
    FakeRsc.active_count = active_count;
    fake_poke(drv_base, 3U << 16);                                     /* DRV_ID: v3.0 */
    fake_poke(drv_base + 0xc, (16U << 27) | ((uint32_t)total << (6 * drv_id))); /* 16 cmds/TCS */
    for (unsigned s = 0; s < total; ++s) fake_poke(FakeRsc.tcs + s * 0x2a0U + 0x28, 1); /* idle */
}

/* CMD DB with VRM entries (slave 4): names and addresses. */
static inline void fake_cmd_db(uintptr_t base, const char *const *names, const uint32_t *addresses,
                               unsigned count)
{
    FakeDbSize = 144 + count * 24 + 64;
    FakeDb = calloc(1, FakeDbSize);
    assert(FakeDb);
    FakeDbBase = base;
    uint32_t magic = 0x0c0330db;
    memcpy(FakeDb + 4, &magic, 4);
    uint16_t header[4] = {4, 0, 0, (uint16_t)count};
    memcpy(FakeDb + 8, header, sizeof(header));
    for (unsigned i = 0; i < count; ++i) {
        uint8_t *e = FakeDb + 144 + i * 24;
        strncpy((char *)e, names[i], 8);
        memcpy(e + 16, &addresses[i], 4);
    }
}

/* ---- stubs ---------------------------------------------------------- */

static unsigned FakeIrqDepth;
unsigned long kshim_irq_save(void) { return FakeIrqDepth++; }
void kshim_irq_restore(unsigned long flags) { FakeIrqDepth = (unsigned)flags; }

int kshim_resource_map(struct kshim_resources *r, uint64_t base, uint64_t size,
                       kshim_mmu_memory_type_t type, uint32_t permissions)
{
    if (!base || !size || r->mapping_count == KSHIM_RESOURCE_MAX) return -1;
    r->mappings[r->mapping_count++] = (kshim_mmu_region_t){base, size, type, permissions};
    return 0;
}

static inline bool fake_mapped(const struct kshim_resources *r, uint64_t base, uint64_t size,
                               kshim_mmu_memory_type_t type)
{
    for (size_t i = 0; i < r->mapping_count; ++i)
        if (r->mappings[i].type == type && r->mappings[i].base <= base &&
            base + size <= r->mappings[i].base + r->mappings[i].size) return true;
    return false;
}
