/* SPDX-License-Identifier: MIT */
/*
 * GT9916 driver (kernel/drivers/input/touch/goodix_gt9916.c) against a model
 * of a Goodix Berlin-D controller behind an SPI controller (kshim/spi.h ops,
 * one chip select per message): the 0xF0/0xF1 framing with 32-bit
 * big-endian addresses and three dummy bytes, the power and reset timing,
 * the boot-option handshake, the firmware-version and IC-information
 * blocks with their checksums, checksummed touch events (one and several
 * reads, stylus slots, header/point corruption), the status release that
 * deasserts the level IRQ, a firmware reset request, inheritance of a
 * running controller, and the polling and quiesce hooks.
 */
#include "touch_goodix_fake.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define TOUCH_DATA_ADDR 0x10308U
#define BOOTOPTION 0x10000U
#define FW_VERSION 0x10014U
#define IC_INFO 0x10070U

struct region {
    uint32_t base;
    size_t span;            /* bytes the region answers for */
    uint8_t bytes[1024];
    size_t length;
};

struct brl_model {
    bool powered;
    uint64_t power_us;
    bool rstb;                  /* physical, high = running */
    uint64_t release_us, assert_us;
    bool booted;                /* echo after release + 4 ms */
    struct region bootoption, version, info, touch;
    unsigned resets, confirms, clears, writes, bad_frames, violations;
    unsigned bootoption_writes;
    uint64_t confirm_us;
};

static struct brl_model model;

static void violate(const char *what)
{
    ++model.violations;
    fprintf(stderr, "GT9916 model: %s at %llu us\n", what, (unsigned long long)fake.now_us);
}

static bool running(void)
{
    return model.powered && model.rstb;
}

static void m_power(void *ctx, bool on)
{
    (void)ctx;
    model.powered = on;
    model.power_us = fake.now_us;
    if (on && model.rstb)
        violate("rails came up with reset released");
}

static void m_rstb(void *ctx, bool level)
{
    (void)ctx;
    if (level == model.rstb)
        return;
    model.rstb = level;
    if (!level) {
        model.assert_us = fake.now_us;
        return;
    }
    if (!model.powered)
        return;
    /* vddio 3 ms + avdd 15 ms before the release; a reset pulse >= 2 ms. */
    if (fake.now_us - model.power_us < 18000U)
        violate("reset released < 18 ms after the rails");
    if (model.assert_us > model.power_us && fake.now_us - model.assert_us < 2000U)
        violate("reset pulse < 2 ms");
    model.release_us = fake.now_us;
    ++model.resets;
    model.touch.bytes[0] = 0;
}

/* INT is level low while an event is pending. */
static bool m_int_level(void *ctx)
{
    (void)ctx;
    return !(running() && model.touch.bytes[0]);
}

static struct region *find(uint32_t addr, size_t length, size_t *offset)
{
    struct region *regions[] = { &model.bootoption, &model.version, &model.info, &model.touch };
    for (unsigned i = 0; i < 4; ++i) {
        struct region *r = regions[i];
        if (addr >= r->base && addr + length <= r->base + r->span) {
            *offset = addr - r->base;
            return r;
        }
    }
    fprintf(stderr, "GT9916 model: access 0x%x+%zu outside the map\n", (unsigned)addr, length);
    assert(0);
    return NULL;
}

static int m_spi(const uint8_t *mosi, uint8_t *miso, size_t length)
{
    assert(length >= 5);
    uint32_t addr = (uint32_t)mosi[1] << 24 | (uint32_t)mosi[2] << 16 |
                    (uint32_t)mosi[3] << 8 | mosi[4];
    if (!running())
        return 0; /* MISO stays 0xff */
    uint64_t since = fake.now_us - model.release_us;
    size_t offset;
    if (mosi[0] == 0xf0) {
        size_t n = length - 5;
        struct region *r = find(addr, n, &offset);
        ++model.writes;
        if (since < 4000U) {
            violate("write before the firmware started");
            return 0;
        }
        memcpy(&r->bytes[offset], &mosi[5], n);
        if (r == &model.bootoption)
            ++model.bootoption_writes;
        if (r == &model.touch && offset == 0 && n == 1 && mosi[5] == 0)
            ++model.clears;
        return 0;
    }
    assert(mosi[0] == 0xf1);
    assert(length >= 8);
    if (mosi[5] != 0xff || mosi[6] != 0xff || mosi[7] != 0xff)
        violate("read dummy bytes are not 0xff");
    for (size_t i = 8; i < length; ++i)
        assert(mosi[i] == 0);
    size_t n = length - 8;
    struct region *r = find(addr, n, &offset);
    if (since < 4000U)
        return 0; /* not booted: idle bus */
    if (r == &model.bootoption) {
        memcpy(&miso[8], &r->bytes[offset], n);
        if (n == 8 && r->bytes[0] == 0xaa && !model.confirm_us)
            model.confirm_us = fake.now_us, ++model.confirms;
        return 0;
    }
    if (r != &model.touch && since < 100000U) {
        memset(&miso[8], 0, n); /* still booting: zeros */
        return 0;
    }
    memcpy(&miso[8], &r->bytes[offset], n);
    return 0;
}

