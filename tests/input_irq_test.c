/* SPDX-License-Identifier: MIT */
/*
 * Interrupt-driven input (docs/interrupts.md layer 3) through a fake GIC
 * core and the cascade register models: PMIC PON/PMIC-GPIO keys and a TLMM
 * key queue events from IRQ context, the FTM5 touch FIFO is drained by the
 * TLMM level handler, GENI console RX is queued by its handler, and every
 * path falls back to polling when its interrupt does not resolve.
 */
#include "irq_fake.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dt.h>
#include <input_irq.h>
#include <kshim_console.h>
#include <qcom_dt.h>
#include <qcom_irq.h>
#include <qcom_keys.h>
#include <qcom_keys_fdt.h>
#include <st_ftm5.h>
#include <Library/cr_debug_uart_poll.h>
#include <kshim/uart.h>

static void *load(const char *name)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", FIXTURES, name);
    FILE *file = fopen(path, "rb");
    assert(file);
    assert(!fseek(file, 0, SEEK_END));
    long size = ftell(file);
    assert(size > 0 && !fseek(file, 0, SEEK_SET));
    void *data = aligned_alloc(8, ((size_t)size + 16384 + 7) & ~(size_t)7);
    assert(data && fread(data, 1, (size_t)size, file) == (size_t)size);
    fclose(file);
    assert(!fdt_open_into(data, data, size + 16384));
    return data;
}

static int compatible(const void *f, const char *name)
{
    for (int n = fdt_node_offset_by_compatible(f, -1, name); n >= 0;
         n = fdt_node_offset_by_compatible(f, n, name))
        if (dt_available(f, n)) return n;
    return -1;
}

/* Key levels straight from the register models (the SPMI/TLMM transports
 * have their own tests). */
static int read_status(void *context, uint64_t address, uint32_t *value)
{
    (void)context;
    if (QCOM_KEY_IS_SPMI_TOKEN(address))
        *value = *fake_pmic(QCOM_KEY_SPMI_TOKEN_SID(address), (uint16_t)QCOM_KEY_SPMI_TOKEN_ADDRESS(address));
    else
        *value = FakeIo.read32(NULL, (uintptr_t)address);
    return 0;
}

static int poll(QcomKeys *keys, kshim_key_event_t *event)
{
    memset(event, 0, sizeof(*event));
    return kshim_key_source_poll(QcomKeysSource(keys), event);
}

static void expect_press(QcomKeys *keys, kshim_key_action_t action, uint32_t code)
{
    kshim_key_event_t e;
    assert(poll(keys, &e) == KSHIM_KEYS_EVENT);
    assert(e.type == KSHIM_KEY_EVENT_PRESS && e.action == action && e.code == code);
}

static void expect_none(QcomKeys *keys)
{
    kshim_key_event_t e;
    assert(poll(keys, &e) == KSHIM_KEYS_NO_EVENT);
}

static void setup_pmic(const void *f, unsigned apids)
{
    fake_reset();
    const uint16_t ppids[] = {0x0008, 0x00c5, 0x0024};
    (void)fake_spmi_init(f, ppids, apids);
    fake_gic_line(FAKE_SPMI_PERIPH_IRQ, fake_spmi_line);
    *fake_pmic(0, 0xc510) = 1; /* volume up (PMIC GPIO6) is active low */
}

static size_t build(const void *f, QcomKeys *keys)
{
    QcomKeyDescriptor d[QCOM_KEYS_MAX];
    size_t count = 0;
    assert(!QcomKeysBuildFromDeviceTree(f, d, QCOM_KEYS_MAX, &count));
    assert(!QcomKeysInitEx(keys, d, count, NULL, read_status, NULL));
    QcomKeysSetDebounce(keys, 2);
    return count;
}

