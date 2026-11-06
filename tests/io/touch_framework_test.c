/* SPDX-License-Identifier: MIT */
/*
 * Touch framework (kernel/drivers/input/touch/core.c) over fake GPIO,
 * regulator, pinctrl, I2C/SPI and TLMM interrupt APIs and a synthetic DT
 * (tests/fixtures/io/touch-framework.dts): driver/node selection, match
 * rejection, bus kinds, DT resource resolution, the power sequence order
 * (common and overridden), the deferred IRQ flow (handler masks, poll
 * handles, unmasks), the 100 ms stuck-level and sealed-cascade fallbacks,
 * late interrupt start, swap/invert/scale, handle errors, failed candidates
 * and the idempotent quiesce with the reset fallback.
 */
#include "touch_fakes.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dt.h>

/* ---- Fake drivers ---------------------------------------------------------- */

struct report { uint8_t id; kshim_touch_contact_action_t action; uint32_t x, y; };
static struct {
    uint32_t reject[8];
    unsigned reject_count;
    bool spi_enabled, use_power_on;
    int probe_status, handle_status, quiesce_status;
    uint32_t max_x, max_y;
    unsigned probes, handles, quiesces, power_ons;
    struct report queue[8];
    unsigned queued;
    uint32_t release_pin;       /* IRQ GPIO deasserted by a handle */
} S;

static int drv_match(struct kshim_touch_dev *dev, const void *fdt)
{
    uint32_t reg = 0;
    (void)dt_u32(fdt, dev->node, "reg", &reg);
    for (unsigned i = 0; i < S.reject_count; ++i)
        if (S.reject[i] == reg) return ERROR_NOT_FOUND;
    return 0;
}

static int drv_spi_match(struct kshim_touch_dev *dev, const void *fdt)
{
    (void)fdt;
    assert(dev->bus == KSHIM_TOUCH_BUS_SPI);   /* single-bus driver: known before probe */
    return S.spi_enabled ? 0 : ERROR_NOT_FOUND;
}

static int drv_power_on(struct kshim_touch_dev *dev)
{
    ++S.power_ons;
    fake_log("override");
    int status = kshim_touch_set_reset(dev, true);
    if (!status) status = kshim_touch_power_supplies(dev, true);
    if (!status) status = kshim_touch_irq_gpio_output(dev, true);
    if (!status) status = kshim_touch_set_reset(dev, false);
    return status;
}

static int drv_probe(struct kshim_touch_dev *dev)
{
    ++S.probes;
    fake_log("probe:%s", dev->driver->name);
    assert(dev->tracker.initialized && dev->powered);
    /* The DT size is the default range probe starts from. */
    assert(dev->max_x == dev->props.size_x && dev->max_y == dev->props.size_y);
    if (S.probe_status) return S.probe_status;
    dev->max_x = S.max_x;
    dev->max_y = S.max_y;
    return 0;
}

static int drv_handle(struct kshim_touch_dev *dev)
{
    ++S.handles;
    fake_log("handle");
    if (S.handle_status) return S.handle_status;
    for (unsigned i = 0; i < S.queued; ++i)
        assert(!kshim_touch_report(dev, S.queue[i].id, S.queue[i].action, S.queue[i].x, S.queue[i].y));
    S.queued = 0;
    if (S.release_pin) fake_pin_input(S.release_pin, true);   /* level line deasserts */
    return 0;
}

static int drv_quiesce(struct kshim_touch_dev *dev)
{
    (void)dev;
    ++S.quiesces;
    fake_log("quiesce");
    return S.quiesce_status;
}

static const char *const Compatibles[] = {"test,touch", NULL};
static const char *const SpiCompatibles[] = {"test,spi-touch", NULL};
static const char *const Supplies[] = {"avdd", "vdd", NULL};

