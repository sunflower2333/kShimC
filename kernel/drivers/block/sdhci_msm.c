/* SPDX-License-Identifier: MIT */
/*
 * Qualcomm SDCC (sdhci-msm v5) platform layer for the kShimC card layer.
 *
 * Resources come from the CrDK SD target descriptor (Target/Sm8250
 * CrSdTarget.c): VDD/VDD-IO RPMh rails, AHB and APPS clocks through CrDK
 * ClockLib (the APPS rate is the card clock: the v5 core bypasses the SDHCI
 * divider). Register semantics, including the vendor power-request
 * handshake (capture / ACK), are CrDK SdhciMsmLib's; this file only polls it,
 * since kShimC owns no interrupts. Card detect is the DT cd-gpios TLMM input.
 *
 * Not ported: interconnect bandwidth votes (inherited) and pad drive/pull
 * (CrDK GpioSdLib needs a full TLMM target context; pads stay as inherited).
 */
#include <kshim/qcom_storage.h>
#include <kshim/sdmmc.h>
#include <config.h>

#include <stdio.h>
#include <string.h>

#ifndef CONFIG_BLK_SDHCI_WRITE
#define CONFIG_BLK_SDHCI_WRITE 0
#endif

struct msm {
    uintptr_t base;
    const struct kshim_storage_dt *dt;
    const struct kshim_dt_sdhc *node;
    const struct kshim_qcom_soc *soc;
    const CR_TARGET_SD_CONTEXT *target;
    struct CrIo io;
    bool rails_on;
};
static struct msm msms[KSHIM_STORAGE_MAX_SDHC];

#define LOG(h, fmt, ...) printf("%s: " fmt "\n", (h)->name, ##__VA_ARGS__)

static CR_STATUS io_ready(void *c) { (void)c; return CR_SUCCESS; }

static CR_STATUS io_read(void *c, UINT32 off, UINT8 width, UINT32 *value)
{
    struct msm *m = c;
    volatile void *p = (volatile void *)(m->base + off);
    if (width == 1) *value = *(volatile uint8_t *)p;
    else if (width == 2) *value = *(volatile uint16_t *)p;
    else if (width == 4) *value = *(volatile uint32_t *)p;
    else return CR_INVALID_PARAMETER;
    return CR_SUCCESS;
}

static CR_STATUS io_write(void *c, UINT32 off, UINT8 width, UINT32 value)
{
    struct msm *m = c;
    volatile void *p = (volatile void *)(m->base + off);
    if (width == 1) *(volatile uint8_t *)p = (uint8_t)value;
    else if (width == 2) *(volatile uint16_t *)p = (uint16_t)value;
    else if (width == 4) *(volatile uint32_t *)p = value;
    else return CR_INVALID_PARAMETER;
    return CR_SUCCESS;
}

static CR_STATUS io_stall(void *c, UINT32 us)
{
    struct msm *m = c;
    m->io.delay_us(m->io.context, us);
    return CR_SUCCESS;
}

static CR_STATUS io_barrier(void *c)
{
    (void)c;
#if defined(__aarch64__)
    __asm__ volatile("dsb sy" ::: "memory");
#endif
    return CR_SUCCESS;
}

static int rails(struct msm *m)
{
    if (m->rails_on) return 0;
    for (unsigned i = 0; i < 2; ++i) {
        const CR_TARGET_SD_RAIL *r = &m->target->Rails[i];
        int st = kshim_qcom_rail(m->dt, r->Name, r->VoltageMv, r->Mode);
        if (st) return st;
    }
    m->rails_on = true;
    return 0;
}

/* Capture any vendor power request and acknowledge it. Supplies stay voted:
 * a BUS_OFF only comes from kShimC's own RESET_ALL, never a real power-down. */
static int service(struct sdmmc_host *h, uint32_t *seen)
{
    struct msm *m = h->platform;
    UINT32 observed = 0;
    CR_STATUS st = CrSdhcPowerCapture(&h->core, &observed);
    if (st != CR_SUCCESS) return ERROR_DEVICE_ERROR;
    UINT32 pending = h->core.PowerPending;
    if (!pending) return 0;
    bool ok = true;
    if (pending & CR_SDHC_POWER_ON) ok = rails(m) == 0;
    if ((pending & CR_SDHC_IO_LOW) && !(pending & CR_SDHC_POWER_OFF)) ok = false; /* 1.8 V unsupported */
    st = CrSdhcPowerComplete(&h->core, pending, ok, ok);
    if (seen) *seen |= pending;
    if (st != CR_SUCCESS) return ERROR_DEVICE_ERROR;
    return ok ? 0 : ERROR_NOT_SUPPORTED;
}

static int wait_cause(struct sdmmc_host *h, uint32_t cause, uint32_t limit_us)
{
    uint32_t seen = 0;
    for (uint32_t t = 0; t < limit_us; t += 10) {
        int st = service(h, &seen);
        if (st) return st;
        if (seen & cause) return 0;
        (void)h->core.Io.StallUs(h->core.Io.Context, 10);
    }
    return ERROR_TIMEOUT;
}

static struct sdmmc_host *sync_owner[KSHIM_STORAGE_MAX_SDHC];

