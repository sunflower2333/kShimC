/* SPDX-License-Identifier: MIT */
/*
 * TLMM summary cascade. Each requested pin gets INTR_CFG detection and
 * polarity, RAW_STATUS_EN and the apps (KPSS) target; the summary SPI then
 * dispatches by INTR_STATUS. Pin addresses come from the DT TLMM region.
 *
 * The register layout is CrDK's TLMM target data (Target/Sm8150, Sm8250,
 * Sm8450 CrGpioTarget.c RegCfg[0], identical on the three) carried as one
 * GpioConfigRegInfo, with each SoC's reserved and special pads; irqchip_test
 * checks both against those tables field by field. The tables are not
 * linked: each is a 13.5 KiB GpioDeviceContext, as is the context CrDK
 * GpioLib's per-pin calls need, and the 4 MiB runtime slot has under 90 KiB
 * left on andromeda/hdk8250. The per-pin sequence follows Linux pinctrl-msm
 * (and differs from GpioSetInterruptCfg, which keeps stale detection bits
 * for level triggers and clears the status latch before RAW_STATUS_EN).
 *
 * Pins are routed through the summary rather than their PDC wake lines: the
 * whole state to restore is one INTR_CFG word per pin plus a status latch,
 * no shared always-on PDC/spi_cfg registers are written, and every pin works
 * the same way whether or not it has a PDC wake pin.
 *
 * Edge pins are acknowledged before their handler runs, so an edge during
 * the handler latches again; level pins after it, once the handler has
 * deasserted the source.
 */
#include "irqchip.h"
#include <dt.h>
#include <qcom_dt.h>
#include <Library/cr_android.h>
#include <string.h>

#define TLMM_SLOTS 8U
/* Summary interrupts without any requested pin pending: a pin left enabled
 * for the apps target by earlier firmware. Seal the summary instead of
 * storming; clients then poll. */
#define TLMM_SPURIOUS_LIMIT 1000U

const GpioConfigRegInfo KshimTlmmLayout = {
    .Index = 0, .ControlRegOffset = 0, .IoRegOffset = 4, .IntCfgRegOffset = 8,
    .IntStatusRegOffset = 12, .IntTargetRegOffset = 8, .InputMsk = 1,
    .InterruptEnMsk = 1, .InterruptPolarityMsk = 2, .InterruptDetectionMsk = 0xc,
    .InterruptRawStatusMsk = 0x10, .InterruptStatusMsk = 1, .InterruptTargetMsk = 0xe0,
    .TargetKpssVal = 3, .InterruptDetectionWidth = 2,
};
/* Normal pins are 0..pins-1; the special pads after them have no interrupt. */
const struct kshim_tlmm_soc KshimTlmmSocs[KSHIM_TLMM_SOCS] = {
    {"qcom,sm8150-pinctrl", 175, false, {0, 1, 2, 3, 126, 127, 128, 129}, 8},
    {"qcom,kona-pinctrl", 180, false, {28, 29, 30, 31, 40, 41, 42, 43}, 8},
    {"qcom,sm8250-tlmm", 180, false, {28, 29, 30, 31, 40, 41, 42, 43}, 8},
    {"qcom,sm8450-tlmm", 210, true, {0}, 0},
};

struct tlmm_slot {
    bool used, masked, level;
    uint32_t pin;
    uintptr_t control;
    kshim_gpio_irq_handler_t handler;
    void *context;
    uint32_t saved_config, saved_target;
};

static struct {
    const void *fdt;
    int node;
    bool ready, summary_requested, sealed;
    unsigned soc; /* kshim_dt_soc() for the Android binding offsets */
    const struct kshim_tlmm_soc *layout;
    const struct CrIo *io;
    struct dt_range reg;
    uint32_t summary;
    unsigned spurious;
    struct tlmm_slot slots[TLMM_SLOTS];
} Tlmm;