/* The GT9916 behind a GENI-like SPI controller (kshim/spi.h ops): the
 * segments of one message share one chip-select assertion, so the model
 * sees them as one MOSI/MISO stream. */
static unsigned cs_assertions;

static int m_transfer(struct kshim_spi_controller *controller,
                      const struct kshim_spi_device *device,
                      const struct kshim_spi_xfer *xfers, size_t count, uint32_t timeout_us)
{
    static uint8_t mosi[4096], miso[4096];
    size_t total = 0;

    (void)controller;
    assert(timeout_us > 0 && count >= 1);
    /* QRD8650 touchscreen@0: CS 0, mode 0 (set by the driver), 8-bit, 1 MHz. */
    assert(device->chip_select == 0);
    assert(device->mode == KSHIM_SPI_MODE_0 && device->bits_per_word == 8);
    assert(device->max_speed_hz == 1000000);
    for (size_t i = 0; i < count; ++i) {
        assert(xfers[i].len && total + xfers[i].len <= sizeof(mosi));
        assert(xfers[i].delay_us == 0);
        if (xfers[i].tx)
            memcpy(&mosi[total], xfers[i].tx, xfers[i].len);
        else
            memset(&mosi[total], 0, xfers[i].len);
        total += xfers[i].len;
    }
    ++cs_assertions;
    memset(miso, 0xff, total);
    int status = m_spi(mosi, miso, total);
    if (status)
        return status;
    total = 0;
    for (size_t i = 0; i < count; ++i) {
        if (xfers[i].rx)
            memcpy(xfers[i].rx, &miso[total], xfers[i].len);
        total += xfers[i].len;
    }
    return 0;
}

static const struct kshim_spi_controller_ops model_ops = { .transfer = m_transfer };
static struct kshim_spi_controller model_controller = {
    .name = "spi@a90000", .num_chipselect = 1, .ops = &model_ops,
};

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, (uint16_t)v);
    put16(p + 2, (uint16_t)(v >> 16));
}

static void checksum(uint8_t *data, size_t length)
{
    uint16_t sum = 0;
    for (size_t i = 0; i + 2 < length; ++i)
        sum = (uint16_t)(sum + data[i]);
    put16(&data[length - 2], sum);
}

/* IC information with every variable list populated. */
static size_t build_ic_info(uint8_t *info, uint32_t touch_addr)
{
    size_t o = 2;
    memset(info, 0, 1024);
    for (unsigned i = 0; i < 16; ++i) info[o++] = (uint8_t)(0x10 + i); /* version */
    for (unsigned i = 0; i < 10; ++i) info[o++] = (uint8_t)(0x20 + i); /* features */
    info[o++] = 17; info[o++] = 38; info[o++] = 0; info[o++] = 0;      /* drv/sen/btn/force */
    const uint8_t lists[5] = { 2, 3, 1, 1, 0 };
    for (unsigned l = 0; l < 5; ++l) {
        info[o++] = lists[l];
        for (unsigned i = 0; i < lists[l]; ++i, o += 2) put16(&info[o], (uint16_t)(1000 + i));
    }
    /* misc: cmd addr 0x10174, ..., touch_data_addr at +44 */
    put32(&info[o], 0x10174);
    put32(&info[o + 44], touch_addr);
    put16(&info[o + 48], 8);   /* touch_data_head_len */
    put16(&info[o + 50], 8);   /* point_struct_len */
    o += 100;
    o += 2;
    put16(info, (uint16_t)o);
    checksum(info, o);
    return o;
}

