/* SPDX-License-Identifier: MIT */
#pragma once
/*
 * PCI configuration-space register model for the PCI host tests: type 0 and
 * type 1 functions with BAR size masks (32/64-bit, I/O, prefetchable),
 * read-only identity registers, W1C status bits, bridge bus-number routing
 * (a function is reachable only through bridges whose secondary/subordinate
 * range covers its bus) and a write log. Bus 0 is the host's root bus.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MODEL_FUNCS 24
#define MODEL_LOG   4096

struct model_fn {
    bool used;
    int parent;              /* upstream bridge function index, -1: root bus */
    uint8_t slot, fn;
    uint8_t cfg[4096];
    uint8_t wmask[4096];
    uint8_t w1c[4096];
};

struct model_write { int fn; uint16_t reg; unsigned width; uint32_t value; };

static struct model_fn Model[MODEL_FUNCS];
static struct model_write ModelLog[MODEL_LOG];
static unsigned ModelLogCount;

static void put(uint8_t *a, uint16_t reg, unsigned width, uint32_t v)
{
    for (unsigned i = 0; i < width; ++i) a[reg + i] = (uint8_t)(v >> (8 * i));
}

static uint32_t get(const uint8_t *a, uint16_t reg, unsigned width)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < width; ++i) v |= (uint32_t)a[reg + i] << (8 * i);
    return v;
}

/* Header: id, class, header type, capabilities (an optional PCIe cap at 0x40). */
static inline int model_add(int parent, uint8_t slot, uint8_t fn, uint16_t vendor, uint16_t device,
                     uint32_t class_code, uint8_t header, uint8_t pin)
{
    for (int i = 0; i < MODEL_FUNCS; ++i) {
        struct model_fn *m = &Model[i];
        if (m->used) continue;
        memset(m, 0, sizeof(*m));
        m->used = true;
        m->parent = parent;
        m->slot = slot;
        m->fn = fn;
        put(m->cfg, 0x00, 2, vendor);
        put(m->cfg, 0x02, 2, device);
        put(m->cfg, 0x08, 4, class_code << 8 | 0x01);
        put(m->cfg, 0x0e, 1, header);
        put(m->cfg, 0x3d, 1, pin);
        put(m->wmask, 0x04, 2, 0x0547);      /* IO MEM MASTER PERR SERR INTX_DISABLE */
        put(m->w1c, 0x06, 2, 0xf900);        /* error status bits */
        if ((header & 0x7f) == 1) {
            put(m->wmask, 0x18, 4, 0xffffffff);  /* bus numbers + latency */
            put(m->wmask, 0x1c, 2, 0xf0f0);      /* I/O base/limit (16-bit) */
            put(m->w1c, 0x1e, 2, 0xf900);        /* secondary status */
            put(m->wmask, 0x20, 4, 0xfff0fff0);  /* memory base/limit */
            put(m->cfg, 0x24, 4, 0x00010001);    /* 64-bit prefetchable */
            put(m->wmask, 0x24, 4, 0xfff0fff0);
            put(m->wmask, 0x28, 4, 0xffffffff);
            put(m->wmask, 0x2c, 4, 0xffffffff);
            put(m->wmask, 0x3e, 2, 0x0fff);
        }
        return i;
    }
    assert(!"model full");
    return -1;
}

static inline void model_pcie_port(int f, unsigned type)
{
    struct model_fn *m = &Model[f];
    put(m->cfg, 0x06, 2, get(m->cfg, 0x06, 2) | 0x10);
    put(m->cfg, 0x34, 1, 0x40);
    put(m->cfg, 0x40, 2, 0x0010);           /* PCIe capability, last */
    put(m->cfg, 0x42, 2, (uint16_t)(0x0002 | type << 4));
}

