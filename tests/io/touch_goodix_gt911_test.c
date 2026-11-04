/* SPDX-License-Identifier: MIT */
/*
 * GT911 driver (kernel/drivers/input/touch/goodix_gt911.c) against a model
 * of the controller behind an I2C adapter (kshim/i2c.h ops, repeated-START
 * register reads): the power-on address select (INT level
 * sampled at the RSTB rising edge, datasheet T2..T5 timings checked), the
 * product/configuration reads, point frames with the status handshake,
 * inheritance of a running controller, and the polling and quiesce hooks.
 * The framework helpers are faked (touch_goodix_fake.c).
 */
#include "touch_goodix_fake.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <kshim/irq.h>

#define REG_BASE 0x8000U
#define REG_SPAN 0x200U

struct gt911_model {
    bool powered;
    uint64_t power_us;
    bool rstb;
    uint64_t rstb_fall_us, rstb_rise_us;
    bool host_int, host_int_level;
    uint64_t int_change_us;
    bool int_pull_high;          /* INT when nobody drives it */
    uint64_t int_low_since_us;   /* host INT low since (for T5) */
    uint8_t address;             /* latched, 0 = not running */
    uint64_t ready_us;
    uint8_t regs[REG_SPAN];
    unsigned address_selects;
    unsigned status_clears;
    unsigned violations;
    char violation[96];
};

static struct gt911_model model;

static void violate(const char *what)
{
    ++model.violations;
    snprintf(model.violation, sizeof(model.violation), "%s at %llu us", what,
             (unsigned long long)fake.now_us);
    fprintf(stderr, "GT911 model: %s\n", model.violation);
}

static bool int_level(void)
{
    return model.host_int ? model.host_int_level : model.int_pull_high;
}

static void latch(void)
{
    model.address = int_level() ? 0x14 : 0x5d;
    model.ready_us = fake.now_us + 5000U; /* firmware start */
    ++model.address_selects;
}

static void m_power(void *ctx, bool on)
{
    (void)ctx;
    model.powered = on;
    model.power_us = fake.now_us;
    if (!on) {
        model.address = 0;
        return;
    }
    /* Without a reset line the chip comes out of its power-on reset by
     * itself and samples INT then. */
    if (model.rstb)
        latch();
}

static void m_rstb(void *ctx, bool level)
{
    (void)ctx;
    if (level == model.rstb)
        return;
    model.rstb = level;
    if (!level) {
        model.rstb_fall_us = fake.now_us;
        model.address = 0;
        return;
    }
    model.rstb_rise_us = fake.now_us;
    if (!model.powered)
        return;
    /* T2: RSTB low > 10 ms with the rails up. */
    uint64_t low_since = model.rstb_fall_us > model.power_us ? model.rstb_fall_us : model.power_us;
    if (fake.now_us - low_since < 10000U)
        violate("T2 (reset low < 10 ms after power)");
    /* T3: INT stable > 100 us before the rising edge. */
    if (model.host_int && fake.now_us - model.int_change_us < 100U)
        violate("T3 (INT setup < 100 us)");
    latch();
}

static void m_int_drive(void *ctx, bool driven, bool level)
{
    (void)ctx;
    bool before = int_level();
    /* T4: INT held > 5 ms after the rising edge. */
    if (model.rstb && model.address && fake.now_us - model.rstb_rise_us < 5000U &&
        (driven ? level : model.int_pull_high) != before)
        violate("T4 (INT changed < 5 ms after reset)");
    if (!driven && model.host_int && !model.host_int_level &&
        fake.now_us - model.int_low_since_us < 50000U && model.address)
        violate("T5 (INT released < 50 ms after the sync low)");
    if (driven && !level && (!model.host_int || model.host_int_level))
        model.int_low_since_us = fake.now_us;
    model.host_int = driven;
    model.host_int_level = level;
    if (int_level() != before)
        model.int_change_us = fake.now_us;
}

static bool m_int_level(void *ctx)
{
    (void)ctx;
    return int_level();
}

