/* SPDX-License-Identifier: MIT */
/*
 * DMA pool for bus-master devices (kshim/dma.h, owner xhci).
 *
 * The pool is a page-aligned static array inside the runtime image, so it is
 * identity mapped (VA == PA == bus address), write-back cacheable once the
 * MMU is on and plain device memory while it is off. It is handed out in
 * 64-byte units (one cache line on the supported cores), so an allocation
 * never shares a cache line with other data and the sync calls may clean and
 * invalidate whole lines. A first-fit bitmap search honours the requested
 * alignment, a no-crossing boundary (xHCI rings: 64 KiB) and an inclusive
 * address limit for 32-bit-only masters.
 *
 * Thread context only (boot CPU). The pool is never handed to the payload:
 * every DMA master is stopped by kshim_io_quiesce() first.
 */
#include <kshim/dma.h>
#include <kshim/dma_cache.h>

#include <config.h>
#include <string.h>

#ifndef CONFIG_KSHIM_DMA_POOL_SIZE
#define CONFIG_KSHIM_DMA_POOL_SIZE 0x40000
#endif

#define DMA_UNIT 64U
#define DMA_POOL_BYTES ((size_t)(CONFIG_KSHIM_DMA_POOL_SIZE) & ~(size_t)4095)
#define DMA_UNITS (DMA_POOL_BYTES / DMA_UNIT)

_Static_assert(DMA_POOL_BYTES >= 4096, "DMA pool must hold at least one page");

static uint8_t Pool[DMA_POOL_BYTES] __attribute__((aligned(4096)));
static uint32_t Used[(DMA_UNITS + 31) / 32];
/* Start unit of each live allocation, so kshim_dma_owns() can tell a
 * buffer that spans two neighbouring allocations from one inside a single
 * allocation. */
static uint32_t Start[(DMA_UNITS + 31) / 32];
static size_t InUse;

static bool unit_used(size_t u) { return (Used[u / 32] >> (u % 32)) & 1U; }
static void unit_set(uint32_t *map, size_t u, bool on)
{
    if (on) map[u / 32] |= 1U << (u % 32);
    else map[u / 32] &= ~(1U << (u % 32));
}
static bool is_pow2(size_t v) { return v && !(v & (v - 1)); }

static bool range_free(size_t first, size_t count)
{
    for (size_t u = first; u < first + count; ++u)
        if (unit_used(u)) return false;
    return true;
}

void *kshim_dma_alloc(size_t size, size_t align, size_t boundary,
                      uint64_t max_addr, uint64_t *bus)
{
    if (!size || size > DMA_POOL_BYTES) return NULL;
    if (align < DMA_UNIT) align = DMA_UNIT;
    if (!is_pow2(align) || (boundary && (!is_pow2(boundary) || size > boundary))) return NULL;
    size_t count = (size + DMA_UNIT - 1) / DMA_UNIT;
    uintptr_t base = (uintptr_t)Pool;
    uintptr_t addr = (base + align - 1) & ~(uintptr_t)(align - 1);
    while (addr + size <= base + DMA_POOL_BYTES) {
        if (boundary && (addr / boundary) != ((addr + size - 1) / boundary)) {
            /* Restart at the next boundary, which is also align-aligned
             * because boundary >= size and both are powers of two. */
            addr = (addr | (boundary - 1)) + 1;
            addr = (addr + align - 1) & ~(uintptr_t)(align - 1);
            continue;
        }
        if ((uint64_t)(addr + size - 1) > max_addr) return NULL;
        size_t first = (addr - base) / DMA_UNIT;
        if (range_free(first, count)) {
            for (size_t u = first; u < first + count; ++u) unit_set(Used, u, true);
            unit_set(Start, first, true);
            InUse += count * DMA_UNIT;
            void *va = (void *)addr;
            memset(va, 0, count * DMA_UNIT);
            kshim_dcache_clean(va, count * DMA_UNIT);
            if (bus) *bus = (uint64_t)addr;
            return va;
        }
        addr += align;
    }
    return NULL;
}

void kshim_dma_free(void *va, size_t size)
{
    uintptr_t base = (uintptr_t)Pool, p = (uintptr_t)va;
    if (!va || !size || p < base || p >= base + DMA_POOL_BYTES || (p - base) % DMA_UNIT) return;
    size_t first = (p - base) / DMA_UNIT, count = (size + DMA_UNIT - 1) / DMA_UNIT;
    if (first + count > DMA_UNITS || !((Start[first / 32] >> (first % 32)) & 1U)) return;
    unit_set(Start, first, false);
    for (size_t u = first; u < first + count; ++u) {
        if (u > first && ((Start[u / 32] >> (u % 32)) & 1U)) break; /* next allocation */
        if (unit_used(u)) InUse -= DMA_UNIT;
        unit_set(Used, u, false);
    }
}

bool kshim_dma_owns(const void *va, size_t len)
{
    uintptr_t base = (uintptr_t)Pool, p = (uintptr_t)va;
    if (!va || !len || p < base || p >= base + DMA_POOL_BYTES || len > base + DMA_POOL_BYTES - p)
        return false;
    size_t first = (p - base) / DMA_UNIT, last = (p + len - 1 - base) / DMA_UNIT;
    for (size_t u = first; u <= last; ++u) {
        if (!unit_used(u)) return false;
        if (u > first && ((Start[u / 32] >> (u % 32)) & 1U)) return false;
    }
    return true;
}

size_t kshim_dma_in_use(void) { return InUse; }
size_t kshim_dma_pool_size(void) { return DMA_POOL_BYTES; }

void kshim_dma_sync_for_device(const void *va, size_t len)
{
    if (va && len) kshim_dcache_clean(va, len);
    else __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

void kshim_dma_sync_for_cpu(void *va, size_t len)
{
    if (va && len) kshim_dcache_invalidate(va, len);
    else __atomic_thread_fence(__ATOMIC_SEQ_CST);
}