static struct kshim_touch_driver DrvA = {
    .name = "a", .compatibles = Compatibles, .bus_mask = KSHIM_TOUCH_BUS_I2C,
    .supplies = Supplies, .power_on_delay_us = 1000, .post_reset_delay_us = 2000,
    .priv_size = 64, .match = drv_match, .probe = drv_probe, .handle = drv_handle,
    .quiesce = drv_quiesce,
};
static const struct kshim_touch_driver DrvSpi = {
    .name = "spi", .compatibles = SpiCompatibles, .bus_mask = KSHIM_TOUCH_BUS_SPI,
    .priv_size = 16, .match = drv_spi_match, .probe = drv_probe, .handle = drv_handle,
    .quiesce = drv_quiesce,
};
const struct kshim_touch_driver *const kshim_touch_drivers[] = {&DrvA, &DrvSpi, NULL};

/* ---- Harness ------------------------------------------------------------- */

static kshim_touch_event_t Events[64];
static unsigned EventCount;
static void emit(void *context, const kshim_touch_event_t *event)
{
    assert(context == &EventCount);
    assert(EventCount < 64);
    Events[EventCount++] = *event;
}

static void *Blob;
static size_t BlobSize;
static void *fresh_dt(void)
{
    static void *copy;
    free(copy);
    copy = malloc(BlobSize + 4096);
    assert(copy && !fdt_open_into(Blob, copy, (int)BlobSize + 4096));
    return copy;
}

static void load_fixture(void)
{
    FILE *file = fopen(GENERATED_FIXTURES "/touch-framework.dtb", "rb");
    assert(file);
    assert(!fseek(file, 0, SEEK_END));
    long size = ftell(file);
    assert(size > 0 && !fseek(file, 0, SEEK_SET));
    Blob = malloc((size_t)size);
    assert(Blob && fread(Blob, 1, (size_t)size, file) == (size_t)size);
    fclose(file);
    BlobSize = (size_t)size;
    assert(!dt_validate(Blob, BlobSize));
}

static void reset(void)
{
    assert(!kshim_touch_quiesce_all());   /* previous case fully stopped */
    fake_touch_reset();
    memset(&S, 0, sizeof(S));
    S.max_x = 4000; S.max_y = 8000;
    DrvA.power_on = NULL;
    DrvA.reset_convention = KSHIM_TOUCH_RESET_ASSERTED;
    EventCount = 0;
}

static void expect_event(unsigned index, kshim_touch_event_type_t type, uint8_t id, uint32_t x, uint32_t y)
{
    assert(index < EventCount);
    const kshim_touch_event_t *e = &Events[index];
    if (e->type != type || e->contact_id != id || e->x != x || e->y != y) {
        fprintf(stderr, "event %u: got %d id %u (%u,%u), want %d id %u (%u,%u)\n", index, e->type,
                e->contact_id, e->x, e->y, type, id, x, y);
        assert(0);
    }
}

static void poll_ok(void) { assert(!kshim_touch_framework_poll()); }

/* ---- Cases ---------------------------------------------------------------- */

