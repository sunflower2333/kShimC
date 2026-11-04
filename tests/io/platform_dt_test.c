/* SPDX-License-Identifier: MIT */
/*
 * SM8650 board resources from the mainline fixtures (tests/fixtures/io):
 * everything the qup, pci, xhci and touch implementers need from the
 * sm8650 layer resolves on the AYANEO Pocket S2 (with the RFC touch node)
 * and on the QRD, without any MMIO:
 *   pcie1 regs, PERST/WAKE GPIOs, clocks/resets/GDSC, PHY supplies; the
 *   uPD720201 regulator-fixed GPIOs and gamepad_pwr_en; i2c4/spi4 SE
 *   window, SE clock and QUP pinctrl; TLMM 161/162; vreg_l14b_3p2;
 *   keys (PON gen3 power/resin, pm8550_gpios 6), console, GIC, timer,
 *   SPMI arbiter, PDC ranges and the TLMM summary layout; and the map
 *   hooks' MMU windows.
 */
#include "platform_fake.h"

#include <kshim/clk.h>
#include <kshim/gpio.h>
#include <kshim/regulator.h>
#include <kshim/soc_tables.h>
#include <kshim_console.h>
#include <kshim/storage_probe.h>
#include <kshim_irq_dt.h>
#include <qcom_irq.h>
#include <qcom_keys_fdt.h>
#include "../../kernel/drivers/gpio/internal.h"

static void gpio_is(const void *f, int node, const char *prop, uint32_t pin, bool active_low)
{
    struct kshim_gpio g;
    int status = kshim_gpio_from_dt(f, node, prop, 0, &g);
    if (status) fprintf(stderr, "%s: %d\n", prop, status);
    assert(!status);
    assert(g.chip == KSHIM_GPIO_CHIP_TLMM && g.pin == pin && g.active_low == active_low);
    assert(g.ctl == 0xf100000 + pin * 0x1000U && g.slot == -1);
    uint32_t irq_pin;
    assert(!kshim_gpio_tlmm_pin(&g, &irq_pin) && irq_pin == pin);
}

static void clk_is(const void *f, int node, const char *name, uintptr_t enable_reg, uint32_t mask,
                   uintptr_t halt_reg, uintptr_t rcg)
{
    struct kshim_clk *c;
    int status = kshim_clk_get(f, node, name, &c);
    if (status) fprintf(stderr, "clock %s: %d\n", name, status);
    assert(!status);
    const struct kshim_clk_desc *d = kshim_clk_desc(c);
    if (d->enable_reg != enable_reg || d->enable_mask != mask || d->halt_reg != halt_reg || d->rcg != rcg)
        fprintf(stderr, "clock %s: %lx %x %lx %lx\n", name, (unsigned long)d->enable_reg, d->enable_mask,
                (unsigned long)d->halt_reg, (unsigned long)d->rcg);
    assert(d->enable_reg == enable_reg && d->enable_mask == mask && d->halt_reg == halt_reg && d->rcg == rcg);
}

static void rpmh_is(const void *f, int consumer, const char *supply, const char *name, const char *resource)
{
    struct kshim_regulator *r;
    int status = kshim_regulator_get(f, consumer, supply, &r);
    if (status) fprintf(stderr, "supply %s: %d\n", supply, status);
    assert(!status);
    if (strcmp(kshim_regulator_name(r), name)) fprintf(stderr, "%s -> %s\n", supply, kshim_regulator_name(r));
    assert(!strcmp(kshim_regulator_name(r), name));
    (void)resource; /* checked through the CMD DB model in platform_regulator_test */
}

