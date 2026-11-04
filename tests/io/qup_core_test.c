/* SPDX-License-Identifier: MIT */
/*
 * QUP core over fake clocks/pinctrl, the fake GIC/PDC and a GENI register
 * model: the registry from the mainline Pocket S2 and Android andromeda DTs,
 * the console reservation, per-window ownership (i2c4 vs spi4 on 0xa90000),
 * the inherited and cold clock paths, a firmware protocol mismatch, an
 * active engine, unwinding on every failure, SE interrupts (mainline 4-cell
 * GIC, Android PDC root), newest-first release at handoff and the windows
 * mapped before the MMU.
 */
#include <kshim/qup.h>
#include <kshim/io.h>
#include <qcom_dt.h>
#include <qcom_irq.h>
#include <Library/cr_geni_regs.h>

#include "qup_fakes.h"
#include "../irq_fake.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void irq_handler(uint32_t intid, void *context) { (void)intid; (void)context; }

static int node_named(const void *f, const char *path)
{
    int node = fdt_path_offset(f, path);
    assert(node >= 0);
    return node;
}

static const void *Other;

static void reset_world(void)
{
    /* A registry rebuilt from another blob first drops every cached clock
     * handle of the previous subtest (kshim_qup_init on the same blob is a
     * no-op). */
    const void *f = kshim_qup_fdt();
    FakeConsoleBase = 0x89c000; /* uart15 on the SM8650 blobs */
    if (f && Other) assert(!kshim_qup_init(Other) && !kshim_qup_init(f));
    fake_reset();
    fake_clk_reset();
    FakePinctrlStatus = 0;
    FakePinctrlCalls = 0;
    FakePinctrlNode = -1;
    kshim_qup_set_io(&FakeIo);
}

static void test_registry(const void *f, const void *other)
{
    reset_world();
    FakeConsoleBase = 0x89c000; /* uart15, stdout */
    assert(!kshim_qup_init(f));
    assert(kshim_qup_fdt() == f && kshim_qup_se_count() == 5);
    assert(!kshim_qup_init(f)); /* idempotent */
    assert(kshim_qup_init(NULL) == ERROR_INVALID_PARAMETER);

    struct kshim_qup_se *console = kshim_qup_se_by_base(0x89c000);
    assert(console && console->owner == KSHIM_QUP_OWNER_CONSOLE && console->owner_node == console->node);
    assert(!strcmp(console->owner_name, "console"));
    /* Nobody else may take the console's engine, or acquire it. */
    assert(kshim_qup_se_claim(console, KSHIM_QUP_OWNER_UART, console->node, "uart") == ERROR_BUSY);
    assert(kshim_qup_se_acquire(console, KSHIM_QUP_PROTOCOL_UART, NULL, NULL) == ERROR_INVALID_PARAMETER);

    int i2c4 = node_named(f, "/soc@0/geniqup@ac0000/i2c@a90000");
    int spi4 = node_named(f, "/soc@0/geniqup@ac0000/spi@a90000");
    struct kshim_qup_se *se4 = kshim_qup_se_by_node(i2c4);
    assert(se4 && se4 == kshim_qup_se_by_node(spi4) && se4 == kshim_qup_se_by_base(0xa90000));
    assert(se4 == kshim_qup_se_get(se4->index) && !kshim_qup_se_get(99));
    assert(kshim_qup_se_mmio(se4) == 0xa90000);
    assert(!kshim_qup_se_by_node(-1) && !kshim_qup_se_by_base(0x1234));
    /* Ownership is per window: one owner through one node. On the Pocket S2
     * spi4 is a disabled sibling of i2c4 and cannot be claimed at all. */
    assert(!kshim_qup_se_claim(se4, KSHIM_QUP_OWNER_I2C, i2c4, "touch i2c"));
    assert(!kshim_qup_se_claim(se4, KSHIM_QUP_OWNER_I2C, i2c4, "touch i2c"));
    assert(kshim_qup_se_claim(se4, KSHIM_QUP_OWNER_SPI, i2c4, "spi") == ERROR_BUSY);
    assert(kshim_qup_se_claim(se4, KSHIM_QUP_OWNER_UART, i2c4, "uart") == ERROR_BUSY);
    assert(kshim_qup_se_claim(se4, KSHIM_QUP_OWNER_SPI, spi4, "spi") == ERROR_NOT_FOUND);
    assert(kshim_qup_se_claim(se4, KSHIM_QUP_OWNER_SPI, console->node, "x") == ERROR_INVALID_PARAMETER);
    assert(!kshim_qup_se_unclaim(se4) && se4->owner == KSHIM_QUP_OWNER_NONE && se4->owner_node == -1);
    assert(kshim_qup_se_claim(se4, KSHIM_QUP_OWNER_SPI, spi4, "spi") == ERROR_NOT_FOUND);
    assert(!kshim_qup_se_claim(se4, KSHIM_QUP_OWNER_UART, i2c4, "uart") && se4->owner_node == i2c4);
    /* Re-init on another blob with live owners is refused. */
    assert(kshim_qup_init(other) == ERROR_BUSY && kshim_qup_fdt() == f);
    assert(!kshim_qup_se_unclaim(se4));
    printf("registry: ok\n");
}

