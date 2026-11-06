/* SPDX-License-Identifier: MIT */
/*
 * ST touch drivers on the framework: the FTM5 driver (protocol core
 * kernel/drivers/st_ftm5.c) on the real andromeda DTB and the new FTM4
 * driver on the kona binding fixture and, when present, the real Android
 * SM8250 HDK DTB (KSHIM_HDK8250_DTB). Byte-level I2C models of both
 * controllers: reset/boot, NACK while booting, ready event, chip ID, system
 * info, scan/sense on and off, FIFO events with the level IRQ line, and
 * spontaneous controller resets.
 *
 * Built twice: TOUCH_FAKE_TLMM=1 (touch_st_test) fakes the TLMM interrupt
 * API; TOUCH_FAKE_TLMM=0 (touch_st_irq_test) runs the andromeda FTM5 case
 * through the real TLMM cascade (kernel/drivers/irqchip/tlmm.c) over the
 * irq_fake.c register model and fake GIC.
 */
#include "touch_fakes.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dt.h>
#include <qcom_dt.h>
#include <st_ftm5.h>
#if !TOUCH_FAKE_TLMM
#include "../irq_fake.h"
#endif

/* ---- Fixtures --------------------------------------------------------------- */

static void *load(const char *path, size_t extra)
{
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    assert(!fseek(file, 0, SEEK_END));
    long size = ftell(file);
    assert(size > 0 && !fseek(file, 0, SEEK_SET));
    void *data = aligned_alloc(8, ((size_t)size + extra + 7) & ~(size_t)7);
    assert(data && fread(data, 1, (size_t)size, file) == (size_t)size);
    fclose(file);
    assert(!fdt_open_into(data, data, (int)((size_t)size + extra)));
    assert(!dt_validate(data, (size_t)size + extra));
    return data;
}