/* The GT911 behind a GENI-like I2C adapter (kshim/i2c.h ops): a register
 * write is one message (address high, low, data); a read is the address
 * write, then a repeated START and the read message. */
static unsigned repeated_starts;

static int m_xfer(struct kshim_i2c_adapter *adapter, const struct kshim_i2c_msg *msgs,
                  size_t count, uint32_t timeout_us)
{
    (void)adapter;
    assert(count == 1 || count == 2);
    assert(timeout_us > 0);
    for (size_t i = 0; i < count; ++i)
        assert(!(msgs[i].flags & KSHIM_I2C_M_TEN) && msgs[i].buf && msgs[i].len);
    assert(!(msgs[0].flags & KSHIM_I2C_M_RD) && msgs[0].len >= 2);
    if (count == 2) {
        assert((msgs[1].flags & KSHIM_I2C_M_RD) && msgs[1].addr == msgs[0].addr);
        assert(!(msgs[0].flags & KSHIM_I2C_M_STOP));
        assert(msgs[0].len == 2);
        ++repeated_starts;
    }
    /* Address phase: NACK unless powered, running and at this address. */
    if (!model.powered || !model.rstb || msgs[0].addr != model.address ||
        fake.now_us < model.ready_us)
        return KSHIM_IO_ERR_NACK;
    const uint8_t *tx = msgs[0].buf;
    unsigned reg = (unsigned)tx[0] << 8 | tx[1];
    assert(reg >= REG_BASE && reg < REG_BASE + REG_SPAN);
    unsigned off = reg - REG_BASE;
    if (count == 2) {
        assert(off + msgs[1].len <= REG_SPAN);
        memcpy(msgs[1].buf, &model.regs[off], msgs[1].len);
        return 0;
    }
    size_t n = msgs[0].len - 2U;
    assert(off + n <= REG_SPAN);
    memcpy(&model.regs[off], &tx[2], n);
    if (reg == 0x814e && n == 1 && tx[2] == 0)
        ++model.status_clears;
    return 0;
}

static const struct kshim_i2c_adapter_ops model_ops = { .xfer = m_xfer };
static struct kshim_i2c_adapter model_adapter = {
    .name = "i2c@a90000", .bus_hz = 400000, .ops = &model_ops,
};

static uint8_t *reg(unsigned address)
{
    return &model.regs[address - REG_BASE];
}

static void put16(unsigned address, uint16_t value)
{
    reg(address)[0] = (uint8_t)value;
    reg(address)[1] = (uint8_t)(value >> 8);
}

static void config_checksum(void)
{
    uint8_t sum = 0;
    for (unsigned a = 0x8047; a <= 0x80fe; ++a)
        sum = (uint8_t)(sum + *reg(a));
    *reg(0x80ff) = (uint8_t)(~sum + 1U);
    *reg(0x8100) = 1; /* Config_Fresh */
}

/* GT911 as on the Pocket S2: product "911", firmware 0x1060, 1440x2560,
 * 5 points, INT falling edge (Module_Switch1 = 1). */
static void model_reset(void)
{
    memset(&model, 0, sizeof(model));
    memcpy(reg(0x8140), "911\0", 4);
    put16(0x8144, 0x1060);
    put16(0x8146, 1440);
    put16(0x8148, 2560);
    *reg(0x8047) = 0x41;
    put16(0x8048, 1440);
    put16(0x804a, 2560);
    *reg(0x804c) = 5;
    *reg(0x804d) = 0x01;
    for (unsigned a = 0x804e; a <= 0x80fe; ++a)
        *reg(a) = (uint8_t)(a * 7U);
    config_checksum();
    fake_world_reset();
    fake.chip = (struct fake_chip){
        .ctx = &model, .power = m_power, .rstb = m_rstb, .int_drive = m_int_drive,
        .int_level = m_int_level,
    };
}

struct point { uint8_t id; uint16_t x, y; };

static void model_frame(const struct point *points, unsigned count)
{
    *reg(0x814e) = (uint8_t)(0x80U | count);
    for (unsigned i = 0; i < count; ++i) {
        unsigned base = 0x814f + i * 8U;
        *reg(base) = points[i].id;
        put16(base + 1, points[i].x);
        put16(base + 3, points[i].y);
        put16(base + 5, 30);
        *reg(base + 7) = 0;
    }
}