static void model_reset(void)
{
    memset(&model, 0, sizeof(model));
    cs_assertions = 0;
    model.bootoption.base = BOOTOPTION;
    model.bootoption.span = FW_VERSION - BOOTOPTION;
    model.version.base = FW_VERSION;
    model.version.span = IC_INFO - FW_VERSION;
    model.info.base = IC_INFO;
    model.info.span = TOUCH_DATA_ADDR - IC_INFO;
    model.touch.base = TOUCH_DATA_ADDR;
    model.touch.span = 128;
    uint8_t *v = model.version.bytes;
    memcpy(v, "BERLIN", 6);
    v[6] = 1; v[7] = 2; v[8] = 3;
    memcpy(&v[10], "9916R\0\0\0", 8);
    v[18] = 0x01; v[19] = 0x00; v[20] = 0x0b; v[21] = 0x03;
    v[23] = 2; /* sensor id */
    checksum(v, 28);
    model.info.length = build_ic_info(model.info.bytes, TOUCH_DATA_ADDR);
    fake_world_reset();
    fake.chip = (struct fake_chip){
        .ctx = &model, .power = m_power, .rstb = m_rstb, .int_level = m_int_level,
    };
}

struct point { uint8_t id, type; uint16_t x, y; };

static void model_touch(const struct point *points, unsigned count)
{
    uint8_t *e = model.touch.bytes;
    memset(e, 0, 128);
    e[0] = 0x80;
    e[2] = (uint8_t)count;
    checksum(e, 8);
    unsigned slots = count;
    for (unsigned i = 0; i < count; ++i) {
        uint8_t *p = &e[8 + i * 8];
        p[0] = (uint8_t)(points[i].id << 4 | points[i].type);
        put16(&p[2], points[i].x);
        put16(&p[4], points[i].y);
        put16(&p[6], 40);
        if (points[i].type == 1 || points[i].type == 3)
            slots = 2;
    }
    if (count)
        checksum(&e[8], slots * 8 + 2);
}

static void model_request(uint8_t code)
{
    uint8_t *e = model.touch.bytes;
    memset(e, 0, 128);
    e[0] = 0x40;
    e[2] = code;
    checksum(e, 8);
}

/* ---- Device as the framework resolves the QRD8650 node ----------------- */

static struct kshim_touch_dev dev;
static uint8_t priv[KSHIM_TOUCH_PRIV_MAX];
static struct kshim_regulator l14b;
static const struct kshim_touch_driver *drv = &kshim_touch_driver_gt9916;

static void setup(void)
{
    model_reset();
    fake_dev_init(&dev, drv, KSHIM_TOUCH_BUS_SPI, priv);
    dev.spi.controller = &model_controller;
    fake_dev_set_reset(&dev, true, true);        /* <&tlmm 161 GPIO_ACTIVE_LOW> */
    fake_dev_set_irq_gpio(&dev, true, true);     /* level low */
    dev.irq_trigger = 8;
    l14b = (struct kshim_regulator){ .name = "vreg_l14b_3p2" };
    fake_dev_add_supply(&dev, &l14b);            /* avdd-supply (vddio absent) */
    dev.props.size_x = 1080;
    dev.props.size_y = 2400;
    dev.max_x = 1080;
    dev.max_y = 2400;
    fake.reset_pull_high = true;                 /* ts_irq state: bias-pull-up on GPIO161 */
}

static void bring_up(void)
{
    assert(drv->power_on(&dev) == 0);
    dev.powered = true;
    assert(drv->probe(&dev) == 0);
    dev.probed = true;
}

static void test_driver_object(void)
{
    assert(!strcmp(drv->compatibles[0], "goodix,gt9916") && !drv->compatibles[1]);
    assert(drv->bus_mask == KSHIM_TOUCH_BUS_SPI);
    assert(!strcmp(drv->supplies[0], "vddio") && !strcmp(drv->supplies[1], "avdd") &&
           !drv->supplies[2]);
    assert(drv->priv_size <= KSHIM_TOUCH_PRIV_MAX);
    assert(!drv->match && drv->power_on && drv->probe && drv->handle && drv->pending &&
           drv->quiesce);
    assert(drv->power_on_delay_us == 18000 && drv->post_reset_delay_us == 4000);
}

static void test_cold_probe(void)
{
    setup();
    assert(drv->power_on(&dev) == 0);
    assert(model.violations == 0 && model.resets == 1);
    assert(dev.spi.mode == KSHIM_SPI_MODE_0 && dev.spi.bits_per_word == 8);
    assert(l14b.on && l14b.refs == 1);
    assert(drv->probe(&dev) == 0);
    assert(model.violations == 0);
    assert(model.confirms == 1 && model.bootoption_writes == 1);
    /* The firmware was given 100 ms after the handshake. */
    assert(fake.now_us - model.confirm_us >= 100000U);
    assert(dev.max_contacts == 10 && dev.max_x == 1080 && dev.max_y == 2400);
    assert(model.clears == 1);
}