static kshim_touch_event_t Events[128];
static unsigned EventCount;
static void emit(void *context, const kshim_touch_event_t *event)
{
    (void)context;
    assert(EventCount < 128);
    Events[EventCount++] = *event;
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

static const char *name_of(const void *f, int node) { return fdt_get_name(f, node, NULL); }

static void start_clean(void)
{
    assert(!kshim_touch_quiesce_all());
    fake_touch_reset();
    EventCount = 0;
}

/* ---- IRQ line ----------------------------------------------------------------- */

static uint32_t LinePin;
#if !TOUCH_FAKE_TLMM
static uintptr_t LineControl;
static const void *LineFdt;
/* The IRQ GPIO level comes from the TLMM register model. */
static bool tlmm_level(const void *fdt, const struct kshim_gpio *gpio, bool *level)
{
    uintptr_t control;
    if (kshim_dt_gpio(fdt, gpio->provider, gpio->pin, &control, NULL, NULL)) return true;
    *level = FakeIo.read32(NULL, control + 4) & 1U;
    return false;
}
#endif
static void line_set(bool level)
{
#if TOUCH_FAKE_TLMM
    fake_pin_input(LinePin, level);
#else
    if (LineControl) fake_tlmm_set(LineControl, level);
#endif
}

/* Deliver the touch interrupt the way the platform would. */
static bool fire(void)
{
#if TOUCH_FAKE_TLMM
    return fake_irq_fire();
#else
    FakeBusForbidden = true;
    int delivered = fake_gic_run();
    FakeBusForbidden = false;
    return delivered > 0;
#endif
}

/* ---- FTM5 model (andromeda: I2C 0x49, reset GPIO54, IRQ GPIO122) ------------ */

static struct {
    bool booted, scanning;
    unsigned nack, scan_on, scan_off, transfers;
    uint8_t chip[2];
    uint8_t fifo[40][8];
    unsigned count;
} M5;

static void m5_line(void) { line_set(M5.count == 0); }   /* active low */

static void m5_queue(const uint8_t event[8])
{
    assert(M5.count < 40);
    memcpy(M5.fifo[M5.count++], event, 8);
    m5_line();
}

static void m5_pointer(uint8_t type, uint8_t id, unsigned x, unsigned y)
{
    const uint8_t e[8] = {type, (uint8_t)((id << 4) | 1U), (uint8_t)x,
                          (uint8_t)(((x >> 8) & 0xfU) | ((y & 0xfU) << 4)), (uint8_t)(y >> 4)};
    m5_queue(e);
}

static void m5_reset_hook(uint32_t pin, bool level)
{
    if (pin != 54) return;
    if (!level) { M5.booted = false; M5.count = 0; M5.scanning = false; m5_line(); return; }
    /* Out of reset only with both rails up (GPIO37 gate and ldoc1). */
    assert(fake_regulator_named("disp_vddio_vreg")->refs && fake_regulator_named("pm8150l_l1")->refs);
    M5.booted = true;
    const uint8_t ready[8] = {0x03};
    m5_queue(ready);
}

static int m5_i2c(uint16_t address, const uint8_t *tx, size_t tx_length, uint8_t *rx, size_t rx_length)
{
    assert(address == 0x49 && tx_length);
    ++M5.transfers;
    if (!M5.booted) return KSHIM_IO_ERR_NACK;
    if (M5.nack) { --M5.nack; return KSHIM_IO_ERR_NACK; }
    switch (tx[0]) {
    case 0x86:   /* FIFO: remaining count in byte 7 of each event */
        assert(tx_length == 1 && rx_length && rx_length % 8 == 0);
        memset(rx, 0, rx_length);
        for (size_t at = 0; at < rx_length && M5.count; at += 8) {
            memcpy(rx + at, M5.fifo[0], 8);
            rx[at + 7] = (uint8_t)(M5.count - 1);
            memmove(M5.fifo, M5.fifo + 1, --M5.count * 8U);
        }
        break;
    case 0xfa:   /* HW read 0x20000000: chip ID */
        assert(tx_length == 5 && tx[1] == 0x20 && rx_length == 2);
        memcpy(rx, M5.chip, 2);
        break;
    case 0xa6:   /* framebuffer 0x0000: system info */
        assert(tx_length == 3 && rx_length == KSHIM_FTM5_SYSTEM_INFO_SIZE);
        memset(rx, 0, rx_length);
        rx[0] = 0xa5; rx[1] = 0x01;
        rx[16] = 0x34; rx[17] = 0x12; rx[20] = 0x78; rx[21] = 0x56;
        rx[80] = 0x38; rx[81] = 0x04; rx[82] = 0x24; rx[83] = 0x09;   /* 1080 x 2340 */
        break;
    case 0xa0:   /* scan mode */
        assert(tx_length == 3 && !rx_length && tx[1] == 0);
        if (tx[2]) { M5.scanning = true; ++M5.scan_on; } else { M5.scanning = false; ++M5.scan_off; }
        break;
    default:
        assert(!"unexpected FTM5 command");
    }
    m5_line();
    return 0;
}

static void m5_setup(void)
{
    memset(&M5, 0, sizeof(M5));
    M5.chip[0] = 0x36; M5.chip[1] = 0x39;
    FakeI2cDevice = m5_i2c;
    FakePinHook = m5_reset_hook;
    LinePin = 122;
}

static void test_ftm5_andromeda(void *f)
{
    start_clean();
    m5_setup();
#if !TOUCH_FAKE_TLMM
    fake_reset();
    fake_gic_line(FAKE_TLMM_SUMMARY, fake_tlmm_line);
    LineControl = 0x03d7a000U;   /* GPIO122 control (CrAndroidPin SM8150) */
    LineFdt = f;
    (void)LineFdt;
    fake_tlmm_pin(LineControl, true);
    FakePinRead = tlmm_level;
#endif
    M5.nack = 3;                 /* still booting after the reset release */
    assert(!kshim_touch_framework_start(f, 1080, 2340, emit, NULL));
    const struct kshim_touch_dev *dev = kshim_touch_active();
    assert(dev && dev->driver == &kshim_touch_driver_ftm5);
    assert(!strcmp(name_of(f, dev->node), "fts@49"));
    assert(!strcmp(name_of(f, fdt_parent_offset(f, dev->node)), "i2c@0xc80000"));
    assert(dev->bus == KSHIM_TOUCH_BUS_I2C && dev->i2c.addr == 0x49);
    /* fts,reset-gpio = <&tlmm 54 0> is a raw level: reset is pulsed low. */
    assert(dev->has_reset && dev->reset_gpio.pin == 54 && dev->reset_gpio.active_low);
    /* interrupts = <122 0x2008>: level low on the IRQ GPIO itself. */
    assert(dev->has_irq && dev->irq_pin == 122 && dev->irq_trigger == KSHIM_IRQ_TYPE_LEVEL_LOW);
    assert(dev->has_irq_gpio && dev->irq_gpio.pin == 122 && dev->irq_gpio.active_low);
    assert(dev->supply_count == 2 && !strcmp(kshim_regulator_name(dev->supplies[0]), "disp_vddio_vreg") &&
           !strcmp(kshim_regulator_name(dev->supplies[1]), "pm8150l_l1"));
    assert(dev->props.size_x == 1080 && dev->props.size_y == 2340 && !dev->props.invert_x &&
           !dev->props.invert_y && !dev->props.swap_xy);
    assert(dev->max_x == 1080 && dev->max_y == 2340 && dev->irq_live);
    /* andromeda-provenance.json: gate, ldoc1, pull-ups once powered,
     * reset low >= 10 ms, then the controller boots. */
    assert(fake_log_order((const char *const[]){"i2c-probe=i2c@0xc80000", "gpio54=0",
        "disp_vddio_vreg=on", "pm8150l_l1=on", "pinctrl=pmx_ts_active", "gpio122=in",
        "delay=10000", "gpio54=1", NULL}));
    assert(M5.scan_on == 1 && M5.scanning && !M5.nack);

    /* Contacts through the interrupt; the handler never reaches the bus. */
    assert(!kshim_touch_framework_poll());          /* the initial pass */
    m5_pointer(0x13, 0, 100, 200);
    m5_pointer(0x23, 0, 110, 210);
    assert(fire());
    assert(dev->irq_pending);
    assert(!kshim_touch_framework_poll());
    assert(EventCount == 2 && M5.count == 0);
    expect_event(0, KSHIM_TOUCH_EVENT_PRESS, 0, 100, 200);
    expect_event(1, KSHIM_TOUCH_EVENT_MOVE, 0, 110, 210);
#if !TOUCH_FAKE_TLMM
    assert(!fire());                                 /* line released after the read */
#endif
    m5_pointer(0x33, 0, 110, 210);
    assert(fire());
    assert(!kshim_touch_framework_poll());
    expect_event(2, KSHIM_TOUCH_EVENT_RELEASE, 0, 110, 210);

    /* Hover (type 5) lifts the contact like the vendor driver's BTN_TOUCH 0. */
    m5_pointer(0x13, 1, 5, 6);
    const uint8_t hover[8] = {0x23, (1U << 4) | 5U, 5, 0x60, 0};
    m5_queue(hover);
    assert(fire());
    assert(!kshim_touch_framework_poll());
    expect_event(3, KSHIM_TOUCH_EVENT_PRESS, 1, 5, 6);
    expect_event(4, KSHIM_TOUCH_EVENT_RELEASE, 1, 5, 6);

    /* Handoff: scanning stops, the interrupt is freed, rails and pins return. */
    fake_log_clear();
    assert(!kshim_touch_quiesce_all());
    assert(M5.scan_off == 1 && !M5.scanning);
    assert(fake_log_order((const char *const[]){"gpio54=0", "pm8150l_l1=off", "disp_vddio_vreg=off",
                                                "free122", "free54", NULL}));
#if TOUCH_FAKE_TLMM
    assert(FakeTlmmIrq.frees == 1 && !FakeTlmmIrq.requested);
#else
    /* tlmm.c gave the pin back: INTR_CFG and the status latch restored. */
    assert(fake_peek(LineControl + 8) == 0 && !FakeGic[FAKE_TLMM_SUMMARY].requested && !FakeIrqMasked);
#endif
    unsigned transfers = M5.transfers;
    assert(!kshim_touch_quiesce_all() && M5.transfers == transfers);
    printf("ftm5 andromeda: ok\n");
}

#if TOUCH_FAKE_TLMM
static void test_ftm5_failures(void *f)
{
    /* Wrong chip: probe fails, everything released, FTM4 does not take the
     * fts,* node either. */
    start_clean();
    m5_setup();
    M5.chip[1] = 0x70;
    assert(kshim_touch_framework_start(f, 1080, 2340, emit, NULL) == KSHIM_IO_ERR_NO_DEVICE);
    assert(!kshim_touch_active() && !fake_regulator_named("pm8150l_l1")->refs &&
           !fake_regulator_named("disp_vddio_vreg")->refs && !fake_pin(54)->refs && !fake_pin(122)->refs);
    assert(!M5.scan_on);

    /* A controller that never boots: bounded NACK retries, then the error. */
    start_clean();
    m5_setup();
    M5.nack = 1000;
    uint64_t before = fake_now_us();
    assert(kshim_touch_framework_start(f, 1080, 2340, emit, NULL) == KSHIM_IO_ERR_NACK);
    assert(fake_now_us() - before < 200000U);

    /* A spontaneous controller reset mid-run disables touch (legacy
     * behaviour): the contact is cancelled; stop still quiesces. */
    start_clean();
    m5_setup();
    assert(!kshim_touch_framework_start(f, 1080, 2340, emit, NULL));
    assert(!kshim_touch_framework_poll());
    m5_pointer(0x13, 0, 300, 400);
    assert(fire() && !kshim_touch_framework_poll());
    const uint8_t ready[8] = {0x03};
    m5_queue(ready);
    assert(fire());
    assert(kshim_touch_framework_poll() == KSHIM_IO_ERR_PROTOCOL);
    expect_event(EventCount - 1, KSHIM_TOUCH_EVENT_CANCEL, 0, 300, 400);
    assert(!kshim_touch_framework_poll());
    assert(!kshim_touch_quiesce_all() && M5.scan_off == 1);

    /* FTM5 polled without interrupts: the IRQ GPIO level gates the reads. */
    start_clean();
    m5_setup();
    FakeIrqReady = false;
    assert(!kshim_touch_framework_start(f, 1080, 2340, emit, NULL));
    unsigned transfers = M5.transfers;
    assert(!kshim_touch_framework_poll() && M5.transfers == transfers);
    m5_pointer(0x13, 2, 7, 8);
    assert(!kshim_touch_framework_poll() && M5.transfers == transfers + 1);
    expect_event(EventCount - 1, KSHIM_TOUCH_EVENT_PRESS, 2, 7, 8);
    assert(!kshim_touch_quiesce_all());
    printf("ftm5 failures: ok\n");
}

/* ---- FTM4 model (SM8250 HDK: I2C 0x49, reset GPIO38, IRQ GPIO39) -------------- */

static struct {
    bool booted, sense, ier, mute_ready;
    unsigned nack, sense_on, sense_off, ier_on, ier_off, flushes, system_resets, transfers;
    uint8_t chip[2];
    uint8_t fifo[80][8];
    unsigned count;
} M4;

static void m4_line(void) { line_set(!(M4.count && M4.ier)); }

static void m4_queue(const uint8_t event[8])
{
    assert(M4.count < 80);
    memcpy(M4.fifo[M4.count++], event, 8);
    m4_line();
}

/* ftsSoftware.h/fts.c layout: ID in byte 1 [3:0], X/Y 12-bit split. */
static void m4_pointer(uint8_t type, uint8_t id, unsigned x, unsigned y)
{
    const uint8_t e[8] = {type, (uint8_t)(0x10U | id), (uint8_t)(x >> 4), (uint8_t)(y >> 4),
                          (uint8_t)(((x & 0xfU) << 4) | (y & 0xfU))};
    m4_queue(e);
}

static void m4_boot(void)
{
    M4.booted = true;
    M4.sense = M4.ier = false;
    M4.count = 0;
    const uint8_t ready[8] = {0x10};
    if (!M4.mute_ready) m4_queue(ready);
}

static void m4_reset_hook(uint32_t pin, bool level)
{
    if (pin != 38) return;
    if (!level) { M4.booted = false; M4.count = 0; m4_line(); return; }
    m4_boot();
}

static int m4_i2c(uint16_t address, const uint8_t *tx, size_t tx_length, uint8_t *rx, size_t rx_length)
{
    assert(address == 0x49 && tx_length);
    ++M4.transfers;
    if (!M4.booted) return KSHIM_IO_ERR_NACK;
    if (M4.nack) { --M4.nack; return KSHIM_IO_ERR_NACK; }
    switch (tx[0]) {
    case 0x85:
        assert(tx_length == 1 && rx_length == 8);
        memset(rx, 0, 8);
        if (M4.count) { memcpy(rx, M4.fifo[0], 8); memmove(M4.fifo, M4.fifo + 1, --M4.count * 8U); }
        break;
    case 0xb6: {
        uint16_t reg = (uint16_t)((tx[1] << 8) | tx[2]);
        if (tx_length == 3) {               /* read: dummy byte first */
            assert(rx_length >= 2);
            memset(rx, 0xee, rx_length);
            if (reg == 0x0004) { assert(rx_length == 3); rx[1] = M4.chip[0]; rx[2] = M4.chip[1]; }
            else if (reg == 0x0008) { assert(rx_length == 3); rx[1] = 0x21; rx[2] = 0x43; }
            else assert(!"unexpected FTM4 register read");
        } else {
            assert(tx_length == 4 && !rx_length);
            if (reg == 0x0028 && tx[3] == 0x80) { ++M4.system_resets; m4_boot(); }
            else if (reg == 0x002c && tx[3] == 0x41) { M4.ier = true; ++M4.ier_on; }
            else if (reg == 0x002c && tx[3] == 0x00) { M4.ier = false; ++M4.ier_off; }
            else assert(!"unexpected FTM4 register write");
        }
        break;
    }
    case 0x93: assert(tx_length == 1); M4.sense = true; ++M4.sense_on; break;
    case 0x92: assert(tx_length == 1); M4.sense = false; ++M4.sense_off; break;
    case 0xa1: assert(tx_length == 1); M4.count = 0; ++M4.flushes; break;
    default:
        assert(!"unexpected FTM4 command");
    }
    m4_line();
    return 0;
}

static void m4_setup(void)
{
    memset(&M4, 0, sizeof(M4));
    M4.chip[0] = 0x36; M4.chip[1] = 0x70;
    FakeI2cDevice = m4_i2c;
    FakePinHook = m4_reset_hook;
    LinePin = 39;
}

/* Common FTM4 checks once probed on an HDK-like DT (flips on both axes). */
static void ftm4_session(const void *f)
{
    const struct kshim_touch_dev *dev = kshim_touch_active();
    assert(dev && dev->driver == &kshim_touch_driver_ftm4);
    assert(dev->bus == KSHIM_TOUCH_BUS_I2C && dev->i2c.addr == 0x49);
    assert(!strcmp(name_of(f, fdt_parent_offset(f, dev->node)), "i2c@a94000"));
    assert(dev->has_reset && dev->reset_gpio.pin == 38 && dev->reset_gpio.active_low);
    assert(dev->has_irq && dev->irq_pin == 39 && dev->irq_trigger == KSHIM_IRQ_TYPE_LEVEL_LOW);
    assert(dev->has_irq_gpio && dev->irq_gpio.pin == 39 && dev->irq_gpio.active_low);
    assert(dev->props.invert_x && dev->props.invert_y && !dev->props.size_x &&
           dev->max_x == 1440 && dev->max_y == 2880 && dev->max_contacts == 10);
    /* AVDD before DVDD; reset low 20 ms; ready; ID; sense on; IER. */
    assert(fake_log_order((const char *const[]){"i2c-probe=i2c@a94000", "gpio38=0", "=on", "=on",
                                                "gpio39=in", "delay=20000", "gpio38=1", NULL}));
    assert(M4.sense && M4.ier && M4.sense_on == 1 && M4.ier_on == 1 && !M4.system_resets);

    assert(!kshim_touch_framework_poll());
    EventCount = 0;
    /* st,x-flip/st,y-flip: 1439 - x, 2879 - y on a 1440 x 2880 surface. */
    m4_pointer(0x03, 0, 100, 200);
    m4_pointer(0x05, 0, 140, 260);
    m4_pointer(0x03, 1, 1440, 2880);                 /* the axis maximum clamps */
    assert(fire());
    assert(!kshim_touch_framework_poll());
    assert(EventCount == 2 && M4.count == 0);
    expect_event(0, KSHIM_TOUCH_EVENT_PRESS, 0, 1339, 2679);
    expect_event(1, KSHIM_TOUCH_EVENT_MOVE, 0, 1299, 2619);
    const uint8_t echo[8] = {0xec, 0x93}, error[8] = {0x0f, 0x01};
    m4_queue(echo);
    m4_queue(error);
    m4_pointer(0x04, 0, 0, 0);
    assert(fire() && !kshim_touch_framework_poll());
    expect_event(2, KSHIM_TOUCH_EVENT_RELEASE, 0, 1299, 2619);
    expect_event(3, KSHIM_TOUCH_EVENT_PRESS, 1, 0, 0);

    /* The controller resets itself: contacts released, sensing re-enabled. */
    const uint8_t ready[8] = {0x10};
    m4_queue(ready);
    assert(fire() && !kshim_touch_framework_poll());
    expect_event(4, KSHIM_TOUCH_EVENT_RELEASE, 1, 0, 0);
    assert(M4.sense_on == 2 && M4.ier_on == 2);
    m4_pointer(0x03, 3, 1, 2);
    assert(fire() && !kshim_touch_framework_poll());
    expect_event(5, KSHIM_TOUCH_EVENT_PRESS, 3, 1438, 2877);
    /* Out-of-range IDs are ignored. */
    m4_pointer(0x03, 12, 1, 2);
    assert(fire() && !kshim_touch_framework_poll() && EventCount == 6);

    fake_log_clear();
    assert(!kshim_touch_quiesce_all());
    assert(M4.sense_off == 1 && M4.ier_off == 1 && M4.flushes == 1 && !M4.sense && !M4.ier);
    expect_event(6, KSHIM_TOUCH_EVENT_CANCEL, 3, 1438, 2877);
    assert(fake_log_order((const char *const[]){"irq-free39", "gpio38=0", "=off", "=off", "free39",
                                                "free38", NULL}));
    assert(!kshim_touch_quiesce_all() && M4.sense_off == 1);
}

static void test_ftm4_kona(const void *fixture, size_t size)
{
    /* Stock kona-hdk.dtsi: qcom,i2c-touch-active = "" selects nothing, and
     * nothing is touched. */
    start_clean();
    m4_setup();
    void *f = malloc(size + 4096);
    assert(f && !fdt_open_into(fixture, f, (int)size + 4096));
    assert(kshim_touch_framework_start(f, 1440, 2880, emit, NULL) == ERROR_NOT_FOUND);
    assert(!FakeI2cProbes && !M4.transfers);

    /* The paired SW43404/ST configuration selects it. */
    int bus = fdt_path_offset(f, "/i2c@a94000");
    assert(bus >= 0 && !fdt_setprop_string(f, bus, "qcom,i2c-touch-active", "st,fts"));
    M4.nack = 2;
    assert(!kshim_touch_framework_start(f, 1440, 2880, emit, NULL));
    const struct kshim_touch_dev *dev = kshim_touch_active();
    assert(dev->supply_count == 2 &&
           ((const struct fake_regulator *)(const void *)dev->supplies[0])->node ==
               fdt_path_offset(f, "/regulators/ldoa13/regulator") &&
           ((const struct fake_regulator *)(const void *)dev->supplies[1])->node ==
               fdt_path_offset(f, "/regulators/ldoc1/regulator"));
    assert(fake_log_has("pinctrl=default"));
    /* AVDD's 20 mA load hint reaches the regulator before its first enable
     * (DVDD gets none). */
    const struct fake_regulator *avdd = (const void *)dev->supplies[0];
    const struct fake_regulator *dvdd = (const void *)dev->supplies[1];
    assert(avdd->load_ua == 20000 && dvdd->load_ua == 0);
    char load[64], on[64];
    snprintf(load, sizeof(load), "%s=load20000", avdd->name);
    snprintf(on, sizeof(on), "%s=on", avdd->name);
    assert(fake_log_order((const char *const[]){load, on, NULL}));
    ftm4_session(f);

    /* The FTM5 driver leaves an st,* node alone even when FTM4 fails. */
    start_clean();
    m4_setup();
    M4.chip[0] = 0x36; M4.chip[1] = 0x39;
    assert(kshim_touch_framework_start(f, 1440, 2880, emit, NULL) == KSHIM_IO_ERR_NO_DEVICE);
    assert(!M4.sense_on && !kshim_touch_active() && !fake_pin(38)->refs && !fake_pin(39)->refs);

    /* No reset GPIO: the system-reset register write is the reset. */
    start_clean();
    m4_setup();
    M4.booted = true;
    int touch = fdt_path_offset(f, "/i2c@a94000/touch@49");
    assert(touch >= 0 && !fdt_delprop(f, touch, "st,reset-gpio"));
    assert(!kshim_touch_framework_start(f, 1440, 2880, emit, NULL));
    assert(M4.system_resets == 1 && !kshim_touch_active()->has_reset && M4.sense_on == 1);
    assert(!kshim_touch_quiesce_all());

    /* No ready event within 500 ms: timeout after 50 polls of 10 ms. */
    start_clean();
    m4_setup();
    M4.booted = M4.mute_ready = true;                /* answers, never says ready */
    assert(kshim_touch_framework_start(f, 1440, 2880, emit, NULL) == ERROR_TIMEOUT);
    assert(fake_log_count("delay=10000") == 50);
    free(f);
    printf("ftm4 kona fixture: ok\n");
}

static void test_ftm4_hdk8250(void *f)
{
    /* The real stock HDK DTB selects no ST controller. */
    start_clean();
    m4_setup();
    assert(kshim_touch_framework_start(f, 1440, 2880, emit, NULL) == ERROR_NOT_FOUND);
    assert(!FakeI2cProbes);
    /* With the paired panel's selection: bus, address, GPIOs, supplies and
     * flips straight from the board DT. */
    int touch = fdt_path_offset(f, "/soc/i2c@a94000/st_fts@49");
    assert(touch >= 0);
    int bus = fdt_parent_offset(f, touch);
    assert(!fdt_setprop_string(f, bus, "qcom,i2c-touch-active", "st,fts"));
    assert(kshim_touch_framework_start(f, 1440, 2880, emit, NULL) == ERROR_NOT_FOUND);  /* panel */
    touch = fdt_path_offset(f, "/soc/i2c@a94000/st_fts@49");   /* offsets moved */
    assert(touch >= 0 && !fdt_delprop(f, touch, "panel"));
    assert(!kshim_touch_framework_start(f, 1440, 2880, emit, NULL));
    const struct kshim_touch_dev *dev = kshim_touch_active();
    assert(dev->node == touch && dev->supply_count == 2 &&
           !strcmp(kshim_regulator_name(dev->supplies[0]), "pm8150_l13") &&
           !strcmp(kshim_regulator_name(dev->supplies[1]), "pm8150a_l1"));
    assert(fake_log_order((const char *const[]){"pm8150_l13=on", "pm8150a_l1=on", "pinctrl=pmx_ts_active",
                                                NULL}));
    ftm4_session(f);
    printf("ftm4 hdk8250 dtb: ok\n");
}
#endif

int main(void)
{
    void *andromeda = load(FIXTURES "/andromeda-fdt.dtb", 16384);
    assert(andromeda);
    test_ftm5_andromeda(andromeda);
#if TOUCH_FAKE_TLMM
    test_ftm5_failures(andromeda);
    size_t kona_size;
    void *kona = load(GENERATED_FIXTURES "/kona-resources.dtb", 0);
    assert(kona);
    kona_size = fdt_totalsize(kona);
    test_ftm4_kona(kona, kona_size);
    void *hdk = load(KSHIM_HDK8250_DTB, 16384);
    if (hdk) test_ftm4_hdk8250(hdk);
    else printf("ftm4 hdk8250 dtb: SKIPPED (%s not found)\n", KSHIM_HDK8250_DTB);
    free(kona);
    free(hdk);
#endif
    assert(!kshim_touch_quiesce_all());
    free(andromeda);
    puts("touch_st_test: ok");
    return 0;
}