/* Shared SM8650 SoC facts, both boards. */
static void soc_common(void *f, const char *se4_path)
{
    struct kshim_console_resource c;
    assert(!kshim_console_probe(f, &c) && c.base == 0x89c000 && c.geni); /* uart15, serial0 */

    struct kshim_gic_desc gic;
    assert(!kshim_dt_gic(f, &gic));
    assert(gic.dist.base == 0x17100000 && gic.redist_count == 1);
    assert(gic.redist[0].base == 0x17180000 && gic.redist[0].size == 0x200000);
    assert(gic.redist_stride == 0x40000); /* GICv4.1 4 x 64K frames */
    struct kshim_timer_desc t;
    assert(!kshim_dt_arch_timer(f, &t) && t.intid[KSHIM_TIMER_PHYS] == 30 && t.intid[KSHIM_TIMER_VIRT] == 27);

    struct kshim_resources *r = calloc(1, sizeof(*r));
    assert(r);
    r->fdt = f;
    assert(!kshim_dt_spmi(f, r) && r->spmi_count == 1);
    assert(r->spmi[0].controller.Regions[SPMI_MEMORY_REGION_TYPE_CORE].BaseAddress == 0xc400000);

    /* TLMM: summary SPI 208 straight to the GIC (mainline root parent). */
    int tlmm = fake_path(f, "/soc@0/pinctrl@f100000");
    assert(kshim_dt_soc(f, tlmm) == KSHIM_SOC_SM8650 && kshim_gpio_tlmm_soc(f, tlmm) == KSHIM_SOC_SM8650);
    assert(kshim_dt_irq_kind(f, tlmm) == KSHIM_DT_IRQ_TLMM);
    uint32_t intid;
    unsigned trigger;
    assert(!kshim_irq_from_fdt(f, tlmm, 0, &intid, &trigger) && intid == 32 + 208 && trigger == 4);
    uintptr_t a;
    assert(!kshim_dt_gpio(f, tlmm, 209, &a, NULL, NULL) && a == 0xf100000 + 209 * 0x1000U);
    assert(kshim_dt_gpio(f, tlmm, 210, &a, NULL, NULL)); /* UFS_RESET pad, not a GPIO */
    assert(kshim_gpio_pin_reserved(f, tlmm, 32) && kshim_gpio_pin_reserved(f, tlmm, 74));
    assert(!kshim_gpio_pin_reserved(f, tlmm, 161) && !kshim_gpio_pin_reserved(f, tlmm, 75));
    struct kshim_gpio g;
    assert(kshim_gpio_from_tlmm_pin(f, tlmm, 33, false, &g) == ERROR_NOT_SUPPORTED);

    /* PDC (SPMI periph_irq is PDC pin 1 -> SPI 481). */
    int pdc = fake_path(f, "/soc@0/interrupt-controller@b220000");
    assert(kshim_dt_irq_kind(f, pdc) == KSHIM_DT_IRQ_PDC);
    int len;
    assert(fdt_getprop(f, pdc, "qcom,pdc-ranges", &len) && len == 6 * 12);

    /* Keys: PMK8550 PON gen3 power + resin (vol-), pm8550_gpios 6 (vol+). */
    QcomKeyDescriptor keys[QCOM_KEYS_MAX];
    size_t count = 0;
    assert(!QcomKeysBuildFromDeviceTree(f, keys, QCOM_KEYS_MAX, &count));
    bool power = false, up = false, down = false;
    for (size_t i = 0; i < count; ++i) {
        uint64_t token = keys[i].Base;
        assert(QCOM_KEY_IS_SPMI_TOKEN(token));
        if (keys[i].Action == QCOM_KEY_ACTION_SELECT) {
            power = QCOM_KEY_SPMI_TOKEN_SID(token) == 0 && QCOM_KEY_SPMI_TOKEN_ADDRESS(token) == 0x1300 &&
                    keys[i].RegisterOffset == 0x10 && keys[i].Mask == (1U << 7);
        } else if (keys[i].Action == QCOM_KEY_ACTION_DOWN) {
            down = QCOM_KEY_SPMI_TOKEN_ADDRESS(token) == 0x1300 && keys[i].Mask == (1U << 6);
        } else if (keys[i].Action == QCOM_KEY_ACTION_UP) {
            up = QCOM_KEY_SPMI_TOKEN_SID(token) == 1 &&
                 QCOM_KEY_SPMI_TOKEN_ADDRESS(token) == 0x8d10 && keys[i].ActiveLow;
        }
    }
    if (!(count == 3 && power && up && down)) fprintf(stderr, "keys %zu %d %d %d\n", count, power, up, down);
    assert(count == 3 && power && up && down);

    /* pm8550_gpios 6 through the GPIO API (vol+, active low). */
    int keys_node = fake_path(f, "/gpio-keys/key-volume-up");
    assert(!kshim_gpio_from_dt(f, keys_node, "gpios", 0, &g));
    assert(g.chip == KSHIM_GPIO_CHIP_PMIC && g.pin == 6 && g.sid == 1 && g.base == 0x8d00 && g.active_low);
    assert(kshim_gpio_tlmm_pin(&g, &intid) == ERROR_NOT_SUPPORTED);

    /* QUP SE4 window shared by i2c4/spi4; GCC_QUPV3_WRAP1_S4_CLK; pinctrl
     * functions qup1_se4 on gpio48/49 (pinctrl-sm8650 group facts). */
    int se4 = fake_path(f, se4_path);
    struct dt_range se;
    assert(dt_available(f, se4));
    assert(!dt_reg(f, se4, 0, &se) && se.base == 0xa90000 && se.size == 0x4000);
    assert(!kshim_irq_from_fdt(f, se4, 0, &intid, NULL) && intid == 32 + 357);
    clk_is(f, se4, "se", 0x100000 + 0x52008, 1U << 26, 0x100000 + 0x183bc, 0x100000 + 0x183c8);
    const fdt32_t *pins = fdt_getprop(f, se4, "pinctrl-0", &len);
    assert(pins && len >= 4);
    int se4_state = dt_phandle(f, fdt32_to_cpu(pins[0]));
    assert(!strcmp(dt_string(f, se4_state, "function"), "qup1_se4"));
    int wrapper = fake_path(f, "/soc@0/geniqup@ac0000");
    clk_is(f, wrapper, "m-ahb", 0x100000 + 0x52008, 1U << 20, 0x100000 + 0x2313c, 0);
    clk_is(f, wrapper, "s-ahb", 0x100000 + 0x52008, 1U << 21, 0x100000 + 0x23140, 0);
    uint32_t mux;
    assert(kshim_soc_pin_function(KSHIM_SOC_SM8650, 48, "qup1_se4", &mux) && mux == 1);
    assert(kshim_soc_pin_function(KSHIM_SOC_SM8650, 49, "qup1_se4", &mux) && mux == 1);
    assert(kshim_soc_pin_function(KSHIM_SOC_SM8650, 98, "pcie1_clk_req_n", &mux) && mux == 1);
    assert(kshim_soc_pin_function(KSHIM_SOC_SM8650, 161, "gpio", &mux) && mux == 0);
    assert(!kshim_soc_pin_function(KSHIM_SOC_SM8650, 161, "qup1_se4", &mux));
    struct kshim_clk *pd;
    assert(kshim_clk_get_power_domain(f, se4, 0, &pd) == ERROR_NOT_SUPPORTED); /* rpmhpd CX */

    /* UFS (firmware loader source): HCI and the mainline ufs_phy_gdsc
     * GDSCR whose PWR_ON lets storage inherit the live controller. */
    static struct kshim_storage_dt storage;
    assert(!kshim_dt_storage(f, &storage) && storage.ufs_count == 1);
    assert(storage.ufs[0].hci.base == 0x1d84000 && storage.ufs[0].gdsc == 0x100000 + 0x77004);
    assert(storage.has_rpmh && storage.rpmh.rsc_base == 0x17a20000 && storage.rpmh.cmd_db_base == 0x81c60000);

    free(r);
}

