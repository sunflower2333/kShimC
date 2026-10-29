/* SPDX-License-Identifier: MIT */
#pragma once
/* Data-cache maintenance for non-coherent DMA on an identity-mapped runtime.
 * No-ops off target (host tests) where DMA is simulated by the CPU. */
#include <stddef.h>
#include <stdint.h>

#if defined(__aarch64__)
static inline uintptr_t kshim_dcache_line(void)
{
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    return (uintptr_t)4 << ((ctr >> 16) & 0xf);
}

/* Write dirty lines back so the device observes CPU writes. */
static inline void kshim_dcache_clean(const void *va, size_t len)
{
    uintptr_t line = kshim_dcache_line(), p = (uintptr_t)va & ~(line - 1), end = (uintptr_t)va + len;
    __asm__ volatile("dsb ish" ::: "memory");
    for (; p < end; p += line) __asm__ volatile("dc cvac, %0" :: "r"(p) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}

/* Clean+invalidate: drops stale lines around device writes. Callers only
 * pass cache-line aligned buffers or buffers they exclusively own. */
static inline void kshim_dcache_invalidate(void *va, size_t len)
{
    uintptr_t line = kshim_dcache_line(), p = (uintptr_t)va & ~(line - 1), end = (uintptr_t)va + len;
    __asm__ volatile("dsb sy" ::: "memory");
    for (; p < end; p += line) __asm__ volatile("dc civac, %0" :: "r"(p) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}
#else
static inline void kshim_dcache_clean(const void *va, size_t len) { (void)va; (void)len; }
static inline void kshim_dcache_invalidate(void *va, size_t len) { (void)va; (void)len; }
#endif

static inline uint64_t kshim_dma_identity(void *context, const void *va)
{
    (void)context;
    return (uint64_t)(uintptr_t)va;
}
static inline void kshim_dma_clean_cb(void *context, const void *va, size_t len)
{
    (void)context;
    kshim_dcache_clean(va, len);
}
static inline void kshim_dma_invalidate_cb(void *context, void *va, size_t len)
{
    (void)context;
    kshim_dcache_invalidate(va, len);
}