/* ---- Device setup as the framework resolves the Pocket S2 node ------------- */

static struct kshim_touch_dev dev;
static uint8_t priv[KSHIM_TOUCH_PRIV_MAX];
static struct kshim_regulator l14b;

static const struct kshim_touch_driver *drv = &kshim_touch_driver_gt911;

static void setup(uint16_t address, bool irq_gpio)
{
    model_reset();
    fake_dev_init(&dev, drv, KSHIM_TOUCH_BUS_I2C, priv);
    dev.i2c.adapter = &model_adapter;
    dev.i2c.addr = address;
    fake_dev_set_reset(&dev, true, false);      /* <&tlmm 161 GPIO_ACTIVE_HIGH> */
    fake_dev_set_irq_gpio(&dev, irq_gpio, true); /* falling edge: active low */
    dev.irq_trigger = KSHIM_IRQ_TYPE_EDGE_FALLING;
    l14b = (struct kshim_regulator){ .name = "vreg_l14b_3p2" };
    fake_dev_add_supply(&dev, &l14b);            /* AVDD28-supply */
    fake_dev_add_supply(&dev, &l14b);            /* VDDIO-supply */
    fake.reset_pull_high = false;                /* ts_reset_default: bias-pull-down */
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
    assert(drv->compatibles && !strcmp(drv->compatibles[0], "goodix,gt911") && !drv->compatibles[1]);
    assert(drv->bus_mask == KSHIM_TOUCH_BUS_I2C);
    assert(!strcmp(drv->supplies[0], "AVDD28") && !strcmp(drv->supplies[1], "VDDIO") &&
           !drv->supplies[2]);
    assert(drv->priv_size <= KSHIM_TOUCH_PRIV_MAX);
    assert(drv->match && drv->power_on && drv->probe && drv->handle && drv->pending &&
           drv->quiesce);
    assert(!drv->reset_gpio_names && !drv->irq_gpio_names);
}

static void test_address_select(uint16_t address)
{
    setup(address, true);
    assert(drv->power_on(&dev) == 0);
    assert(model.violations == 0);
    assert(model.address_selects == 1);
    assert(model.address == address);
    /* RSTB is left driven high (the pad's pull-down would reset it). */
    assert(fake.reset_driven && fake.reset_physical);
    /* INT handed back to the chip after the 50 ms sync low. */
    assert(!fake.int_driven);
    assert(l14b.on && l14b.refs == 2 && l14b.enables == 1);
    assert(drv->probe(&dev) == 0);
    assert(dev.i2c.addr == address);
    assert(dev.max_x == 1440 && dev.max_y == 2560);
    assert(dev.max_contacts == 5);
    assert(dev.irq_trigger == KSHIM_IRQ_TYPE_EDGE_FALLING);
    assert(model.status_clears == 1);
}