static void test_probe_failures(void)
{
    /* No DT size: the binding requires it. */
    setup();
    dev.props.size_x = 0;
    assert(drv->power_on(&dev) == 0);
    assert(drv->probe(&dev) == ERROR_INVALID_PARAMETER);

    /* Corrupt IC information. */
    setup();
    model.info.bytes[40] ^= 0x5a;
    assert(drv->power_on(&dev) == 0);
    assert(drv->probe(&dev) == KSHIM_IO_ERR_PROTOCOL);

    /* IC information whose lists run past its length. */
    setup();
    put16(model.info.bytes, 40);
    checksum(model.info.bytes, 40);
    assert(drv->power_on(&dev) == 0);
    assert(drv->probe(&dev) == KSHIM_IO_ERR_PROTOCOL);

    /* Corrupt firmware version. */
    setup();
    model.version.bytes[3] ^= 1;
    assert(drv->power_on(&dev) == 0);
    assert(drv->probe(&dev) == KSHIM_IO_ERR_PROTOCOL);

    /* No controller (MISO idle high): the handshake fails. */
    setup();
    assert(drv->power_on(&dev) == 0);
    model.powered = false;
    assert(drv->probe(&dev) == KSHIM_IO_ERR_NO_DEVICE);
}

static void test_events(void)
{
    setup();
    bring_up();

    /* Nothing pending: one read, no release write. */
    unsigned messages = fake.spi_messages, clears = model.clears;
    assert(drv->handle(&dev) == 0);
    assert(fake.spi_messages == messages + 1 && model.clears == clears);

    /* One finger: header + 2 slots + checksum in one read, then release. */
    model_touch((const struct point[]){ { 0, 0, 540, 1200 } }, 1);
    bool active;
    assert(kshim_gpio_get(&dev.irq_gpio, &active) == 0 && active);
    assert(drv->pending(&dev));
    messages = fake.spi_messages;
    assert(drv->handle(&dev) == 0);
    assert(fake.spi_messages == messages + 2);
    assert(fake.event_count == 1);
    fake_expect_event(0, 0, KSHIM_TOUCH_CONTACT_ENTER, 540, 1200);
    assert(model.clears == clears + 1);
    assert(kshim_gpio_get(&dev.irq_gpio, &active) == 0 && !active);

    /* Four fingers: the remaining two points need a second read. */
    fake_events_clear();
    model_touch((const struct point[]){ { 0, 0, 541, 1201 }, { 1, 0, 10, 20 },
                                        { 2, 0, 30, 40 }, { 9, 0, 1079, 2399 } }, 4);
    messages = fake.spi_messages;
    assert(drv->handle(&dev) == 0);
    assert(fake.spi_messages == messages + 3);
    assert(cs_assertions == fake.spi_messages);
    assert(fake.event_count == 4);
    fake_expect_event(0, 0, KSHIM_TOUCH_CONTACT_MOVE, 541, 1201);
    fake_expect_event(1, 1, KSHIM_TOUCH_CONTACT_ENTER, 10, 20);
    fake_expect_event(2, 2, KSHIM_TOUCH_CONTACT_ENTER, 30, 40);
    fake_expect_event(3, 9, KSHIM_TOUCH_CONTACT_ENTER, 1079, 2399);

    /* Stylus slots are skipped; finger 1 stays, the others leave. */
    fake_events_clear();
    model_touch((const struct point[]){ { 5, 3, 7, 7 }, { 1, 0, 11, 21 } }, 2);
    assert(drv->handle(&dev) == 0);
    assert(fake.event_count == 4);
    fake_expect_event(0, 1, KSHIM_TOUCH_CONTACT_MOVE, 11, 21);
    fake_expect_event(1, 0, KSHIM_TOUCH_CONTACT_LEAVE, 541, 1201);
    fake_expect_event(2, 2, KSHIM_TOUCH_CONTACT_LEAVE, 30, 40);
    fake_expect_event(3, 9, KSHIM_TOUCH_CONTACT_LEAVE, 1079, 2399);

    /* Touch event with no points: lift-off. */
    fake_events_clear();
    model_touch(NULL, 0);
    assert(drv->handle(&dev) == 0);
    assert(fake.event_count == 1);
    fake_expect_event(0, 1, KSHIM_TOUCH_CONTACT_LEAVE, 11, 21);

    /* Header checksum broken: nothing reported, event still released. */
    fake_events_clear();
    model_touch((const struct point[]){ { 3, 0, 1, 1 } }, 1);
    model.touch.bytes[7] ^= 0x80;
    clears = model.clears;
    assert(drv->handle(&dev) == KSHIM_IO_ERR_PROTOCOL);
    assert(fake.event_count == 0 && model.clears == clears + 1);

    /* Point checksum broken. */
    model_touch((const struct point[]){ { 3, 0, 1, 1 } }, 1);
    model.touch.bytes[10] ^= 0x01;
    assert(drv->handle(&dev) == KSHIM_IO_ERR_PROTOCOL);
    assert(fake.event_count == 0);

    /* More than ten points. */
    model_touch(NULL, 0);
    model.touch.bytes[2] = 11;
    checksum(model.touch.bytes, 8);
    assert(drv->handle(&dev) == KSHIM_IO_ERR_PROTOCOL);
    assert(fake.event_count == 0);

    /* Bus failure on the first read: the event stays pending. */
    model_touch((const struct point[]){ { 4, 0, 5, 6 } }, 1);
    fake.fail_bus = ERROR_TIMEOUT;
    assert(drv->handle(&dev) == ERROR_TIMEOUT);
    assert(model.touch.bytes[0] == 0x80);
    assert(drv->handle(&dev) == 0);
    fake_expect_event(0, 4, KSHIM_TOUCH_CONTACT_ENTER, 5, 6);
}

