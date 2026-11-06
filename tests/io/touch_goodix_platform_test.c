/* SPDX-License-Identifier: MIT */
/*
 * The Goodix drivers and the touch framework over the real platform layer:
 * kernel/drivers/gpio (TLMM registry and pinctrl), kernel/drivers/regulator
 * (RPMh ldob14 over the RSC/TCS and CMD DB model) and the SoC tables, on the
 * mainline SM8650 fixtures. touch_goodix_framework_test checks the same
 * paths over touch-st's GPIO/regulator fakes; here every pin and rail
 * change is a TLMM register or an RPMh vote, and the chip models see only
 * those:
 *  - Pocket S2 GT911 (I2C, reset TLMM 161 goodix,gt9xx active-high, INT 162
 *    falling): cold start (RSTB low, INT low, rail, RSTB rising latches
 *    0x5d), the pinctrl states in the TLMM CFG registers, HPM vote,
 *    interrupt-driven report, handoff (RSTB released to its pull-down before
 *    the rail vote drops, never driven low, every pad and vote back as
 *    found), and an inherited running controller kept running at 0x5d;
 *  - QRD8650 GT9916 (SPI, reset 161 active-low, INT 162 level-low): reset
 *    held while ldob14 comes up, released after, report, handoff with reset
 *    asserted before the rail drops.
 * One scenario per process (cold, inherited, qrd).
 * The I2C/SPI clients and the TLMM interrupt API are still touch_fakes.c
 * (TOUCH_REAL_PLATFORM=1 drops its GPIO/pinctrl/regulator fakes); the QUP
 * and GENI controllers have their own register-model tests.
 */
#include "platform_fake.h"

#include <kshim/gpio.h>
#include <kshim/irq.h>
#include <kshim/regulator.h>
#include <kshim/soc_tables.h>
#include <kshim/touch_driver.h>
#include "../../kernel/drivers/gpio/internal.h"

/* platform_fake.h owns FakeLog (a register write log); touch_fakes.h's text
 * log is not used here. */
#define FakeLog TouchFakeTextLog
#include "touch_fakes.h"
#undef FakeLog

#define TLMM 0xf100000UL
#define DRV2 0x17a20000UL
#define CMD_DB 0x81c60000UL
#define L14B 0x40d00U
#define RESET_PIN 161U
#define INT_PIN 162U

static uintptr_t cfg(unsigned pin) { return TLMM + pin * 0x1000UL; }
static uintptr_t io(unsigned pin) { return cfg(pin) + 4; }

static kshim_touch_event_t Events[16];
static unsigned EventCount;
static void emit(void *context, const kshim_touch_event_t *event)
{
    (void)context;
    assert(EventCount < 16);
    Events[EventCount++] = *event;
}

static void le16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

/* ---- Pads and the rail, as the hardware sees them ------------------------- */

static bool rail_on(void) { return fake_rpmh_get(L14B + 4) & 1U; }
static bool driven(unsigned pin) { return (fake_peek(cfg(pin)) & KSHIM_TLMM_CFG_OE) != 0; }

/* Physical level: the output when OE is set, else the device's drive or the
 * pad bias (pull 1 = down, 3 = up). */
static int ChipInt = 1;   /* INT as driven by the chip when the pad is an input */
static bool level(unsigned pin)
{
    uint32_t c = fake_peek(cfg(pin));
    if (c & KSHIM_TLMM_CFG_OE) return (fake_peek(io(pin)) & KSHIM_TLMM_IO_OUT) != 0;
    if (pin == INT_PIN && ChipInt >= 0) return ChipInt;
    return (c & KSHIM_TLMM_CFG_PULL_MASK) == 3U;
}

static void (*PinWatch)(unsigned pin, bool level);
static bool Level[2];
static unsigned RailDropsWithResetHigh;
static bool RailWasOn;

/* After every register write: refresh the IN bits and report pad edges
 * and the rail's state changes to the chip model. */