/* touch@10: mainline style, IRQ-driven, full lifecycle. */
static void test_lifecycle(void)
{
    reset();
    const void *f = fresh_dt();
    S.release_pin = 10;
    assert(!kshim_touch_framework_start(f, 500, 1000, emit, &EventCount));
    const struct kshim_touch_dev *dev = kshim_touch_active();
    assert(dev && dev->driver == &DrvA && !strcmp(fdt_get_name(f, dev->node, NULL), "touch@10"));
    assert(dev->bus == KSHIM_TOUCH_BUS_I2C && dev->i2c.addr == 0x10);
    assert(dev->has_reset && dev->reset_gpio.pin == 11 && dev->reset_gpio.active_low);
    assert(dev->has_irq && dev->irq_pin == 10 && dev->irq_trigger == KSHIM_IRQ_TYPE_LEVEL_LOW);
    assert(dev->has_irq_gpio && dev->irq_gpio.pin == 10 && dev->irq_gpio.active_low);
    assert(dev->supply_count == 2 && dev->props.size_x == 1000 && dev->props.size_y == 2000);
    assert(dev->priv && dev->powered && dev->probed && dev->irq_live);
    /* Common sequence; pinctrl only once the rails are up. */
    static const char *const order[] = {"i2c-probe=i2c@a00000", "req11", "gpio11=0", "vreg_a=on",
        "vreg_b=on", "pinctrl=default", "req10", "gpio10=in", "delay=1000", "gpio11=1",
        "delay=2000", "probe:a", "irq-req10/8", NULL};
    assert(fake_log_order(order));
    assert(S.probes == 1 && FakeI2cProbes == 1);

    /* One pass for anything latched before the interrupt existed. */
    poll_ok();
    assert(S.handles == 1 && FakeTlmmIrq.unmasks == 1 && !FakeTlmmIrq.masked);
    poll_ok();
    assert(S.handles == 1);

    /* Deferred model: the handler masks and marks; the poll reads. */
    S.queue[S.queued++] = (struct report){0, KSHIM_TOUCH_CONTACT_ENTER, 100, 200};
    fake_pin_input(10, false);
    assert(fake_irq_fire());
    assert(FakeTlmmIrq.masked && dev->irq_pending && S.handles == 1 && !EventCount);
    assert(!fake_irq_fire());                       /* masked: nothing more */
    poll_ok();
    assert(S.handles == 2 && !FakeTlmmIrq.masked && !dev->irq_pending);
    /* 1000 x 2000 controller range onto 500 x 1000. */
    assert(EventCount == 1);
    expect_event(0, KSHIM_TOUCH_EVENT_PRESS, 0, 50, 100);

    /* MOVE of a contact that is not down counts as ENTER; the primary
     * passes to it when contact 0 lifts. */
    S.queue[S.queued++] = (struct report){1, KSHIM_TOUCH_CONTACT_MOVE, 999, 1999};
    S.queue[S.queued++] = (struct report){0, KSHIM_TOUCH_CONTACT_LEAVE, 0, 0};
    fake_pin_input(10, false);
    assert(fake_irq_fire());
    poll_ok();
    assert(EventCount == 3);
    expect_event(1, KSHIM_TOUCH_EVENT_RELEASE, 0, 50, 100);
    expect_event(2, KSHIM_TOUCH_EVENT_PRESS, 1, 499, 999);
    kshim_touch_report_release_all((struct kshim_touch_dev *)dev);
    assert(EventCount == 4);
    expect_event(3, KSHIM_TOUCH_EVENT_RELEASE, 1, 499, 999);

    /* Level held 100 ms with no delivery: the interrupt is given up. */
    unsigned handles = S.handles;
    S.release_pin = 0;
    fake_pin_input(10, false);
    for (unsigned ms = 0; ms < 99; ms += 10) {
        poll_ok();
        fake_advance_us(10000);
    }
    assert(dev->irq_live && S.handles == handles);
    fake_advance_us(10000);
    poll_ok();
    assert(!dev->irq_live && FakeTlmmIrq.frees == 1 && S.handles == handles + 1);
    poll_ok();                                       /* polled while the level is active */
    assert(S.handles == handles + 2);
    fake_pin_input(10, true);
    poll_ok();
    assert(S.handles == handles + 2);

    /* Quiesce: driver stop, reset held while the rails go down, pins back. */
    fake_log_clear();
    assert(!kshim_touch_quiesce_all());
    static const char *const stop[] = {"quiesce", "gpio11=0", "vreg_b=off", "vreg_a=off",
        "free10", "free11", NULL};
    assert(fake_log_order(stop));
    assert(!fake_log_has("irq-free"));               /* already freed by the fallback */
    assert(!kshim_touch_active() && !fake_regulator_named("vreg_a")->refs &&
           !fake_regulator_named("vreg_b")->refs && !fake_pin(10)->refs && !fake_pin(11)->refs);
    assert(!fake_pin(11)->output);                   /* reset back to its (input) snapshot */
    fake_log_clear();
    assert(!kshim_touch_quiesce_all() && !kshim_touch_framework_stop());
    assert(!FakeLog[0] && S.quiesces == 1);
    assert(!kshim_touch_framework_poll());
    puts("lifecycle: ok");
}

/* An interrupt taken between the poll's irq_pending read and its
 * stuck-line check: the handler's own mask must not read as a sealed line. */
