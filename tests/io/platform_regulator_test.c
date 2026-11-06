/* SPDX-License-Identifier: MIT */
/*
 * Regulators over the TLMM model and a fake apps RSC (v3, drv-2, ACTIVE
 * TCS 0-2) with a CMD DB, on the Pocket S2 fixture:
 *  - regulator-fixed: vin-supply chain (vph_pwr, always on), pinctrl,
 *    level before OE, startup delay, inherited-on GPIO left unwritten,
 *    last disable back to the inherited level, bulk enable/unwind;
 *  - RPMh: mainline resource names (ldob14, ldoe3, ldoi3, ldoi1), own-DRV
 *    observation by RPMh read, voltage/mode/enable votes, inherited-on
 *    rails not written, restore to the observed request (enable, mode,
 *    voltage), unknown inherited request kept, a hung TCS retained;
 *  - "kona": Android qcom,resource-name rails (ldoa13) on SM8250, and the
 *    load hint turned into a PMIC5 mode from qcom,supported-modes /
 *    qcom,mode-threshold-currents (FTM4 AVDD 20 mA: HPM 7).
 */
#include "platform_fake.h"

#include <kshim/gpio.h>
#include <kshim/regulator.h>
#include <kshim/soc_tables.h>
#include "../../kernel/drivers/gpio/internal.h"

#define TLMM 0xf100000UL
#define DRV2 0x17a20000UL
#define CMD_DB 0x81c60000UL
static uintptr_t cfg(unsigned pin) { return TLMM + pin * 0x1000UL; }
static uintptr_t io(unsigned pin) { return cfg(pin) + 4; }

enum { L14B = 0x40d00, L3E = 0x41100, L3I = 0x41200, L1I = 0x41300, L13A = 0x40e00 };

static void rsc_and_db(void)
{
    static const char *const names[] = {"ldob14", "ldoe3", "ldoi3", "ldoi1", "ldoa13"};
    static const uint32_t addresses[] = {L14B, L3E, L3I, L1I, L13A};
    fake_cmd_db(CMD_DB, names, addresses, 5);
    /* tcs-config <ACTIVE 3>, <SLEEP 2>, <WAKE 2>, <CONTROL 0>. */
    fake_rsc_setup(DRV2, 0xd00, 2, 0, 3, 7);
}

static int index_of(uintptr_t address, unsigned from)
{
    for (unsigned i = from; i < FakeLogCount; ++i) if (FakeLog[i].address == address) return (int)i;
    return -1;
}

static void fixed_rails(void *f)
{
    int ep = fake_path(f, "/soc@0/pcie@1c08000/pcie@0/usb-controller@0");
    struct kshim_regulator *avdd33, *vdd10, *vdd33;
    assert(!kshim_regulator_get(f, ep, "avdd33", &avdd33));
    assert(!kshim_regulator_get(f, ep, "vdd10", &vdd10));
    assert(!kshim_regulator_get(f, ep, "vdd33", &vdd33));
    /* 123: input, pull-down, low. 122: inherited on, already in the
     * pinctrl state (gpio, 2 mA, bias-disable, output high). 121: off. */
    fake_poke(cfg(123), 0x1);
    fake_poke(cfg(122), KSHIM_TLMM_CFG_OE);
    fake_poke(io(122), KSHIM_TLMM_IO_OUT | KSHIM_TLMM_IO_IN);
    fake_poke(cfg(121), 0x1);
    assert(!kshim_regulator_is_enabled(avdd33) && kshim_regulator_is_enabled(vdd10));

    uint64_t t0 = FakeNow;
    unsigned mark = FakeLogCount;
    assert(!kshim_regulator_enable(avdd33));
    assert(FakeNow - t0 >= 5000);
    assert(fake_peek(cfg(123)) == KSHIM_TLMM_CFG_OE && (fake_peek(io(123)) & KSHIM_TLMM_IO_OUT));
    int level = index_of(io(123), mark), last_cfg = -1;
    for (int at = index_of(cfg(123), mark); at >= 0; at = index_of(cfg(123), (unsigned)at + 1)) last_cfg = at;
    assert(level >= 0 && last_cfg > level); /* OE set after the level */
    assert(kshim_regulator_is_enabled(avdd33));

    unsigned writes = FakeWrites;
    assert(!kshim_regulator_enable(vdd10) && FakeWrites == writes); /* inherited-on */
    assert(!kshim_regulator_enable(vdd10) && !kshim_regulator_disable(vdd10));

    /* Last disable: back to the inherited (off) level. */
    assert(!kshim_regulator_disable(avdd33));
    assert(!(fake_peek(io(123)) & KSHIM_TLMM_IO_OUT));
    assert(kshim_regulator_disable(avdd33) == ERROR_INVALID_PARAMETER);

    /* Bulk: the three supplies in property order, unwound on failure. */
    assert(!kshim_regulator_bulk_enable(f, ep));
    assert((fake_peek(io(123)) & KSHIM_TLMM_IO_OUT) && (fake_peek(io(121)) & KSHIM_TLMM_IO_OUT));
    assert(!kshim_regulator_bulk_disable(f, ep));
    assert(!(fake_peek(io(121)) & KSHIM_TLMM_IO_OUT));
    int bad = fake_path(f, "/bulk-test");
    assert(kshim_regulator_bulk_enable(f, bad) == ERROR_NOT_SUPPORTED);
    assert(!(fake_peek(io(123)) & KSHIM_TLMM_IO_OUT) && !(fake_peek(io(121)) & KSHIM_TLMM_IO_OUT));

    /* Left enabled at handoff: regulators drop their votes, then the GPIO
     * layer puts every pad back. */
    assert(!kshim_regulator_enable(vdd33));
    assert(!kshim_regulator_restore_all());
    assert(!(fake_peek(io(121)) & KSHIM_TLMM_IO_OUT));
    assert(!kshim_gpio_restore_all());
    assert(fake_peek(cfg(121)) == 0x1 && fake_peek(cfg(123)) == 0x1);
    assert(fake_peek(cfg(122)) == KSHIM_TLMM_CFG_OE && (fake_peek(io(122)) & KSHIM_TLMM_IO_OUT));
}

