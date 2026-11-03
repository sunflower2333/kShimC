/* SPDX-License-Identifier: MIT */
/*
 * Qualcomm interrupt cascades (docs/interrupts.md layer 2) against register
 * models and the real andromeda/hdk8150 runtime DTs: DT resolution, TLMM
 * per-pin configuration/dispatch/edge-vs-level acknowledge/restore, PDC
 * mapping/restore and SPMI arbiter dispatch/acknowledge/restore, all through
 * a fake GIC core behind kshim/irq.h.
 */
#include "irq_fake.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dt.h>
#include <qcom_dt.h>
#include <qcom_irq.h>
#include "../kernel/drivers/irqchip/irqchip.h"

/* CrDK Target/<Soc>/CrGpioTarget.c, built for the host only. */
GpioDeviceContext *KshimCrdkSm8150GpioContext(void);
GpioDeviceContext *KshimCrdkSm8250GpioContext(void);
GpioDeviceContext *KshimCrdkSm8450GpioContext(void);

static void *load(const char *name)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", FIXTURES, name);
    FILE *file = fopen(path, "rb");
    assert(file);
    assert(!fseek(file, 0, SEEK_END));
    long size = ftell(file);
    assert(size > 0 && !fseek(file, 0, SEEK_SET));
    void *data = aligned_alloc(8, ((size_t)size + 16384 + 7) & ~(size_t)7);
    assert(data && fread(data, 1, (size_t)size, file) == (size_t)size);
    fclose(file);
    assert(!fdt_open_into(data, data, size + 16384));
    assert(!dt_validate(data, fdt_totalsize(data)));
    return data;
}

static int compatible(const void *f, const char *name)
{
    for (int n = fdt_node_offset_by_compatible(f, -1, name); n >= 0;
         n = fdt_node_offset_by_compatible(f, n, name))
        if (dt_available(f, n)) return n;
    return -1;
}

static void expect_spec(const struct kshim_dt_irq *s, int parent, unsigned count,
                        uint32_t a, uint32_t b, uint32_t c)
{
    assert(s->parent == parent && s->count == count);
    assert(s->cells[0] == a && s->cells[1] == b);
    if (count > 2) assert(s->cells[2] == c);
}

/* ---- DT resolution of every interrupt kShimC uses ---------------------- */