static void hook(uintptr_t a, uint32_t v)
{
    (void)a; (void)v;
    for (unsigned i = 0; i < 2; ++i) {
        unsigned pin = i ? INT_PIN : RESET_PIN;
        bool now = level(pin);
        uint32_t value = fake_peek(io(pin));
        fake_poke(io(pin), now ? value | KSHIM_TLMM_IO_IN : value & ~KSHIM_TLMM_IO_IN);
        if (now != Level[i]) {
            Level[i] = now;
            if (PinWatch) PinWatch(pin, now);
        }
    }
    bool on = rail_on();
    if (RailWasOn && !on && driven(RESET_PIN) && level(RESET_PIN)) ++RailDropsWithResetHigh;
    RailWasOn = on;
}

static void world(const char *fixture, void **fdt)
{
    fake_reset();
    fake_touch_reset();
    kshim_platform_set_io(&FakeIo);
    static const char *const names[] = {"ldob14"};
    static const uint32_t addresses[] = {L14B};
    fake_cmd_db(CMD_DB, names, addresses, 1);
    fake_rsc_setup(DRV2, 0xd00, 2, 0, 3, 7);
    *fdt = fake_fixture(fixture);
    assert(!kshim_gpio_init(*fdt) && !kshim_regulator_init(*fdt));
    PinWatch = NULL;
    RailDropsWithResetHigh = 0;
    RailWasOn = false;
    EventCount = 0;
    ChipInt = 1;
    FakeHook = hook;
}

static void settle(void)
{
    Level[0] = level(RESET_PIN);
    Level[1] = level(INT_PIN);
    hook(0, 0);
}

/* ---- GT911 model --------------------------------------------------------- */

static struct {
    uint8_t mem[0x200];
    uint16_t address;
    bool rstb;
    unsigned latches;
} G;

static uint8_t *gt911_reg(uint16_t reg)
{
    assert(reg >= 0x8000 && reg < 0x8200);
    return &G.mem[reg - 0x8000];
}

static void gt911_pin(unsigned pin, bool high)
{
    if (pin != RESET_PIN) return;
    G.rstb = high;
    if (!high || !rail_on()) { G.address = 0; return; }
    /* INT sampled at the RSTB rising edge: low selects 0x5d. */
    G.address = level(INT_PIN) ? 0x14 : 0x5d;
    ++G.latches;
}

static int gt911_i2c(uint16_t address, const uint8_t *tx, size_t tx_length, uint8_t *rx, size_t rx_length)
{
    if (!rail_on() || !G.rstb || address != G.address) return KSHIM_IO_ERR_NACK;
    uint16_t reg = (uint16_t)(tx[0] << 8 | tx[1]);
    if (rx_length) memcpy(rx, gt911_reg(reg), rx_length);
    else memcpy(gt911_reg(reg), tx + 2, tx_length - 2);
    return 0;
}

static void gt911_init(void)
{
    memset(&G, 0, sizeof(G));
    memcpy(gt911_reg(0x8140), "911\0", 4);
    le16(gt911_reg(0x8146), 720);
    le16(gt911_reg(0x8148), 1280);
    uint8_t *config = gt911_reg(0x8047);
    config[0] = 0x41;
    le16(config + 1, 1440);
    le16(config + 3, 2560);
    config[5] = 5;
    config[6] = 0x01;            /* INT falling, as the DT */
    uint8_t sum = 0;
    for (unsigned i = 0; i < 184; ++i) sum = (uint8_t)(sum + config[i]);
    config[184] = (uint8_t)(0U - sum);
}

static void gt911_touch(uint16_t x, uint16_t y)
{
    uint8_t *s = gt911_reg(0x814e);
    s[0] = 0x81;
    s[1] = 0;
    le16(s + 2, x);
    le16(s + 4, y);
}

