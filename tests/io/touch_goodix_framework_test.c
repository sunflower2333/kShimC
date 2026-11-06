/* SPDX-License-Identifier: MIT */
/*
 * Integration test (merged tree): the real touch framework
 * (kernel/drivers/input/touch/core.c, registry.c) with the real Goodix
 * GT911 and GT9916 drivers on the mainline SM8650 fixtures:
 *  - AYANEO Pocket S2 (tests/fixtures/io/sm8650-ayaneo-pocket-s2.dtb, with
 *    the RFC touch node): GT911 at 0x5d on i2c4, IRQ TLMM 162 falling edge,
 *    reset TLMM 161 GPIO_ACTIVE_HIGH (goodix,gt9xx convention: the logical
 *    value is the RSTB level), AVDD28 and VDDIO from vreg_l14b_3p2.
 *  - QRD8650 (tests/fixtures/io/sm8650-qrd.dtb): GT9916 on spi4 CS 0,
 *    IRQ TLMM 162 level low, reset TLMM 161 active low, avdd from
 *    vreg_l14b_3p2.
 *
 * The Goodix branch tested its drivers against a fake of the framework
 * helpers; touch-st tested the framework with fake drivers. This test runs
 * both real halves together over touch-st's platform fakes
 * (tests/io/touch_fakes.c: GPIO, pinctrl, regulators, I2C/SPI clients and
 * the TLMM interrupt API), with chip models written from the same protocol
 * facts as the drivers (GT911 programming guide / datasheet timing; the
 * Berlin framing documented in goodix_gt9916.c). It checks the framework
 * contract assumptions listed in docs/touch-goodix.md: reset helper
 * polarity, the IRQ-GPIO physical-level helper, the power_on override, the
 * IRQ requested after probe with the trigger probe chose, the I2C address
 * at call time, DT size preset before probe, and an idempotent quiesce.
 */
#include "touch_fakes.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dt.h>
#include <kshim/irq.h>
#include <kshim/time.h>

#define RESET_PIN 161U
#define INT_PIN 162U
#define RAIL "vreg_l14b_3p2"

static void *load(const char *path)
{
    FILE *file = fopen(path, "rb");
    assert(file);
    assert(!fseek(file, 0, SEEK_END));
    long size = ftell(file);
    assert(size > 0 && !fseek(file, 0, SEEK_SET));
    void *data = aligned_alloc(8, ((size_t)size + 7) & ~(size_t)7);
    assert(data && fread(data, 1, (size_t)size, file) == (size_t)size);
    fclose(file);
    assert(!fdt_check_header(data));
    return data;
}

static kshim_touch_event_t Events[64];
static unsigned EventCount;
static void emit(void *context, const kshim_touch_event_t *event)
{
    (void)context;
    assert(EventCount < 64);
    Events[EventCount++] = *event;
}

static bool rail_on(void)
{
    const struct fake_regulator *r = fake_regulator_named(RAIL);
    return r && r->refs > 0;
}

static void le16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

/* ---- GT911 model (I2C) ------------------------------------------------------ */

static struct {
    uint8_t mem[0x200];        /* 0x8000..0x81ff */
    uint16_t address;          /* latched at the RSTB rising edge, 0 = none */
    bool rstb;
    uint64_t int_set_us;
    unsigned latches, status_clears;
} G;

static uint8_t *gt911_reg(uint16_t reg)
{
    assert(reg >= 0x8000 && reg < 0x8200);
    return &G.mem[reg - 0x8000];
}

static void gt911_pin(uint32_t pin, bool level)
{
    if (pin == INT_PIN) {
        G.int_set_us = fake_now_us();
        return;
    }
    if (pin != RESET_PIN || level == G.rstb) return;
    G.rstb = level;
    if (!level || !rail_on()) { G.address = 0; return; }
    /* INT stable > 100 us before RSTB rises (datasheet T3); INT low = 0x5d.
     * The full T2-T5 timing is checked by touch_goodix_gt911_test. */
    assert(fake_now_us() - G.int_set_us >= 100U);
    G.address = fake_pin_level(INT_PIN) ? 0x14 : 0x5d;
    ++G.latches;
}