static void test_reports(void)
{
    setup(0x5d, true);
    bring_up();
    unsigned clears = model.status_clears;

    /* Nothing latched: one status read, no clear. */
    unsigned transfers = fake.i2c_transfers;
    assert(drv->handle(&dev) == 0);
    assert(fake.event_count == 0 && fake.i2c_transfers == transfers + 1);
    assert(model.status_clears == clears);

    /* One contact: status+point in one read, then the clear. */
    model_frame((const struct point[]){ { 0, 100, 200 } }, 1);
    transfers = fake.i2c_transfers;
    unsigned starts = repeated_starts;
    assert(drv->handle(&dev) == 0);
    assert(fake.i2c_transfers == transfers + 2 && repeated_starts == starts + 1);
    assert(fake.event_count == 1);
    fake_expect_event(0, 0, KSHIM_TOUCH_CONTACT_ENTER, 100, 200);
    assert(*reg(0x814e) == 0 && model.status_clears == clears + 1);

    /* Three contacts: second read for points 2..3. */
    fake_events_clear();
    model_frame((const struct point[]){ { 0, 110, 210 }, { 1, 500, 600 }, { 2, 1439, 2559 } }, 3);
    transfers = fake.i2c_transfers;
    assert(drv->handle(&dev) == 0);
    assert(fake.i2c_transfers == transfers + 3);
    assert(fake.event_count == 3);
    fake_expect_event(0, 0, KSHIM_TOUCH_CONTACT_MOVE, 110, 210);
    fake_expect_event(1, 1, KSHIM_TOUCH_CONTACT_ENTER, 500, 600);
    fake_expect_event(2, 2, KSHIM_TOUCH_CONTACT_ENTER, 1439, 2559);

    /* Contact 0 lifts: it leaves at its last position. */
    fake_events_clear();
    model_frame((const struct point[]){ { 1, 505, 605 }, { 2, 1430, 2550 } }, 2);
    assert(drv->handle(&dev) == 0);
    assert(fake.event_count == 3);
    fake_expect_event(0, 1, KSHIM_TOUCH_CONTACT_MOVE, 505, 605);
    fake_expect_event(1, 2, KSHIM_TOUCH_CONTACT_MOVE, 1430, 2550);
    fake_expect_event(2, 0, KSHIM_TOUCH_CONTACT_LEAVE, 110, 210);

    /* Ready with zero points: everything lifts. */
    fake_events_clear();
    model_frame(NULL, 0);
    assert(drv->handle(&dev) == 0);
    assert(fake.event_count == 2);
    fake_expect_event(0, 1, KSHIM_TOUCH_CONTACT_LEAVE, 505, 605);
    fake_expect_event(1, 2, KSHIM_TOUCH_CONTACT_LEAVE, 1430, 2550);
    assert(*reg(0x814e) == 0);

    /* More points than configured: protocol error, buffer still released. */
    fake_events_clear();
    *reg(0x814e) = 0x87;
    clears = model.status_clears;
    assert(drv->handle(&dev) == KSHIM_IO_ERR_PROTOCOL);
    assert(fake.event_count == 0 && model.status_clears == clears + 1);

    /* A failed status read reports nothing and leaves the buffer alone. */
    model_frame((const struct point[]){ { 3, 1, 2 } }, 1);
    fake.fail_bus = ERROR_TIMEOUT;
    assert(drv->handle(&dev) == ERROR_TIMEOUT);
    assert(fake.event_count == 0 && *reg(0x814e) == 0x81);
    assert(drv->handle(&dev) == 0);
    fake_expect_event(0, 3, KSHIM_TOUCH_CONTACT_ENTER, 1, 2);
}

/* UEFI left it powered, addressed and running: no reset sequence. */
static void test_inherit(void)
{
    setup(0x5d, true);
    l14b.on = l14b.inherited_on = true;
    model.powered = true;
    model.power_us = 0;
    fake.reset_driven = true;
    fake.reset_physical = true;
    model.rstb = true;
    model.address = 0x5d;
    fake.now_us = 1000000;
    bring_up();
    assert(model.address_selects == 0);
    assert(l14b.enables == 0 && l14b.refs == 2);
    assert(fake.reset_driven && fake.reset_physical);
    assert(!fake.int_driven);
    assert(dev.max_x == 1440);

    /* Inherited at the other address: answers there, so a cold select
     * moves it to the DT address. */
    setup(0x5d, true);
    l14b.on = l14b.inherited_on = true;
    model.powered = true;
    fake.reset_driven = true;
    fake.reset_physical = true;
    model.rstb = true;
    model.address = 0x14;
    fake.now_us = 1000000;
    bring_up();
    assert(model.address_selects == 1 && model.address == 0x5d && model.violations == 0);

    /* Rails on but RSTB held low by the pad pull: cold path. */
    setup(0x14, true);
    l14b.on = l14b.inherited_on = true;
    model.powered = true;
    fake.now_us = 1000000;
    bring_up();
    assert(model.address_selects == 1 && model.address == 0x14 && model.violations == 0);
}

/* No IRQ GPIO to drive: the address follows the INT pull, and probe finds
 * the controller at the other address. */
