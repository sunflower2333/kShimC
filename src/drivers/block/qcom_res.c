/* SPDX-License-Identifier: MIT */
/*
 * Qualcomm storage resources for kShimC, built only from unmodified CrDK
 * pieces: per-SoC UFS/SD target descriptors (Target/Sm8250, Target/Sm8450),
 * the UFS FDT power parser and policy, ClockLib with the SM8250 GCC graph,
 * and the port's polled CMD DB / RPMh TCS API. Each SoC's CrTarget* entry
 * points are renamed at compile time (see block.cmake) so several targets
 * coexist and the runtime DT selects one.
 */
#include <kshim/qcom_storage.h>
#include <error_no.h>

#include <Library/CrTargetClockLib.h>
#include <Library/cr_resources.h>
#include <oskal/cr_memmap.h>
#include <stdio.h>
#include <string.h>

#ifndef KSHIM_STORAGE_SM8250
#define KSHIM_STORAGE_SM8250 1
#endif
#ifndef KSHIM_STORAGE_SM8450
#define KSHIM_STORAGE_SM8450 1
#endif
/* CrDK ClockLib with the SM8250 GCC graph (~80 KiB); optional. */
#ifndef KSHIM_STORAGE_CLOCKS
#define KSHIM_STORAGE_CLOCKS 1
#endif

#define LOG(fmt, ...) printf("qcom-storage: " fmt "\n", ##__VA_ARGS__)

#if KSHIM_STORAGE_SM8250
const CR_TARGET_UFS_CONTEXT *kshim_crdk_sm8250_ufs(void);
const CR_TARGET_SD_CONTEXT *kshim_crdk_sm8250_sd(void);
#endif
#if KSHIM_STORAGE_SM8250 && KSHIM_STORAGE_CLOCKS
#define SM8250_CLOCKS 1
ClockDriverContext *kshim_crdk_sm8250_clock(void);
CR_STATUS kshim_crdk_sm8250_clock_init(ClockDriverContext *);
CR_STATUS kshim_crdk_sm8250_clock_reset(const CHAR8 *, const CHAR8 *, BOOLEAN);
#else
#define SM8250_CLOCKS 0
#endif
#if KSHIM_STORAGE_SM8450
const CR_TARGET_UFS_CONTEXT *kshim_crdk_sm8450_ufs(void);
const CR_TARGET_SD_CONTEXT *kshim_crdk_sm8450_sd(void);
#endif

static const struct kshim_qcom_soc socs[] = {
#if KSHIM_STORAGE_SM8250
    {8250, kshim_crdk_sm8250_ufs, kshim_crdk_sm8250_sd, SM8250_CLOCKS,
     {{SM8250_CLOCKS ? 0x00100000 : 0, 0x1f0000}, {SM8250_CLOCKS ? 0x0af00000 : 0, 0x20000}}},
#endif
#if KSHIM_STORAGE_SM8450
    /* SM8450 GCC needs CrDK Sm8450ClockLib, which is not part of this port:
     * its UFS can only be inherited and its SD controller is not used. */
    {8450, kshim_crdk_sm8450_ufs, kshim_crdk_sm8450_sd, false, {{0, 0}, {0, 0}}},
#endif
};

const struct kshim_qcom_soc *kshim_qcom_soc(unsigned soc)
{
    for (unsigned i = 0; i < sizeof(socs) / sizeof(socs[0]); ++i)
        if (socs[i].soc == soc) return &socs[i];
    return NULL;
}

/* kShimC is identity mapped; the controller windows are added to the MMU
 * map by kshim_storage_map_resources() before the MMU is enabled. */
__attribute__((weak)) CR_STATUS MapDeviceIORegion(UINTN BaseAddress, UINTN Size)
{
    (void)BaseAddress; (void)Size;
    return CR_SUCCESS;
}

__attribute__((weak)) CR_STATUS UnMapMemRegion(UINTN BaseAddress, UINTN Size)
{
    (void)BaseAddress; (void)Size;
    return CR_SUCCESS;
}

static ClockDriverContext *clock_context(const struct kshim_qcom_soc *s)
{
#if SM8250_CLOCKS
    static ClockDriverContext *ctx;
    static bool failed;
    if (!s || !s->has_clocks || s->soc != 8250 || failed) return NULL;
    if (ctx) return ctx;
    ClockDriverContext *c = kshim_crdk_sm8250_clock();
    CR_STATUS st = ClockLibInit(&c);
    if (st == CR_SUCCESS) st = kshim_crdk_sm8250_clock_init(c);
    if (st != CR_SUCCESS) {
        LOG("ClockLib init failed (%d)", st);
        failed = true;
        return NULL;
    }
    ctx = c;
    return ctx;
#else
    (void)s;
    return NULL;
#endif
}

static int node(const struct kshim_qcom_soc *s, const char *controller, const char *name,
                ClockDriverContext **ctx, ClockNode **n)
{
    *ctx = clock_context(s);
    if (!*ctx) return ERROR_NOT_SUPPORTED;
    if (strcmp(controller, "gcc")) return ERROR_NOT_SUPPORTED;
    ClockControllerType type = CC_TYPE_GCC;
    UINT32 id = 0;
    return GetClockNode(*ctx, name, &type, &id, n) == CR_SUCCESS ? 0 : ERROR_NOT_FOUND;
}

