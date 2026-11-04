/* SPDX-License-Identifier: MIT */
/*
 * Devicetree facts the Goodix drivers and the touch framework resolve on
 * the real boards:
 *  - QRD8650: mainline sm8650-qrd.dtb, goodix,gt9916 on &spi4;
 *  - AYANEO Pocket S2: mainline sm8650-ayaneo-pocket-s2.dtb has no touch
 *    node; the RFC node is applied as an overlay (pocket-s2-touch.dtso)
 *    with libfdt and then resolved like any other node.
 * Checked: one node per compatible on its QUP serial engine (0xa90000, the
 * window i2c4 and spi4 share), the bus parameters, the TLMM interrupt (pin
 * and trigger through kshim_dt_irq), the reset GPIO and its flags, every
 * supply the driver tables name (case-sensitive "AVDD28"/"VDDIO"), the
 * RPMh rail behind them, the touchscreen size, the pinctrl states, and the
 * drivers' match hooks. Fixture provenance: tests/fixtures/io/goodix/README.md.
 */
#include "touch_goodix_fake.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dt.h>
#include <libfdt.h>
#include <qcom_irq.h>

#ifndef GOODIX_FIXTURES
#error "GOODIX_FIXTURES must name tests/fixtures/io/goodix"
#endif
#ifndef GOODIX_GENERATED
#error "GOODIX_GENERATED must name the generated overlay directory"
#endif

static void *read_blob(const char *path, size_t *size)
{
    FILE *file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "cannot open %s\n", path);
        exit(1);
    }
    assert(fseek(file, 0, SEEK_END) == 0);
    long length = ftell(file);
    assert(length > 0);
    rewind(file);
    void *data = malloc((size_t)length);
    assert(data && fread(data, 1, (size_t)length, file) == (size_t)length);
    fclose(file);
    *size = (size_t)length;
    return data;
}

/* A writable copy with room for an overlay. */
static void *load_dtb(const char *name, size_t *capacity)
{
    char path[512];
    size_t size;
    snprintf(path, sizeof(path), "%s/%s", GOODIX_FIXTURES, name);
    void *blob = read_blob(path, &size);
    *capacity = size + 65536;
    void *copy = malloc(*capacity);
    assert(copy && fdt_open_into(blob, copy, (int)*capacity) == 0);
    free(blob);
    return copy;
}

static int only_node(const void *fdt, const char *compatible)
{
    int found = -1, count = 0;
    for (int n = fdt_node_offset_by_compatible(fdt, -1, compatible); n >= 0;
         n = fdt_node_offset_by_compatible(fdt, n, compatible)) {
        found = n;
        ++count;
    }
    assert(count <= 1);
    return found;
}

static uint32_t u32(const void *fdt, int node, const char *property)
{
    uint32_t value;
    if (dt_u32(fdt, node, property, &value)) {
        fprintf(stderr, "missing %s\n", property);
        assert(0);
    }
    return value;
}

static bool has(const void *fdt, int node, const char *property)
{
    return fdt_getprop(fdt, node, property, NULL) != NULL;
}

/* The QUP serial engine window the node sits on. */
static void check_bus_parent(const void *fdt, int node, const char *compatible)
{
    int parent = fdt_parent_offset(fdt, node);
    uint64_t base, size;
    assert(parent >= 0);
    assert(!fdt_node_check_compatible(fdt, parent, compatible));
    assert(dt_available(fdt, parent));
    assert(dt_raw_reg(fdt, parent, 0, &base, &size) == 0);
    assert(base == 0xa90000 && size == 0x4000);
    assert(!fdt_node_check_compatible(fdt, fdt_parent_offset(fdt, parent), "qcom,geni-se-qup"));
}

static void check_irq(const void *fdt, int node, uint32_t pin, uint32_t trigger)
{
    struct kshim_dt_irq irq;
    assert(kshim_dt_irq(fdt, node, 0, &irq) == 0);
    assert(kshim_dt_irq_kind(fdt, irq.parent) == KSHIM_DT_IRQ_TLMM);
    assert(!fdt_node_check_compatible(fdt, irq.parent, "qcom,sm8650-tlmm"));
    assert(irq.count == 2 && irq.cells[0] == pin && kshim_dt_irq_trigger(irq.cells[1]) == trigger);
    /* No irq-gpios: the framework takes the IRQ GPIO from the TLMM pin. */
    assert(!has(fdt, node, "irq-gpios"));
}

static void check_reset(const void *fdt, int node, uint32_t pin, uint32_t flags)
{
    struct dt_spec spec;
    assert(dt_specifier(fdt, node, "reset-gpios", "#gpio-cells", 0, &spec) == 0);
    assert(!fdt_node_check_compatible(fdt, spec.node, "qcom,sm8650-tlmm"));
    assert(spec.count == 2 && spec.args[0] == pin && spec.args[1] == flags);
}