/* pcie1 (enabled on the Pocket S2 only; disabled on the public QRD). */
static void pcie1_checks(void *f)
{
    int pcie1 = fake_path(f, "/soc@0/pcie@1c08000");
    struct dt_range reg;
    assert(!dt_reg_named(f, pcie1, "parf", &reg) && reg.base == 0x1c08000);
    assert(!dt_reg_named(f, pcie1, "dbi", &reg) && reg.base == 0x40000000);
    assert(!dt_reg_named(f, pcie1, "config", &reg) && reg.base == 0x40100000 && reg.size == 0x100000);
    assert(!dt_reg_named(f, pcie1, "atu", &reg) && reg.base == 0x40001000);
    clk_is(f, pcie1, "aux", 0x100000 + 0x52000, 1U << 29, 0x100000 + 0x8d038, 0x100000 + 0x8d07c);
    clk_is(f, pcie1, "cfg", 0x100000 + 0x52000, 1U << 28, 0x100000 + 0x8d034, 0);
    clk_is(f, pcie1, "bus_master", 0x100000 + 0x52000, 1U << 27, 0x100000 + 0x8d028, 0);
    clk_is(f, pcie1, "bus_slave", 0x100000 + 0x52000, 1U << 26, 0x100000 + 0x8d01c, 0);
    clk_is(f, pcie1, "slave_q2a", 0x100000 + 0x52000, 1U << 25, 0x100000 + 0x8d018, 0);
    struct kshim_clk *gdsc, *rst, *byname;
    assert(!kshim_clk_get_power_domain(f, pcie1, 0, &gdsc));
    const struct kshim_clk_desc *d = kshim_clk_desc(gdsc);
    assert(d->kind == KSHIM_CLK_GDSC && d->enable_reg == 0x100000 + 0x5214c && d->enable_mask == 2 &&
           d->halt_reg == 0x100000 + 0x8d004);
    assert(!kshim_clk_get_reset(f, pcie1, "pci", &rst));
    d = kshim_clk_desc(rst);
    assert(d->kind == KSHIM_CLK_RESET && d->enable_reg == 0x100000 + 0x8d000 && d->enable_mask == 1);
    assert(!kshim_clk_get_reset(f, pcie1, "link_down", &rst) &&
           kshim_clk_desc(rst)->enable_reg == 0x100000 + 0x8e014);
    /* CrDK PcieTargetClock-style lookups by name, shared handles. */
    struct kshim_clk *aux;
    assert(!kshim_clk_get(f, pcie1, "aux", &aux));
    assert(!kshim_clk_get_by_name("gcc", "gcc_pcie_1_aux_clk", &byname) && byname == aux);
    assert(!kshim_clk_get_by_name("gcc", "gcc_pcie_1_pipe_clk", &byname));
    assert(!kshim_clk_get_by_name("gcc", "gcc_pcie_1_phy_bcr", &byname) &&
           kshim_clk_desc(byname)->enable_reg == 0x100000 + 0x8e01c);
    assert(!kshim_clk_get_by_name("tcsr", "tcsr_pcie_1_clkref_en", &byname) &&
           kshim_clk_desc(byname)->enable_reg == 0x1fc0000 + 0x31114);
    assert(kshim_clk_get_by_name("gcc", "no_such_clk", &byname) == ERROR_NOT_SUPPORTED);
    struct kshim_clk *ddr;
    assert(!kshim_clk_get(f, pcie1, "ddrss_sf_tbu", &ddr));
    assert(kshim_clk_get(f, pcie1, "nonexistent", &ddr) == ERROR_NOT_FOUND);
    gpio_is(f, pcie1, "perst-gpios", 97, true);
    gpio_is(f, pcie1, "wake-gpios", 99, false);
    int phy = fake_path(f, "/soc@0/phy@1c0e000");
    struct kshim_regulator *vreg;
    assert(!kshim_regulator_get(f, phy, "vdda-phy", &vreg));
    assert(!strcmp(kshim_regulator_name(vreg), "vreg_l3e_0p9"));
}