static void test_dt(const void *f, bool has_touch)
{
    struct kshim_dt_irq s;
    unsigned trigger = 99;
    uint32_t intid;
    int gic = compatible(f, "arm,gic-v3"), pdc = compatible(f, "qcom,pdc-sm8150");
    int tlmm = compatible(f, "qcom,sm8150-pinctrl"), arb = kshim_dt_spmi_controller(f, 0);
    assert(gic >= 0 && pdc >= 0 && tlmm >= 0 && arb >= 0);
    assert(kshim_dt_irq_kind(f, gic) == KSHIM_DT_IRQ_GIC);
    assert(kshim_dt_irq_kind(f, pdc) == KSHIM_DT_IRQ_PDC);
    assert(kshim_dt_irq_kind(f, tlmm) == KSHIM_DT_IRQ_TLMM);
    assert(kshim_dt_irq_kind(f, arb) == KSHIM_DT_IRQ_SPMI);
    /* The root interrupt-parent is the PDC: the summary, the arbiter and the
     * console UART are PDC children in GIC form, which the core declines. */
    assert(kshim_dt_irq_parent(f, tlmm) == pdc && kshim_dt_irq_parent(f, arb) == pdc);
    assert(!kshim_dt_irq(f, tlmm, 0, &s)); expect_spec(&s, pdc, 3, 0, 208, 0);
    assert(!kshim_dt_irq_named(f, arb, "periph_irq", &s)); expect_spec(&s, pdc, 3, 0, 481, 0);
    assert(kshim_irq_from_fdt(f, arb, 0, &intid, &trigger) == ERROR_NOT_SUPPORTED);
    int uart = compatible(f, "qcom,msm-geni-console");
    assert(uart >= 0 && !kshim_dt_irq(f, uart, 0, &s)); expect_spec(&s, pdc, 3, 0, 357, 0);
    /* PMIC children inherit the arbiter: PON kpdpwr/resin, PMIC GPIO6. */
    int pon = kshim_dt_spmi_irq_find(f, 0, 0, 0x08, 0, &trigger);
    assert(pon > 0 && trigger == KSHIM_IRQ_TYPE_NONE && kshim_dt_irq_parent(f, pon) == arb);
    assert(kshim_dt_spmi_irq_find(f, 0, 0, 0x08, 1, &trigger) == pon);
    assert(!kshim_dt_irq_named(f, pon, "resin", &s)); expect_spec(&s, arb, 4, 0, 0x08, 1);
    assert(kshim_dt_spmi_irq_find(f, 0, 0, 0xc5, 0, &trigger) > 0);
    assert(kshim_dt_spmi_irq_find(f, 0, 0, 0x08, 3, &trigger) < 0);  /* not described */
    assert(kshim_dt_spmi_irq_find(f, 0, 0, 0xc4, 0, &trigger) < 0);  /* gpio5 not listed */
    assert(kshim_dt_spmi_irq_find(f, 1, 0, 0x08, 0, &trigger) < 0);  /* one arbiter */
    if (has_touch) {
        struct kshim_touch_resource t;
        assert(!kshim_dt_touch(f, &t));
        assert(!kshim_dt_irq(f, t.node, 0, &s)); expect_spec(&s, tlmm, 2, 122, 0x2008, 0);
        assert(kshim_dt_irq_trigger(s.cells[1]) == KSHIM_IRQ_TYPE_LEVEL_LOW);
    }
}

/* ---- TLMM ------------------------------------------------------------- */

/* kShimC's TLMM layout is CrDK's target data: same interrupt fields, same
 * interrupt-capable pins, same reserved pins. */
static void test_tlmm_layout(void)
{
    GpioDeviceContext *(*const targets[KSHIM_TLMM_SOCS])(void) = {KshimCrdkSm8150GpioContext,
        KshimCrdkSm8250GpioContext, KshimCrdkSm8250GpioContext, KshimCrdkSm8450GpioContext};
    const GpioConfigRegInfo *l = &KshimTlmmLayout;
    for (unsigned i = 0; i < KSHIM_TLMM_SOCS; ++i) {
        const GpioDeviceContext *t = targets[i]();
        const GpioConfigRegInfo *r = &t->RegCfg[0];
        const struct kshim_tlmm_soc *soc = &KshimTlmmSocs[i];
        assert(r->Index == 0 && r->IntCfgRegOffset == l->IntCfgRegOffset &&
               r->IntStatusRegOffset == l->IntStatusRegOffset && r->IntTargetRegOffset == l->IntTargetRegOffset &&
               r->IoRegOffset == l->IoRegOffset && r->InputMsk == l->InputMsk &&
               r->InterruptEnMsk == l->InterruptEnMsk && r->InterruptPolarityMsk == l->InterruptPolarityMsk &&
               r->InterruptDetectionMsk == l->InterruptDetectionMsk &&
               r->InterruptRawStatusMsk == l->InterruptRawStatusMsk &&
               r->InterruptStatusMsk == l->InterruptStatusMsk && r->InterruptTargetMsk == l->InterruptTargetMsk &&
               r->TargetKpssVal == l->TargetKpssVal &&
               r->InterruptDetectionWidth == l->InterruptDetectionWidth);
        unsigned normal = 0;
        for (unsigned pin = 0; pin < t->PinCount; ++pin) {
            if (t->GpioPins[pin].pRegCfg == r) assert(pin == normal++);
            bool reserved = false;
            for (unsigned k = 0; k < soc->reserved_count; ++k) reserved |= soc->reserved[k] == pin;
            assert(reserved == (t->GpioPins[pin].Reserved != 0));
        }
        assert(normal == soc->pins);
    }
}