static int gt911_i2c(uint16_t address, const uint8_t *tx, size_t tx_length, uint8_t *rx,
                     size_t rx_length)
{
    if (!rail_on() || !G.rstb || address != G.address) return KSHIM_IO_ERR_NACK;
    assert(tx_length >= 2);
    uint16_t reg = (uint16_t)(tx[0] << 8 | tx[1]);
    if (rx_length) {
        memcpy(rx, gt911_reg(reg), rx_length);
        return 0;
    }
    memcpy(gt911_reg(reg), tx + 2, tx_length - 2);
    if (reg == 0x814e && tx_length == 3 && tx[2] == 0) ++G.status_clears;
    return 0;
}

static void gt911_model_init(void)
{
    memset(&G, 0, sizeof(G));
    uint8_t *info = gt911_reg(0x8140);
    memcpy(info, "911\0", 4);
    le16(info + 4, 0x1060);
    le16(info + 6, 720);           /* product resolution (DT size wins) */
    le16(info + 8, 1280);
    /* Configuration 0x8047..0x80FE: X/Y max, 5 points, INT falling (1). */
    uint8_t *config = gt911_reg(0x8047);
    config[0] = 0x41;
    le16(config + 1, 1440);
    le16(config + 3, 2560);
    config[5] = 5;
    config[6] = 0x01;
    uint8_t sum = 0;
    for (unsigned i = 0; i < 184; ++i) sum = (uint8_t)(sum + config[i]);
    config[184] = (uint8_t)(0U - sum);
}

static void gt911_frame(unsigned count, uint8_t id, uint16_t x, uint16_t y)
{
    uint8_t *status = gt911_reg(0x814e);
    status[0] = (uint8_t)(0x80U | count);
    if (count) {
        uint8_t *p = status + 1;
        p[0] = id;
        le16(p + 1, x);
        le16(p + 3, y);
        le16(p + 5, 30);
    }
}

static void test_pocket_s2(void)
{
    fake_touch_reset();
    gt911_model_init();
    EventCount = 0;
    void *fdt = load(FIXTURES "/io/sm8650-ayaneo-pocket-s2.dtb");
    FakePinHook = gt911_pin;
    FakeI2cDevice = gt911_i2c;
    fake_pin_input(RESET_PIN, false);   /* ts_reset_default pull-down */
    fake_pin_input(INT_PIN, true);

    assert(kshim_touch_framework_start(fdt, 1440, 2560, emit, NULL) == 0);
    const struct kshim_touch_dev *dev = kshim_touch_active();
    assert(dev && dev->driver == &kshim_touch_driver_gt911 && dev->bus == KSHIM_TOUCH_BUS_I2C);
    /* Both supplies are the one l14b rail; two references, enabled once each. */
    const struct fake_regulator *rail = fake_regulator_named(RAIL);
    assert(rail && dev->supply_count == 2 && rail->refs == 2);
    /* Address select at the RSTB edge with INT driven low: 0x5d, once. */
    assert(G.latches == 1 && G.address == 0x5d && dev->i2c.addr == 0x5d);
    assert(fake_pin(RESET_PIN)->output && fake_pin_level(RESET_PIN));   /* RSTB driven high */
    assert(!fake_pin(INT_PIN)->output);                                 /* INT an input again */
    /* DT size kept (the product block says 720x1280), 5 points from the config. */
    assert(dev->props.size_x == 1440 && dev->props.size_y == 2560 && dev->max_contacts == 5);
    /* The IRQ follows probe's trigger (config INT mode falling = the DT's). */
    assert(FakeTlmmIrq.requested && FakeTlmmIrq.pin == INT_PIN &&
           FakeTlmmIrq.trigger == KSHIM_IRQ_TYPE_EDGE_FALLING);
    assert(dev->irq_live);

    /* The pass right after the IRQ request finds nothing. */
    unsigned clears = G.status_clears;
    assert(kshim_touch_framework_poll() == 0 && EventCount == 0);
    fake_log_clear();

    /* One finger: interrupt -> deferred handle -> PRESS, status released. */
    gt911_frame(1, 0, 700, 1200);
    assert(fake_irq_fire() && FakeTlmmIrq.masked);
    assert(kshim_touch_framework_poll() == 0);
    assert(!FakeTlmmIrq.masked && G.status_clears > clears);
    assert(EventCount == 1 && Events[0].type == KSHIM_TOUCH_EVENT_PRESS &&
           Events[0].x == 700 && Events[0].y == 1200);
    gt911_frame(1, 0, 710, 1210);
    assert(fake_irq_fire() && kshim_touch_framework_poll() == 0);
    assert(EventCount == 2 && Events[1].type == KSHIM_TOUCH_EVENT_MOVE && Events[1].x == 710);
    gt911_frame(0, 0, 0, 0);
    assert(fake_irq_fire() && kshim_touch_framework_poll() == 0);
    assert(EventCount == 3 && Events[2].type == KSHIM_TOUCH_EVENT_RELEASE);
    assert(!(gt911_reg(0x814e)[0] & 0x80));

    /* Handoff: interrupt freed, rails back to no reference, pads restored
     * (RSTB released to its pulled-down input, INT input). Idempotent. */
    fake_log_clear();
    assert(kshim_touch_quiesce_all() == 0);
    assert(!FakeTlmmIrq.requested && FakeTlmmIrq.frees == 1);
    assert(rail->refs == 0 && !kshim_touch_active());
    assert(fake_pin(RESET_PIN)->refs == 0 && !fake_pin(RESET_PIN)->output);
    assert(fake_pin(INT_PIN)->refs == 0 && !fake_pin(INT_PIN)->output);
    assert(!fake_pin_level(RESET_PIN));
    /* Cold path (kShimC reset and powered it): KSHIM_TOUCH_RESET_LEVEL
     * turns the RSTB pad into an input before the rail drops, so no high
     * level is driven into the unpowered chip until the pad restore (with a
     * pinctrl state, at handoff step 9); RSTB is never driven low either. */
    const char *const order[] = {"gpio161=in", RAIL "=off", RAIL "=off", "free161", NULL};
    assert(fake_log_order(order));
    assert(!fake_log_has("gpio161=1") && !fake_log_has("gpio161=0"));
    assert(kshim_touch_quiesce_all() == 0 && FakeTlmmIrq.frees == 1);
    free(fdt);
    printf("touch_goodix_framework_test: pocket-s2 GT911 ok\n");
}

