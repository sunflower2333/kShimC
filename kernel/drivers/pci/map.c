/* SPDX-License-Identifier: MIT */
/*
 * kshim_pci_map_resources(): before the MMU is built, add every host's
 * configuration and register windows and its (capped) "ranges" windows as
 * device memory. DT only, no MMIO. A host whose mappings do not fit is
 * rolled back on its own and skipped by kshim_pci_init().
 */
#include "pci_internal.h"

#include <config.h>
#include <qcom_dt.h>
#include <string.h>

int kshim_pci_map_device(struct kshim_resources *r, uint64_t base, uint64_t size)
{
    return kshim_resource_map(r, base, size, KSHIM_MMU_DEVICE,
                              KSHIM_MMU_READ | KSHIM_MMU_WRITE) ? ERROR_NOT_SUPPORTED : 0;
}

int kshim_pci_map_windows(struct kshim_resources *r, const struct kshim_pci_host *host)
{
    for (unsigned i = 0; i < host->window_count; ++i) {
        int status = kshim_pci_map_device(r, host->windows[i].cpu_base, host->windows[i].size);
        if (status) return status;
    }
    return 0;
}

int kshim_pci_map_guarded(struct kshim_resources *r, int node, kshim_pci_map_fn fn,
                          void *context)
{
    static kshim_mmu_region_t saved[KSHIM_RESOURCE_MAX];
    size_t count = r->mapping_count;
    memcpy(saved, r->mappings, count * sizeof(*saved));
    int status = fn(r, context);
    if (status) {
        memcpy(r->mappings, saved, count * sizeof(*saved));
        r->mapping_count = count;
    }
    kshim_pci_mark_node(node, status == 0);
    return status;
}

int kshim_pci_map_resources(struct kshim_resources *r)
{
    if (!r || !r->fdt) return 0;
#if CONFIG_KSHIM_PCI_ECAM
    (void)kshim_pci_ecam_map(r->fdt, r);
#endif
#if CONFIG_KSHIM_PCI_QCOM
    (void)kshim_pci_qcom_map(r->fdt, r);
#endif
    return 0;
}
