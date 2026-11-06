/* SPDX-License-Identifier: MIT */
/*
 * Per-SoC TLMM and clock-controller tables (kshim/soc_tables.h).
 *
 * SM8150/SM8250 (Android DTs) adapt the CrDK AndroidBindingsLib tables:
 * CrAndroidPin (per-pin control offset and function names, from the pinned
 * OnePlus/MiCode pinctrl sources listed in ports/crdk) and CrAndroidClock
 * (Android clock IDs to vote/CBCR/RCG offsets). SM8650 (mainline DTs) uses
 * the generated kernel/soc/sm8650_tables.c (Linux facts, provenance in that
 * file). Pin counts: CrDK Target/Sm8150, Sm8250 and Sm8650 CrGpioTarget.c
 * (normal pads 175, 180 and 210).
 */
#include <kshim/soc_tables.h>
#include <kshim/clk.h>
#include <Library/cr_android.h>
#include <string.h>

static const struct kshim_soc_tlmm Tlmm[] = {
    {KSHIM_SOC_SM8150, 175, false, false},
    {KSHIM_SOC_SM8250, 180, false, false},
    {KSHIM_SOC_SM8650, 210, true, true},
};

const struct kshim_soc_tlmm *kshim_soc_tlmm(unsigned soc)
{
    for (size_t i = 0; i < sizeof(Tlmm) / sizeof(Tlmm[0]); ++i)
        if (Tlmm[i].soc == soc) return &Tlmm[i];
    return NULL;
}

bool kshim_soc_pin_offset(unsigned soc, uint32_t pin, uint32_t *offset)
{
    const struct kshim_soc_tlmm *t = kshim_soc_tlmm(soc);
    if (!t || !offset || pin >= t->pins) return false;
    if (t->linear) {
        *offset = pin * 0x1000U;
        return true;
    }
    const struct CrAndroidPin *layout = CrAndroidPin(soc, pin);
    if (!layout || !layout->functions[0]) return false;
    *offset = layout->offset;
    return true;
}

bool kshim_soc_pin_function(unsigned soc, uint32_t pin, const char *function, uint32_t *mux)
{
    const struct kshim_soc_tlmm *t = kshim_soc_tlmm(soc);
    if (!t || !function || !mux || pin >= t->pins) return false;
    if (!strcmp(function, "gpio")) { *mux = 0; return true; }
    if (soc == KSHIM_SOC_SM8650) {
        for (uint32_t slot = 0; slot < 10; ++slot) {
            uint8_t name = kshim_sm8650_pin_mux[pin][slot];
            if (name && name < kshim_sm8650_pin_name_count &&
                !strcmp(kshim_sm8650_pin_names[name], function)) {
                *mux = slot + 1;
                return true;
            }
        }
        return false;
    }
    const struct CrAndroidPin *layout = CrAndroidPin(soc, pin);
    if (!layout) return false;
    for (uint32_t i = 0; i < 10; ++i)
        if (layout->functions[i] && !strcmp(layout->functions[i], function)) { *mux = i; return true; }
    return false;
}

static const struct kshim_soc_clock_controller Controllers[] = {
    {"qcom,sm8650-gcc", "gcc", KSHIM_SOC_SM8650, kshim_sm8650_gcc, &kshim_sm8650_gcc_count,
     kshim_sm8650_gcc_rates, &kshim_sm8650_gcc_rates_count},
    {"qcom,sm8650-tcsr", "tcsr", KSHIM_SOC_SM8650, kshim_sm8650_tcsr, &kshim_sm8650_tcsr_count,
     NULL, NULL},
    /* Android IDs only: mainline qcom,gcc-sm8150/-sm8250 use other IDs. */
    {"qcom,gcc-sm8150-v2", "gcc", KSHIM_SOC_SM8150, NULL, NULL, NULL, NULL},
    {"qcom,gcc-kona", "gcc", KSHIM_SOC_SM8250, NULL, NULL, NULL, NULL},
};

const struct kshim_soc_clock_controller *kshim_soc_clock_controller(const char *compatible)
{
    if (!compatible) return NULL;
    for (size_t i = 0; i < sizeof(Controllers) / sizeof(Controllers[0]); ++i)
        if (!strcmp(Controllers[i].compatible, compatible)) return &Controllers[i];
    return NULL;
}

const struct kshim_soc_clock_controller *kshim_soc_clock_controller_at(size_t index)
{
    return index < sizeof(Controllers) / sizeof(Controllers[0]) ? &Controllers[index] : NULL;
}

/* CrAndroidClock rows: vote register + bit with the CBCR as halt, or a
 * plain CBCR (vote 0). */
static bool android_leaf(unsigned soc, uint32_t id, struct kshim_soc_clk *leaf)
{
    const struct CrAndroidClock *c = CrAndroidClock(soc, id);
    if (!c) return false;
    *leaf = (struct kshim_soc_clk){
        .name = NULL, .space = KSHIM_SOC_CLK_SPACE_CLOCK, .id = (uint16_t)id,
        .kind = c->vote ? KSHIM_CLK_VOTE : KSHIM_CLK_BRANCH, .halt = KSHIM_CLK_HALT_BIT31,
        .enable_reg = c->vote ? c->vote : c->halt, .enable_mask = c->vote ? c->mask : 1U,
        .halt_reg = c->halt, .rcg = c->rcg};
    return true;
}

bool kshim_soc_clock_leaf(const struct kshim_soc_clock_controller *c, uint8_t space,
                          uint32_t id, struct kshim_soc_clk *leaf)
{
    if (!c || !leaf) return false;
    if (!c->leaves) return space == KSHIM_SOC_CLK_SPACE_CLOCK && android_leaf(c->soc, id, leaf);
    for (size_t i = 0; i < *c->count; ++i)
        if (c->leaves[i].space == space && c->leaves[i].id == id) { *leaf = c->leaves[i]; return true; }
    return false;
}

bool kshim_soc_clock_leaf_by_name(const struct kshim_soc_clock_controller *c,
                                  const char *name, struct kshim_soc_clk *leaf)
{
    if (!c || !leaf || !name || !c->leaves) return false;
    for (size_t i = 0; i < *c->count; ++i)
        if (!strcmp(c->leaves[i].name, name)) { *leaf = c->leaves[i]; return true; }
    return false;
}

const struct kshim_soc_clk_rate *kshim_soc_clock_rate(const struct kshim_soc_clock_controller *c,
                                                      uint32_t rcg, uint32_t hz)
{
    if (!c || !c->rates || !rcg) return NULL;
    for (size_t i = 0; i < *c->rate_count; ++i)
        if (c->rates[i].rcg == rcg && c->rates[i].hz == hz) return &c->rates[i];
    return NULL;
}