/* UEFI left the GT911 powered (another l14b user holds the rail), out of
 * reset and addressed at 0x5d, with RSTB driven high. kShimC inherits it and
 * the handoff must leave it running at the same address: no reset pulse. */
static void test_pocket_s2_inherited(void)
{
    fake_touch_reset();
    gt911_model_init();
    EventCount = 0;
    void *fdt = load(FIXTURES "/io/sm8650-ayaneo-pocket-s2.dtb");
    FakePinHook = gt911_pin;
    FakeI2cDevice = gt911_i2c;
    fake_pin_input(INT_PIN, true);
    /* The shared rail is on: a reference held by its other consumer. */
    int touch = fdt_node_offset_by_compatible(fdt, -1, "goodix,gt911");
    assert(touch >= 0);
    struct kshim_regulator *other;
    assert(!kshim_regulator_get(fdt, touch, "VDDIO", &other) && !kshim_regulator_enable(other));
    /* RSTB driven high by the previous owner; the chip latched 0x5d. */
    struct fake_pin *rst = fake_pin(RESET_PIN);
    rst->output = rst->out_level = true;
    rst->snapshot_output = rst->snapshot_level = true;
    G.rstb = true;
    G.address = 0x5d;

    assert(kshim_touch_framework_start(fdt, 1440, 2560, emit, NULL) == 0);
    const struct kshim_touch_dev *dev = kshim_touch_active();
    assert(dev && dev->driver == &kshim_touch_driver_gt911 && dev->probed);
    assert(G.latches == 0 && G.rstb && dev->i2c.addr == 0x5d);
    assert(!fake_log_has("gpio161=0"));                /* no reset at start */
    gt911_frame(1, 0, 10, 20);
    assert(fake_irq_fire() && kshim_touch_framework_poll() == 0 && EventCount == 1);

    fake_log_clear();
    assert(kshim_touch_quiesce_all() == 0);
    /* RSTB never left the high level: the chip still answers at 0x5d. */
    assert(!fake_log_has("gpio161=0") && !fake_log_has("gpio161=in"));
    assert(G.rstb && G.address == 0x5d && G.latches == 0);
    assert(fake_pin(RESET_PIN)->output && fake_pin_level(RESET_PIN));
    const struct fake_regulator *rail = fake_regulator_named(RAIL);
    assert(rail && rail->refs == 1);                   /* only the other consumer's */
    free(fdt);
    printf("touch_goodix_framework_test: pocket-s2 GT911 inherited ok\n");
}

