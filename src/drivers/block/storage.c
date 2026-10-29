/* SPDX-License-Identifier: MIT */
/* Qualcomm storage discovery, MMU windows, probe and handoff quiesce. */
#include <kshim/storage.h>
#include <kshim/qcom_storage.h>
#include <kshim/sdmmc.h>
#include <kshim/ufshci.h>
#include <config.h>
#include <qcom_dt.h>

#include <stdio.h>
#include <string.h>

#ifndef CONFIG_BLK_UFS
#define CONFIG_BLK_UFS 0
#endif
#ifndef CONFIG_BLK_SDHCI
#define CONFIG_BLK_SDHCI 0
#endif

static struct kshim_storage_dt storage_dt;
static bool storage_dt_valid;
/* Fallback until the blkdev core is linked; its strong definition wins. */
__attribute__((weak)) int kshim_blkdev_register(struct kshim_blkdev *dev)
{
    printf("storage: %s found, but no blkdev core is linked\n", dev ? dev->name : "?");
    return ERROR_NOT_SUPPORTED;
}

#if CONFIG_BLK_UFS
static struct ufshci_host ufs_hosts[KSHIM_STORAGE_MAX_UFS];
static bool ufs_live[KSHIM_STORAGE_MAX_UFS];
#endif
#if CONFIG_BLK_SDHCI
static struct sdmmc_host sd_hosts[KSHIM_STORAGE_MAX_SDHC];
static bool sd_live[KSHIM_STORAGE_MAX_SDHC];
#endif

static int map_device(struct kshim_resources *r, uint64_t base, uint64_t size)
{
    return kshim_resource_map(r, base, size, KSHIM_MMU_DEVICE, KSHIM_MMU_READ | KSHIM_MMU_WRITE);
}

static int map_all(struct kshim_resources *r, struct kshim_storage_dt *dt)
{
    const struct kshim_qcom_soc *soc = kshim_qcom_soc(dt->soc);
    bool need_rpmh = false, need_clocks = false;
    if (CONFIG_BLK_UFS)
        for (unsigned i = 0; i < dt->ufs_count; ++i) {
            const struct kshim_dt_ufs *u = &dt->ufs[i];
            if (map_device(r, u->hci.base, u->hci.size < 0x3000 ? 0x3000 : u->hci.size)) return -1;
            uint64_t gdsc = u->gdsc;
            const CR_TARGET_UFS_CONTEXT *t = soc && soc->ufs ? soc->ufs() : NULL;
            if (!gdsc && t && t->PowerPolicy) gdsc = t->PowerPolicy->GdscBase;
            if (gdsc && map_device(r, gdsc, 4)) return -1;
            need_rpmh = need_clocks = true;
        }
    if (CONFIG_BLK_SDHCI)
        for (unsigned i = 0; i < dt->sdhc_count; ++i) {
            const struct kshim_dt_sdhc *s = &dt->sdhc[i];
            if (map_device(r, s->regs.base, s->regs.size)) return -1;
            if (s->has_cd && map_device(r, s->cd_io, 4)) return -1;
            need_rpmh = need_clocks = true;
        }
    if (need_clocks && soc && soc->has_clocks)
        for (unsigned i = 0; i < 2; ++i)
            if (soc->clock_windows[i][1] && map_device(r, soc->clock_windows[i][0], soc->clock_windows[i][1]))
                return -1;
    if (need_rpmh && dt->has_rpmh) {
        struct CrQupResources *q = &dt->rpmh;
        if (map_device(r, q->rsc_base, q->rsc_size)) return -1;
        if (q->cmd_db_dictionary) {
            /* Snapshot the CMD DB location before the MMU, as the touch path does. */
            const struct CrIo *io = CrIoDefault();
            if (!io || q->cmd_db_dictionary > UINTPTR_MAX - 8) return -1;
            q->cmd_db_base = io->read32(io->context, q->cmd_db_dictionary);
            q->cmd_db_size = io->read32(io->context, q->cmd_db_dictionary + 4);
            q->cmd_db_dictionary = 0;
            if ((q->cmd_db_base & 3) || q->cmd_db_size < 144 || q->cmd_db_size > 0x20000) return -1;
        }
        if (kshim_resource_map(r, q->cmd_db_base, q->cmd_db_size, KSHIM_MMU_NORMAL_NC, KSHIM_MMU_READ))
            return -1;
    }
    return 0;
}

int kshim_storage_map_resources(struct kshim_resources *r)
{
    storage_dt_valid = false;
    if (!r || !r->fdt || kshim_dt_storage(r->fdt, &storage_dt)) return -1;
    kshim_mmu_region_t saved[KSHIM_RESOURCE_MAX];
    size_t count = r->mapping_count;
    memcpy(saved, r->mappings, count * sizeof(*saved));
    if (map_all(r, &storage_dt)) {
        memcpy(r->mappings, saved, count * sizeof(*saved));
        r->mapping_count = count;
        printf("storage: MMIO windows unavailable; storage disabled\n");
        return -1;
    }
    storage_dt_valid = true;
    return 0;
}

int kshim_storage_probe(const void *fdt)
{
    int registered = 0;
    if (!storage_dt_valid) {
    printf("storage: no mapped storage resources\n");
    return 0;
    }
    (void)fdt;
#if CONFIG_BLK_UFS
    for (unsigned i = 0; i < storage_dt.ufs_count; ++i) {
        struct ufshci_host *h = &ufs_hosts[i];
        int st = kshim_ufs_qcom_probe(fdt, &storage_dt, &storage_dt.ufs[i], h);
        ufs_live[i] = h->ready;
        if (!st) registered += (int)h->lu_count;
        else printf("ufs0: unavailable (%d)\n", st);
    }
#endif
#if CONFIG_BLK_SDHCI
    for (unsigned i = 0; i < storage_dt.sdhc_count; ++i) {
        struct sdmmc_host *h = &sd_hosts[i];
        int st = kshim_sdhci_msm_probe(&storage_dt, &storage_dt.sdhc[i], h);
        sd_live[i] = h->core.Initialized;
        if (!st && h->ready) ++registered;
    }
#endif
    return registered;
}

void kshim_storage_quiesce(void)
{
#if CONFIG_BLK_UFS
    for (unsigned i = 0; i < KSHIM_STORAGE_MAX_UFS; ++i)
        if (ufs_live[i]) { (void)ufshci_quiesce(&ufs_hosts[i]); ufs_live[i] = false; }
#endif
#if CONFIG_BLK_SDHCI
    for (unsigned i = 0; i < KSHIM_STORAGE_MAX_SDHC; ++i)
        if (sd_live[i]) { (void)sdmmc_quiesce(&sd_hosts[i]); sd_live[i] = false; }
#endif
}
