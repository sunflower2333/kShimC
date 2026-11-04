/* SPDX-License-Identifier: MIT */
/*
 * Bus address allocator for BARs and bridge windows: an aperture with a
 * sorted list of busy spans, first fit with natural alignment. Inherited
 * assignments are reserved first, new ones are allocated around them.
 */
#include "pci_internal.h"

#include <string.h>

void kshim_pci_aperture_init(struct kshim_pci_aperture *a, uint64_t base, uint64_t size)
{
    memset(a, 0, sizeof(*a));
    if (size && base <= UINT64_MAX - (size - 1)) {
        a->base = base;
        a->size = size;
    }
}

bool kshim_pci_aperture_contains(const struct kshim_pci_aperture *a, uint64_t base,
                                 uint64_t size)
{
    return a->size && size && base >= a->base && base - a->base <= a->size - 1 &&
           size <= a->size - (base - a->base);
}

bool kshim_pci_aperture_reserve(struct kshim_pci_aperture *a, uint64_t base, uint64_t size)
{
    if (!kshim_pci_aperture_contains(a, base, size) || a->count == KSHIM_PCI_SPAN_MAX)
        return false;
    unsigned at = 0;
    while (at < a->count && a->busy[at].base < base) ++at;
    /* Overlap with the neighbours on either side. */
    if (at > 0 && a->busy[at - 1].base + a->busy[at - 1].size > base) return false;
    if (at < a->count && base + size > a->busy[at].base) return false;
    memmove(&a->busy[at + 1], &a->busy[at], (a->count - at) * sizeof(a->busy[0]));
    a->busy[at] = (struct kshim_pci_span){base, size};
    ++a->count;
    return true;
}

static bool align_up(uint64_t value, uint64_t align, uint64_t *out)
{
    uint64_t mask = align - 1;
    if (value > UINT64_MAX - mask) return false;
    *out = (value + mask) & ~mask;
    return true;
}

bool kshim_pci_aperture_alloc(struct kshim_pci_aperture *a, uint64_t size, uint64_t align,
                              uint64_t floor, uint64_t *base)
{
    if (!a->size || !size || !align || (align & (align - 1))) return false;
    uint64_t cursor = a->base > floor ? a->base : floor;
    for (unsigned i = 0; i <= a->count; ++i) {
        uint64_t candidate;
        if (!align_up(cursor, align, &candidate)) return false;
        uint64_t gap_end = i < a->count ? a->busy[i].base : a->base + (a->size - 1) + 1;
        bool fits = candidate <= UINT64_MAX - size && candidate + size <= gap_end &&
                    candidate >= cursor;
        /* The last gap ends at the aperture end, which may be 2^64. */
        if (i == a->count && a->base + (a->size - 1) == UINT64_MAX)
            fits = candidate <= UINT64_MAX - (size - 1);
        if (fits && kshim_pci_aperture_reserve(a, candidate, size)) {
            *base = candidate;
            return true;
        }
        if (i < a->count) {
            uint64_t end = a->busy[i].base + a->busy[i].size;
            if (end > cursor) cursor = end;
        }
    }
    return false;
}

void kshim_pci_aperture_release(struct kshim_pci_aperture *a, uint64_t base, uint64_t size)
{
    for (unsigned i = 0; i < a->count; ++i) {
        if (a->busy[i].base != base || a->busy[i].size != size) continue;
        memmove(&a->busy[i], &a->busy[i + 1], (a->count - i - 1) * sizeof(a->busy[0]));
        --a->count;
        return;
    }
}