/* The GT911 configuration selects rising-edge INT (Module_Switch1 = 0)
 * while the RFC node says falling: probe switches the trigger, and the IRQ
 * GPIO's active level must follow, or INT idling low reads as an asserted
 * level and the 100 ms stuck fallback drops a working interrupt. */
static void test_pocket_s2_trigger(void)
{
    fake_touch_reset();
    gt911_model_init();
    uint8_t *config = gt911_reg(0x8047);
    config[6] = 0x00;
    uint8_t sum = 0;
    for (unsigned i = 0; i < 184; ++i) sum = (uint8_t)(sum + config[i]);
    config[184] = (uint8_t)(0U - sum);
    EventCount = 0;
    void *fdt = load(FIXTURES "/io/sm8650-ayaneo-pocket-s2.dtb");
    FakePinHook = gt911_pin;
    FakeI2cDevice = gt911_i2c;
    fake_pin_input(RESET_PIN, false);
    fake_pin_input(INT_PIN, false);                    /* rising-edge INT idles low */

    assert(kshim_touch_framework_start(fdt, 1440, 2560, emit, NULL) == 0);
    const struct kshim_touch_dev *dev = kshim_touch_active();
    assert(dev && dev->irq_trigger == KSHIM_IRQ_TYPE_EDGE_RISING);
    assert(FakeTlmmIrq.trigger == KSHIM_IRQ_TYPE_EDGE_RISING);
    assert(dev->has_irq_gpio && !dev->irq_gpio.active_low);
    for (unsigned i = 0; i < 15; ++i) {
        assert(kshim_touch_framework_poll() == 0);
        fake_advance_us(10000);
    }
    assert(dev->irq_live && FakeTlmmIrq.requested && !FakeTlmmIrq.frees);
    gt911_frame(1, 0, 100, 200);
    assert(fake_irq_fire() && kshim_touch_framework_poll() == 0 && EventCount == 1);
    assert(kshim_touch_quiesce_all() == 0);
    free(fdt);
    printf("touch_goodix_framework_test: pocket-s2 GT911 trigger ok\n");
}

/* ---- GT9916 model (Berlin SPI) ---------------------------------------------- */

#define BRL_TOUCH 0x10308U
static struct {
    bool rstb;                       /* physical, high = running */
    uint64_t release_us;
    uint8_t bootoption[0x14], version[28], info[1024], touch[128];
    unsigned resets, clears;
} B;

static void brl_checksum(uint8_t *data, size_t length)
{
    uint16_t sum = 0;
    for (size_t i = 0; i + 2 < length; ++i) sum = (uint16_t)(sum + data[i]);
    le16(&data[length - 2], sum);
}

static void brl_int(void)
{
    /* INT is level low while an event is pending. */
    fake_pin_input(INT_PIN, !(rail_on() && B.rstb && B.touch[0]));
}

static void brl_pin(uint32_t pin, bool level)
{
    if (pin != RESET_PIN || level == B.rstb) return;
    B.rstb = level;
    if (level) {
        B.release_us = fake_now_us();
        ++B.resets;
        B.touch[0] = 0;
    }
    brl_int();
}

static uint8_t *brl_region(uint32_t addr, size_t length)
{
    struct { uint32_t base; uint8_t *bytes; size_t span; } map[] = {
        {0x10000, B.bootoption, sizeof(B.bootoption)}, {0x10014, B.version, sizeof(B.version)},
        {0x10070, B.info, BRL_TOUCH - 0x10070}, {BRL_TOUCH, B.touch, sizeof(B.touch)},
    };
    for (unsigned i = 0; i < 4; ++i)
        if (addr >= map[i].base && addr + length <= map[i].base + map[i].span)
            return map[i].bytes + (addr - map[i].base);
    fprintf(stderr, "GT9916 model: access 0x%x+%zu outside the map\n", (unsigned)addr, length);
    assert(0);
    return NULL;
}