static unsigned RaceFires;
static void fire_once(void)
{
    if (RaceFires) return;
    ++RaceFires;
    assert(fake_irq_fire());
}

static void test_irq_race(void)
{
    reset();
    const void *f = fresh_dt();
    S.release_pin = 10;
    assert(!kshim_touch_framework_start(f, 500, 1000, emit, &EventCount));
    const struct kshim_touch_dev *dev = kshim_touch_active();
    assert(dev && dev->irq_live);
    poll_ok();                                       /* the pass after the request */
    unsigned handles = S.handles;
    S.queue[S.queued++] = (struct report){0, KSHIM_TOUCH_CONTACT_ENTER, 100, 200};
    fake_pin_input(10, false);
    RaceFires = 0;
    FakeIrqLiveHook = fire_once;
    poll_ok();                                       /* irq_pending read false, then the IRQ */
    FakeIrqLiveHook = NULL;
    assert(RaceFires == 1 && dev->irq_live && FakeTlmmIrq.requested && !FakeTlmmIrq.frees);
    assert(dev->irq_pending && FakeTlmmIrq.masked && S.handles == handles);
    assert(!fake_log_has("irq-free"));
    poll_ok();                                       /* the deferred work runs next */
    assert(S.handles == handles + 1 && !FakeTlmmIrq.masked && dev->irq_live && EventCount == 1);
    puts("irq race: ok");
}

/* KSHIM_TOUCH_RESET_LEVEL: set_reset maps "held in reset" to logical 0;
 * stop never drives the reset level. Cold (reset was asserted): the pad
 * becomes an input before the rails drop. Inherited (never asserted): the
 * pad is left driven as it was until the restore. */
static int drv_level_power_on(struct kshim_touch_dev *dev)
{
    ++S.power_ons;
    if (S.use_power_on) {
        /* inherited running: just keep it out of reset */
        int status = kshim_touch_power_supplies(dev, true);
        return status ? status : kshim_touch_set_reset(dev, false);
    }
    int status = kshim_touch_set_reset(dev, true);
    if (!status) status = kshim_touch_power_supplies(dev, true);
    if (!status) status = kshim_touch_set_reset(dev, false);
    return status;
}

static void test_reset_level(void)
{
    for (unsigned inherited = 0; inherited < 2; ++inherited) {
        reset();
        const void *f = fresh_dt();
        DrvA.power_on = drv_level_power_on;
        DrvA.reset_convention = KSHIM_TOUCH_RESET_LEVEL;
        S.use_power_on = inherited;
        fake_pin_input(11, false);
        assert(!kshim_touch_framework_start(f, 500, 1000, emit, &EventCount));
        const struct kshim_touch_dev *dev = kshim_touch_active();
        /* touch@10's reset GPIO is active low: logical 1 (running) = pad low. */
        assert(dev && dev->reset_gpio.active_low);
        static const char *const cold_on[] = {"gpio11=1", "vreg_a=on", "gpio11=0", "probe:a", NULL};
        static const char *const warm_on[] = {"vreg_a=on", "gpio11=0", "probe:a", NULL};
        assert(fake_log_order(inherited ? warm_on : cold_on));
        assert(inherited || fake_log_count("gpio11=1") == 1);
        fake_log_clear();
        assert(!kshim_touch_quiesce_all());
        assert(!fake_log_has("gpio11=1"));           /* never held in reset at stop */
        if (inherited) {
            assert(!fake_log_has("gpio11=in"));
        } else {
            static const char *const stop[] = {"quiesce", "gpio11=in", "vreg_b=off", "vreg_a=off",
                "free11", NULL};
            assert(fake_log_order(stop));
        }
        DrvA.reset_convention = KSHIM_TOUCH_RESET_ASSERTED;
    }
    puts("reset level convention: ok");
}