static void test_keys(const void *f)
{
    static QcomKeys keys;
    setup_pmic(f, 3);
    assert(build(f, &keys) == 3);
    /* Power and volume down are PON KPDPWR/RESIN, volume up is PMIC GPIO6. */
    assert(kshim_keys_irq_start(&keys, f) == 3);
    assert(keys.IrqDriven == 7 && keys.IrqDebounceMs == KSHIM_KEYS_IRQ_DEBOUNCE_MS);
    assert(FakeGic[FAKE_SPMI_PERIPH_IRQ].requested && !FakeIrqMasked);
    expect_none(&keys);               /* initial resample: nothing held */
    assert(!keys.IrqResync);

    /* The handler queues the press in IRQ context; the poll only drains. */
    FakeNowUs = 1000000;
    fake_pmic_rt(0, 0x08, 0, true);
    assert(fake_gic_run() == 1 && keys.PendingPress == 1);
    expect_press(&keys, KSHIM_KEY_ACTION_SELECT, QCOM_KEYCODE_POWER);
    expect_none(&keys);
    /* Bounce inside the quiet window: no event, one resample afterwards. */
    fake_pmic_rt(0, 0x08, 0, false);
    assert(fake_gic_run() == 1 && keys.IrqResync == 1);
    fake_pmic_rt(0, 0x08, 0, true);
    assert(fake_gic_run() == 1 && keys.IrqResync == 1 && !keys.PendingPress);
    expect_none(&keys);
    FakeNowUs += 20000;
    expect_none(&keys);               /* still pressed: no second press */
    assert(!keys.IrqResync);
    FakeNowUs += 20000;
    fake_pmic_rt(0, 0x08, 0, false);
    assert(fake_gic_run() == 1);
    expect_none(&keys);               /* releases are not emitted by default */
    QcomKeysSetEmitRelease(&keys, 1);
    FakeNowUs += 20000;
    fake_pmic_rt(0, 0x08, 1, true);   /* RESIN: volume down */
    assert(fake_gic_run() == 1);
    expect_press(&keys, KSHIM_KEY_ACTION_DOWN, QCOM_KEYCODE_VOLUMEDOWN);
    FakeNowUs += 20000;
    fake_pmic_rt(0, 0x08, 1, false);
    assert(fake_gic_run() == 1);
    kshim_key_event_t e;
    assert(poll(&keys, &e) == KSHIM_KEYS_EVENT && e.type == KSHIM_KEY_EVENT_RELEASE &&
           e.action == KSHIM_KEY_ACTION_DOWN);
    QcomKeysSetEmitRelease(&keys, 0);
    fake_pmic_rt(0, 0xc5, 0, false);  /* active low: pressed */
    assert(fake_gic_run() == 1);
    expect_press(&keys, KSHIM_KEY_ACTION_UP, QCOM_KEYCODE_VOLUMEUP);

    /* A foreign PMIC interrupt seals the arbiter cascade: the keys notice
     * and go back to the debounced poll, which still reports presses. */
    FakeNowUs += 20000;
    fake_pmic_rt(0, 0xc5, 0, true);
    assert(fake_gic_run() == 1);
    fake_spmi_set_acc_enable(2, 1);
    fake_spmi_raise(2, 1);
    assert(fake_gic_run() == 1 && !kshim_spmi_irq_live(0));
    expect_none(&keys);
    assert(!keys.IrqDriven);
    fake_pmic_rt(0, 0x08, 0, true);
    assert(fake_gic_run() == 0);      /* sealed */
    expect_none(&keys);               /* debounce sample 1 */
    expect_press(&keys, KSHIM_KEY_ACTION_SELECT, QCOM_KEYCODE_POWER);

    assert(!kshim_keys_irq_stop(&keys));
    assert(!FakeGic[FAKE_SPMI_PERIPH_IRQ].requested && FakeSpmiGrantRefs == 0);
    assert(*fake_pmic(0, 0x815) == 0 && *fake_pmic(0, 0xc515) == 0 && fake_spmi_acc_enable(0) == 0);
    assert(!keys.IrqAlive);
    printf("keys: ok\n");
}

