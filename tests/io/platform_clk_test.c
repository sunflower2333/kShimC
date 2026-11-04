/* SPDX-License-Identifier: MIT */
/*
 * Clock leaves over a GCC register model on the Pocket S2 fixture: cold
 * vote clocks wait for CLK_OFF, inherited running branches are never
 * written, reference counts, the votable pcie_1_gdsc, BCR resets, CXO
 * selection on a plain RCG and on a DFS RCG, a timeout that undoes its
 * write, and restore newest first. "kona": the SM8250 Android table
 * (CrAndroidClock) through the same API.
 */
#include "platform_fake.h"

#include <kshim/clk.h>
#include <kshim/soc_tables.h>
#include "../../kernel/drivers/gpio/internal.h"

#define GCC 0x100000UL
#define VOTE (GCC + 0x52008)
#define S4_CBCR (GCC + 0x183bc)
#define S4_RCG (GCC + 0x183c8)
#define AHB_M_CBCR (GCC + 0x2313c)
#define GDSC_VOTE (GCC + 0x5214c)
#define GDSCR (GCC + 0x8d004)
#define BCR (GCC + 0x8d000)

static bool Responsive = true;

/* Vote bits gate the branch CLK_OFF; the collapse vote gates PWR_ON;
 * RCG UPDATE self-clears. */
static void gcc_model(uintptr_t a, uint32_t v)
{
    if (!Responsive) return;
    if (a == VOTE) {
        fake_poke(S4_CBCR, (v & (1U << 26)) ? 0 : 1U << 31);
        fake_poke(AHB_M_CBCR, (v & (1U << 20)) ? 0 : 1U << 31);
    } else if (a == GDSC_VOTE) {
        fake_poke(GDSCR, (v & 2U) ? 0 : 1U << 31);
    } else if (a == S4_RCG && (v & 1U)) {
        fake_poke(S4_RCG, v & ~1U);
    }
}

static int index_of(uintptr_t address, unsigned from)
{
    for (unsigned i = from; i < FakeLogCount; ++i) if (FakeLog[i].address == address) return (int)i;
    return -1;
}

