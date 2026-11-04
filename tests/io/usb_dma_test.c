/* SPDX-License-Identifier: MIT */
/*
 * kernel/core/dma.c: alignment, 64 KiB no-crossing boundary, address limit,
 * zeroing, ownership checks, free/reuse and exhaustion.
 */
#include <kshim/dma.h>

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    size_t pool = kshim_dma_pool_size();
    assert(pool >= 0x10000 && kshim_dma_in_use() == 0);
    uint64_t bus = 0;

    /* Alignment, zeroing, bus == VA (identity). */
    uint8_t *a = kshim_dma_alloc(100, 64, 0, UINT64_MAX, &bus);
    assert(a && ((uintptr_t)a & 63) == 0 && bus == (uint64_t)(uintptr_t)a);
    for (unsigned i = 0; i < 100; ++i) assert(a[i] == 0);
    memset(a, 0xa5, 100);
    assert(kshim_dma_in_use() == 128);
    uint8_t *b = kshim_dma_alloc(4096, 4096, 4096, UINT64_MAX, &bus);
    assert(b && ((uintptr_t)b & 4095) == 0);

    /* Ownership: inside one allocation only. */
    assert(kshim_dma_owns(a, 100) && kshim_dma_owns(a + 64, 64));
    assert(!kshim_dma_owns(a, 129));            /* spills into a free unit */
    uint8_t outside[16];
    assert(!kshim_dma_owns(outside, sizeof(outside)));
    assert(!kshim_dma_owns(NULL, 1) && !kshim_dma_owns(a, 0));

    /* Boundary: a 48 KiB block never crosses 64 KiB. */
    for (unsigned i = 0; i < 4; ++i) {
        uint8_t *c = kshim_dma_alloc(48 * 1024, 64, 0x10000, UINT64_MAX, &bus);
        if (!c) break;
        assert(((uintptr_t)c >> 16) == (((uintptr_t)c + 48 * 1024 - 1) >> 16));
        kshim_dma_free(c, 48 * 1024);
    }
    /* Invalid requests. */
    assert(!kshim_dma_alloc(0, 64, 0, UINT64_MAX, &bus));
    assert(!kshim_dma_alloc(64, 96, 0, UINT64_MAX, &bus));        /* align not a power of two */
    assert(!kshim_dma_alloc(8192, 64, 4096, UINT64_MAX, &bus));   /* larger than boundary */
    assert(!kshim_dma_alloc(64, 64, 0, 0x1000, &bus));            /* below the pool */

    /* Neighbouring allocations are distinct for kshim_dma_owns(). */
    uint8_t *d = kshim_dma_alloc(64, 64, 0, UINT64_MAX, &bus);
    uint8_t *e = kshim_dma_alloc(64, 64, 0, UINT64_MAX, &bus);
    assert(d && e && d != e);
    if (e == d + 64) assert(!kshim_dma_owns(d, 128));

    /* Free and reuse, with zeroing on the second allocation. */
    kshim_dma_free(a, 100);
    assert(!kshim_dma_owns(a, 1));
    uint8_t *a2 = kshim_dma_alloc(64, 64, 0, UINT64_MAX, &bus);
    assert(a2 == a && a2[0] == 0 && a2[63] == 0);
    kshim_dma_free(a2, 64);
    kshim_dma_free(b, 4096);
    kshim_dma_free(d, 64);
    kshim_dma_free(e, 64);
    /* Double free and foreign pointers are ignored. */
    kshim_dma_free(b, 4096);
    kshim_dma_free(outside, 16);
    assert(kshim_dma_in_use() == 0);

    /* Exhaustion: everything, then NULL, then free makes room again. */
    uint8_t *all = kshim_dma_alloc(pool, 4096, 0, UINT64_MAX, &bus);
    assert(all && kshim_dma_in_use() == pool);
    assert(!kshim_dma_alloc(64, 64, 0, UINT64_MAX, &bus));
    kshim_dma_free(all, pool);
    assert(kshim_dma_in_use() == 0 && kshim_dma_alloc(64, 64, 0, UINT64_MAX, &bus));

    kshim_dma_sync_for_device(NULL, 0);
    kshim_dma_sync_for_cpu(NULL, 0);
    puts("usb_dma_test: ok");
    return 0;
}