static void pocket_s2_cold(void)
{
    void *f;
    world("io/sm8650-ayaneo-pocket-s2.dtb", &f);
    gt911_init();
    /* As found: rail off (observed own-DRV request zero), RSTB an input on
     * its pull-down, INT an input the (unpowered) chip leaves high. */
    fake_poke(cfg(RESET_PIN), 0x1);
    fake_poke(cfg(INT_PIN), 0x0);
    const uint32_t cfg161 = fake_peek(cfg(RESET_PIN)), cfg162 = fake_peek(cfg(INT_PIN));
    settle();
    PinWatch = gt911_pin;
    FakeI2cDevice = gt911_i2c;

    assert(!kshim_touch_framework_start(f, 1440, 2560, emit, NULL));
    const struct kshim_touch_dev *dev = kshim_touch_active();
    assert(dev && dev->driver == &kshim_touch_driver_gt911);
    /* RPMh: ldob14 at 3200 mV, HPM (regulator-initial-mode), enabled. */
    assert(rail_on() && fake_rpmh_get(L14B) == 3200 && fake_rpmh_get(L14B + 8) == 7);
    /* Address select through the real TLMM registers: 0x5d, once. */
    assert(G.latches == 1 && G.address == 0x5d && dev->i2c.addr == 0x5d);
    /* Pinctrl states in CFG: gpio function, 8 mA (DRV 3); RSTB driven high
     * over its pull-down, INT an input with no bias. */
    uint32_t c161 = fake_peek(cfg(RESET_PIN)), c162 = fake_peek(cfg(INT_PIN));
    assert((c161 & KSHIM_TLMM_CFG_FUNC_MASK) == 0 && ((c161 & KSHIM_TLMM_CFG_DRV_MASK) >> 6) == 3);
    assert((c161 & KSHIM_TLMM_CFG_PULL_MASK) == 1 && (c161 & KSHIM_TLMM_CFG_OE) && level(RESET_PIN));
    assert(((c162 & KSHIM_TLMM_CFG_DRV_MASK) >> 6) == 3 && !(c162 & KSHIM_TLMM_CFG_PULL_MASK));
    assert(!(c162 & KSHIM_TLMM_CFG_OE));
    assert(FakeTlmmIrq.requested && FakeTlmmIrq.pin == INT_PIN &&
           FakeTlmmIrq.trigger == KSHIM_IRQ_TYPE_EDGE_FALLING && dev->irq_live);
    assert(!kshim_touch_framework_poll());

    gt911_touch(700, 1200);
    assert(fake_irq_fire() && !kshim_touch_framework_poll());
    assert(EventCount == 1 && Events[0].type == KSHIM_TOUCH_EVENT_PRESS && Events[0].x == 700);

    /* Handoff: RSTB is released to its pull-down before the vote drops (no
     * high level into the unpowered chip) and never driven low; the rail
     * vote is back to the observed request. */
    assert(!kshim_touch_quiesce_all());
    assert(!rail_on() && !fake_rpmh_get(L14B) && !fake_rpmh_get(L14B + 8));
    assert(RailDropsWithResetHigh == 0 && !driven(RESET_PIN) && !level(RESET_PIN));
    /* Step 9: the pinctrl snapshots restore both pads exactly. */
    assert(!kshim_regulator_restore_all() && !kshim_gpio_restore_all());
    assert(fake_peek(cfg(RESET_PIN)) == cfg161 && fake_peek(cfg(INT_PIN)) == cfg162);
    free(f);
    puts("pocket-s2 GT911 cold: ok");
}