static void map_hooks(void *f, bool rsc)
{
    struct kshim_resources *r = calloc(1, sizeof(*r));
    assert(r);
    r->fdt = f;
    assert(!kshim_gpio_map_resources(r));
    assert(fake_mapped(r, 0xf100000, 0x300000, KSHIM_MMU_DEVICE));
    assert(!kshim_clk_map_resources(r));
    assert(fake_mapped(r, 0x100000, 0x1f4200, KSHIM_MMU_DEVICE));
    assert(fake_mapped(r, 0x1fc0000, 0xa0000, KSHIM_MMU_DEVICE));
    assert(!kshim_regulator_map_resources(r));
    if (rsc) {
        assert(fake_mapped(r, 0x17a20000, 0x10000, KSHIM_MMU_DEVICE)); /* drv-2 */
        assert(fake_mapped(r, 0x81c60000, 0x20000, KSHIM_MMU_NORMAL_NC));
    }
    free(r);
}

static void pocket_s2(void)
{
    void *f = fake_fixture("io/sm8650-ayaneo-pocket-s2.dtb");
    kshim_platform_set_io(&FakeIo);
    assert(!kshim_gpio_init(f) && !kshim_clk_init(f) && !kshim_regulator_init(f));
    soc_common(f, "/soc@0/geniqup@ac0000/i2c@a90000");
    pcie1_checks(f);

    /* uPD720201 endpoint under pcie1_port0: three regulator-fixed rails,
     * TLMM 123/122/121 active-high, vin vph_pwr; pinctrl gamepad_pwr_en. */
    int ep = fake_path(f, "/soc@0/pcie@1c08000/pcie@0/usb-controller@0");
    static const struct { const char *supply, *name; uint32_t pin; } rails[] = {
        {"avdd33", "upd720201_avdd33", 123}, {"vdd10", "upd720201_vdd10", 122}, {"vdd33", "upd720201_vdd33", 121}};
    for (unsigned i = 0; i < 3; ++i) {
        struct kshim_regulator *r;
        assert(!kshim_regulator_get(f, ep, rails[i].supply, &r));
        assert(!strcmp(kshim_regulator_name(r), rails[i].name));
        int node = fdt_node_offset_by_prop_value(f, -1, "regulator-name", rails[i].name,
                                                 (int)strlen(rails[i].name) + 1);
        assert(node >= 0);
        gpio_is(f, node, "gpios", rails[i].pin, false);
        struct kshim_regulator *again;
        assert(!kshim_regulator_get_node(f, node, &again) && again == r);
    }
    int len;
    const fdt32_t *pc = fdt_getprop(f, ep, "pinctrl-0", &len);
    assert(pc && len == 4);
    int state = dt_phandle(f, fdt32_to_cpu(*pc));
    assert(!strcmp(fdt_get_name(f, state, NULL), "gamepad-pwr-en-active-state"));
    assert(fdt_getprop(f, state, "output-high", NULL));
    struct kshim_regulator *none;
    assert(kshim_regulator_get(f, ep, "vddpe-3v3", &none) == ERROR_NOT_FOUND);

    /* PHY rails (cold init only): l3e, l3i, l1i. */
    int phy = fake_path(f, "/soc@0/phy@1c0e000");
    rpmh_is(f, phy, "vdda-phy", "vreg_l3e_0p9", "ldoe3");
    /* The upstream label is vreg_l3i_1p2 (1.2 V) but regulator-name says 0p91. */
    rpmh_is(f, phy, "vdda-pll", "vreg_l3i_0p91", "ldoi3");
    rpmh_is(f, phy, "vdda-qref", "vreg_l1i_0p88", "ldoi1");

    /* RFC touch: i2c4 @ 400 kHz, GT911 @ 0x5d, IRQ TLMM 162 falling, reset
     * TLMM 161 active-high (as written), VDDIO/AVDD28 = l14b. */
    int i2c4 = fake_path(f, "/soc@0/geniqup@ac0000/i2c@a90000");
    uint32_t hz;
    assert(dt_available(f, i2c4) && !dt_u32(f, i2c4, "clock-frequency", &hz) && hz == 400000);
    int ts = fake_path(f, "/soc@0/geniqup@ac0000/i2c@a90000/touchscreen@5d");
    struct kshim_dt_irq irq;
    assert(!kshim_dt_irq(f, ts, 0, &irq));
    assert(kshim_dt_irq_kind(f, irq.parent) == KSHIM_DT_IRQ_TLMM && irq.cells[0] == 162 &&
           irq.cells[1] == KSHIM_IRQ_TYPE_EDGE_FALLING);
    gpio_is(f, ts, "reset-gpios", 161, false);
    rpmh_is(f, ts, "VDDIO", "vreg_l14b_3p2", "ldob14");
    rpmh_is(f, ts, "AVDD28", "vreg_l14b_3p2", "ldob14");
    struct kshim_regulator *a, *b;
    assert(!kshim_regulator_get(f, ts, "VDDIO", &a) && !kshim_regulator_get(f, ts, "AVDD28", &b) && a == b);
    assert(kshim_regulator_get(f, ts, "vddio", &a) == ERROR_NOT_FOUND); /* case-sensitive */
    struct kshim_gpio irq_gpio;
    assert(!kshim_gpio_from_tlmm_pin(f, irq.parent, irq.cells[0], false, &irq_gpio) && irq_gpio.pin == 162);
    map_hooks(f, true);
    free(f);
}