/* vreg_l14b_3p2: PM8550 LDO14 over RPMh ("ldob14"), fixed 3.2 V. */
static void check_l14b(const void *fdt, int rail)
{
    char name[32];
    int length;
    assert(rail >= 0);
    const char *node_name = fdt_get_name(fdt, rail, NULL);
    assert(node_name && !strcmp(node_name, "ldo14"));
    assert(!strcmp(dt_string(fdt, rail, "regulator-name"), "vreg_l14b_3p2"));
    assert(u32(fdt, rail, "regulator-min-microvolt") == 3200000);
    assert(u32(fdt, rail, "regulator-max-microvolt") == 3200000);
    int pmic = fdt_parent_offset(fdt, rail);
    assert(!fdt_node_check_compatible(fdt, pmic, "qcom,pm8550-rpmh-regulators"));
    const char *id = fdt_getprop(fdt, pmic, "qcom,pmic-id", &length);
    assert(id && !strcmp(id, "b"));
    snprintf(name, sizeof(name), "ldo%s%s", id, node_name + 3);
    assert(!strcmp(name, "ldob14"));
}

/* Resolve the driver's supply table the way the framework does
 * ("<name>-supply", case-sensitive, missing ones skipped). */
static unsigned resolve_supplies(const void *fdt, int node, const struct kshim_touch_driver *driver,
                                 int *rails)
{
    unsigned count = 0;
    for (const char *const *s = driver->supplies; *s; ++s) {
        char property[64];
        snprintf(property, sizeof(property), "%s-supply", *s);
        if (!has(fdt, node, property))
            continue;
        rails[count++] = dt_reference(fdt, node, property, 0);
    }
    return count;
}

struct pin_state {
    const char *pins;
    uint32_t drive;
    const char *bias;
};

static void check_pinctrl(const void *fdt, int node, const struct pin_state *expect, unsigned count)
{
    int length;
    const char *names = fdt_getprop(fdt, node, "pinctrl-names", &length);
    assert(names && !strcmp(names, "default"));
    const fdt32_t *states = fdt_getprop(fdt, node, "pinctrl-0", &length);
    assert(states && (unsigned)length == count * 4U);
    for (unsigned i = 0; i < count; ++i) {
        int state = dt_phandle(fdt, fdt32_to_cpu(states[i]));
        assert(state >= 0);
        assert(!fdt_node_check_compatible(fdt, fdt_parent_offset(fdt, state), "qcom,sm8650-tlmm"));
        assert(!strcmp(dt_string(fdt, state, "pins"), expect[i].pins));
        assert(!strcmp(dt_string(fdt, state, "function"), "gpio"));
        assert(u32(fdt, state, "drive-strength") == expect[i].drive);
        assert(has(fdt, state, expect[i].bias));
    }
}

static void test_qrd8650(void)
{
    size_t capacity;
    void *fdt = load_dtb("sm8650-qrd.dtb", &capacity);
    assert(dt_validate(fdt, capacity) == 0);
    const struct kshim_touch_driver *drv = &kshim_touch_driver_gt9916;

    int node = only_node(fdt, drv->compatibles[0]);
    assert(node >= 0 && dt_available(fdt, node));
    assert(only_node(fdt, "goodix,gt911") < 0);
    check_bus_parent(fdt, node, "qcom,geni-spi");
    /* i2c4 shares the window and stays disabled on the QRD. */
    int i2c4 = fdt_path_offset(fdt, "/soc@0/geniqup@ac0000/i2c@a90000");
    assert(i2c4 >= 0 && !dt_available(fdt, i2c4));
    assert(drv->bus_mask == KSHIM_TOUCH_BUS_SPI);

    /* SPI device: CS 0, 1 MHz, mode 0 (no spi-cpha/cpol). */
    assert(u32(fdt, node, "reg") == 0);
    assert(u32(fdt, node, "spi-max-frequency") == 1000000);
    assert(!has(fdt, node, "spi-cpha") && !has(fdt, node, "spi-cpol") &&
           !has(fdt, node, "spi-cs-high"));

    check_irq(fdt, node, 162, KSHIM_IRQ_TYPE_LEVEL_LOW);
    check_reset(fdt, node, 161, KSHIM_GPIO_DT_ACTIVE_LOW);

    int rails[KSHIM_TOUCH_MAX_SUPPLIES];
    assert(!has(fdt, node, "vddio-supply"));
    assert(resolve_supplies(fdt, node, drv, rails) == 1);
    check_l14b(fdt, rails[0]);

    assert(u32(fdt, node, "touchscreen-size-x") == 1080);
    assert(u32(fdt, node, "touchscreen-size-y") == 2400);
    assert(!has(fdt, node, "touchscreen-swapped-x-y"));

    /* Mainline quirk: the state labelled ts_irq configures GPIO161 (the
     * reset line) and ts_reset GPIO162 (the interrupt). Both lines end up
     * pulled up at 8 mA either way. */
    const struct pin_state states[] = {
        { "gpio161", 8, "bias-pull-up" },
        { "gpio162", 8, "bias-pull-up" },
    };
    check_pinctrl(fdt, node, states, 2);
    int ts_irq = dt_phandle(fdt, fdt32_to_cpu(*(const fdt32_t *)fdt_getprop(fdt, node, "pinctrl-0", NULL)));
    assert(!strcmp(fdt_get_name(fdt, ts_irq, NULL), "ts-irq-state"));
    assert(has(fdt, ts_irq, "output-disable"));

    /* GT9916 has no match hook: every available node is accepted. */
    assert(drv->match == NULL);
    free(fdt);
}

