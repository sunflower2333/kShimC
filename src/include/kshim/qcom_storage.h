/* SPDX-License-Identifier: MIT */
#pragma once
/*
 * Qualcomm storage resources from unmodified CrDK target data and libraries.
 * Requires src/drivers/block/crdk_shim ahead of the CrDK include directory
 * (CrDK target headers include <Uefi.h>).
 */
#include <stdbool.h>
#include <stdint.h>

#include <Library/CrTargetUfsLib.h>
#include <Library/CrTargetSdLib.h>
#include <Library/UfsPowerPolicyCrLib.h>
#include <kshim/storage.h>

struct ClockDriverContext;

struct kshim_qcom_soc {
    unsigned soc;
    const CR_TARGET_UFS_CONTEXT *(*ufs)(void);
    const CR_TARGET_SD_CONTEXT *(*sd)(void);
    bool has_clocks;      /* CrDK ClockLib target graph linked for this SoC */
    uint64_t clock_windows[2][2]; /* {base,size} of controllers ClockLib maps */
};

/* NULL when kShimC has no CrDK data for the SoC (e.g. SM8150). */
const struct kshim_qcom_soc *kshim_qcom_soc(unsigned soc);

/* CrDK ClockLib, lazily initialized for the SoC. hz==0 keeps the inherited
 * or table default rate for gates. */
int kshim_qcom_clock_on(const struct kshim_qcom_soc *s, const char *controller,
                        const char *name, uint64_t hz);
int kshim_qcom_clock_off(const struct kshim_qcom_soc *s, const char *controller, const char *name);
int kshim_qcom_gdsc_on(const struct kshim_qcom_soc *s, const char *controller, const char *name);
int kshim_qcom_reset(const struct kshim_qcom_soc *s, const char *controller, const char *name,
                     bool assert);
/* Collapse state of a GDSC control register (bit31 PWR_ON). */
bool kshim_qcom_gdsc_powered(uintptr_t reg);

/* RPMh VRM vote through the polled CMD DB + TCS port API. */
int kshim_qcom_rail(const struct kshim_storage_dt *dt, const char *name, uint32_t mv, uint8_t mode);

/* Rails for the UFS host: CrUfsPowerFdtParse + CrUfsPowerResolve against the
 * runtime DT; falls back to the target's legacy list only when the target
 * does not require boot-FDT qualification. */
int kshim_qcom_ufs_rails(const void *fdt, const CR_TARGET_UFS_CONTEXT *target,
                         CR_TARGET_UFS_RAIL *rails, unsigned *count, bool *from_dt);

struct ufshci_host;
struct sdmmc_host;
/* Platform bring-up + core probe. Returns 0 when block devices registered. */
int kshim_ufs_qcom_probe(const void *fdt, const struct kshim_storage_dt *dt,
                         const struct kshim_dt_ufs *ufs, struct ufshci_host *host);
int kshim_sdhci_msm_probe(const struct kshim_storage_dt *dt, const struct kshim_dt_sdhc *sdhc,
                          struct sdmmc_host *host);