/* BAR i with a size; flags: 1 I/O, 4 64-bit, 8 prefetchable. */
static inline void model_bar(int f, unsigned i, uint64_t size, uint32_t flags)
{
    struct model_fn *m = &Model[f];
    uint16_t reg = (uint16_t)(0x10 + 4 * i);
    uint64_t mask = ~(size - 1);
    put(m->cfg, reg, 4, flags);
    put(m->wmask, reg, 4, (uint32_t)mask & (flags & 1 ? 0xfffffffcU : 0xfffffff0U));
    if (flags & 4) put(m->wmask, (uint16_t)(reg + 4), 4, (uint32_t)(mask >> 32));
}

static inline void model_set(int f, uint16_t reg, unsigned width, uint32_t v)
{
    put(Model[f].cfg, reg, width, v);
}

static inline uint32_t model_get(int f, uint16_t reg, unsigned width)
{
    return get(Model[f].cfg, reg, width);
}

static unsigned model_bus(int f)
{
    return Model[f].parent < 0 ? 0 : model_get(Model[f].parent, 0x19, 1);
}

static bool model_reachable(int f, unsigned bus)
{
    for (int p = Model[f].parent; p >= 0; p = Model[p].parent) {
        unsigned sec = model_get(p, 0x19, 1), sub = model_get(p, 0x1a, 1);
        if (!sec || bus < sec || bus > sub) return false;
    }
    return true;
}

static int model_find(unsigned bus, uint8_t devfn)
{
    for (int i = 0; i < MODEL_FUNCS; ++i) {
        const struct model_fn *m = &Model[i];
        if (!m->used || m->slot != (devfn >> 3) || m->fn != (devfn & 7)) continue;
        if (model_bus(i) == bus && model_reachable(i, bus)) return i;
    }
    return -1;
}

static inline uint32_t model_cfg_read(unsigned bus, uint8_t devfn, uint16_t reg, unsigned width)
{
    int f = model_find(bus, devfn);
    if (f < 0) return width == 4 ? 0xffffffffU : (1U << (8 * width)) - 1;
    return get(Model[f].cfg, reg, width);
}

static inline void model_cfg_write(unsigned bus, uint8_t devfn, uint16_t reg, unsigned width, uint32_t v)
{
    int f = model_find(bus, devfn);
    if (f < 0) return;
    struct model_fn *m = &Model[f];
    if (ModelLogCount < MODEL_LOG) ModelLog[ModelLogCount++] = (struct model_write){f, reg, width, v};
    for (unsigned i = 0; i < width; ++i) {
        uint8_t b = (uint8_t)(v >> (8 * i));
        uint16_t r = (uint16_t)(reg + i);
        m->cfg[r] = (uint8_t)((m->cfg[r] & ~m->wmask[r]) | (b & m->wmask[r]));
        m->cfg[r] &= (uint8_t)~(b & m->w1c[r]);
    }
}

/* Count logged writes to one function's register range. */
static inline unsigned model_writes(int f, uint16_t from, uint16_t to)
{
    unsigned n = 0;
    for (unsigned i = 0; i < ModelLogCount; ++i)
        if (ModelLog[i].fn == f && ModelLog[i].reg >= from && ModelLog[i].reg < to) ++n;
    return n;
}

static inline void model_snapshot(uint8_t (*out)[4096])
{
    for (int i = 0; i < MODEL_FUNCS; ++i) memcpy(out[i], Model[i].cfg, 4096);
}

static inline void model_expect(uint8_t (*saved)[4096])
{
    for (int i = 0; i < MODEL_FUNCS; ++i) {
        if (!Model[i].used) continue;
        for (unsigned r = 0; r < 4096; ++r)
            if (saved[i][r] != Model[i].cfg[r]) {
                printf("function %d (slot %u fn %u) +0x%x: inherited 0x%02x now 0x%02x\n", i,
                       Model[i].slot, Model[i].fn, r, saved[i][r], Model[i].cfg[r]);
                assert(!"configuration not restored");
            }
    }
}
