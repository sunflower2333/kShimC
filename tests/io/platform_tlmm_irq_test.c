/* SPDX-License-Identifier: MIT */
/*
 * The TLMM summary cascade on SM8650 (Pocket S2 fixture, irq_fake GIC):
 * the apps target goes to INTR_CFG bits 10:8 (pinctrl-sm8650, CrDK
 * Target/Sm8650 InterruptTargetMsk 0x700) and the direct-connect wake bits
 * 6/7 are preserved; the summary SPI 208 comes from the 4-cell mainline GIC
 * specifier; DT gpio-reserved-ranges pads are refused; free restores the
 * inherited word exactly.
 */
#include "../irq_fake.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <qcom_dt.h>
#include <qcom_irq.h>

static unsigned Calls;
static void handler(uint32_t pin, void *context) { (void)pin; (void)context; ++Calls; }

int main(void)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/io/sm8650-ayaneo-pocket-s2.dtb", FIXTURES);
    FILE *fp = fopen(path, "rb");
    assert(fp);
    static uint8_t blob[262144];
    size_t size = fread(blob, 1, sizeof(blob), fp);
    fclose(fp);
    assert(size > 0 && !dt_validate(blob, size));
    fake_reset();
    int tlmm = fdt_path_offset(blob, "/soc@0/pinctrl@f100000");
    assert(tlmm >= 0);
    uintptr_t pin162;
    assert(!kshim_dt_gpio(blob, tlmm, 162, &pin162, NULL, NULL) && pin162 == 0xf100000 + 162 * 0x1000);
    assert(!kshim_tlmm_irq_init(blob));
    uint32_t pin;
    assert(!kshim_tlmm_pin(pin162, &pin) && pin == 162);

    /* Inherited: wake present/enable (6/7) and a stale latch. */
    const uint32_t inherited = (1U << 6) | (1U << 7);
    fake_poke(pin162 + 8, inherited);
    fake_poke(pin162 + 0xc, 1);
    assert(!kshim_gpio_irq_request(162, KSHIM_IRQ_TYPE_EDGE_FALLING, handler, NULL));
    /* EN | RAW | falling (detection 2, polarity 1) | KPSS 3 << 8, wake kept. */
    uint32_t want = inherited | (3U << 8) | 0x10U | (2U << 2) | 2U | 1U;
    if (fake_peek(pin162 + 8) != want) fprintf(stderr, "cfg %x want %x\n", fake_peek(pin162 + 8), want);
    assert(fake_peek(pin162 + 8) == want);
    assert(fake_peek(pin162 + 0xc) == 0);
    struct fake_gic_irq *g = &FakeGic[KSHIM_IRQ_SPI(208)];
    assert(g->requested && g->enabled && g->trigger == KSHIM_IRQ_TYPE_LEVEL_HIGH);
    assert(kshim_gpio_irq_live(162));

    /* NFC pads reserved by the board DT; pad 210 is UFS_RESET. */
    assert(kshim_gpio_irq_request(33, KSHIM_IRQ_TYPE_LEVEL_LOW, handler, NULL) == ERROR_NOT_FOUND);
    assert(kshim_gpio_irq_request(74, KSHIM_IRQ_TYPE_LEVEL_LOW, handler, NULL) == ERROR_NOT_FOUND);
    assert(kshim_gpio_irq_request(210, KSHIM_IRQ_TYPE_LEVEL_LOW, handler, NULL) == ERROR_NOT_FOUND);

    /* Level-low on 99 (pcie1 WAKE pad): target in 10:8 again. */
    uintptr_t pin99 = 0xf100000 + 99 * 0x1000;
    fake_poke(pin99 + 8, 0);
    assert(!kshim_gpio_irq_request(99, KSHIM_IRQ_TYPE_LEVEL_LOW, handler, NULL));
    assert(fake_peek(pin99 + 8) == ((3U << 8) | 0x10U | 1U));

    assert(!kshim_gpio_irq_free(162));
    assert(fake_peek(pin162 + 8) == inherited && fake_peek(pin162 + 0xc) == 0);
    assert(!kshim_tlmm_irq_restore());
    assert(fake_peek(pin99 + 8) == 0 && !g->requested);
    puts("platform_tlmm_irq_test: ok");
    return 0;
}