static void pocket_s2_inherited(void)
{
    void *f;
    world("io/sm8650-ayaneo-pocket-s2.dtb", &f);
    gt911_init();
    /* UEFI left ldob14 on at 3200 mV HPM, RSTB driven high with the pinctrl
     * state applied, the chip addressed at 0x5d. */
    fake_rpmh_set(L14B + 0, 3200);
    fake_rpmh_set(L14B + 4, 1);
    fake_rpmh_set(L14B + 8, 7);
    fake_poke(cfg(RESET_PIN), 0x1 | (3U << 6) | KSHIM_TLMM_CFG_OE);
    fake_poke(io(RESET_PIN), KSHIM_TLMM_IO_OUT);
    fake_poke(cfg(INT_PIN), 3U << 6);
    const uint32_t cfg161 = fake_peek(cfg(RESET_PIN));
    settle();
    G.rstb = true;
    G.address = 0x5d;
    PinWatch = gt911_pin;
    FakeI2cDevice = gt911_i2c;
    unsigned votes = FakeRsc.writes;

    assert(!kshim_touch_framework_start(f, 1440, 2560, emit, NULL));
    assert(kshim_touch_active() && G.latches == 0 && G.rstb && G.address == 0x5d);
    assert(FakeRsc.writes == votes);              /* inherited-on rail: no vote */
    gt911_touch(10, 20);
    assert(fake_irq_fire() && !kshim_touch_framework_poll() && EventCount == 1);

    assert(!kshim_touch_quiesce_all());
    /* Never reset: RSTB stayed driven high, the chip still answers at 0x5d. */
    assert(G.rstb && G.address == 0x5d && G.latches == 0 && driven(RESET_PIN) && level(RESET_PIN));
    assert(rail_on() && FakeRsc.writes == votes);
    assert(!kshim_regulator_restore_all() && !kshim_gpio_restore_all());
    assert(fake_peek(cfg(RESET_PIN)) == cfg161 && G.rstb && G.address == 0x5d);
    free(f);
    puts("pocket-s2 GT911 inherited: ok");
}

/* ---- GT9916 model (Berlin SPI) -------------------------------------------- */

#define BRL_TOUCH 0x10308U
static struct {
    bool rstb;                      /* physical, high = running */
    uint64_t release_us;
    uint8_t bootoption[0x14], version[28], info[1024], touch[128];
    unsigned resets, releases_with_rail_off;
} B;

static void brl_checksum(uint8_t *data, size_t length)
{
    uint16_t sum = 0;
    for (size_t i = 0; i + 2 < length; ++i) sum = (uint16_t)(sum + data[i]);
    le16(&data[length - 2], sum);
}

static void brl_int(void) { ChipInt = !(rail_on() && B.rstb && B.touch[0]); }