static struct kshim_qup_se *claim_i2c4(const void *f)
{
    int i2c4 = node_named(f, "/soc@0/geniqup@ac0000/i2c@a90000");
    struct kshim_qup_se *se = kshim_qup_se_by_node(i2c4);
    assert(se && !kshim_qup_se_claim(se, KSHIM_QUP_OWNER_I2C, i2c4, "i2c"));
    return se;
}

static void test_acquire(const void *f)
{
    int i2c4 = node_named(f, "/soc@0/geniqup@ac0000/i2c@a90000");
    int wrapper = node_named(f, "/soc@0/geniqup@ac0000");
    uint32_t hz;
    uint8_t dfs;

    /* Cold: wrapper AHB clocks voted first, SE clock on CXO through a DFS
     * slot, pins applied, everything undone by release. */
    reset_world();
    FakeConsoleBase = 0x89c000;
    geni_model_reset(0xa90000, 3, 0);
    struct kshim_qup_se *se = claim_i2c4(f);
    struct kshim_clk *m = fake_clk_of(f, wrapper, "m-ahb"), *s = fake_clk_of(f, wrapper, "s-ahb");
    struct kshim_clk *sec = fake_clk_of(f, i2c4, "se");
    assert(m != s && s != sec);
    sec->dfs = 2;
    assert(!kshim_qup_se_acquire(se, KSHIM_QUP_PROTOCOL_I2C, &hz, &dfs));
    assert(hz == 19200000 && dfs == 2 && se->acquired && !se->inherited_active);
    assert(m->on && m->ours && s->on && s->ours && sec->on && sec->ours && sec->rate_calls == 1);
    assert(FakePinctrlCalls == 1 && FakePinctrlNode == i2c4);
    /* Acquire again: cached, nothing new. */
    assert(!kshim_qup_se_acquire(se, KSHIM_QUP_PROTOCOL_I2C, &hz, &dfs) && hz == 19200000 && dfs == 2);
    assert(sec->enables == 1 && sec->rate_calls == 1);
    assert(kshim_qup_se_acquire(se, KSHIM_QUP_PROTOCOL_SPI, &hz, &dfs) == ERROR_BUSY);
    assert(kshim_qup_se_unclaim(se) == ERROR_BUSY);
    assert(!kshim_qup_se_release(se) && !se->acquired && !fake_clk_enabled_by_us());
    assert(!m->on && !s->on && !sec->on);
    assert(!kshim_qup_se_release(se));

    /* Inherited: everything already runs; nothing is switched on or off and
     * the running SE clock is not handed to the clock driver for a rate:
     * its RCG (read only) already gives CXO undivided. */
    fake_clk_all(true);
    uintptr_t rcg = sec->desc.rcg;
    assert(!kshim_qup_se_acquire(se, KSHIM_QUP_PROTOCOL_I2C, &hz, &dfs));
    assert(se->inherited_active && !fake_clk_enabled_by_us() && dfs == 0 && hz == 19200000);
    assert(sec->rate_calls == 1 && !fake_writes_in(rcg, 0x100));
    assert(!kshim_qup_se_release(se) && m->on && s->on && sec->on);
    assert(!m->refs && !s->refs && !sec->refs);
    /* ... on another source: never retuned. */
    fake_poke(rcg + 0x4, 1U << 8);
    assert(kshim_qup_se_acquire(se, KSHIM_QUP_PROTOCOL_I2C, &hz, &dfs) == ERROR_BUSY);
    assert(!se->acquired && sec->rate_calls == 1 && !m->refs && !sec->refs);
    /* ... with DFS: the engine's own level (GENI_CLK_SEL 5) when it is CXO,
     * else another CXO level, else refused. */
    fake_poke(rcg + 0x14, 1);
    for (unsigned i = 0; i < 8; ++i) fake_poke(rcg + 0x1c + 4 * i, (1U << 8) | 3U);
    assert(kshim_qup_se_acquire(se, KSHIM_QUP_PROTOCOL_I2C, &hz, &dfs) == ERROR_NOT_SUPPORTED);
    fake_poke(rcg + 0x1c + 4 * 3, 1);
    assert(!kshim_qup_se_acquire(se, KSHIM_QUP_PROTOCOL_I2C, &hz, &dfs) && dfs == 3);
    assert(!kshim_qup_se_release(se));
    fake_poke(rcg + 0x1c + 4 * 5, 0);
    assert(!kshim_qup_se_acquire(se, KSHIM_QUP_PROTOCOL_I2C, &hz, &dfs) && dfs == 5);
    assert(!kshim_qup_se_release(se) && sec->rate_calls == 1 && !fake_writes_in(rcg, 0x100));
    fake_poke(rcg + 0x14, 0);
    fake_poke(rcg + 0x4, 0);

    /* Firmware for another protocol: refused, unwound. */
    fake_clk_all(false);
    Geni.regs[GENI_SE_FW_REVISION_RO / 4] = 1U << 8; /* SPI loaded */
    assert(kshim_qup_se_acquire(se, KSHIM_QUP_PROTOCOL_I2C, &hz, &dfs) == ERROR_NOT_SUPPORTED);
    assert(!se->acquired && !fake_clk_enabled_by_us() && !m->refs && !sec->refs);
    Geni.regs[GENI_SE_FW_REVISION_RO / 4] = 3U << 8;

    /* An active engine is never retuned or taken over. */
    Geni.s_active = true;
    unsigned rate_calls = sec->rate_calls;
    assert(kshim_qup_se_acquire(se, KSHIM_QUP_PROTOCOL_I2C, &hz, &dfs) == ERROR_BUSY);
    assert(sec->rate_calls == rate_calls && !fake_clk_enabled_by_us());
    /* ... but a UART that keeps its inherited rate accepts its RX command
     * (uart14, qcom,geni-uart at 0x898000, its S sequencer running). */
    int uart14 = node_named(f, "/soc@0/geniqup@8c0000/serial@898000");
    struct kshim_qup_se *u14 = kshim_qup_se_by_node(uart14);
    fake_mmio_region(0x898000, 0x4000, NULL, NULL);
    fake_poke(0x898000 + GENI_SE_FW_REVISION_RO, 2U << 8);
    fake_poke(0x898000 + GENI_SE_STATUS, GENI_SE_STATUS_S_GENI_CMD_ACTIVE);
    assert(!kshim_qup_se_claim(u14, KSHIM_QUP_OWNER_UART, uart14, "uart"));
    assert(kshim_qup_se_acquire(u14, KSHIM_QUP_PROTOCOL_SPI, NULL, NULL) == ERROR_INVALID_PARAMETER);
    struct kshim_clk *u14clk = fake_clk_of(f, uart14, "se");
    assert(!kshim_qup_se_acquire(u14, KSHIM_QUP_PROTOCOL_UART, NULL, NULL));
    assert(!u14clk->rate_calls && u14clk->on && u14->acquired);
    assert(!kshim_qup_se_release(u14) && !kshim_qup_se_unclaim(u14) && !fake_clk_enabled_by_us());
    /* A disabled sibling node cannot be claimed (spi4 on the Pocket S2). */
    assert(!kshim_qup_se_unclaim(se));
    int spi4 = node_named(f, "/soc@0/geniqup@ac0000/spi@a90000");
    assert(kshim_qup_se_claim(se, KSHIM_QUP_OWNER_SPI, spi4, "spi") == ERROR_NOT_FOUND);
    Geni.s_active = false;
    Geni.regs[GENI_SE_FW_REVISION_RO / 4] = 3U << 8;
    se = claim_i2c4(f);

    /* The clock driver refuses the rate: unwound. */
    sec->rate_status = ERROR_BUSY;
    assert(kshim_qup_se_acquire(se, KSHIM_QUP_PROTOCOL_I2C, &hz, &dfs) == ERROR_BUSY);
    assert(!fake_clk_enabled_by_us());
    sec->rate_status = 0;
    /* An AHB clock fails to enable: nothing left on. */
    s->enable_status = ERROR_TIMEOUT;
    assert(kshim_qup_se_acquire(se, KSHIM_QUP_PROTOCOL_I2C, &hz, &dfs) == ERROR_TIMEOUT);
    assert(!fake_clk_enabled_by_us() && !m->refs);
    s->enable_status = 0;

    /* Pinctrl failure: fatal on a cold engine, tolerated when inherited. */
    FakePinctrlStatus = ERROR_NOT_SUPPORTED;
    assert(kshim_qup_se_acquire(se, KSHIM_QUP_PROTOCOL_I2C, &hz, &dfs) == ERROR_NOT_SUPPORTED);
    assert(!se->acquired && !fake_clk_enabled_by_us());
    fake_clk_all(true);
    assert(!kshim_qup_se_acquire(se, KSHIM_QUP_PROTOCOL_I2C, &hz, &dfs) && se->inherited_active);
    assert(!kshim_qup_se_release(se) && !kshim_qup_se_unclaim(se));
    FakePinctrlStatus = 0;
    fake_clk_all(false);
    printf("acquire: ok\n");
}