static void test_reset_request(void)
{
    setup();
    bring_up();
    model_touch((const struct point[]){ { 2, 0, 100, 100 } }, 1);
    assert(drv->handle(&dev) == 0);
    fake_events_clear();
    unsigned resets = model.resets;
    model_request(3);
    uint64_t start = fake.now_us;
    assert(drv->handle(&dev) == 0);
    assert(model.resets == resets + 1 && model.violations == 0);
    assert(fake.now_us - start >= 102000U);
    /* The tracked contact was lifted. */
    assert(fake.event_count == 1);
    fake_expect_event(0, 2, KSHIM_TOUCH_CONTACT_LEAVE, 100, 100);
    /* Reset released again afterwards. */
    assert(fake.reset_driven && fake.reset_physical);

    /* Other request codes are only logged. */
    model_request(1);
    resets = model.resets;
    assert(drv->handle(&dev) == 0);
    assert(model.resets == resets);
}

static void test_inherit(void)
{
    setup();
    l14b.on = l14b.inherited_on = true;
    model.powered = true;
    model.rstb = true;
    fake.reset_driven = false;
    fake.reset_pull_high = true; /* deasserted (active low) */
    fake.now_us = 2000000;
    model.release_us = 0;
    bring_up();
    assert(model.resets == 0 && model.bootoption_writes == 0);
    assert(l14b.enables == 0 && l14b.refs == 1);
    /* Reset now driven deasserted explicitly. */
    assert(fake.reset_driven && fake.reset_physical);
    model_touch((const struct point[]){ { 1, 0, 3, 4 } }, 1);
    assert(drv->handle(&dev) == 0);
    fake_expect_event(0, 1, KSHIM_TOUCH_CONTACT_ENTER, 3, 4);

    /* Rails on but reset asserted: cold path with a full reset. */
    setup();
    l14b.on = l14b.inherited_on = true;
    model.powered = true;
    fake.reset_driven = true;
    fake.reset_physical = false;
    fake.now_us = 2000000;
    model.power_us = 0;
    bring_up();
    assert(model.resets == 1 && model.confirms == 1 && model.violations == 0);
}

static void test_pending_and_quiesce(void)
{
    setup();
    bring_up();
    /* INT inactive: due only at the polling cadence. */
    assert(!drv->pending(&dev));
    delay(10000);
    assert(drv->pending(&dev));
    assert(drv->handle(&dev) == 0);
    assert(!drv->pending(&dev));
    /* Without an IRQ GPIO the cadence alone decides. */
    dev.has_irq_gpio = false;
    model_touch((const struct point[]){ { 0, 0, 1, 1 } }, 1);
    assert(!drv->pending(&dev));
    dev.has_irq_gpio = true;
    assert(drv->pending(&dev));

    unsigned clears = model.clears;
    assert(drv->quiesce(&dev) == 0);
    assert(model.clears == clears + 1 && model.touch.bytes[0] == 0);
    assert(drv->quiesce(&dev) == 0);
    model.powered = false;
    assert(drv->quiesce(&dev) == 0);
    dev.probed = false;
    unsigned messages = fake.spi_messages;
    assert(drv->quiesce(&dev) == 0 && fake.spi_messages == messages);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    test_driver_object();
    test_cold_probe();
    test_probe_failures();
    test_events();
    test_reset_request();
    test_inherit();
    test_pending_and_quiesce();
    printf("touch_goodix_gt9916_test: PASS\n");
    return 0;
}