static void brl_pin(unsigned pin, bool high)
{
    if (pin != RESET_PIN || high == B.rstb) return;
    B.rstb = high;
    if (high) {
        if (!rail_on()) ++B.releases_with_rail_off;
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
    assert(!"GT9916 model: access outside the map");
    return NULL;
}

static int brl_spi(const struct kshim_spi_device *device, const struct kshim_spi_xfer *xfers, size_t count)
{
    static uint8_t mosi[2048], miso[2048];
    size_t total = 0;
    assert(device->chip_select == 0);
    for (size_t i = 0; i < count; ++i) {
        if (xfers[i].tx) memcpy(&mosi[total], xfers[i].tx, xfers[i].len);
        else memset(&mosi[total], 0, xfers[i].len);
        total += xfers[i].len;
    }
    memset(miso, 0xff, total);
    uint32_t addr = (uint32_t)mosi[1] << 24 | (uint32_t)mosi[2] << 16 | (uint32_t)mosi[3] << 8 | mosi[4];
    bool up = rail_on() && B.rstb && fake_now_us() - B.release_us >= 4000U;
    if (up && mosi[0] == 0xf0) {
        memcpy(brl_region(addr, total - 5), &mosi[5], total - 5);
    } else if (up && mosi[0] == 0xf1) {
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
    hook(0, 0);
    return 0;
}

static void brl_init(void)
{
    memset(&B, 0, sizeof(B));
    memcpy(B.version, "BERLIN", 6);
    B.version[6] = 1; B.version[7] = 2; B.version[8] = 3;
    memcpy(&B.version[10], "9916R\0\0\0", 8);
    B.version[18] = 0x01; B.version[20] = 0x0b; B.version[21] = 0x03; B.version[23] = 2;
    brl_checksum(B.version, sizeof(B.version));
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
    le16(&info[o], 0x0174); le16(&info[o + 2], 0x0001);
    le16(&info[o + 44], (uint16_t)BRL_TOUCH); le16(&info[o + 46], (uint16_t)(BRL_TOUCH >> 16));
    le16(&info[o + 48], 8);
    le16(&info[o + 50], 8);
    o += 102;
    le16(info, (uint16_t)o);
    brl_checksum(info, o);
}

static void brl_touch(uint8_t id, uint16_t x, uint16_t y)
{
    uint8_t *e = B.touch;
    memset(e, 0, sizeof(B.touch));
    e[0] = 0x80;
    e[2] = 1;
    brl_checksum(e, 8);
    uint8_t *p = &e[8];
    p[0] = (uint8_t)(id << 4);
    le16(&p[2], x);
    le16(&p[4], y);
    le16(&p[6], 40);
    brl_checksum(&e[8], 8 + 2);
    brl_int();
    hook(0, 0);
}

static void qrd8650_cold(void)
{
    void *f;
    world("io/sm8650-qrd.dtb", &f);
    brl_init();
    /* As found: rail off, reset (161) and INT (162) inputs on their pull-ups. */
    fake_poke(cfg(RESET_PIN), 0x3);
    fake_poke(cfg(INT_PIN), 0x3);
    const uint32_t cfg161 = fake_peek(cfg(RESET_PIN)), cfg162 = fake_peek(cfg(INT_PIN));
    settle();
    B.rstb = level(RESET_PIN);
    brl_int();
    settle();
    PinWatch = brl_pin;
    FakeSpiDevice = brl_spi;

    assert(!kshim_touch_framework_start(f, 1080, 2400, emit, NULL));
    const struct kshim_touch_dev *dev = kshim_touch_active();
    assert(dev && dev->driver == &kshim_touch_driver_gt9916 && dev->bus == KSHIM_TOUCH_BUS_SPI);
    /* Reset (active low) driven low before the vote, released once after
     * it: the chip never came out of reset unpowered. */
    assert(rail_on() && B.resets == 1 && B.releases_with_rail_off == 0 && B.rstb);
    assert(driven(RESET_PIN) && level(RESET_PIN) && B.bootoption[0] == 0xaa);
    assert(FakeTlmmIrq.requested && FakeTlmmIrq.trigger == KSHIM_IRQ_TYPE_LEVEL_LOW && dev->irq_live);
    assert(!kshim_touch_framework_poll());

    brl_touch(2, 500, 1800);
    assert(!level(INT_PIN));
    assert(fake_irq_fire() && !kshim_touch_framework_poll());
    assert(!B.touch[0] && level(INT_PIN) && !FakeTlmmIrq.masked);
    assert(EventCount == 1 && Events[0].type == KSHIM_TOUCH_EVENT_PRESS && Events[0].x == 500);

    /* Handoff: reset asserted (pad low) while the rail drops, then every
     * pad and the vote back as found. */
    assert(!kshim_touch_quiesce_all());
    assert(!rail_on() && RailDropsWithResetHigh == 0 && !B.rstb);
    assert(!kshim_regulator_restore_all() && !kshim_gpio_restore_all());
    assert(fake_peek(cfg(RESET_PIN)) == cfg161 && fake_peek(cfg(INT_PIN)) == cfg162);
    free(f);
    puts("qrd8650 GT9916 cold: ok");
}

/* One scenario per process: the regulator and GPIO registries keep their
 * objects for the whole boot, as on hardware. */
int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *which = argc > 1 ? argv[1] : "cold";
    if (!strcmp(which, "cold")) pocket_s2_cold();
    else if (!strcmp(which, "inherited")) pocket_s2_inherited();
    else if (!strcmp(which, "qrd")) qrd8650_cold();
    else return 2;
    puts("touch_goodix_platform_test: ok");
    return 0;
}