static void rpmh_rails(void *f)
{
    int ts = fake_path(f, "/soc@0/geniqup@ac0000/i2c@a90000/touchscreen@5d");
    int phy = fake_path(f, "/soc@0/phy@1c0e000");
    struct kshim_regulator *l14b, *l3e, *l3i;
    assert(!kshim_regulator_get(f, ts, "AVDD28", &l14b));
    assert(!kshim_regulator_get(f, phy, "vdda-phy", &l3e) && !kshim_regulator_get(f, phy, "vdda-pll", &l3i));

    /* l14b inherited off: vote 3200 mV, HPM (PMIC5 LDO 7), enable. */
    fake_rpmh_set(L14B + 0, 0);
    fake_rpmh_set(L14B + 4, 0);
    fake_rpmh_set(L14B + 8, 4);
    assert(!kshim_regulator_enable(l14b));
    assert(fake_rpmh_get(L14B) == 3200 && fake_rpmh_get(L14B + 4) == 1 && fake_rpmh_get(L14B + 8) == 7);
    assert(FakeRsc.reads == 3 && FakeRsc.writes == 3);
    /* Borrowed TCS restored: idle, no trigger, IRQ status clear. */
    assert(!(fake_peek(DRV2 + 0xd00 + 0x24) & ((1U << 16) | (1U << 24))) && !fake_peek(DRV2 + 0xd00 + 4));
    assert(kshim_regulator_is_enabled(l14b));
    /* Clamped to the DT range. */
    assert(!kshim_regulator_set_voltage(l14b, 3000000, 3300000) && fake_rpmh_get(L14B) == 3200);
    assert(kshim_regulator_set_voltage(l14b, 1000000, 1100000) == ERROR_NOT_SUPPORTED);
    assert(!kshim_regulator_disable(l14b));
    assert(fake_rpmh_get(L14B) == 0 && fake_rpmh_get(L14B + 4) == 0 && fake_rpmh_get(L14B + 8) == 4);

    /* l3e inherited on at 900 mV HPM: not written. */
    fake_rpmh_set(L3E + 0, 900);
    fake_rpmh_set(L3E + 4, 1);
    fake_rpmh_set(L3E + 8, 7);
    unsigned writes = FakeRsc.writes;
    assert(!kshim_regulator_enable(l3e) && FakeRsc.writes == writes);
    assert(!kshim_regulator_disable(l3e) && FakeRsc.writes == writes);

    /* l3i: the inherited request cannot be read; the vote is kept. */
    FakeRsc.read_unsupported = true;
    assert(!kshim_regulator_enable(l3i));
    FakeRsc.read_unsupported = false;
    assert(fake_rpmh_get(L3I + 4) == 1 && fake_rpmh_get(L3I) == 1200);
    writes = FakeRsc.writes;
    assert(!kshim_regulator_restore_all());
    assert(FakeRsc.writes == writes && fake_rpmh_get(L3I + 4) == 1);

    /* Handoff with an enabled observed rail: enable, mode, voltage back. */
    assert(!kshim_regulator_enable(l14b));
    assert(!kshim_regulator_restore_all());
    assert(fake_rpmh_get(L14B + 4) == 0 && fake_rpmh_get(L14B + 8) == 4 && fake_rpmh_get(L14B) == 0);
    writes = FakeRsc.writes;
    assert(!kshim_regulator_restore_all() && FakeRsc.writes == writes);

    /* A TCS that never completes stays claimed; RPMh is then unavailable. */
    struct kshim_regulator *l1i;
    assert(!kshim_regulator_get(f, phy, "vdda-qref", &l1i));
    FakeRsc.hang = true;
    assert(kshim_regulator_enable(l1i));
    assert(fake_peek(DRV2 + 0xd00 + 0x24) & (1U << 24)); /* still triggered, not reused */
    FakeRsc.hang = false;
    assert(kshim_regulator_enable(l1i) == ERROR_NOT_READY);
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
    static const char *const names[] = {"ldoa13"};
    static const uint32_t addresses[] = {L13A};
    fake_cmd_db(0x80860000, names, addresses, 1);
    fake_rsc_setup(0x18220000, 0xd00, 2, 0, 2, 9); /* <2 2> first: ACTIVE 0-1 */
    assert(!kshim_regulator_init(blob));
    int touch = fdt_path_offset(blob, "/i2c@a94000/touch@49");
    assert(touch >= 0);
    struct kshim_regulator *avdd, *vdd;
    assert(!kshim_regulator_get(blob, touch, "avdd", &avdd) && !kshim_regulator_get(blob, touch, "vdd", &vdd));
    assert(!strcmp(kshim_regulator_name(avdd), "regulator"));
    /* No load hint: no mode vote (the aggregated mode stays as inherited). */
    assert(kshim_regulator_mode(avdd) == -1);
    /* Inherited on at 3008 mV in LPM (PMIC5 LDO 4, the DT's init mode). The
     * touch framework hints before the first enable: 5 mA is still LPM,
     * 20 mA (FTM4 AVDD) needs HPM (7); a lower hint never lowers it. */
    fake_rpmh_set(L13A + 0, 3008);
    fake_rpmh_set(L13A + 4, 1);
    fake_rpmh_set(L13A + 8, 4);
    assert(!kshim_regulator_set_load(avdd, 5000) && kshim_regulator_mode(avdd) == 4);
    assert(!kshim_regulator_set_load(avdd, 20000) && kshim_regulator_mode(avdd) == 7);
    assert(!kshim_regulator_set_load(avdd, 100) && kshim_regulator_mode(avdd) == 7);
    unsigned writes = FakeRsc.writes;
    assert(!kshim_regulator_enable(avdd));
    /* Only the mode is written; voltage and enable were already right. */
    assert(fake_rpmh_get(L13A + 8) == 7 && FakeRsc.writes == writes + 1);
    assert(fake_rpmh_get(L13A) == 3008 && fake_rpmh_get(L13A + 4) == 1);
    assert(kshim_regulator_enable(vdd) == ERROR_NOT_FOUND); /* ldoc1 missing from this CMD DB */
    assert(!kshim_regulator_set_load(vdd, 20000) && kshim_regulator_mode(vdd) == -1); /* no facts */
    /* Release and handoff: the mode goes back to the observed LPM request. */
    assert(!kshim_regulator_disable(avdd) && fake_rpmh_get(L13A + 8) == 4 && fake_rpmh_get(L13A + 4) == 1);
    assert(!kshim_regulator_enable(avdd) && fake_rpmh_get(L13A + 8) == 7);
    assert(!kshim_regulator_restore_all() && fake_rpmh_get(L13A + 8) == 4 && fake_rpmh_get(L13A + 4) == 1);
}