static void *pocket_s2_with_touch(size_t *capacity)
{
    void *fdt = load_dtb("sm8650-ayaneo-pocket-s2.dtb", capacity);
    assert(dt_validate(fdt, *capacity) == 0);
    /* Mainline has no touch node yet. */
    assert(only_node(fdt, "goodix,gt911") < 0 && only_node(fdt, "goodix,gt9916") < 0);
    int i2c4 = fdt_path_offset(fdt, "/soc@0/geniqup@ac0000/i2c@a90000");
    assert(i2c4 >= 0 && !dt_available(fdt, i2c4));

    char path[512];
    size_t size;
    snprintf(path, sizeof(path), "%s/pocket-s2-touch.dtbo", GOODIX_GENERATED);
    void *overlay = read_blob(path, &size);
    int status = fdt_overlay_apply(fdt, overlay);
    if (status) {
        fprintf(stderr, "fdt_overlay_apply: %d\n", status);
        assert(0);
    }
    free(overlay);
    assert(fdt_pack(fdt) == 0);
    *capacity = fdt_totalsize(fdt);
    assert(dt_validate(fdt, *capacity) == 0);
    return fdt;
}

static void test_pocket_s2(void)
{
    size_t capacity;
    void *fdt = pocket_s2_with_touch(&capacity);
    const struct kshim_touch_driver *drv = &kshim_touch_driver_gt911;

    int node = only_node(fdt, drv->compatibles[0]);
    assert(node >= 0 && dt_available(fdt, node));
    assert(only_node(fdt, "goodix,gt9916") < 0);
    check_bus_parent(fdt, node, "qcom,geni-i2c");
    int parent = fdt_parent_offset(fdt, node);
    assert(u32(fdt, parent, "clock-frequency") == 400000);
    /* spi4 shares the window and stays disabled on the Pocket S2. */
    int spi4 = fdt_path_offset(fdt, "/soc@0/geniqup@ac0000/spi@a90000");
    assert(spi4 >= 0 && !dt_available(fdt, spi4));
    assert(drv->bus_mask == KSHIM_TOUCH_BUS_I2C);

    /* 0x5d: INT low at the reset release (GT911 address select). */
    assert(u32(fdt, node, "reg") == 0x5d);
    struct kshim_touch_dev dev;
    uint8_t priv[KSHIM_TOUCH_PRIV_MAX];
    fake_dev_init(&dev, drv, KSHIM_TOUCH_BUS_I2C, priv);
    dev.fdt = fdt;
    dev.node = node;
    assert(drv->match(&dev, fdt) == 0);

    check_irq(fdt, node, 162, KSHIM_IRQ_TYPE_EDGE_FALLING);
    /* GPIO_ACTIVE_HIGH: logical value == RSTB level (goodix,gt9xx convention). */
    check_reset(fdt, node, 161, 0);

    int rails[KSHIM_TOUCH_MAX_SUPPLIES];
    assert(!has(fdt, node, "vddio-supply") && !has(fdt, node, "avdd28-supply"));
    assert(resolve_supplies(fdt, node, drv, rails) == 2);
    assert(rails[0] == rails[1]);
    check_l14b(fdt, rails[0]);

    assert(u32(fdt, node, "touchscreen-size-x") == 1440);
    assert(u32(fdt, node, "touchscreen-size-y") == 2560);

    const struct pin_state states[] = {
        { "gpio161", 8, "bias-pull-down" }, /* ts_reset_default: holds RSTB low */
        { "gpio162", 8, "bias-disable" },   /* ts_irq_default: the GT911 drives INT */
    };
    check_pinctrl(fdt, node, states, 2);

    /* match: reg outside the binding's {0x5d, 0x14} is refused. */
    assert(fdt_setprop_inplace_u32(fdt, node, "reg", 0x14) == 0);
    assert(drv->match(&dev, fdt) == 0);
    assert(fdt_setprop_inplace_u32(fdt, node, "reg", 0x20) == 0);
    assert(drv->match(&dev, fdt) == ERROR_NOT_SUPPORTED);
    assert(fdt_delprop(fdt, node, "reg") == 0);
    assert(drv->match(&dev, fdt) == ERROR_INVALID_PARAMETER);
    free(fdt);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    test_qrd8650();
    test_pocket_s2();
    printf("touch_goodix_dt_test: PASS\n");
    return 0;
}