static void test_irq_and_quiesce(const void *f)
{
    reset_world();
    FakeConsoleBase = 0x89c000;
    geni_model_reset(0xa90000, 3, 0);
    struct kshim_qup_se *se4 = claim_i2c4(f);
    uint32_t intid = 0;

    FakeIrqReady = false;
    assert(kshim_qup_se_irq_request(se4, irq_handler, NULL, &intid) == ERROR_NOT_READY);
    FakeIrqReady = true;
    /* <GIC_SPI 357 IRQ_TYPE_LEVEL_HIGH 0>, 4-cell GIC. */
    assert(!kshim_qup_se_irq_request(se4, irq_handler, NULL, &intid));
    assert(intid == KSHIM_IRQ_SPI(357) && se4->intid == intid);
    assert(FakeGic[intid].requested && FakeGic[intid].trigger == KSHIM_IRQ_TYPE_LEVEL_HIGH);
    assert(kshim_qup_se_irq_request(se4, irq_handler, NULL, &intid) == ERROR_BUSY);
    assert(kshim_qup_se_unclaim(se4) == ERROR_BUSY);

    /* Two engines acquired; handoff releases both, newest first, and frees
     * the interrupt left behind; the console's engine is not touched. */
    uint32_t hz;
    uint8_t dfs;
    assert(!kshim_qup_se_acquire(se4, KSHIM_QUP_PROTOCOL_I2C, &hz, &dfs));
    int i2c3 = node_named(f, "/soc@0/geniqup@ac0000/i2c@a8c000");
    struct kshim_qup_se *se3 = kshim_qup_se_by_node(i2c3);
    assert(se3 && !kshim_qup_se_claim(se3, KSHIM_QUP_OWNER_I2C, i2c3, "i2c3"));
    fake_mmio_region(0xa8c000, 0x4000, NULL, NULL);
    fake_poke(0xa8c000 + GENI_SE_FW_REVISION_RO, 3U << 8);
    assert(!kshim_qup_se_acquire(se3, KSHIM_QUP_PROTOCOL_I2C, &hz, &dfs));
    struct kshim_clk *c3 = fake_clk_of(f, i2c3, "se"), *c4 = fake_clk_of(f, se4->node, "se");
    assert(c3->on && c4->on);
    assert(!kshim_qup_quiesce_all());
    assert(!se3->acquired && !se4->acquired && !se4->intid && !FakeGic[intid].requested);
    assert(!c3->on && !c4->on && !fake_clk_enabled_by_us());
    assert(c3->disables == 1 && c4->disables == 1);
    assert(kshim_qup_se_by_base(0x89c000)->owner == KSHIM_QUP_OWNER_CONSOLE);
    assert(!kshim_qup_quiesce_all()); /* idempotent */
    assert(!kshim_qup_se_unclaim(se3) && !kshim_qup_se_unclaim(se4));
    printf("irq and quiesce: ok\n");
}