#define L KshimTlmmLayout
static uint32_t rd(uintptr_t a) { return Tlmm.io->read32(Tlmm.io->context, a); }
static void wr(uintptr_t a, uint32_t v) { Tlmm.io->write32(Tlmm.io->context, a, v); }
static uint32_t field(uint32_t value, uint32_t mask) { return (value << __builtin_ctz(mask)) & mask; }
static uintptr_t cfg_reg(const struct tlmm_slot *s) { return s->control + L.IntCfgRegOffset; }
static uintptr_t target_reg(const struct tlmm_slot *s) { return s->control + L.IntTargetRegOffset; }
static uintptr_t status_reg(const struct tlmm_slot *s) { return s->control + L.IntStatusRegOffset; }
static bool pending(const struct tlmm_slot *s) { return (rd(status_reg(s)) & L.InterruptStatusMsk) != 0; }
/* Write-zero acknowledge (msm_ack_intr_status without intr_ack_high). */
static void ack(const struct tlmm_slot *s) { wr(status_reg(s), rd(status_reg(s)) & ~L.InterruptStatusMsk); }

static bool slots_live(void)
{
    for (unsigned i = 0; i < TLMM_SLOTS; ++i) if (Tlmm.slots[i].used) return true;
    return false;
}

int kshim_tlmm_irq_init(const void *f)
{
    if (!f) return ERROR_INVALID_PARAMETER;
    if (Tlmm.ready && Tlmm.fdt == f) return 0;
    if (slots_live() || Tlmm.summary_requested) return ERROR_DEVICE_ERROR;
    int node = -1;
    unsigned soc = 0;
    for (unsigned i = 0; i < KSHIM_TLMM_SOCS; ++i) {
        for (int n = fdt_node_offset_by_compatible(f, -1, KshimTlmmSocs[i].compatible); n >= 0;
             n = fdt_node_offset_by_compatible(f, n, KshimTlmmSocs[i].compatible)) {
            if (!dt_available(f, n) || kshim_dt_irq_kind(f, n) != KSHIM_DT_IRQ_TLMM) continue;
            if (node >= 0 && node != n) return ERROR_INVALID_PARAMETER;
            node = n; soc = i;
        }
    }
    if (node < 0) return ERROR_NOT_FOUND;
    const struct CrIo *io = kshim_irqchip_io();
    struct dt_range reg;
    if (!io || dt_reg(f, node, 0, &reg)) return ERROR_NOT_SUPPORTED;
    Tlmm.io = io;
    Tlmm.fdt = f; Tlmm.node = node; Tlmm.reg = reg;
    Tlmm.layout = &KshimTlmmSocs[soc];
    Tlmm.soc = kshim_dt_soc(f, node);
    Tlmm.summary_requested = Tlmm.sealed = false;
    Tlmm.spurious = 0;
    Tlmm.ready = true;
    return 0;
}

static int pin_address(uint32_t pin, uintptr_t *address)
{
    const struct kshim_tlmm_soc *soc = Tlmm.layout;
    if (pin >= soc->pins) return -1;
    for (unsigned i = 0; i < soc->reserved_count; ++i) if (soc->reserved[i] == pin) return -1;
    /* kshim_dt_gpio() without re-reading the TLMM reg for every pin. */
    uint64_t at = Tlmm.reg.base + (uint64_t)pin * 0x1000U;
    if (!soc->linear) {
        const struct CrAndroidPin *layout = CrAndroidPin(Tlmm.soc, pin);
        if (!layout || !layout->functions[0]) return -1;
        at = Tlmm.reg.base + layout->offset;
    }
    if (!dt_contains(Tlmm.reg, at, 0x10) || at > UINTPTR_MAX) return -1;
    *address = (uintptr_t)at;
    return 0;
}

int kshim_tlmm_pin(uintptr_t control, uint32_t *pin)
{
    if (!Tlmm.ready || !pin) return ERROR_NOT_FOUND;
    for (uint32_t p = 0; p < Tlmm.layout->pins; ++p) {
        uintptr_t address;
        if (!pin_address(p, &address) && address == control) { *pin = p; return 0; }
    }
    return ERROR_NOT_FOUND;
}

