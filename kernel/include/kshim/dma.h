/* SPDX-License-Identifier: MIT */
#pragma once
/*
 * DMA memory for bus-master devices (xHCI rings, USB transfer buffers).
 * Owner: xhci (kernel/core/dma.c). See docs/io-stack.md.
 *
 * kShimC is identity-mapped: the bus address of a buffer is its physical
 * address, which equals its VA. PCI devices behind a Qualcomm root complex
 * also go through the SMMU stream the previous bootloader left; kShimC does
 * not program the SMMU and assumes bypass/identity for that stream (as the
 * UFS driver does). A host bridge "dma-ranges" offset, if one is ever needed,
 * is applied by the PCI core (kshim_pci_dma_addr), not here.
 *
 * Buffers are cacheable normal memory when the MMU is on and uncached device
 * memory while it is off (serial-menu path), so ownership transfers always
 * use the sync calls; they reduce to barriers when maintenance is not needed.
 * The pool is never handed to the payload: kshim_io_quiesce() stops every
 * DMA master before the runtime memory is released.
 */
#include <stddef.h>
#include <stdint.h>

/* Thread context. Zeroed, `align` (power of two, >= 64) aligned, never
 * crossing a `boundary` (power of two, 0 = none; xHCI rings use 64 KiB)
 * and below `max_addr` (inclusive; UINT64_MAX = no limit, 0xffffffff for a
 * 32-bit-only master). Returns NULL when exhausted. *bus receives the
 * device-visible address. */
void *kshim_dma_alloc(size_t size, size_t align, size_t boundary,
                      uint64_t max_addr, uint64_t *bus);
void kshim_dma_free(void *va, size_t size);

/* CPU wrote [va, va+len): make it visible to the device (clean). */
void kshim_dma_sync_for_device(const void *va, size_t len);
/* Device wrote [va, va+len): drop stale CPU lines before reading
 * (clean+invalidate; callers own whole cache lines of the range). */
void kshim_dma_sync_for_cpu(void *va, size_t len);