static int brl_spi(const struct kshim_spi_device *device, const struct kshim_spi_xfer *xfers,
                   size_t count)
{
    static uint8_t mosi[2048], miso[2048];
    size_t total = 0;
    /* QRD8650 touchscreen@0: CS 0, mode 0, 8-bit words, 1 MHz. */
    assert(device->chip_select == 0 && device->mode == KSHIM_SPI_MODE_0 &&
           device->bits_per_word == 8 && device->max_speed_hz == 1000000);
    for (size_t i = 0; i < count; ++i) {
        assert(total + xfers[i].len <= sizeof(mosi));
        if (xfers[i].tx) memcpy(&mosi[total], xfers[i].tx, xfers[i].len);
        else memset(&mosi[total], 0, xfers[i].len);
        total += xfers[i].len;
    }
    memset(miso, 0xff, total);
    assert(total >= 5);
    uint32_t addr = (uint32_t)mosi[1] << 24 | (uint32_t)mosi[2] << 16 | (uint32_t)mosi[3] << 8 | mosi[4];
    bool up = rail_on() && B.rstb && fake_now_us() - B.release_us >= 4000U;
    if (up && mosi[0] == 0xf0) {
        memcpy(brl_region(addr, total - 5), &mosi[5], total - 5);
        if (addr == BRL_TOUCH && total == 6 && mosi[5] == 0) ++B.clears;
    } else if (up && mosi[0] == 0xf1) {
        assert(total >= 8 && mosi[5] == 0xff && mosi[6] == 0xff && mosi[7] == 0xff);
        const uint8_t *src = brl_region(addr, total - 8);
        bool booting = addr != 0x10000 && addr != BRL_TOUCH && fake_now_us() - B.release_us < 100000U;
        if (booting) memset(&miso[8], 0, total - 8);
        else memcpy(&miso[8], src, total - 8);
    }
    total = 0;
    for (size_t i = 0; i < count; ++i) {
        if (xfers[i].rx) memcpy(xfers[i].rx, &miso[total], xfers[i].len);
        total += xfers[i].len;
    }
    brl_int();
    return 0;
}

static void brl_model_init(void)
{
    memset(&B, 0, sizeof(B));
    memcpy(B.version, "BERLIN", 6);
    B.version[6] = 1; B.version[7] = 2; B.version[8] = 3;
    memcpy(&B.version[10], "9916R\0\0\0", 8);
    B.version[18] = 0x01; B.version[20] = 0x0b; B.version[21] = 0x03; B.version[23] = 2;
    brl_checksum(B.version, sizeof(B.version));
    /* IC information: length, version (16), features (10), counts (4), five
     * counted LE16 lists, misc block with the touch data address at +44,
     * checksum. */
    uint8_t *info = B.info;
    size_t o = 2;
    for (unsigned i = 0; i < 16; ++i) info[o++] = (uint8_t)(0x10 + i);
    for (unsigned i = 0; i < 10; ++i) info[o++] = (uint8_t)(0x20 + i);
    info[o++] = 17; info[o++] = 38; info[o++] = 0; info[o++] = 0;
    const uint8_t lists[5] = {2, 3, 1, 1, 0};
    for (unsigned l = 0; l < 5; ++l) {
        info[o++] = lists[l];
        for (unsigned i = 0; i < lists[l]; ++i, o += 2) le16(&info[o], (uint16_t)(1000 + i));
    }
    le16(&info[o], 0x0174); le16(&info[o + 2], 0x0001);            /* cmd addr 0x10174 */
    le16(&info[o + 44], (uint16_t)BRL_TOUCH); le16(&info[o + 46], (uint16_t)(BRL_TOUCH >> 16));
    le16(&info[o + 48], 8);
    le16(&info[o + 50], 8);
    o += 102;
    le16(info, (uint16_t)o);
    brl_checksum(info, o);
}