static void test_keys_fallback(void *f)
{
    static QcomKeys keys;
    /* No GIC core yet: every key is polled. */
    setup_pmic(f, 2);
    FakeIrqReady = false;
    assert(build(f, &keys) == 3 && kshim_keys_irq_start(&keys, f) == 0 && !keys.IrqDriven);
    assert(!FakeGic[FAKE_SPMI_PERIPH_IRQ].requested);
    fake_pmic_rt(0, 0x08, 0, true);
    expect_none(&keys);
    expect_press(&keys, KSHIM_KEY_ACTION_SELECT, QCOM_KEYCODE_POWER);
    assert(keys.IrqStart && !FakeGic[FAKE_SPMI_PERIPH_IRQ].requested);

    /* The GIC core comes up later: the first poll finishes the setup. */
    FakeIrqReady = true;
    fake_pmic_rt(0, 0x08, 0, false);
    expect_none(&keys);
    assert(keys.IrqDriven == 7 && !keys.IrqStart && FakeGic[FAKE_SPMI_PERIPH_IRQ].requested);
    FakeNowUs += 20000;
    fake_pmic_rt(0, 0x08, 0, true);
    assert(fake_gic_run() == 1);
    expect_press(&keys, KSHIM_KEY_ACTION_SELECT, QCOM_KEYCODE_POWER);
    assert(!kshim_keys_irq_stop(&keys) && !FakeGic[FAKE_SPMI_PERIPH_IRQ].requested);
    /* A stopped source never starts later (handoff). */
    FakeIrqReady = false;
    assert(kshim_keys_irq_start(&keys, f) == 0 && keys.IrqStart);
    assert(!kshim_keys_irq_stop(&keys) && !keys.IrqStart);
    FakeIrqReady = true;
    expect_none(&keys);
    assert(!keys.IrqDriven && !FakeGic[FAKE_SPMI_PERIPH_IRQ].requested);

    /* The DT does not describe the PON interrupts: PON keys poll, the PMIC
     * GPIO key is interrupt-driven. */
    setup_pmic(f, 2);
    int pon = kshim_dt_spmi_irq_find(f, 0, 0, 0x08, 0, &(unsigned){0});
    assert(pon > 0 && !fdt_delprop(f, pon, "interrupts"));
    assert(build(f, &keys) == 3 && kshim_keys_irq_start(&keys, f) == 1);
    assert(keys.IrqDriven == 4);
    expect_none(&keys);
    fake_pmic_rt(0, 0xc5, 0, false);
    assert(fake_gic_run() == 1);
    expect_press(&keys, KSHIM_KEY_ACTION_UP, QCOM_KEYCODE_VOLUMEUP);
    fake_pmic_rt(0, 0x08, 1, true);
    assert(fake_gic_run() == 0);      /* RESIN has no interrupt here */
    expect_none(&keys);
    expect_press(&keys, KSHIM_KEY_ACTION_DOWN, QCOM_KEYCODE_VOLUMEDOWN);
    assert(!kshim_keys_irq_stop(&keys) && !FakeGic[FAKE_SPMI_PERIPH_IRQ].requested);
    printf("keys fallback: ok\n");
}

static void test_tlmm_key(const void *f)
{
    static QcomKeys keys;
    QcomKeyDescriptor d;
    uintptr_t pin41;
    fake_reset();
    fake_gic_line(FAKE_TLMM_SUMMARY, fake_tlmm_line);
    assert(!kshim_dt_gpio(f, compatible(f, "qcom,sm8150-pinctrl"), 41, &pin41, NULL, NULL));
    fake_tlmm_pin(pin41, true);
    assert(!QcomKeyMakeGpioAt(&d, pin41 + QCOM_TLMM_GPIO_IN_OFFSET, 1, QCOM_KEYCODE_VOLUMEDOWN, 1));
    assert(!QcomKeysInitEx(&keys, &d, 1, NULL, read_status, NULL));
    assert(kshim_keys_irq_start(&keys, f) == 1 && keys.IrqDriven == 1);
    /* gpio-keys "ai_key" on GPIO 41 has debounce-interval = <39>. */
    assert(keys.IrqDebounceMs == 39);
    assert((fake_peek(pin41 + 8) & 0xfU) == ((3U << 2) | 2U | 1U)); /* both edges */
    expect_none(&keys);
    fake_tlmm_set(pin41, false);
    assert(fake_gic_run() == 1 && keys.PendingPress == 1);
    expect_press(&keys, KSHIM_KEY_ACTION_DOWN, QCOM_KEYCODE_VOLUMEDOWN);

    /* The interrupt resolves but is never delivered (routing broken on the
     * board): the audit reports the edges late and then polls the key. */
    FakeGic[FAKE_TLMM_SUMMARY].enabled = false;
    fake_tlmm_set(pin41, true);
    assert(fake_gic_run() == 0);
    FakeNowUs += 150000;
    expect_none(&keys);                 /* release found by the audit */
    assert(keys.IrqDriven == 1 && keys.IrqMisses[0] == 1);
    fake_tlmm_set(pin41, false);
    FakeNowUs += 150000;
    expect_press(&keys, KSHIM_KEY_ACTION_DOWN, QCOM_KEYCODE_VOLUMEDOWN);
    assert(!keys.IrqDriven);            /* second miss: polled from now on */
    fake_tlmm_set(pin41, true);
    expect_none(&keys); expect_none(&keys);
    fake_tlmm_set(pin41, false);
    expect_none(&keys);
    expect_press(&keys, KSHIM_KEY_ACTION_DOWN, QCOM_KEYCODE_VOLUMEDOWN);
    FakeGic[FAKE_TLMM_SUMMARY].enabled = true;
    assert(fake_gic_run() == 1);        /* the stale latch is ignored */
    expect_none(&keys);
    assert(!kshim_keys_irq_stop(&keys));
    assert(fake_peek(pin41 + 8) == 0 && !FakeGic[FAKE_TLMM_SUMMARY].requested);
    printf("tlmm key: ok\n");
}