static void sm8650(void)
{
    void *f = fake_fixture("io/sm8650-ayaneo-pocket-s2.dtb");
    assert(!kshim_clk_init(f) && !kshim_clk_init(f));
    int i2c4 = fake_path(f, "/soc@0/geniqup@ac0000/i2c@a90000");
    int wrapper = fake_path(f, "/soc@0/geniqup@ac0000");
    int pcie1 = fake_path(f, "/soc@0/pcie@1c08000");
    struct kshim_clk *se, *ahb, *gdsc, *bcr;
    assert(!kshim_clk_get(f, i2c4, "se", &se) && !kshim_clk_get(f, wrapper, "m-ahb", &ahb));
    assert(!kshim_clk_get_power_domain(f, pcie1, 0, &gdsc) && !kshim_clk_get_reset(f, pcie1, "pci", &bcr));
    struct kshim_clk *again;
    assert(!kshim_clk_get(f, i2c4, NULL, &again) && again == se); /* index 0 is "se" */

    /* Cold: both off. */
    fake_poke(VOTE, 0x1);
    fake_poke(S4_CBCR, 1U << 31);
    fake_poke(AHB_M_CBCR, 1U << 31);
    fake_poke(S4_RCG + 4, (1U << 8) | 5U); /* GPLL source, divider 3 */
    fake_poke(GDSC_VOTE, 0x12);
    fake_poke(GDSCR, 0);
    assert(!kshim_clk_is_on(se) && !kshim_clk_is_on(ahb) && !kshim_clk_is_on(gdsc));

    /* CXO on a plain RCG before the branch runs (CrDK clock_poll order). */
    uint8_t dfs = 7;
    assert(!kshim_clk_set_rate_cxo(se, &dfs) && dfs == 0);
    assert(fake_peek(S4_RCG + 4) == 1U && !(fake_peek(S4_RCG) & 1U));
    assert(!kshim_clk_enable(ahb) && kshim_clk_is_on(ahb));
    assert(!kshim_clk_enable(se) && kshim_clk_is_on(se));
    assert(fake_peek(VOTE) == (0x1 | (1U << 20) | (1U << 26)));
    assert(!kshim_clk_enable(se)); /* refcount 2 */
    assert(!kshim_clk_disable(se) && kshim_clk_is_on(se));
    assert(!kshim_clk_disable(se) && !kshim_clk_is_on(se));
    assert(kshim_clk_disable(se) == ERROR_INVALID_PARAMETER);
    assert(!kshim_clk_enable(se));

    /* GDSC (votable): clear the collapse bit, PWR_ON follows. */
    assert(!kshim_clk_enable(gdsc) && kshim_clk_is_on(gdsc) && fake_peek(GDSC_VOTE) == 0x10);
    assert(!kshim_clk_reset(bcr, true) && (fake_peek(BCR) & 1U));
    assert(!kshim_clk_reset(bcr, false) && !(fake_peek(BCR) & 1U));
    assert(!kshim_clk_reset(bcr, true));
    assert(kshim_clk_enable(bcr) == ERROR_INVALID_PARAMETER);

    /* Handoff: newest first: BCR, GDSC, SE vote, AHB vote, RCG. */
    unsigned mark = FakeLogCount;
    assert(!kshim_clk_restore_all());
    int bcr_at = index_of(BCR, mark), gdsc_at = index_of(GDSC_VOTE, mark), rcg_at = index_of(S4_RCG + 4, mark);
    assert(bcr_at >= 0 && gdsc_at > bcr_at && rcg_at > gdsc_at);
    assert(!(fake_peek(BCR) & 1U) && fake_peek(GDSC_VOTE) == 0x12 && fake_peek(VOTE) == 0x1);
    assert(fake_peek(S4_RCG + 4) == ((1U << 8) | 5U));
    unsigned writes = FakeWrites;
    assert(!kshim_clk_restore_all() && FakeWrites == writes);

    /* Inherited running branch: never written, never retuned. */
    fake_poke(VOTE, 0x1 | (1U << 26));
    fake_poke(S4_CBCR, 0);
    writes = FakeWrites;
    assert(!kshim_clk_enable(se) && FakeWrites == writes);
    assert(kshim_clk_set_rate_cxo(se, &dfs) == ERROR_BUSY);
    assert(!kshim_clk_disable(se) && FakeWrites == writes && kshim_clk_is_on(se));

    /* DFS RCG: pick the verified CXO level, write nothing. */
    fake_poke(VOTE, 0x1);
    fake_poke(S4_CBCR, 1U << 31);
    fake_poke(S4_RCG + 0x14, 1);
    fake_poke(S4_RCG + 0x1c, (1U << 8) | 1U);
    fake_poke(S4_RCG + 0x20, (2U << 12) | 1U);
    fake_poke(S4_RCG + 0x24, 1U);
    writes = FakeWrites;
    assert(!kshim_clk_set_rate_cxo(se, &dfs) && dfs == 2 && FakeWrites == writes);
    fake_poke(S4_RCG + 0x24, (1U << 8));
    for (unsigned i = 3; i < 8; ++i) fake_poke(S4_RCG + 0x1c + i * 4, 3U << 8);
    assert(kshim_clk_set_rate_cxo(se, &dfs) == ERROR_NOT_SUPPORTED);
    fake_poke(S4_RCG + 0x14, 0);

    /* A branch that never reports on: timeout, the vote is undone. */
    Responsive = false;
    assert(kshim_clk_enable(se) == ERROR_TIMEOUT);
    assert(fake_peek(VOTE) == 0x1);
    Responsive = true;
    /* PHY "ref" is the TCSR clkref; GCC's own bi_tcxo parent is RPMh. */
    struct kshim_clk *ref, *rpmh_cxo;
    int phy = fake_path(f, "/soc@0/phy@1c0e000");
    assert(!kshim_clk_get(f, phy, "ref", &ref) && kshim_clk_desc(ref)->enable_reg == 0x1fc0000 + 0x31114);
    int gcc = fake_path(f, "/soc@0/clock-controller@100000");
    assert(kshim_clk_get(f, gcc, NULL, &rpmh_cxo) == ERROR_NOT_SUPPORTED);
    assert(!kshim_clk_restore_all());
    free(f);
}

static void kona(void)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/kona-resources.dtb", GENERATED_FIXTURES);
    FILE *fp = fopen(path, "rb");
    assert(fp);
    static uint8_t blob[65536];
    size_t size = fread(blob, 1, sizeof(blob), fp);
    fclose(fp);
    assert(size > 0 && !dt_validate(blob, size));
    assert(!kshim_clk_init(blob));
    int se = fdt_path_offset(blob, "/i2c@a94000");
    assert(se >= 0);
    struct kshim_clk *c, *m;
    assert(!kshim_clk_get(blob, se, "se-clk", &c) && !kshim_clk_get(blob, se, "m-ahb", &m));
    const struct kshim_clk_desc *d = kshim_clk_desc(c);
    /* CrAndroidClock(8250, 116): gcc_qupv3_wrap1_s5_clk. */
    assert(d->kind == KSHIM_CLK_VOTE && d->enable_reg == 0x100000 + 0x52008 && d->enable_mask == (1U << 27));
    assert(d->halt_reg == 0x100000 + 0x185fc && d->rcg == 0x100000 + 0x18600);
    assert(kshim_clk_desc(m)->rcg == 0);
    assert(kshim_clk_get_by_name("gcc", "gcc_qupv3_wrap1_s5_clk", &c) == ERROR_NOT_SUPPORTED);
}

int main(int argc, char **argv)
{
    fake_reset();
    kshim_platform_set_io(&FakeIo);
    FakeHook = gcc_model;
    if (argc > 1 && !strcmp(argv[1], "kona")) kona();
    else sm8650();
    puts("platform_clk_test: ok");
    return 0;
}