static void brl_touch(unsigned count, uint8_t id, uint16_t x, uint16_t y)
{
    uint8_t *e = B.touch;
    memset(e, 0, sizeof(B.touch));
    e[0] = 0x80;
    e[2] = (uint8_t)count;
    brl_checksum(e, 8);
    if (count) {
        uint8_t *p = &e[8];
        p[0] = (uint8_t)(id << 4);
        le16(&p[2], x);
        le16(&p[4], y);
        le16(&p[6], 40);
        brl_checksum(&e[8], 8 + 2);
    }
    brl_int();
}

static void test_qrd8650(void)
{
    fake_touch_reset();
    brl_model_init();
    EventCount = 0;
    void *fdt = load(FIXTURES "/io/sm8650-qrd.dtb");
    FakePinHook = brl_pin;
    FakeSpiDevice = brl_spi;
    fake_pin_input(RESET_PIN, true);
    brl_int();

    assert(kshim_touch_framework_start(fdt, 1080, 2400, emit, NULL) == 0);
    const struct kshim_touch_dev *dev = kshim_touch_active();
    assert(dev && dev->driver == &kshim_touch_driver_gt9916 && dev->bus == KSHIM_TOUCH_BUS_SPI);
    assert(dev->spi.chip_select == 0 && dev->spi.max_speed_hz == 1000000);
    /* Cold path: reset asserted (active low -> pad low) before the rail, one
     * release after it, handshake answered. */
    assert(B.resets == 1 && B.rstb && fake_pin(RESET_PIN)->output);
    assert(B.bootoption[0] == 0xaa);
    const struct fake_regulator *rail = fake_regulator_named(RAIL);
    assert(rail && dev->supply_count == 1 && rail->refs == 1);
    assert(dev->props.size_x == 1080 && dev->props.size_y == 2400);
    assert(FakeTlmmIrq.requested && FakeTlmmIrq.pin == INT_PIN &&
           FakeTlmmIrq.trigger == KSHIM_IRQ_TYPE_LEVEL_LOW && dev->irq_live);
    assert(kshim_touch_framework_poll() == 0 && EventCount == 0);
    fake_log_clear();

    /* Level IRQ: the handle reads the event and releases it, which
     * deasserts INT before the framework unmasks the pin. */
    brl_touch(1, 2, 500, 1800);
    assert(!fake_pin_level(INT_PIN));
    assert(fake_irq_fire() && kshim_touch_framework_poll() == 0);
    assert(B.clears >= 1 && !B.touch[0] && fake_pin_level(INT_PIN) && !FakeTlmmIrq.masked);
    assert(EventCount == 1 && Events[0].type == KSHIM_TOUCH_EVENT_PRESS &&
           Events[0].x == 500 && Events[0].y == 1800);
    brl_touch(0, 0, 0, 0);
    assert(fake_irq_fire() && kshim_touch_framework_poll() == 0);
    assert(EventCount == 2 && Events[1].type == KSHIM_TOUCH_EVENT_RELEASE);

    /* A level that stays asserted with no delivery for 100 ms: the
     * framework drops the interrupt and polls (the driver still works). */
    fake_log_clear();
    brl_touch(1, 1, 100, 200);
    for (unsigned i = 0; i < 12 && dev->irq_live; ++i) {
        fake_advance_us(10000);
        assert(kshim_touch_framework_poll() == 0);
    }
    assert(!dev->irq_live && !FakeTlmmIrq.requested);
    assert(EventCount >= 3 && Events[2].type == KSHIM_TOUCH_EVENT_PRESS && Events[2].x == 100);

    /* Handoff: reset asserted while the rail drops, then pads restored. */
    fake_log_clear();
    assert(kshim_touch_quiesce_all() == 0);
    assert(rail->refs == 0 && !kshim_touch_active());
    assert(fake_pin(RESET_PIN)->refs == 0 && !fake_pin(RESET_PIN)->output);
    const char *const order[] = {"gpio161=0", RAIL "=off", "free161", NULL};
    assert(fake_log_order(order));
    assert(kshim_touch_quiesce_all() == 0);
    free(fdt);
    printf("touch_goodix_framework_test: qrd8650 GT9916 ok\n");
}

int main(void)
{
    test_pocket_s2();
    test_pocket_s2_inherited();
    test_pocket_s2_trigger();
    test_qrd8650();
    printf("touch_goodix_framework_test: OK\n");
    return 0;
}
