/* SPDX-License-Identifier: MIT */
#pragma once
/* DT-driven discovery and probing of Qualcomm UFS and SDHCI storage. */
#include <stdbool.h>
#include <stdint.h>

#include <dt.h>
#include <Library/cr_resources.h> /* struct CrQupResources: RPMh/CMD-DB carrier */

#define KSHIM_STORAGE_MAX_UFS  1
#define KSHIM_STORAGE_MAX_SDHC 2

struct kshim_dt_ufs {
    int node;
    struct dt_range hci, phy;
    uint64_t gdsc;            /* GDSC control register from DT, 0 if unknown */
    bool has_phy;
    unsigned index;
};

struct kshim_dt_sdhc {
    int node;
    struct dt_range regs;
    unsigned index;           /* "sdhc<index>" */
    bool non_removable;       /* eMMC / soldered */
    bool has_cd;
    uintptr_t cd_io;          /* TLMM IN/OUT register of the card-detect pin */
    bool cd_active_low;
    uint32_t max_hz;
};

struct kshim_storage_dt {
    unsigned soc;             /* 8150, 8250, 8450 or 0 */
    struct kshim_dt_ufs ufs[KSHIM_STORAGE_MAX_UFS];
    unsigned ufs_count;
    struct kshim_dt_sdhc sdhc[KSHIM_STORAGE_MAX_SDHC];
    unsigned sdhc_count;
    bool has_rpmh;            /* apps RSC + CMD DB found */
    struct CrQupResources rpmh;
};

/* Pure DT parsing; no MMIO. */
unsigned kshim_dt_storage_soc(const void *fdt);
int kshim_dt_storage(const void *fdt, struct kshim_storage_dt *out);

struct kshim_resources;
/* Discover storage and add its MMIO/RPMh windows to the MMU map. Optional:
 * a failure leaves the existing mappings unchanged and storage disabled. */
int kshim_storage_map_resources(struct kshim_resources *resources);
/* Bring up every discovered controller and register its block devices.
 * Returns the number of block devices registered. */
int kshim_storage_probe(const void *fdt);
/* Stop storage DMA before handing off to a payload. */
void kshim_storage_quiesce(void);