/* ---- Touch: FTM5 FIFO drained by the TLMM level-low handler ------------ */

static uint8_t Fifo[32][8];
static unsigned FifoCount;
static uintptr_t TouchPin;
static struct kshim_touch_queue Queue;
static kshim_ftm5_t Ftm5;

static int fifo_transfer(void *context, const uint8_t *command, size_t command_size,
                         uint8_t *response, size_t response_size)
{
    (void)context;
    assert(FakeInHandler && command_size == 1 && command[0] == 0x86 && response_size % 8 == 0);
    memset(response, 0, response_size);
    for (size_t at = 0; at < response_size && FifoCount; at += 8) {
        memcpy(response + at, Fifo[0], 8);
        response[at + 7] = (uint8_t)(FifoCount - 1);
        memmove(Fifo, Fifo + 1, --FifoCount * 8U);
    }
    return 0;
}
static void fifo_delay(void *context, uint32_t us) { (void)context; (void)us; }
static void fifo_event(uint8_t type, unsigned x, unsigned y)
{
    uint8_t e[8] = {type, 0x01 /* contact 0, finger */, (uint8_t)x,
                    (uint8_t)(((x >> 8) & 0xfU) | ((y & 0xfU) << 4)), (uint8_t)(y >> 4)};
    memcpy(Fifo[FifoCount++], e, 8);
}
static void touch_emit(void *context, const kshim_touch_event_t *event)
{
    (void)context;
    assert(FakeInHandler);
    kshim_touch_queue_push(&Queue, event);
}
static void touch_irq(uint32_t pin, void *context)
{
    assert(pin == 122 && context == &Ftm5);
    assert(!kshim_ftm5_poll(&Ftm5));
    if (!FifoCount) fake_tlmm_set(TouchPin, true); /* FTM5 releases the line */
}

static void test_touch(const void *f)
{
    struct kshim_touch_resource t;
    uint32_t pin;
    unsigned trigger;
    fake_reset();
    fake_gic_line(FAKE_TLMM_SUMMARY, fake_tlmm_line);
    assert(!kshim_dt_touch(f, &t));
    assert(!kshim_touch_irq_resolve(f, t.node, t.qup.irq_pin, t.qup.irq_active_low, &pin, &trigger));
    assert(pin == 122 && trigger == KSHIM_IRQ_TYPE_LEVEL_LOW);
    /* The interrupt must be the IRQ GPIO kShimC power-sequences. */
    assert(kshim_touch_irq_resolve(f, t.node, t.qup.reset_pin, true, &pin, &trigger) ==
           ERROR_INVALID_PARAMETER);

    static kshim_touch_t touch;
    kshim_touch_config_t config = {.source_width = 1440, .source_height = 2880,
        .output_width = 1440, .output_height = 2880, .emit = touch_emit};
    assert(!kshim_touch_init(&touch, &config));
    Ftm5 = (kshim_ftm5_t){.transport = {NULL, fifo_transfer, fifo_delay, KSHIM_FTM5_BUS_I2C},
                          .touch = &touch, .initialized = true};
    TouchPin = t.qup.irq_pin;
    fake_tlmm_pin(TouchPin, true);
    kshim_touch_queue_reset(&Queue);
    assert(!kshim_gpio_irq_request(122, trigger, touch_irq, &Ftm5));

    /* One FIFO batch: enter and two moves; the queue coalesces the moves. */
    fifo_event(0x13, 100, 200); fifo_event(0x23, 110, 210); fifo_event(0x23, 120, 220);
    fake_tlmm_set(TouchPin, false);
    assert(fake_gic_run() == 1 && !FifoCount);
    kshim_touch_event_t e;
    assert(kshim_touch_queue_pop(&Queue, &e) && e.type == KSHIM_TOUCH_EVENT_PRESS && e.x == 100 && e.y == 200);
    assert(kshim_touch_queue_pop(&Queue, &e) && e.type == KSHIM_TOUCH_EVENT_MOVE && e.x == 120 && e.y == 220);
    assert(!kshim_touch_queue_pop(&Queue, &e));
    fifo_event(0x33, 120, 220);
    fake_tlmm_set(TouchPin, false);
    assert(fake_gic_run() == 1);
    assert(kshim_touch_queue_pop(&Queue, &e) && e.type == KSHIM_TOUCH_EVENT_RELEASE);
    assert(!kshim_touch_queue_pop(&Queue, &e));
    assert(!kshim_gpio_irq_free(122) && fake_peek(TouchPin + 8) == 0);

    /* Full queue: presses/releases outrank a trailing move, the rest drop. */
    kshim_touch_event_t press = {.type = KSHIM_TOUCH_EVENT_PRESS}, move = {.type = KSHIM_TOUCH_EVENT_MOVE};
    for (unsigned i = 0; i < KSHIM_TOUCH_QUEUE_DEPTH - 1; ++i) kshim_touch_queue_push(&Queue, &press);
    kshim_touch_queue_push(&Queue, &move);
    kshim_touch_queue_push(&Queue, &press);
    kshim_touch_queue_push(&Queue, &press);
    assert(Queue.count == KSHIM_TOUCH_QUEUE_DEPTH && Queue.dropped == 2);
    kshim_touch_queue_reset(&Queue);
    assert(!FakeIrqMasked);
    printf("touch: ok\n");
}