static void qrd(void)
{
    void *f = fake_fixture("io/sm8650-qrd.dtb");
    assert(!kshim_gpio_init(f) && !kshim_clk_init(f) && !kshim_regulator_init(f));
    soc_common(f, "/soc@0/geniqup@ac0000/spi@a90000");
    assert(!dt_available(f, fake_path(f, "/soc@0/pcie@1c08000")));
    /* GT9916 on spi4 CS0, 1 MHz, IRQ TLMM 162 level-low, reset 161 active-low. */
    int ts = fake_path(f, "/soc@0/geniqup@ac0000/spi@a90000/touchscreen@0");
    uint32_t cs, hz;
    assert(!dt_u32(f, ts, "reg", &cs) && cs == 0 && !dt_u32(f, ts, "spi-max-frequency", &hz) && hz == 1000000);
    struct kshim_dt_irq irq;
    assert(!kshim_dt_irq(f, ts, 0, &irq) && irq.cells[0] == 162 && irq.cells[1] == KSHIM_IRQ_TYPE_LEVEL_LOW);
    gpio_is(f, ts, "reset-gpios", 161, true);
    rpmh_is(f, ts, "avdd", "vreg_l14b_3p2", "ldob14");
    map_hooks(f, true);
    free(f);
}

/* One board per process: the drivers bind one runtime DT for good. */
int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "qrd")) qrd();
    else pocket_s2();
    puts("platform_dt_test: ok");
    return 0;
}