/* touchscreen.yaml: invert in controller space, then swap. */
static void test_transform(void)
{
    reset();
    void *f = fresh_dt();
    int node = fdt_path_offset(f, "/i2c@a00000/touch@10");
    assert(node >= 0);
    assert(!fdt_setprop(f, node, "touchscreen-swapped-x-y", NULL, 0));
    assert(!fdt_setprop(f, node, "touchscreen-inverted-x", NULL, 0));
    assert(!kshim_touch_framework_start(f, 2000, 1000, emit, &EventCount));
    struct kshim_touch_dev *dev = (struct kshim_touch_dev *)kshim_touch_active();
    assert(dev->props.swap_xy && dev->props.invert_x && !dev->props.invert_y);
    assert(dev->tracker.config.source_width == 1000 && dev->tracker.config.source_height == 2000 &&
           dev->tracker.config.rotation == KSHIM_TOUCH_ROTATION_90);
    assert(!kshim_touch_report(dev, 2, KSHIM_TOUCH_CONTACT_ENTER, 100, 300));
    expect_event(0, KSHIM_TOUCH_EVENT_PRESS, 2, 300, 899);
    assert(!kshim_touch_report(dev, 2, KSHIM_TOUCH_CONTACT_MOVE, 0, 0));
    expect_event(1, KSHIM_TOUCH_EVENT_MOVE, 2, 0, 999);
    assert(kshim_touch_report(dev, KSHIM_TOUCH_MAX_CONTACTS, KSHIM_TOUCH_CONTACT_ENTER, 0, 0) ==
           ERROR_INVALID_PARAMETER);
    dev->max_contacts = 2;
    assert(kshim_touch_report(dev, 2, KSHIM_TOUCH_CONTACT_MOVE, 1, 1) == ERROR_INVALID_PARAMETER);
    /* A second start only rescales. */
    assert(!kshim_touch_framework_start(f, 200, 100, emit, &EventCount));
    assert(dev->tracker.config.output_width == 200 && S.probes == 1);
    /* No DT size: probe's range, swapped. */
    reset();
    f = fresh_dt();
    node = fdt_path_offset(f, "/i2c@a00000/touch@10");
    assert(!fdt_delprop(f, node, "touchscreen-size-x") && !fdt_delprop(f, node, "touchscreen-size-y"));
    assert(!fdt_setprop(f, node, "touchscreen-swapped-x-y", NULL, 0));
    S.max_x = 300; S.max_y = 700;
    assert(!kshim_touch_framework_start(f, 700, 300, emit, &EventCount));
    dev = (struct kshim_touch_dev *)kshim_touch_active();
    assert(dev->tracker.config.source_width == 300 && dev->tracker.config.source_height == 700);
    assert(!kshim_touch_report(dev, 0, KSHIM_TOUCH_CONTACT_ENTER, 10, 20));
    expect_event(EventCount - 1, KSHIM_TOUCH_EVENT_PRESS, 0, 20, 10);
    puts("transform: ok");
}

/* touch@20: Android names, IRQ GPIO only, late interrupt start. */
static void test_android_style(void)
{
    reset();
    S.reject[S.reject_count++] = 0x10;
    FakeIrqReady = false;
    const void *f = fresh_dt();
    assert(!kshim_touch_framework_start(f, 100, 100, emit, &EventCount));
    const struct kshim_touch_dev *dev = kshim_touch_active();
    assert(!strcmp(fdt_get_name(f, dev->node, NULL), "touch@20"));
    assert(!dev->has_reset && dev->has_irq_gpio && dev->has_irq && dev->irq_pin == 20 &&
           dev->irq_trigger == KSHIM_IRQ_TYPE_LEVEL_LOW && !dev->irq_live);
    assert(dev->supply_count == 1);                    /* vreg_x left as inherited */
    assert(fake_log_has("pinctrl=pmx_ts_active") && !fake_log_has("vreg_x"));
    assert(fake_log_order((const char *const[]){"vreg_b=on", "pinctrl=pmx_ts_active", "gpio20=in",
                                                "delay=1000", "probe:a", NULL}));
    /* Polled: handle only while the IRQ GPIO is at its active level. */
    S.release_pin = 20;
    poll_ok();
    assert(!S.handles);
    fake_pin_input(20, false);
    poll_ok();
    assert(S.handles == 1);
    poll_ok();
    assert(S.handles == 1);
    /* The GIC comes up later: the next poll requests the interrupt and
     * makes the initial pass at once. */
    FakeIrqReady = true;
    poll_ok();
    assert(dev->irq_live && FakeTlmmIrq.pin == 20 && S.handles == 2);
    poll_ok();
    assert(S.handles == 2);
    /* A cascade that sealed the pin falls back at once. */
    fake_pin_input(20, false);
    FakeTlmmIrq.sealed = true;
    poll_ok();
    assert(!dev->irq_live && S.handles == 3 && FakeTlmmIrq.frees == 1);
    puts("android style: ok");
}