int main(int argc, char **argv)
{
    fake_reset();
    kshim_platform_set_io(&FakeIo);
    if (argc > 1 && !strcmp(argv[1], "kona")) {
        kona();
    } else {
        void *f = fake_fixture("io/sm8650-ayaneo-pocket-s2.dtb");
        /* DT edits first: node offsets move with every edit. A 5 ms
         * startup delay on avdd33, and a consumer whose last supply is not
         * a regulator (bulk unwind). */
        int node = fdt_node_offset_by_prop_value(f, -1, "regulator-name", "upd720201_avdd33",
                                                 sizeof("upd720201_avdd33"));
        assert(node >= 0 && !fdt_setprop_u32(f, node, "startup-delay-us", 5000));
        int ep = fake_path(f, "/soc@0/pcie@1c08000/pcie@0/usb-controller@0");
        uint32_t avdd = fdt32_to_cpu(*(const fdt32_t *)fdt_getprop(f, ep, "avdd33-supply", NULL));
        uint32_t vdd33 = fdt32_to_cpu(*(const fdt32_t *)fdt_getprop(f, ep, "vdd33-supply", NULL));
        uint32_t tlmm = fdt_get_phandle(f, fake_path(f, "/soc@0/pinctrl@f100000"));
        int bulk = fdt_add_subnode(f, 0, "bulk-test");
        assert(bulk >= 0 && !fdt_setprop_u32(f, bulk, "avdd33-supply", avdd) &&
               !fdt_setprop_u32(f, bulk, "vdd33-supply", vdd33) &&
               !fdt_setprop_u32(f, bulk, "vbad-supply", tlmm));
        rsc_and_db();
        assert(!kshim_gpio_init(f) && !kshim_regulator_init(f));
        fixed_rails(f);
        rpmh_rails(f);
        free(f);
    }
    free(FakeDb);
    puts("platform_regulator_test: ok");
    return 0;
}