int kshim_qcom_clock_on(const struct kshim_qcom_soc *s, const char *controller, const char *name,
                        uint64_t hz)
{
    ClockDriverContext *ctx; ClockNode *n;
    int status = node(s, controller, name, &ctx, &n);
    if (status) return status;
    BOOLEAN owned = FALSE;
    CR_STATUS st = ClockAcquire(ctx, n, hz, &owned);
    if (st != CR_SUCCESS) LOG("clock %s @%llu failed (%d)", name, (unsigned long long)hz, st);
    return st == CR_SUCCESS ? 0 : ERROR_DEVICE_ERROR;
}

int kshim_qcom_clock_off(const struct kshim_qcom_soc *s, const char *controller, const char *name)
{
    ClockDriverContext *ctx; ClockNode *n;
    int status = node(s, controller, name, &ctx, &n);
    if (status) return status;
    return ClockEnable(ctx, n, 0, FALSE) == CR_SUCCESS ? 0 : ERROR_DEVICE_ERROR;
}

int kshim_qcom_gdsc_on(const struct kshim_qcom_soc *s, const char *controller, const char *name)
{
    ClockDriverContext *ctx = clock_context(s);
    ClockNode *n;
    if (!ctx || strcmp(controller, "gcc")) return ERROR_NOT_SUPPORTED;
    if (GetGdscNode(ctx, name, &n) != CR_SUCCESS) return ERROR_NOT_FOUND;
    BOOLEAN owned = FALSE;
    CR_STATUS st = ClockGdscAcquire(ctx, n, &owned);
    if (st != CR_SUCCESS) LOG("GDSC %s failed (%d)", name, st);
    return st == CR_SUCCESS ? 0 : ERROR_DEVICE_ERROR;
}

int kshim_qcom_reset(const struct kshim_qcom_soc *s, const char *controller, const char *name, bool assert)
{
#if SM8250_CLOCKS
    if (clock_context(s) && s->soc == 8250)
        return kshim_crdk_sm8250_clock_reset(controller, name, assert) == CR_SUCCESS ? 0 : ERROR_DEVICE_ERROR;
#endif
    (void)s; (void)controller; (void)name; (void)assert;
    return ERROR_NOT_SUPPORTED;
}

bool kshim_qcom_gdsc_powered(uintptr_t reg)
{
    const struct CrIo *io = CrIoDefault();
    if (!io || !reg) return false;
    return (io->read32(io->context, reg) >> 31) & 1;
}

int kshim_qcom_rail(const struct kshim_storage_dt *dt, const char *name, uint32_t mv, uint8_t mode)
{
    const struct CrIo *io = CrIoDefault();
    if (!dt || !dt->has_rpmh || !io) return ERROR_NOT_SUPPORTED;
    /* Carrier for the port's polled API: only io and the RSC/CMD DB fields
     * of the board description are used by CmdDB/RPMh. */
    struct CrQupContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.io = *io;
    ctx.board = &dt->rpmh;
    uint32_t address = 0;
    int st = dt->rpmh.cmd_db_dictionary
        ? CrCmdDbLookupDictionary(io, dt->rpmh.cmd_db_dictionary, name, &address)
        : CrCmdDbLookup(io, dt->rpmh.cmd_db_base, dt->rpmh.cmd_db_size, name, &address);
    if (st || ((address >> 16) & 7U) != 4U) {
        LOG("CMD DB has no VRM %s (%d)", name, st);
        return ERROR_NOT_FOUND;
    }
    /* VRM: +0 voltage (mV), +4 enable, +8 mode. Same order as the port's
     * touch rails: level, mode, then enable. */
    if (mv && (st = CrRpmhPollWrite(&ctx, address, mv))) goto out;
    if (mode && (st = CrRpmhPollWrite(&ctx, address + 8U, mode))) goto out;
    st = CrRpmhPollWrite(&ctx, address + 4U, 1);
out:
    if (st) LOG("RPMh vote %s %u mV failed (%d)", name, (unsigned)mv, st);
    return st ? ERROR_DEVICE_ERROR : 0;
}

int kshim_qcom_ufs_rails(const void *fdt, const CR_TARGET_UFS_CONTEXT *target,
                         CR_TARGET_UFS_RAIL *rails, unsigned *count, bool *from_dt)
{
    static CR_UFS_POWER_FDT facts __attribute__((aligned(8)));
    CR_UFS_POWER_RESOLVED resolved;
    *count = 0;
    *from_dt = false;
    if (!target || !CrTargetUfsValid(target)) return ERROR_NOT_SUPPORTED;
    CR_STATUS st = fdt ? CrUfsPowerFdtParse(fdt, fdt_totalsize(fdt), &facts) : CR_NOT_FOUND;
    if (st == CR_SUCCESS) st = CrUfsPowerResolve(target, &facts, &resolved);
    if (st == CR_SUCCESS) {
        for (unsigned i = 0; i < resolved.Count; ++i) rails[i] = resolved.Rails[i];
        *count = resolved.Count;
        *from_dt = true;
        return 0;
    }
    if (target->PowerPolicy && target->PowerPolicy->RequireHob) {
        /* CrDK policy: a failed FDT qualification is not permission to vote
         * a guessed voltage. Leave the inherited rails untouched. */
        LOG("UFS supplies not qualified from DT (%d); rails left inherited", st);
        return ERROR_NOT_SUPPORTED;
    }
    for (unsigned i = 0; i < target->RailCount && i < CR_TARGET_UFS_MAX_RAILS; ++i)
        rails[i] = target->Rails[i];
    *count = target->RailCount;
    return 0;
}