/* touch@40: interrupt and IRQ GPIO disagree; SPI binding; selection misses. */
static void test_selection(void)
{
    reset();
    S.reject[S.reject_count++] = 0x10;
    S.reject[S.reject_count++] = 0x20;
    const void *f = fresh_dt();
    assert(!kshim_touch_framework_start(f, 100, 100, emit, &EventCount));
    const struct kshim_touch_dev *dev = kshim_touch_active();
    assert(!strcmp(fdt_get_name(f, dev->node, NULL), "touch@40"));
    assert(!dev->has_irq && dev->has_irq_gpio && dev->irq_gpio.pin == 41 && !FakeTlmmIrq.requests);

    /* Every I2C node rejected: the I2C-only driver skips the SPI node
     * without a probe, and the SPI driver binds it. */
    reset();
    S.reject[S.reject_count++] = 0x10;
    S.reject[S.reject_count++] = 0x20;
    S.reject[S.reject_count++] = 0x40;
    S.spi_enabled = true;
    f = fresh_dt();
    assert(!kshim_touch_framework_start(f, 100, 100, emit, &EventCount));
    dev = kshim_touch_active();
    assert(dev->driver == &DrvSpi && dev->bus == KSHIM_TOUCH_BUS_SPI && dev->spi.chip_select == 0);
    assert(dev->has_irq && dev->irq_pin == 50 && dev->irq_trigger == KSHIM_IRQ_TYPE_EDGE_FALLING &&
           dev->has_irq_gpio && dev->irq_gpio.active_low && !FakeI2cProbes);
    uint8_t byte = 0x5a;
    struct kshim_spi_xfer xfer = {.tx = &byte, .len = 1};
    assert(kshim_touch_spi_sync((struct kshim_touch_dev *)dev, &xfer, 1) == ERROR_DEVICE_ERROR);
    assert(kshim_touch_i2c_write_read((struct kshim_touch_dev *)dev, &byte, 1, NULL, 0) ==
           ERROR_INVALID_PARAMETER);

    /* Nothing matches: ERROR_NOT_FOUND and no device was touched. */
    reset();
    S.reject[S.reject_count++] = 0x10;
    S.reject[S.reject_count++] = 0x20;
    S.reject[S.reject_count++] = 0x40;
    S.reject[S.reject_count++] = 0x0;
    f = fresh_dt();
    assert(kshim_touch_framework_start(f, 100, 100, emit, &EventCount) == ERROR_NOT_FOUND);
    assert(!kshim_touch_active() && !FakeI2cProbes && !FakeLog[0]);
    puts("selection: ok");
}

