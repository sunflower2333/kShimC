/* SPDX-License-Identifier: MIT */
/*
 * Qualcomm UFS host bring-up for kShimC.
 *
 * Path selection (logged at boot):
 *  1. Power: with a CrDK target for the SoC (SM8250 today), rails are voted
 *     from CrUfsPowerFdtParse/CrUfsPowerResolve, then the UFS GDSC, the seven
 *     non-lane clocks and the three lane symbol clocks are acquired through
 *     CrDK ClockLib at the target rates. All of these are idempotent on the
 *     state XBL/ABL left behind. Without a clock graph (SM8150, SM8450) the
 *     HCI is only touched when its GDSC reports power-on.
 *  2. Lane symbol clocks only run when the QMP PHY is calibrated and locked.
 *     If they cannot be acquired the PHY is not running; kShimC does not
 *     calibrate the PHY (see below) and stops before touching the HCI.
 *  3. The UFSHCI core then prefers the inherited controller/link (verified:
 *     HCE, DP, list/UIC ready, idle doorbells, no fatal status, live UniPro
 *     lanes, NOP OUT answered) and falls back to a standard UFSHCI reset +
 *     DME_LINKSTARTUP + fDeviceInit on the still-calibrated PHY.
 *
 * Not implemented: QMP PHY calibration and the vendor HCI configuration that
 * surrounds it. CrDK's tables are reusable data, but the only sequencing for
 * them is in CrDK's UfsHcCrDxe.c (no license tag; follows Linux ufs-qcom /
 * phy-qcom-qmp-ufs), so a cold (unpowered PHY) start is reported as
 * unsupported instead of guessed.
 */
#include <kshim/qcom_storage.h>
#include <kshim/ufshci.h>
#include <kshim/dma_cache.h>
#include <config.h>

#include <stdio.h>
#include <string.h>

/* Kconfig leaves a disabled bool undefined, so test for definition. */
#ifdef CONFIG_BLK_UFS_FULL_INIT
#define UFS_POLICY UFSHCI_PREFER_INHERIT
#else
#define UFS_POLICY UFSHCI_INHERIT_ONLY
#endif
#ifndef CONFIG_BLK_UFS_WRITE
#define CONFIG_BLK_UFS_WRITE 0
#endif

#define LOG(fmt, ...) printf("ufs0: " fmt "\n", ##__VA_ARGS__)

static const CR_TARGET_UFS_CONTEXT *match_target(const struct kshim_qcom_soc *soc,
                                                 const struct kshim_dt_ufs *u)
{
    const CR_TARGET_UFS_CONTEXT *t = soc && soc->ufs ? soc->ufs() : NULL;
    if (!t) return NULL;
    if (!CrTargetUfsValid(t)) { LOG("CrDK UFS target rejected by validation"); return NULL; }
    if (t->HciBase != u->hci.base || (u->has_phy && t->PhyBase != u->phy.base)) {
        LOG("DT HCI %#llx/PHY %#llx do not match CrDK target %#llx/%#llx",
            (unsigned long long)u->hci.base, (unsigned long long)u->phy.base,
            (unsigned long long)t->HciBase, (unsigned long long)t->PhyBase);
        return NULL;
    }
    return t;
}

static const char *clock_controller(const CR_TARGET_UFS_CONTEXT *t, unsigned i)
{
    return t->ClockControllers ? t->ClockControllers[i] : t->ClockController;
}

/* Returns 0 when the HCI is powered and clocked with a running PHY. */
static int power_up(const void *fdt, const struct kshim_storage_dt *dt,
                    const struct kshim_qcom_soc *soc, const CR_TARGET_UFS_CONTEXT *t,
                    uint64_t gdsc_reg)
{
    if (!t || !soc->has_clocks) {
        if (!gdsc_reg) {
            LOG("no GDSC register and no CrDK clock graph; HCI left untouched");
            return ERROR_NOT_SUPPORTED;
        }
        if (!kshim_qcom_gdsc_powered((uintptr_t)gdsc_reg)) {
            LOG("UFS GDSC collapsed and no CrDK clock graph for SM%u; cold start unsupported", dt->soc);
            return ERROR_NOT_SUPPORTED;
        }
        LOG("SM%u: power and clocks inherited (GDSC on)", dt->soc);
        return 0;
    }

    CR_TARGET_UFS_RAIL rails[CR_TARGET_UFS_MAX_RAILS];
    unsigned count = 0;
    bool from_dt = false;
    if (!kshim_qcom_ufs_rails(fdt, t, rails, &count, &from_dt)) {
        for (unsigned i = 0; i < count; ++i) {
            int st = kshim_qcom_rail(dt, rails[i].Name, rails[i].VoltageMv, rails[i].Mode);
            if (st) LOG("rail %s not voted (%d); relying on inherited vote", rails[i].Name, st);
        }
        LOG("%u rails voted from %s", count, from_dt ? "DT-qualified policy" : "target profile");
    }
    int st = kshim_qcom_gdsc_on(soc, t->ClockController, t->Gdsc);
    if (st) return st;
    for (unsigned i = 0; i < t->PrePhyClockCount; ++i)
        if ((st = kshim_qcom_clock_on(soc, clock_controller(t, i), t->Clocks[i].Name, t->Clocks[i].RateHz)))
            return st;
    for (unsigned i = t->PrePhyClockCount; i < t->ClockCount; ++i)
        if (kshim_qcom_clock_on(soc, clock_controller(t, i), t->Clocks[i].Name, t->Clocks[i].RateHz)) {
            LOG("lane clock %s not running: QMP PHY is not calibrated; cold start unsupported",
                t->Clocks[i].Name);
            return ERROR_NOT_SUPPORTED;
        }
    if (gdsc_reg && !kshim_qcom_gdsc_powered((uintptr_t)gdsc_reg)) return ERROR_DEVICE_ERROR;
    return 0;
}

int kshim_ufs_qcom_probe(const void *fdt, const struct kshim_storage_dt *dt,
                         const struct kshim_dt_ufs *u, struct ufshci_host *h)
{
    const struct kshim_qcom_soc *soc = kshim_qcom_soc(dt->soc);
    const CR_TARGET_UFS_CONTEXT *t = match_target(soc, u);
    uint64_t gdsc = u->gdsc;
    if (!gdsc && t && t->PowerPolicy) gdsc = t->PowerPolicy->GdscBase;
    LOG("HCI %#llx SoC SM%u, CrDK target %s", (unsigned long long)u->hci.base, dt->soc,
        t ? "matched" : "none");
    int st = power_up(fdt, dt, soc, t, gdsc);
    if (st) return st;

    const struct CrIo *io = CrIoDefault();
    if (!io) return ERROR_NOT_SUPPORTED;
    memset(h, 0, sizeof(*h));
    h->base = (uintptr_t)u->hci.base;
    h->io = *io;
    h->dma = (struct ufshci_dma){NULL, kshim_dma_identity, kshim_dma_clean_cb, kshim_dma_invalidate_cb};
    h->name = "ufs0";
    h->policy = UFS_POLICY;
    h->allow_write = CONFIG_BLK_UFS_WRITE;
    st = ufshci_init(h);
    if (st) {
        LOG("controller init failed (%d), path %s%s%s", st, ufshci_path_name(h->path),
            h->inherit_reject ? ", inherit rejected: " : "", h->inherit_reject ? h->inherit_reject : "");
        return st;
    }
    return ufshci_scan(h);
}
