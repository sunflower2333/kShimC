/* SPDX-License-Identifier: MIT */
#pragma once
/*
 * PCI subsystem internals shared by the core (core.c, dt.c, alloc.c) and the
 * host bridges (ecam.c, qcom.c, qcom_pcielib.c). Not a public interface:
 * see kshim/pci.h and docs/pci.md.
 */
#include <kshim/pci.h>
#include <kshim/io.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <mmio.h>

struct kshim_resources;

/* Largest part of one "ranges" window that is mapped and used for BAR
 * assignment. QEMU virt describes a 512 GiB 64-bit window; nothing kShimC
 * drives needs more than a few MiB, so each window is cut to 1 GiB. */
#define KSHIM_PCI_WINDOW_CAP   UINT64_C(0x40000000)
/* ECAM: one MiB of configuration space per bus. */
#define KSHIM_PCI_ECAM_BUS_SIZE UINT64_C(0x100000)

/* ---- MMIO -------------------------------------------------------------- */
/* Every host register access goes through these, so the host tests can put
 * a register model behind them. Widths 1, 2, 4; naturally aligned. */
#ifdef KSHIM_HOST_TEST
struct kshim_pci_test_mmio {
    uint32_t (*read)(uintptr_t address, unsigned width);
    void (*write)(uintptr_t address, unsigned width, uint32_t value);
};
extern const struct kshim_pci_test_mmio *kshim_pci_test_mmio;
static inline uint32_t pci_mmio_read(uintptr_t a, unsigned width)
{
    return kshim_pci_test_mmio->read(a, width);
}
static inline void pci_mmio_write(uintptr_t a, unsigned width, uint32_t v)
{
    kshim_pci_test_mmio->write(a, width, v);
}
#else
static inline void pci_barrier(void)
{
#if defined(__aarch64__)
    __asm__ volatile("dsb sy" ::: "memory");
#else
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}
static inline uint32_t pci_mmio_read(uintptr_t a, unsigned width)
{
    uint32_t v;
    pci_barrier();
    if (width == 1) v = kshim_mmio_read8(a);
    else if (width == 2) v = kshim_mmio_read16(a);
    else v = kshim_mmio_read32(a);
    pci_barrier();
    return v;
}
static inline void pci_mmio_write(uintptr_t a, unsigned width, uint32_t v)
{
    pci_barrier();
    if (width == 1) kshim_mmio_write8(a, (uint8_t)v);
    else if (width == 2) kshim_mmio_write16(a, (uint16_t)v);
    else kshim_mmio_write32(a, v);
    pci_barrier();
}
#endif
static inline uint32_t pci_read32(uintptr_t a) { return pci_mmio_read(a, 4); }
static inline void pci_write32(uintptr_t a, uint32_t v) { pci_mmio_write(a, 4, v); }

/* ---- Devicetree (dt.c) ------------------------------------------------- */
/* Translate a bus address of `node`'s address space (the space its "reg"
 * and "ranges" parent side live in, i.e. the parent's) to a CPU address
 * through every ancestor "ranges". */
int kshim_pci_dt_translate(const void *fdt, int node, uint64_t address, uint64_t size,
                           uint64_t *cpu);
/* "ranges" of a host bridge into host->windows (capped, see above). */
int kshim_pci_dt_windows(const void *fdt, int node, struct kshim_pci_host *host);
/* Map a root-bus unit address (bus << 16 | devfn << 8) and INTx pin (1..4)
 * through the host's interrupt-map to a GIC INTID. ERROR_NOT_FOUND: no
 * entry; ERROR_NOT_SUPPORTED: the parent is not the GIC. */
int kshim_pci_dt_intx(const void *fdt, int host_node, uint32_t unit_hi, uint8_t pin,
                      uint32_t *intid, unsigned *trigger);
/* Map every window of `host` (device, RW). */
int kshim_pci_map_windows(struct kshim_resources *r, const struct kshim_pci_host *host);

/* ---- Core services for hosts (core.c) ---------------------------------- */
/* Runtime FDT given to kshim_pci_init (NULL before). */
const void *kshim_pci_fdt(void);
/* Thread-context delay that sleeps on interrupts when live. */
void kshim_pci_delay_us(uint32_t us);
uint64_t kshim_pci_time_us(void);
/* Whether kshim_pci_map_resources() was run and failed for this host node:
 * its windows are not mapped and it must not be touched. */
bool kshim_pci_node_unmapped(int node);
void kshim_pci_mark_node(int node, bool mapped);

/* ---- Host providers ---------------------------------------------------- */
/* map: DT only, adds the host's windows (kshim_pci_map_resources). probe:
 * thread context, from kshim_pci_init(): register every available host. */
int kshim_pci_ecam_map(const void *fdt, struct kshim_resources *r);
int kshim_pci_ecam_probe(const void *fdt);
int kshim_pci_qcom_map(const void *fdt, struct kshim_resources *r);
int kshim_pci_qcom_probe(const void *fdt);

/* ---- Optional self-test driver (selftest.c, CONFIG_KSHIM_PCI_SELFTEST) -- */
int kshim_pci_selftest_register(void);

/* ---- Bus resource allocator (alloc.c) ---------------------------------- */
/* A set of busy intervals inside one aperture; first fit with alignment. */
#define KSHIM_PCI_SPAN_MAX 32U
struct kshim_pci_span { uint64_t base, size; };
struct kshim_pci_aperture {
    uint64_t base, size;        /* bus addresses; size 0 = none */
    unsigned count;
    struct kshim_pci_span busy[KSHIM_PCI_SPAN_MAX];
};
void kshim_pci_aperture_init(struct kshim_pci_aperture *a, uint64_t base, uint64_t size);
bool kshim_pci_aperture_contains(const struct kshim_pci_aperture *a, uint64_t base, uint64_t size);
/* Reserve [base, base + size); false when outside or overlapping. */
bool kshim_pci_aperture_reserve(struct kshim_pci_aperture *a, uint64_t base, uint64_t size);
/* Allocate size bytes aligned to align (power of two) at or above `floor`. */
bool kshim_pci_aperture_alloc(struct kshim_pci_aperture *a, uint64_t size, uint64_t align,
                              uint64_t floor, uint64_t *base);
/* Drop an exact span added by reserve/alloc. */
void kshim_pci_aperture_release(struct kshim_pci_aperture *a, uint64_t base, uint64_t size);

/* ---- Map helper (map.c) ------------------------------------------------ */
/* Run fn for one host node; on failure roll back exactly the mappings it
 * added and remember the node as unmapped. */
typedef int (*kshim_pci_map_fn)(struct kshim_resources *r, void *context);
int kshim_pci_map_guarded(struct kshim_resources *r, int node, kshim_pci_map_fn fn,
                          void *context);
/* kshim_resource_map() for a device RW window. */
int kshim_pci_map_device(struct kshim_resources *r, uint64_t base, uint64_t size);