/* ---- GENI console RX -------------------------------------------------- */

static uintptr_t Se;
static uint32_t RxWords[64], RxLast;
static unsigned RxCount, TxBytes;

static uint32_t geni_read(uintptr_t a)
{
    switch (a - Se) {
    case 0x68: return 2U << 8;                   /* UART firmware */
    case 0xe24: case 0xe28: return 16U << 16;    /* FIFO depths */
    case 0x804: return RxCount | (RxCount == 1 && RxLast ? (1U << 31) | (RxLast << 28) : 0);
    case 0x780: {
        assert(RxCount);
        uint32_t w = RxWords[0];
        memmove(RxWords, RxWords + 1, --RxCount * 4U);
        if (!RxCount) RxLast = 0;
        return w;
    }
    default: return fake_peek(a);
    }
}
static void geni_write(uintptr_t a, uint32_t v)
{
    switch (a - Se) {
    case 0x648: fake_poke(Se + 0x640, fake_peek(Se + 0x640) & ~v); return;  /* S_IRQ_CLEAR */
    case 0x618: fake_poke(Se + 0x610, fake_peek(Se + 0x610) & ~v); return;  /* M_IRQ_CLEAR */
    case 0x630: fake_poke(Se + 0x40, fake_peek(Se + 0x40) | (1U << 12)); return; /* RX on */
    case 0x634:                                                                /* RX abort */
        fake_poke(Se + 0x40, fake_peek(Se + 0x40) & ~(1U << 12));
        fake_poke(Se + 0x640, fake_peek(Se + 0x640) | (1U << 5));
        return;
    case 0x600: TxBytes = 0; return;
    case 0x700:
        TxBytes += 4;
        if (TxBytes >= fake_peek(Se + 0x270)) fake_poke(Se + 0x610, fake_peek(Se + 0x610) | 1U);
        return;
    default: fake_poke(a, v);
    }
}
static void geni_receive(const char *text)
{
    size_t n = strlen(text);
    for (size_t i = 0; i < n; i += 4) {
        uint32_t w = 0;
        for (size_t j = 0; j < 4 && i + j < n; ++j) w |= (uint32_t)(uint8_t)text[i + j] << (8 * j);
        RxWords[RxCount++] = w;
    }
    RxLast = n % 4 ? (uint32_t)(n % 4) : 0;
    fake_poke(Se + 0x640, fake_peek(Se + 0x640) | (1U << 26) | (RxLast ? 1U << 27 : 0));
}
static bool geni_line(void)
{
    return (fake_peek(Se + 0xe1c) & 8U) &&
           (fake_peek(Se + 0x640) & fake_peek(Se + 0x644) & ((1U << 26) | (1U << 27)));
}