static int find(uint32_t pin)
{
    for (unsigned i = 0; i < TLMM_SLOTS; ++i)
        if (Tlmm.slots[i].used && Tlmm.slots[i].pin == pin) return (int)i;
    return -1;
}

static void summary_isr(uint32_t intid, void *context)
{
    (void)context;
    bool handled = false;
    for (unsigned i = 0; i < TLMM_SLOTS; ++i) {
        struct tlmm_slot *s = &Tlmm.slots[i];
        if (!s->used || s->masked || !pending(s)) continue;
        handled = true;
        if (!s->level) ack(s);
        s->handler(s->pin, s->context);
        if (s->used && s->level) ack(s);
    }
    if (handled) Tlmm.spurious = 0;
    else if (++Tlmm.spurious >= TLMM_SPURIOUS_LIMIT && !Tlmm.sealed) {
        kshim_irq_disable(intid);
        Tlmm.sealed = true;
    }
}

/* Interrupt fields owned while a pin is requested, in INTR_CFG. */
static uint32_t config_fields(void)
{
    uint32_t m = L.InterruptEnMsk | L.InterruptRawStatusMsk | L.InterruptDetectionMsk |
                 L.InterruptPolarityMsk;
    return L.IntTargetRegOffset == L.IntCfgRegOffset ? m | L.InterruptTargetMsk : m;
}

static void restore_slot(unsigned i)
{
    struct tlmm_slot *s = &Tlmm.slots[i];
    uintptr_t cfg = cfg_reg(s), target = target_reg(s);
    uint32_t mask = config_fields();
    wr(cfg, rd(cfg) & ~L.InterruptEnMsk);
    wr(cfg, (rd(cfg) & ~mask) | (s->saved_config & mask));
    if (target != cfg)
        wr(target, (rd(target) & ~L.InterruptTargetMsk) | (s->saved_target & L.InterruptTargetMsk));
    /* A latch produced by kShimC's configuration must not reach the payload. */
    ack(s);
    *s = (struct tlmm_slot){0};
}