static uintptr_t Pin122, Pin41, Pin38;
static unsigned LevelCalls, EdgeCalls, DeassertAfter;

static void level_handler(uint32_t pin, void *context)
{
    assert(pin == 122 && context == &LevelCalls && FakeInHandler);
    /* Level: acknowledged after the handler, so still latched here. */
    assert(fake_peek(Pin122 + 0xc) & 1U);
    if (++LevelCalls >= DeassertAfter) fake_tlmm_set(Pin122, true);
}

static void edge_handler(uint32_t pin, void *context)
{
    (void)context;
    assert(pin == 41);
    /* Edge: acknowledged first, so an edge during the handler relatches. */
    assert(!(fake_peek(Pin41 + 0xc) & 1U));
    ++EdgeCalls;
}

static void test_tlmm(const void *f)
{
    fake_reset();
    int tlmm = compatible(f, "qcom,sm8150-pinctrl");
    assert(!kshim_dt_gpio(f, tlmm, 122, &Pin122, NULL, NULL));
    assert(!kshim_dt_gpio(f, tlmm, 41, &Pin41, NULL, NULL));
    assert(!kshim_dt_gpio(f, tlmm, 38, &Pin38, NULL, NULL));
    fake_tlmm_pin(Pin122, true);
    fake_tlmm_pin(Pin41, true);
    fake_tlmm_pin(Pin38, true);
    fake_gic_line(FAKE_TLMM_SUMMARY, fake_tlmm_line);
    assert(kshim_gpio_irq_request(122, KSHIM_IRQ_TYPE_LEVEL_LOW, level_handler, NULL) == ERROR_NOT_FOUND);
    assert(!kshim_tlmm_irq_init(f));
    uint32_t pin;
    assert(!kshim_tlmm_pin(Pin122, &pin) && pin == 122);
    assert(kshim_tlmm_pin(Pin122 + 4, &pin) == ERROR_NOT_FOUND);

    /* Inherited: rising-edge detection, polarity, no target, an unrelated
     * bit 9 and a stale status latch. */
    const uint32_t inherited = 0x200U | (1U << 2) | 2U;
    fake_poke(Pin122 + 8, inherited);
    fake_poke(Pin122 + 0xc, 1);
    assert(!kshim_gpio_irq_request(122, KSHIM_IRQ_TYPE_LEVEL_LOW, level_handler, &LevelCalls));
    assert(!FakeIrqMasked);
    /* EN | RAW | target KPSS (3 << 5), level low: detection 0, polarity 0. */
    assert(fake_peek(Pin122 + 8) == (0x200U | 0x60U | 0x10U | 1U));
    assert(fake_peek(Pin122 + 0xc) == 0);
    struct fake_gic_irq *g = &FakeGic[FAKE_TLMM_SUMMARY];
    assert(g->requested && g->enabled && g->trigger == KSHIM_IRQ_TYPE_LEVEL_HIGH);
    /* SPI 208 is a PDC pass-through: the PDC is not written. */
    assert(fake_writes_in(FAKE_PDC_BASE, 0x400) == 0);
    assert(kshim_gpio_irq_live(122));
    assert(kshim_gpio_irq_request(122, KSHIM_IRQ_TYPE_LEVEL_LOW, level_handler, NULL) == ERROR_DEVICE_ERROR);
    assert(kshim_gpio_irq_request(0, KSHIM_IRQ_TYPE_LEVEL_LOW, level_handler, NULL) == ERROR_NOT_FOUND);
    assert(kshim_gpio_irq_request(300, KSHIM_IRQ_TYPE_LEVEL_LOW, level_handler, NULL) == ERROR_NOT_FOUND);
    assert(kshim_gpio_irq_request(41, 7, edge_handler, NULL) == ERROR_INVALID_PARAMETER);

    /* Level dispatch: the latch follows the line until the source lets go. */
    DeassertAfter = 1;
    fake_tlmm_set(Pin122, false);
    assert(fake_tlmm_line() && fake_gic_run() == 1 && LevelCalls == 1);
    assert(!fake_tlmm_line() && fake_peek(Pin122 + 0xc) == 0);
    LevelCalls = 0; DeassertAfter = 3;
    fake_tlmm_set(Pin122, false);
    assert(fake_gic_run() == 3 && LevelCalls == 3 && !fake_tlmm_line());

    /* Edge pin: both edges, acknowledged before the handler. */
    assert(!kshim_gpio_irq_request(41, KSHIM_IRQ_TYPE_EDGE_BOTH, edge_handler, NULL));
    assert(fake_peek(Pin41 + 8) == (0x60U | 0x10U | (3U << 2) | 2U | 1U));
    assert(g->requests == 1);
    fake_tlmm_set(Pin41, false);
    assert(fake_gic_run() == 1 && EdgeCalls == 1);
    fake_tlmm_set(Pin41, true);
    assert(fake_gic_run() == 1 && EdgeCalls == 2 && !fake_tlmm_line());
    kshim_gpio_irq_mask(41, true);
    assert(!kshim_gpio_irq_live(41) && !(fake_peek(Pin41 + 8) & 1U));
    fake_tlmm_set(Pin41, false);
    assert(fake_gic_run() == 0 && EdgeCalls == 2);
    kshim_gpio_irq_mask(41, false);
    assert(kshim_gpio_irq_live(41) && (fake_peek(Pin41 + 8) & 1U));
    fake_poke(Pin41 + 0xc, 0);
    fake_tlmm_set(Pin41, true);

    /* Free restores the exact inherited fields and clears the latch. */
    assert(!kshim_gpio_irq_free(41));
    assert(fake_peek(Pin41 + 8) == 0 && fake_peek(Pin41 + 0xc) == 0);
    assert(g->requested);
    assert(!kshim_gpio_irq_free(122));
    assert(fake_peek(Pin122 + 8) == inherited && fake_peek(Pin122 + 0xc) == 0);
    assert(!g->requested && g->frees == 1);
    assert(kshim_gpio_irq_free(122) == ERROR_NOT_FOUND);

    /* A pin left enabled for the apps target by firmware storms the summary:
     * the cascade seals it instead and its clients fall back to polling. */
    assert(!kshim_gpio_irq_request(122, KSHIM_IRQ_TYPE_LEVEL_LOW, level_handler, &LevelCalls));
    fake_poke(Pin38 + 8, 0x60U | 0x10U | 1U);
    fake_poke(Pin38 + 0xc, 1);
    LevelCalls = 0;
    assert(fake_gic_run() == 1000 && LevelCalls == 0);
    assert(!g->enabled && g->disables == 1 && !kshim_gpio_irq_live(122));
    assert(kshim_gpio_irq_request(41, KSHIM_IRQ_TYPE_EDGE_BOTH, edge_handler, NULL) == ERROR_DEVICE_ERROR);
    assert(!kshim_tlmm_irq_restore());
    assert(!g->requested && fake_peek(Pin122 + 8) == inherited);
    assert(fake_peek(Pin38 + 8) == (0x60U | 0x10U | 1U)); /* not ours: untouched */
    assert(!kshim_tlmm_irq_restore());
    printf("tlmm: ok\n");
}