static CR_STATUS sync_power(CR_SDHC *core, UINT32 request, BOOLEAN begin, CR_STATUS write_status)
{
    struct sdmmc_host *h = NULL;
    for (unsigned i = 0; i < KSHIM_STORAGE_MAX_SDHC; ++i)
        if (sync_owner[i] && &sync_owner[i]->core == core) h = sync_owner[i];
    if (!h) return CR_INVALID_PARAMETER;
    if (begin) return service(h, NULL) ? CR_DEVICE_ERROR : CR_SUCCESS;
    if (write_status != CR_SUCCESS) return write_status;
    int st = wait_cause(h, request, 100000);
    if (st == ERROR_TIMEOUT && request == CR_SDHC_IO_HIGH) {
        UINT32 ctrl2 = 0;
        /* No request is raised when the pad level does not change. */
        if (CrSdhcRead(core, 0x3e, 2, &ctrl2) == CR_SUCCESS && !(ctrl2 & BIT3)) return CR_SUCCESS;
    }
    return st ? CR_TIMEOUT : CR_SUCCESS;
}

static int attach(struct sdmmc_host *h)
{
    for (unsigned i = 0; i < KSHIM_STORAGE_MAX_SDHC; ++i)
        if (!sync_owner[i] || sync_owner[i] == h) { sync_owner[i] = h; h->core.SyncPower = sync_power; return 0; }
    return ERROR_NOT_SUPPORTED;
}

static int prepare(struct sdmmc_host *h)
{
    int st = CrSdhcColdInit(&h->core) == CR_SUCCESS ? 0 : ERROR_DEVICE_ERROR;
    if (!st && CrSdhcPowerMask(&h->core, TRUE) != CR_SUCCESS) st = ERROR_DEVICE_ERROR;
    if (!st && CrSdhcIoPadHigh(&h->core, TRUE) != CR_SUCCESS) st = ERROR_DEVICE_ERROR;
    return st;
}

static int set_clock(struct sdmmc_host *h, uint32_t hz)
{
    struct msm *m = h->platform;
    uint32_t rate = hz <= 400000 ? 400000 : hz <= 25000000 ? 25000000 : 50000000;
    if (CrSdhcPrepareSdr(&h->core) != CR_SUCCESS || CrSdhcDefaultMclk(&h->core) != CR_SUCCESS)
        return ERROR_DEVICE_ERROR;
    (void)kshim_qcom_clock_off(m->soc, m->target->ClockController, m->target->Clocks[1]);
    return kshim_qcom_clock_on(m->soc, m->target->ClockController, m->target->Clocks[1], rate);
}

static int set_timing(struct sdmmc_host *h, uint32_t hz)
{
    return CrSdhcSdrSpeed(&h->core, hz > 25000000) == CR_SUCCESS ? 0 : ERROR_DEVICE_ERROR;
}

static int power(struct sdmmc_host *h, bool on)
{
    if (!on) return 0;
    return wait_cause(h, CR_SDHC_POWER_ON, 1000000);
}

static int present(struct sdmmc_host *h, bool *p)
{
    struct msm *m = h->platform;
    if (m->node->non_removable || !m->node->has_cd) { *p = true; return 0; }
    bool high = m->io.read32(m->io.context, m->node->cd_io) & 1;
    *p = high != m->node->cd_active_low;
    return 0;
}

static int poll(struct sdmmc_host *h)
{
    return service(h, NULL);
}

static const struct sdmmc_platform_ops msm_ops = {
    .attach = attach, .prepare = prepare, .set_clock = set_clock, .set_timing = set_timing,
    .power = power, .card_present = present, .poll = poll,
};

int kshim_sdhci_msm_probe(const struct kshim_storage_dt *dt, const struct kshim_dt_sdhc *s,
                          struct sdmmc_host *h)
{
    unsigned slot = (unsigned)(s - dt->sdhc);
    if (slot >= KSHIM_STORAGE_MAX_SDHC) return ERROR_INVALID_PARAMETER;
    struct msm *m = &msms[slot];
    memset(m, 0, sizeof(*m));
    memset(h, 0, sizeof(*h));
    snprintf(h->name, sizeof(h->name), "sdhc%u", s->index);
    m->dt = dt; m->node = s; m->base = (uintptr_t)s->regs.base;
    m->soc = kshim_qcom_soc(dt->soc);
    m->target = m->soc && m->soc->sd ? m->soc->sd() : NULL;
    if (!m->target || !CrTargetSdValid(m->target) || m->target->Base != s->regs.base || !m->soc->has_clocks) {
        LOG(h, "no CrDK SD target with a clock graph for SM%u at %#llx; skipped", dt->soc,
            (unsigned long long)s->regs.base);
        return ERROR_NOT_SUPPORTED;
    }
    const struct CrIo *io = CrIoDefault();
    if (!io || !dt->has_rpmh) { LOG(h, "no RPMh for SD supplies; skipped"); return ERROR_NOT_SUPPORTED; }
    m->io = *io;
    int st = rails(m);
    if (!st) st = kshim_qcom_clock_on(m->soc, m->target->ClockController, m->target->Clocks[0], 0);
    if (!st) st = kshim_qcom_clock_on(m->soc, m->target->ClockController, m->target->Clocks[1], 400000);
    if (st) { LOG(h, "SD resources failed (%d)", st); return st; }
    h->io = (CR_SDHC_IO){m, io_ready, io_read, io_write, io_stall, io_barrier,
                         (UINT32)s->regs.size, CR_SDHC_WINDOW_ALL};
    h->ops = &msm_ops;
    h->platform = m;
    h->max_hz = s->max_hz;
    h->emmc_only = s->non_removable;
    h->allow_write = CONFIG_BLK_SDHCI_WRITE;
    return sdmmc_probe(h);
}