static void test_no_irq_gpio(void)
{
    setup(0x14, false);
    model.int_pull_high = false;
    bring_up();
    assert(model.address == 0x5d);
    assert(dev.i2c.addr == 0x5d);
    assert(model.violations == 0);

    /* No reset line either: power-on reset, then the same fallback. */
    setup(0x5d, false);
    dev.has_reset = false;
    model.rstb = true; /* RSTB strapped high on the board */
    model.int_pull_high = true;
    bring_up();
    assert(model.address == 0x14 && dev.i2c.addr == 0x14);
}

static void test_config(void)
{
    /* Configured for a low-level INT: the chip's waveform wins over DT. */
    setup(0x5d, true);
    *reg(0x804d) = 0x02;
    config_checksum();
    bring_up();
    assert(dev.irq_trigger == KSHIM_IRQ_TYPE_LEVEL_LOW);

    /* Bad configuration checksum: product resolution, 5 points. */
    setup(0x5d, true);
    put16(0x8048, 9999);
    *reg(0x804c) = 9;
    *reg(0x804d) = 0x03;
    bring_up();
    assert(dev.max_x == 1440 && dev.max_y == 2560 && dev.max_contacts == 5);
    assert(dev.irq_trigger == KSHIM_IRQ_TYPE_EDGE_FALLING);

    /* Configuration read fails: product resolution, probe still succeeds. */
    setup(0x5d, true);
    assert(drv->power_on(&dev) == 0);
    fake.fail_bus = ERROR_TIMEOUT;
    fake.fail_skip = 1; /* the product block read passes */
    assert(drv->probe(&dev) == 0);
    assert(dev.max_x == 1440 && dev.max_y == 2560 && dev.max_contacts == 5);

    /* DT size given: the framework's range stays. */
    setup(0x5d, true);
    dev.props.size_x = 1080;
    dev.props.size_y = 1920;
    dev.max_x = 1080;
    dev.max_y = 1920;
    bring_up();
    assert(dev.max_x == 1080 && dev.max_y == 1920);

    /* An unexpected product ID still probes (GT9xx register map). */
    setup(0x5d, true);
    memcpy(reg(0x8140), "9147", 4);
    bring_up();

    /* Nothing answers at either address. */
    setup(0x5d, true);
    assert(drv->power_on(&dev) == 0);
    model.powered = false;
    assert(drv->probe(&dev) == KSHIM_IO_ERR_NACK);
    assert(dev.i2c.addr == 0x5d);
}

static void test_pending_and_quiesce(void)
{
    setup(0x5d, true);
    bring_up();
    assert(!drv->pending(&dev));
    delay(9000);
    assert(!drv->pending(&dev));
    delay(1000);
    assert(drv->pending(&dev));
    assert(drv->handle(&dev) == 0);
    assert(!drv->pending(&dev));

    model_frame((const struct point[]){ { 0, 1, 1 } }, 1);
    unsigned clears = model.status_clears;
    fake_events_clear();
    assert(drv->quiesce(&dev) == 0);
    assert(model.status_clears == clears + 1 && fake.event_count == 0);
    /* Contacts were forgotten, so a later frame enters again. */
    model_frame((const struct point[]){ { 0, 1, 1 } }, 1);
    assert(drv->handle(&dev) == 0);
    fake_expect_event(0, 0, KSHIM_TOUCH_CONTACT_ENTER, 1, 1);

    /* Quiesce never blocks the handoff, even with the bus dead. */
    model.powered = false;
    assert(drv->quiesce(&dev) == 0);
    assert(drv->quiesce(&dev) == 0);
    /* Not probed: no bus access. */
    unsigned transfers = fake.i2c_transfers;
    dev.probed = false;
    assert(drv->quiesce(&dev) == 0 && fake.i2c_transfers == transfers);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    test_driver_object();
    test_address_select(0x5d);
    test_address_select(0x14);
    test_reports();
    test_inherit();
    test_no_irq_gpio();
    test_config();
    test_pending_and_quiesce();
    printf("touch_goodix_gt911_test: PASS\n");
    return 0;
}