/* ---- PDC -------------------------------------------------------------- */

static void spi_handler(uint32_t intid, void *context) { (void)intid; (void)context; }

static void test_pdc(void *f)
{
    fake_reset();
    struct kshim_dt_irq s;
    uint32_t intid;
    int arb = kshim_dt_spmi_controller(f, 0), pdc = compatible(f, "qcom,pdc-sm8150");
    int tlmm = compatible(f, "qcom,sm8150-pinctrl");
    assert(!kshim_dt_irq_named(f, arb, "periph_irq", &s));
    /* Inherited pin 1: falling edge plus an unrelated bit; bank 0 has pins
     * 0 and 2 enabled by firmware. */
    fake_poke(FAKE_PDC_CFG(1), 0x100U | 2U);
    fake_poke(FAKE_PDC_ENABLE(1), 5U);
    assert(!kshim_qcom_irq_request_spec(f, &s, KSHIM_IRQ_TYPE_LEVEL_HIGH, spi_handler, NULL, &intid));
    assert(intid == FAKE_SPMI_PERIPH_IRQ);
    assert(fake_peek(FAKE_PDC_CFG(1)) == (0x100U | 4U) && fake_peek(FAKE_PDC_ENABLE(1)) == 7U);
    assert(FakeGic[intid].requested && FakeGic[intid].trigger == KSHIM_IRQ_TYPE_LEVEL_HIGH);
    assert(kshim_qcom_irq_request_spec(f, &s, KSHIM_IRQ_TYPE_LEVEL_HIGH, spi_handler, NULL, &intid) ==
           ERROR_DEVICE_ERROR);
    assert(!kshim_qcom_irq_free(FAKE_SPMI_PERIPH_IRQ));
    assert(fake_peek(FAKE_PDC_CFG(1)) == (0x100U | 2U) && fake_peek(FAKE_PDC_ENABLE(1)) == 5U);
    assert(!FakeGic[FAKE_SPMI_PERIPH_IRQ].requested);

    /* Falling edge: the PDC inverts, the GIC sees rising. */
    s.cells[1] = 482; s.cells[2] = KSHIM_IRQ_TYPE_EDGE_FALLING;
    assert(!kshim_qcom_irq_request_spec(f, &s, KSHIM_IRQ_TYPE_NONE, spi_handler, NULL, &intid));
    assert(intid == KSHIM_IRQ_SPI(482) && fake_peek(FAKE_PDC_CFG(2)) == 2U);
    assert(FakeGic[intid].trigger == KSHIM_IRQ_TYPE_EDGE_RISING);
    /* No type anywhere: keep the inherited PDC type (rising edge here). */
    fake_poke(FAKE_PDC_CFG(3), 6U);
    s.cells[1] = 483; s.cells[2] = 0;
    assert(!kshim_qcom_irq_request_spec(f, &s, KSHIM_IRQ_TYPE_NONE, spi_handler, NULL, &intid));
    assert(fake_peek(FAKE_PDC_CFG(3)) == 6U && FakeGic[intid].trigger == KSHIM_IRQ_TYPE_EDGE_RISING);
    assert(fake_peek(FAKE_PDC_ENABLE(1)) == (5U | 4U | 8U));
    assert(!kshim_pdc_restore());
    assert(fake_peek(FAKE_PDC_CFG(2)) == 0 && fake_peek(FAKE_PDC_CFG(3)) == 6U);
    assert(fake_peek(FAKE_PDC_ENABLE(1)) == 5U);
    assert(!FakeGic[KSHIM_IRQ_SPI(482)].requested && !FakeGic[KSHIM_IRQ_SPI(483)].requested);

    /* Pass-through: the TLMM summary and an SPI past the ranges. */
    unsigned writes = fake_writes_in(FAKE_PDC_BASE, 0x400);
    assert(!kshim_qcom_irq_request(f, tlmm, 0, KSHIM_IRQ_TYPE_LEVEL_HIGH, spi_handler, NULL, &intid));
    assert(intid == FAKE_TLMM_SUMMARY && fake_writes_in(FAKE_PDC_BASE, 0x400) == writes);
    assert(!kshim_qcom_irq_free(intid) && !FakeGic[intid].requested);
    struct kshim_dt_irq low = {.parent = pdc, .count = 3, .cells = {0, 208, KSHIM_IRQ_TYPE_LEVEL_LOW}};
    assert(kshim_qcom_irq_request_spec(f, &low, 0, spi_handler, NULL, &intid) == ERROR_NOT_SUPPORTED);
    struct kshim_dt_irq ppi = {.parent = pdc, .count = 3, .cells = {1, 5, 4}};
    assert(kshim_qcom_irq_request_spec(f, &ppi, 0, spi_handler, NULL, &intid) == ERROR_INVALID_PARAMETER);

    /* Upstream <pin type> with qcom,pdc-ranges, which takes precedence over
     * the built-in Android table. */
    const fdt32_t ranges[] = {cpu_to_fdt32(0), cpu_to_fdt32(700), cpu_to_fdt32(4)};
    assert(!fdt_setprop(f, pdc, "qcom,pdc-ranges", ranges, sizeof(ranges)));
    pdc = compatible(f, "qcom,pdc-sm8150");
    struct kshim_dt_irq up = {.parent = pdc, .count = 2, .cells = {1, KSHIM_IRQ_TYPE_LEVEL_HIGH}};
    assert(!kshim_qcom_irq_request_spec(f, &up, 0, spi_handler, NULL, &intid));
    assert(intid == KSHIM_IRQ_SPI(701) && (fake_peek(FAKE_PDC_CFG(1)) & 7U) == 4U);
    assert(!kshim_qcom_irq_free(intid) && fake_peek(FAKE_PDC_CFG(1)) == (0x100U | 2U));
    up.cells[0] = 9;
    assert(kshim_qcom_irq_request_spec(f, &up, 0, spi_handler, NULL, &intid) == ERROR_NOT_FOUND);
    assert(!fdt_delprop(f, pdc, "qcom,pdc-ranges"));

    /* MMU mapping of the PDC aperture (kshim_resources_init). */
    static struct kshim_resources r;
    memset(&r, 0, sizeof(r));
    r.fdt = f;
    kshim_irqchip_map_resources(&r);
    assert(r.mapping_count == 1 && r.mappings[0].base == FAKE_PDC_BASE && r.mappings[0].size == 0x400);
    assert(!kshim_pdc_unmapped);
    printf("pdc: ok\n");
}