int kshim_gpio_irq_request(uint32_t pin, unsigned trigger,
                           kshim_gpio_irq_handler_t handler, void *context)
{
    uintptr_t address;
    uint32_t detection, polarity;
    if (!Tlmm.ready) return ERROR_NOT_FOUND;
    /* Width-2 detection: 1 rising, 2 falling, 3 both, 0 level (pinctrl-msm). */
    switch (trigger) {
    case KSHIM_IRQ_TYPE_EDGE_RISING: detection = 1; polarity = 1; break;
    case KSHIM_IRQ_TYPE_EDGE_FALLING: detection = 2; polarity = 1; break;
    case KSHIM_IRQ_TYPE_EDGE_BOTH: detection = 3; polarity = 1; break;
    case KSHIM_IRQ_TYPE_LEVEL_HIGH: detection = 0; polarity = 1; break;
    case KSHIM_IRQ_TYPE_LEVEL_LOW: detection = 0; polarity = 0; break;
    default: return ERROR_INVALID_PARAMETER;
    }
    if (!handler) return ERROR_INVALID_PARAMETER;
    if (Tlmm.sealed) return ERROR_DEVICE_ERROR;
    if (pin_address(pin, &address)) return ERROR_NOT_FOUND;
    if (find(pin) >= 0) return ERROR_DEVICE_ERROR;
    unsigned i = 0;
    while (i < TLMM_SLOTS && Tlmm.slots[i].used) ++i;
    if (i == TLMM_SLOTS) return ERROR_NOT_SUPPORTED;

    unsigned long flags = kshim_irq_save();
    struct tlmm_slot *s = &Tlmm.slots[i];
    *s = (struct tlmm_slot){.used = true, .pin = pin, .control = address, .handler = handler,
        .context = context, .level = detection == 0};
    uintptr_t cfg = cfg_reg(s), target = target_reg(s);
    s->saved_config = rd(cfg);
    s->saved_target = rd(target);
    /* Route to the apps processor first, with the pin disabled. */
    uint32_t value = s->saved_config & ~L.InterruptEnMsk;
    if (target != cfg) wr(target, (s->saved_target & ~L.InterruptTargetMsk) |
                                  field(L.TargetKpssVal, L.InterruptTargetMsk));
    else value = (value & ~L.InterruptTargetMsk) | field(L.TargetKpssVal, L.InterruptTargetMsk);
    /* RAW_STATUS_EN stays on while the pin is requested: toggling it can set
     * an edge status (pinctrl-msm). Enabling it can latch a stale event,
     * which is acknowledged before the pin is enabled. */
    value = (value & ~(L.InterruptDetectionMsk | L.InterruptPolarityMsk)) | L.InterruptRawStatusMsk |
            field(detection, L.InterruptDetectionMsk) | field(polarity, L.InterruptPolarityMsk);
    wr(cfg, value);
    ack(s);
    wr(cfg, value | L.InterruptEnMsk);
    kshim_irq_restore(flags);
    if (!Tlmm.summary_requested) {
        /* Android DTs: <0 208 0> under the PDC root (pass-through); level
         * high per the TLMM binding and the CrDK target. */
        int st = kshim_qcom_irq_request(Tlmm.fdt, Tlmm.node, 0, KSHIM_IRQ_TYPE_LEVEL_HIGH,
                                        summary_isr, NULL, &Tlmm.summary);
        if (st) {
            flags = kshim_irq_save();
            restore_slot(i);
            kshim_irq_restore(flags);
            return st;
        }
        Tlmm.summary_requested = true;
    }
    return 0;
}

static void release_summary(void)
{
    if (!Tlmm.summary_requested) return;
    if (!kshim_qcom_irq_free(Tlmm.summary)) Tlmm.summary_requested = false;
    Tlmm.sealed = false;
    Tlmm.spurious = 0;
}

int kshim_gpio_irq_free(uint32_t pin)
{
    int i = find(pin);
    if (i < 0) return ERROR_NOT_FOUND;
    unsigned long flags = kshim_irq_save();
    restore_slot((unsigned)i);
    bool last = !slots_live();
    kshim_irq_restore(flags);
    if (last) release_summary();
    return last && Tlmm.summary_requested ? ERROR_DEVICE_ERROR : 0;
}

void kshim_gpio_irq_mask(uint32_t pin, bool masked)
{
    int i = find(pin);
    if (i < 0) return;
    struct tlmm_slot *s = &Tlmm.slots[i];
    unsigned long flags = kshim_irq_save();
    uint32_t cfg = rd(cfg_reg(s));
    /* pinctrl-msm: a level pin also drops RAW_STATUS_EN so its latch stops;
     * an edge pin keeps it (its status would not clear otherwise). */
    if (masked) cfg &= ~(L.InterruptEnMsk | (s->level ? L.InterruptRawStatusMsk : 0));
    else cfg |= L.InterruptEnMsk | L.InterruptRawStatusMsk;
    wr(cfg_reg(s), cfg);
    s->masked = masked;
    kshim_irq_restore(flags);
}

bool kshim_gpio_irq_live(uint32_t pin)
{
    int i = find(pin);
    return i >= 0 && !Tlmm.slots[i].masked && Tlmm.summary_requested && !Tlmm.sealed;
}

int kshim_tlmm_irq_restore(void)
{
    /* Stop summary delivery first, then hand every pin back. */
    release_summary();
    unsigned long flags = kshim_irq_save();
    for (unsigned i = 0; i < TLMM_SLOTS; ++i)
        if (Tlmm.slots[i].used) restore_slot(i);
    kshim_irq_restore(flags);
    return Tlmm.summary_requested ? ERROR_DEVICE_ERROR : 0;
}