static void test_console(const void *f)
{
    /* The console's GENI RX interrupt now runs on struct kshim_uart
     * (kshim/uart.h); the console opens it with kshim_uart_open_mmio(). */
    struct kshim_console_resource console;
    static struct kshim_uart uart;
    uint8_t c;
    fake_reset();
    assert(!kshim_console_probe(f, &console) && console.geni);
    Se = console.base;
    fake_mmio_region(Se, console.size, geni_read, geni_write);
    const uint32_t uart_intid = KSHIM_IRQ_SPI(357);
    fake_gic_line(uart_intid, geni_line);
    /* Firmware left the M (TX) interrupt enabled; DebugUartLib masks it. */
    fake_poke(Se + 0xe1c, 4U);
    fake_poke(Se + 0x614, 1U);
    memset(&uart, 0, sizeof(uart));
    uart.kind = KSHIM_UART_GENI;
    uart.base = Se;
    uart.size = console.size;
    uart.geni = (struct CrDebugUartPoll){.io = FakeIo, .base = Se, .size = console.size};
    assert(!CrDebugUartPollInit(&uart.geni));
    uart.open = true;

    /* Before the GIC core is up the polled read is used. */
    FakeIrqReady = false;
    assert(kshim_geni_rx_irq_start(&uart, f, -1) == ERROR_NOT_READY);
    assert(kshim_geni_rx_irq_read(&uart, &c) == ERROR_NOT_READY);
    geni_receive("x");
    assert(CrDebugUartPollRead(&uart.geni, &c) == 1 && c == 'x');
    fake_poke(Se + 0x640, 0);

    FakeIrqReady = true;
    assert(!kshim_geni_rx_irq_start(&uart, f, -1) && kshim_geni_rx_irq_read(&uart, &c) == 0);
    struct fake_gic_irq *g = &FakeGic[uart_intid];
    assert(g->requested && g->trigger == KSHIM_IRQ_TYPE_LEVEL_HIGH);
    assert(fake_peek(Se + 0xe1c) == 8U && fake_peek(Se + 0x644) == ((1U << 26) | (1U << 27)));
    assert(fake_peek(Se + 0x614) == 0); /* TX interrupt untouched */
    assert(fake_writes_in(FAKE_PDC_BASE, 0x400) == 0); /* SPI 357: PDC pass-through */

    /* The handler drains the FIFO into the queue; reads only dequeue. */
    geni_receive("ok\r");
    assert(geni_line() && fake_gic_run() == 1 && !RxCount && !geni_line());
    assert(kshim_geni_rx_irq_read(&uart, &c) == 1 && c == 'o');
    assert(kshim_geni_rx_irq_read(&uart, &c) == 1 && c == 'k');
    assert(kshim_geni_rx_irq_read(&uart, &c) == 1 && c == '\r');
    assert(kshim_geni_rx_irq_read(&uart, &c) == 0);
    geni_receive("12345");
    assert(fake_gic_run() == 1);
    for (const char *p = "12345"; *p; ++p)
        assert(kshim_geni_rx_irq_read(&uart, &c) == 1 && c == (uint8_t)*p);
    /* A byte whose latch was lost is still read by the next poll. */
    geni_receive("z");
    fake_poke(Se + 0x640, 0);
    assert(fake_gic_run() == 0);
    assert(kshim_geni_rx_irq_read(&uart, &c) == 1 && c == 'z');
    /* TX stays on the polled path while RX is interrupt-driven. */
    assert(!CrDebugUartPollWrite(&uart.geni, "hello", 5));

    assert(!kshim_geni_rx_irq_stop(&uart));
    assert(!g->requested && fake_peek(Se + 0xe1c) == 0 && fake_peek(Se + 0x644) == 0);
    assert(!kshim_geni_rx_irq_stop(&uart));
    assert(kshim_geni_rx_irq_read(&uart, &c) == ERROR_NOT_READY); /* polled again */
    assert(!CrDebugUartPollQuiesce(&uart.geni));
    assert(fake_peek(Se + 0xe1c) == 4U && fake_peek(Se + 0x614) == 1U); /* inherited */
    assert(!FakeIrqMasked);
    printf("console rx: ok\n");
}

int main(void)
{
    void *andromeda = load("andromeda-fdt.dtb"), *hdk = load("hdk8150-fdt.dtb");
    test_keys(andromeda);
    test_keys(hdk);
    test_tlmm_key(andromeda);
    test_touch(andromeda);
    test_console(andromeda);
    test_keys_fallback(hdk);
    free(andromeda); free(hdk);
    return 0;
}