/* ---- SPMI arbiter ----------------------------------------------------- */

static SpmiDeviceContext *Spmi;
static unsigned PonCalls, GpioCalls, LevelPmicCalls;
static uint8_t PonSeen;

static void pon_handler(void *context)
{
    assert(context == &PonCalls && FakeInHandler);
    /* Acknowledged before the callback: arbiter status and PMIC latch. */
    assert(!(*fake_pmic(0, 0x818) & 1U));
    /* Bus reads are allowed after the shared dispatch returned. */
    assert(SpmiRead(Spmi, 0, 0x810, &PonSeen, 1) == CR_SUCCESS);
    ++PonCalls;
}
static void gpio_handler(void *context) { (void)context; ++GpioCalls; }
static void level_pmic_handler(void *context)
{
    (void)context;
    if (++LevelPmicCalls == 2) fake_pmic_rt(0, 0x61, 1, false);
}

static void test_spmi(const void *f)
{
    fake_reset();
    PonCalls = GpioCalls = LevelPmicCalls = 0;
    const uint16_t ppids[] = {0x0008, 0x00c5, 0x0024, 0x0061};
    Spmi = fake_spmi_init(f, ppids, 4);
    fake_gic_line(FAKE_SPMI_PERIPH_IRQ, fake_spmi_line);
    /* Firmware left PON bit 5 configured and enabled. */
    *fake_pmic(0, 0x811) = 0x20; *fake_pmic(0, 0x812) = 0x20; *fake_pmic(0, 0x815) = 0x20;
    *fake_pmic(0, 0xc510) = 1; /* PMIC GPIO6 idles high (active-low key) */

    assert(!kshim_spmi_irq_request(f, 0, 0, 0x08, 0, KSHIM_IRQ_TYPE_EDGE_BOTH, pon_handler, &PonCalls));
    assert(!FakeIrqMasked && FakeSpmiGrantRefs == 1 && kshim_spmi_irq_live(0));
    assert(*fake_pmic(0, 0x811) == 0x21 && *fake_pmic(0, 0x812) == 0x21 &&
           *fake_pmic(0, 0x813) == 0x01 && *fake_pmic(0, 0x815) == 0x21);
    assert(fake_spmi_acc_enable(0) == 1);
    struct fake_gic_irq *g = &FakeGic[FAKE_SPMI_PERIPH_IRQ];
    assert(g->requested && g->trigger == KSHIM_IRQ_TYPE_LEVEL_HIGH);
    assert((fake_peek(FAKE_PDC_CFG(1)) & 7U) == 4U && (fake_peek(FAKE_PDC_ENABLE(1)) & 2U));
    assert(Spmi->HasInterrupt && Spmi->InterruptExternallyManaged && Spmi->SharedParentIrqScan);
    assert(kshim_spmi_irq_request(f, 0, 0, 0x08, 0, KSHIM_IRQ_TYPE_EDGE_BOTH, pon_handler, NULL) ==
           ERROR_DEVICE_ERROR);
    assert(kshim_spmi_irq_request(f, 0, 0, 0x99, 0, KSHIM_IRQ_TYPE_EDGE_BOTH, gpio_handler, NULL) ==
           ERROR_DEVICE_ERROR); /* no APID */
    /* One cascade instance: a second arbiter's clients poll. */
    assert(kshim_spmi_irq_request(f, 1, 0, 0x08, 0, KSHIM_IRQ_TYPE_EDGE_BOTH, gpio_handler, NULL) ==
           ERROR_NOT_SUPPORTED && !kshim_spmi_irq_live(1));
    assert(FakeSpmiGrantRefs == 1);
    assert(!kshim_spmi_irq_request(f, 0, 0, 0x08, 1, KSHIM_IRQ_TYPE_EDGE_BOTH, pon_handler, &PonCalls));
    assert(!kshim_spmi_irq_request(f, 0, 0, 0xc5, 0, KSHIM_IRQ_TYPE_EDGE_BOTH, gpio_handler, NULL));
    assert(FakeSpmiGrantRefs == 3 && g->requests == 1);

    /* Press and release: one interrupt each, acknowledged and dispatched. */
    fake_pmic_rt(0, 0x08, 0, true);
    assert(fake_spmi_line() && fake_gic_run() == 1 && PonCalls == 1 && (PonSeen & 1U));
    assert(!fake_spmi_line() && !(*fake_pmic(0, 0x818) & 1U));
    fake_pmic_rt(0, 0x08, 0, false);
    assert(fake_gic_run() == 1 && PonCalls == 2 && !(PonSeen & 1U));
    fake_pmic_rt(0, 0xc5, 0, false);
    assert(fake_gic_run() == 1 && GpioCalls == 1 && PonCalls == 2);
    /* A level source relatches until the handler deasserts it. */
    assert(!kshim_spmi_irq_request(f, 0, 0, 0x61, 1, KSHIM_IRQ_TYPE_LEVEL_HIGH, level_pmic_handler, NULL));
    assert(*fake_pmic(0, 0x6111) == 0 && *fake_pmic(0, 0x6112) == 2 && *fake_pmic(0, 0x6113) == 0);
    fake_pmic_rt(0, 0x61, 1, true);
    assert(fake_gic_run() >= 2 && LevelPmicCalls >= 2 && !fake_spmi_line());
    assert(!kshim_spmi_irq_free(0, 0, 0x61, 1));
    assert(FakeSpmiBadWrites == 0);

    /* A pending APID without a client (temperature alarm left enabled by
     * firmware) fails the shared dispatch: periph_irq is sealed, the foreign
     * APID is not acknowledged (no grant), clients fall back to polling. */
    fake_spmi_set_acc_enable(2, 1);
    fake_spmi_raise(2, 1);
    assert(fake_gic_run() == 1 && !g->enabled && g->disables == 1);
    assert(!kshim_spmi_irq_live(0) && FakeSpmiBadWrites == 1);
    assert(kshim_spmi_irq_request(f, 0, 0, 0x61, 1, KSHIM_IRQ_TYPE_LEVEL_HIGH, gpio_handler, NULL) ==
           ERROR_DEVICE_ERROR);

    /* Restore: every bit and the PIC enable back to the snapshot. */
    assert(!kshim_spmi_irq_restore());
    assert(*fake_pmic(0, 0x811) == 0x20 && *fake_pmic(0, 0x812) == 0x20 &&
           *fake_pmic(0, 0x813) == 0 && *fake_pmic(0, 0x815) == 0x20);
    assert(*fake_pmic(0, 0xc511) == 0 && *fake_pmic(0, 0xc512) == 0 && *fake_pmic(0, 0xc515) == 0);
    assert(*fake_pmic(0, 0x6115) == 0 && *fake_pmic(0, 0x6112) == 0);
    assert(fake_spmi_acc_enable(0) == 0 && fake_spmi_acc_enable(1) == 0 && fake_spmi_acc_enable(3) == 0);
    assert(fake_spmi_acc_enable(2) == 1); /* foreign: untouched */
    assert(!g->requested && fake_peek(FAKE_PDC_CFG(1)) == 0 && fake_peek(FAKE_PDC_ENABLE(1)) == 0);
    assert(FakeSpmiGrantRefs == 0);
    assert(!Spmi->HasInterrupt && !Spmi->InterruptExternallyManaged && !Spmi->SharedParentIrqScan);
    for (unsigned i = 0; i < SPMI_PMIC_ARB_MAX_IRQ_ENTRIES; ++i) assert(!Spmi->IrqEntries[i].Handler);
    assert(!kshim_spmi_irq_restore());

    /* Reopen after restore, then the whole step-3 handoff with the TLMM. */
    fake_spmi_set_acc_enable(2, 0);
    assert(!kshim_spmi_irq_request(f, 0, 0, 0x08, 0, KSHIM_IRQ_TYPE_EDGE_BOTH, pon_handler, &PonCalls));
    assert(g->requested && g->enabled && kshim_spmi_irq_live(0));
    uintptr_t pin122;
    assert(!kshim_dt_gpio(f, compatible(f, "qcom,sm8150-pinctrl"), 122, &pin122, NULL, NULL));
    fake_tlmm_pin(pin122, true);
    assert(!kshim_tlmm_irq_init(f));
    assert(!kshim_gpio_irq_request(122, KSHIM_IRQ_TYPE_LEVEL_LOW, level_handler, &LevelCalls));
    assert(!kshim_irqchip_restore());
    for (unsigned id = 0; id < 1020; ++id) assert(!FakeGic[id].requested);
    assert(fake_peek(pin122 + 8) == 0 && *fake_pmic(0, 0x815) == 0x20 && fake_spmi_acc_enable(0) == 0);
    assert(fake_peek(FAKE_PDC_ENABLE(1)) == 0 && FakeSpmiGrantRefs == 0);
    assert(!kshim_irqchip_restore());
    printf("spmi: ok\n");
}

int main(void)
{
    void *andromeda = load("andromeda-fdt.dtb"), *hdk = load("hdk8150-fdt.dtb");
    test_dt(andromeda, true);
    test_dt(hdk, false);
    printf("dt: ok\n");
    test_tlmm_layout();
    printf("tlmm layout: ok\n");
    test_tlmm(andromeda);
    test_pdc(andromeda);
    test_spmi(andromeda);
    test_spmi(hdk);
    free(andromeda); free(hdk);
    return 0;
}