static void test_android(const void *f)
{
    reset_world();
    FakeConsoleBase = 0xa90000;
    assert(kshim_qup_init(f) == 0);
    assert(kshim_qup_se_count() == 7 && kshim_qup_se_by_base(0xa90000)->owner == KSHIM_QUP_OWNER_CONSOLE);
    geni_model_reset(0xc80000, 3, 0);
    struct kshim_qup_se *se = kshim_qup_se_by_base(0xc80000);
    assert(!kshim_qup_se_claim(se, KSHIM_QUP_OWNER_I2C, se->node, "touch"));
    /* Android: all three clocks are the engine node's own. */
    struct kshim_clk *sec = fake_clk_of(f, se->node, "se-clk");
    struct kshim_clk *m = fake_clk_of(f, se->node, "m-ahb");
    uint32_t hz;
    uint8_t dfs;
    assert(!kshim_qup_se_acquire(se, KSHIM_QUP_PROTOCOL_I2C, &hz, &dfs));
    assert(sec->on && sec->rate_calls == 1 && m->on && hz == 19200000);
    assert(se->wrapper && se->wrapper->base == 0xcc0000 && !se->wrapper->m_ahb);
    /* <0 0x175 0> under the PDC root: a PDC pass-through to SPI 373. */
    uint32_t intid;
    assert(!kshim_qup_se_irq_request(se, irq_handler, NULL, &intid));
    assert(intid == KSHIM_IRQ_SPI(0x175) && FakeGic[intid].requested);
    assert(FakeGic[intid].trigger == KSHIM_IRQ_TYPE_LEVEL_HIGH);
    assert(!kshim_qup_se_irq_free(se) && !FakeGic[intid].requested);
    assert(!kshim_qup_se_release(se) && !kshim_qup_se_unclaim(se) && !fake_clk_enabled_by_us());
    printf("android: ok\n");
}