/* Failed candidates release everything; the next one is tried. */
static void test_failures(void)
{
    reset();
    S.reject[S.reject_count++] = 0x20;
    S.reject[S.reject_count++] = 0x40;
    S.spi_enabled = true;
    FakeRegulatorFail = "vreg_b";
    const void *f = fresh_dt();
    assert(!kshim_touch_framework_start(f, 100, 100, emit, &EventCount));
    assert(kshim_touch_active()->driver == &DrvSpi);
    assert(fake_log_order((const char *const[]){"vreg_a=on", "vreg_b=fail", "vreg_a=off", "free11",
                                                "spi-probe", NULL}));
    assert(!fake_regulator_named("vreg_a")->refs && !fake_pin(11)->refs && !S.quiesces);

    /* Probe failure: bounded driver quiesce, then everything released. */
    reset();
    S.reject[S.reject_count++] = 0x20;
    S.reject[S.reject_count++] = 0x40;
    S.probe_status = KSHIM_IO_ERR_NO_DEVICE;
    f = fresh_dt();
    assert(kshim_touch_framework_start(f, 100, 100, emit, &EventCount) == KSHIM_IO_ERR_NO_DEVICE);
    assert(!kshim_touch_active() && S.quiesces == 1 && !fake_regulator_named("vreg_a")->refs &&
           !fake_regulator_named("vreg_b")->refs && !fake_pin(10)->refs && !fake_pin(11)->refs);

    /* The bus controller refuses (SE owned elsewhere). */
    reset();
    S.reject[S.reject_count++] = 0x20;
    S.reject[S.reject_count++] = 0x40;
    FakeI2cGetStatus = ERROR_BUSY;
    f = fresh_dt();
    assert(kshim_touch_framework_start(f, 100, 100, emit, &EventCount) == ERROR_BUSY);
    assert(!S.probes && !FakeLog[0]);

    /* A handle error disables touch until stop, which still quiesces. */
    reset();
    f = fresh_dt();
    assert(!kshim_touch_framework_start(f, 100, 100, emit, &EventCount));
    struct kshim_touch_dev *dev = (struct kshim_touch_dev *)kshim_touch_active();
    poll_ok();
    assert(!kshim_touch_report(dev, 0, KSHIM_TOUCH_CONTACT_ENTER, 10, 10));
    S.handle_status = KSHIM_IO_ERR_PROTOCOL;
    fake_pin_input(10, false);
    assert(fake_irq_fire());
    assert(kshim_touch_framework_poll() == KSHIM_IO_ERR_PROTOCOL);
    expect_event(EventCount - 1, KSHIM_TOUCH_EVENT_CANCEL, 0, 1, 0);
    assert(!dev->irq_live && FakeTlmmIrq.frees == 1);
    unsigned handles = S.handles;
    poll_ok();
    assert(S.handles == handles);
    assert(!kshim_touch_quiesce_all() && S.quiesces == 1);

    /* Driver stop fails: with a reset GPIO the controller is held in reset. */
    reset();
    f = fresh_dt();
    assert(!kshim_touch_framework_start(f, 100, 100, emit, &EventCount));
    S.quiesce_status = ERROR_TIMEOUT;
    fake_log_clear();
    assert(!kshim_touch_quiesce_all());
    assert(fake_log_order((const char *const[]){"irq-free10", "quiesce", "gpio11=0", "delay=1000",
                                                "vreg_b=off", NULL}));
    /* Without one the error is returned, nothing is released, and the next
     * call retries. */
    reset();
    S.reject[S.reject_count++] = 0x10;
    f = fresh_dt();
    assert(!kshim_touch_framework_start(f, 100, 100, emit, &EventCount));
    S.quiesce_status = ERROR_TIMEOUT;
    assert(kshim_touch_quiesce_all() == ERROR_TIMEOUT);
    assert(kshim_touch_active() && fake_regulator_named("vreg_b")->refs == 1);
    S.quiesce_status = 0;
    assert(!kshim_touch_quiesce_all() && S.quiesces == 2 && !fake_regulator_named("vreg_b")->refs);
    puts("failures: ok");
}

/* driver->power_on replaces the common sequence; pinctrl comes first and
 * the IRQ GPIO is an input again afterwards. */
static void test_power_override(void)
{
    reset();
    DrvA.power_on = drv_power_on;
    const void *f = fresh_dt();
    assert(!kshim_touch_framework_start(f, 100, 100, emit, &EventCount));
    assert(S.power_ons == 1);
    assert(fake_log_order((const char *const[]){"pinctrl=default", "override", "gpio11=0", "vreg_a=on",
                                                "vreg_b=on", "gpio10=1", "gpio11=1", "gpio10=in",
                                                "probe:a", NULL}));
    assert(!fake_log_has("delay=1000"));
    puts("power override: ok");
}

int main(void)
{
    load_fixture();
    test_lifecycle();
    test_irq_race();
    test_reset_level();
    test_transform();
    test_android_style();
    test_selection();
    test_failures();
    test_power_override();
    reset();
    puts("touch_framework_test: ok");
    return 0;
}
