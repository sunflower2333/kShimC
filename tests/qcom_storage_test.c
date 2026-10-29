/* SPDX-License-Identifier: MIT */
/* DT discovery for Qualcomm UFS/SDCC plus CrDK target matching and the
 * CrDK UFS power FDT parser/policy, on the kona storage fixture and the real
 * HDK8150 Android DT. Pure data; no MMIO. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Library/cr_android.h>
#include <error_no.h>
#include <kshim/qcom_storage.h>

static void *load(const char *path)
{
    FILE *f = fopen(path, "rb");
    assert(f);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *buf = aligned_alloc(8, (size_t)(n + 7) & ~(size_t)7);
    assert(buf && fread(buf, 1, (size_t)n, f) == (size_t)n);
    fclose(f);
    assert(!dt_validate(buf, (size_t)n));
    return buf;
}

static void kona(void)
{
    void *fdt = load(GENERATED_FIXTURES "/kona-storage.dtb");
    struct kshim_storage_dt dt;
    assert(kshim_dt_storage(fdt, &dt) == 0);
    assert(dt.soc == 8250 && dt.ufs_count == 1 && dt.sdhc_count == 1 && dt.has_rpmh);
    const struct kshim_dt_ufs *u = &dt.ufs[0];
    assert(u->hci.base == 0x1d84000 && u->hci.size == 0x3000 && u->has_phy && u->phy.base == 0x1d87000);
    assert(u->gdsc == 0x177004);
    const struct kshim_dt_sdhc *s = &dt.sdhc[0];
    assert(s->regs.base == 0x8804000 && s->index == 2 && !s->non_removable);
    assert(s->has_cd && s->cd_active_low);
    assert(s->cd_io == 0xf000000 + CrAndroidPin(8250, 77)->offset + 4);
    assert(dt.rpmh.rsc_base == 0x18220000 && dt.rpmh.drv_id == 2 && dt.rpmh.active_count == 2);
    assert(dt.rpmh.cmd_db_base == 0x80860000);

    const struct kshim_qcom_soc *soc = kshim_qcom_soc(8250);
    assert(soc && soc->has_clocks);
    const CR_TARGET_UFS_CONTEXT *t = soc->ufs();
    assert(CrTargetUfsValid(t) && t->HciBase == u->hci.base && t->PhyBase == u->phy.base);
    const CR_TARGET_SD_CONTEXT *sd = soc->sd();
    assert(CrTargetSdValid(sd) && sd->Base == s->regs.base && sd->CardDetectActiveLow);

    /* CrDK FDT facts + board policy select one vote per rail. */
    CR_TARGET_UFS_RAIL rails[CR_TARGET_UFS_MAX_RAILS];
    unsigned count; bool from_dt;
    assert(kshim_qcom_ufs_rails(fdt, t, rails, &count, &from_dt) == 0 && from_dt && count == 5);
    static const struct { const char *name; uint32_t mv; } want[] = {
        {"ldoa5", 880}, {"ldoa9", 1200}, {"ldoa17", 2504}, {"ldoa6", 1200}, {"smpa4", 1800}};
    for (unsigned i = 0; i < 5; ++i) {
        assert(!strcmp(rails[i].Name, want[i].name) && rails[i].VoltageMv == want[i].mv);
        assert(rails[i].Mode == 7);
    }
    free(fdt);
}

static void hdk8150(void)
{
    void *fdt = load(FIXTURES "/hdk8150-fdt.dtb");
    struct kshim_storage_dt dt;
    assert(kshim_dt_storage(fdt, &dt) == 0);
    assert(dt.soc == 8150 && dt.ufs_count == 1 && dt.sdhc_count == 1 && dt.has_rpmh);
    assert(dt.ufs[0].hci.base == 0x1d84000 && dt.ufs[0].hci.size == 0x2500);
    assert(dt.ufs[0].has_phy && dt.ufs[0].phy.base == 0x1d87000 && dt.ufs[0].gdsc == 0x177004);
    assert(dt.sdhc[0].regs.base == 0x8804000 && dt.sdhc[0].index == 2 && dt.sdhc[0].has_cd);
    /* No CrDK storage data for SM8150: UFS is inherit-only behind GDSC. */
    assert(!kshim_qcom_soc(8150));
    /* The SM8250 policy requires DT qualification; an SM8150 tree must not
     * produce guessed votes. */
    const CR_TARGET_UFS_CONTEXT *t = kshim_qcom_soc(8250)->ufs();
    CR_TARGET_UFS_RAIL rails[CR_TARGET_UFS_MAX_RAILS];
    unsigned count = 99; bool from_dt = true;
    assert(kshim_qcom_ufs_rails(fdt, t, rails, &count, &from_dt) == ERROR_NOT_SUPPORTED);
    assert(count == 0 && !from_dt);
    free(fdt);
}

int main(void)
{
    kona();
    hdk8150();
    printf("qcom_storage_test: PASS\n");
    return 0;
}