static void test_map_resources(const void *f)
{
    static struct kshim_resources r;
    reset_world();
    memset(&r, 0, sizeof(r));
    r.fdt = f;
    FakeConsoleBase = 0xa90000;
    assert(!kshim_qup_map_resources(&r));
    /* Engines with an available child (i2c) or UART role; not the childless
     * spi@88c000. */
    assert(r.mapping_count == 6);
    for (size_t i = 0; i < r.mapping_count; ++i) {
        assert(r.mappings[i].type == KSHIM_MMU_DEVICE && r.mappings[i].size == 0x4000);
        assert(r.mappings[i].permissions == (KSHIM_MMU_READ | KSHIM_MMU_WRITE));
        assert(r.mappings[i].base != 0x88c000);
    }
    /* The registry (same blob, rebuilt) refuses the unmapped window. */
    assert(!kshim_qup_init(f));
    struct kshim_qup_se *spi = kshim_qup_se_by_base(0x88c000);
    assert(spi && !kshim_qup_se_claim(spi, KSHIM_QUP_OWNER_SPI, spi->node, "spi"));
    uint32_t hz;
    uint8_t dfs;
    assert(kshim_qup_se_acquire(spi, KSHIM_QUP_PROTOCOL_SPI, &hz, &dfs) == ERROR_NOT_SUPPORTED);
    assert(!FakeClkGets && !kshim_qup_se_unclaim(spi));
    /* A mapped one is acquired. */
    struct kshim_qup_se *touch = kshim_qup_se_by_base(0xc80000);
    geni_model_reset(0xc80000, 3, 0);
    assert(!kshim_qup_se_claim(touch, KSHIM_QUP_OWNER_I2C, touch->node, "touch"));
    assert(!kshim_qup_se_acquire(touch, KSHIM_QUP_PROTOCOL_I2C, &hz, &dfs));
    assert(!kshim_qup_quiesce_all() && !kshim_qup_se_unclaim(touch));
    assert(kshim_qup_map_resources(NULL) == ERROR_INVALID_PARAMETER);
    printf("map resources: ok\n");
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    void *s2 = qup_load_fixture(QUP_FIXTURES, "sm8650-ayaneo-pocket-s2-touch.dtb");
    void *andromeda = qup_load_fixture(FIXTURES, "andromeda-fdt.dtb");
    Other = qup_load_fixture(QUP_FIXTURES, "sm8650-qrd.dtb");
    test_registry(s2, andromeda);
    test_acquire(s2);
    test_irq_and_quiesce(s2);
    test_android(andromeda);
    test_map_resources(andromeda);
    free(s2);
    free(andromeda);
    free((void *)Other);
    puts("qup_core_test: ok");
    return 0;
}
