/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <st_ftm4.h>

static struct {
    uint8_t fifo[128][8];
    unsigned count, head, writes, reads, delays, bus_calls;
    int wrong_id, fail_sense, fail_stop, fail_read, fail_irq_off;
    unsigned presses, releases, cancels, moves;
    uint32_t x, y;
} model;

static void event_sink(void *context, const kshim_touch_event_t *event)
{
    (void)context;
    model.x = event->x;
    model.y = event->y;
    if (event->type == KSHIM_TOUCH_EVENT_PRESS) ++model.presses;
    if (event->type == KSHIM_TOUCH_EVENT_RELEASE) ++model.releases;
    if (event->type == KSHIM_TOUCH_EVENT_MOVE) ++model.moves;
    if (event->type == KSHIM_TOUCH_EVENT_CANCEL) ++model.cancels;
}

static void push(uint8_t type, uint8_t id, uint32_t x, uint32_t y)
{
    assert(model.count < 128);
    uint8_t *e = model.fifo[model.count++];
    e[0] = type; e[1] = id; e[2] = x >> 4; e[3] = y >> 4;
    e[4] = ((x & 15) << 4) | (y & 15);
}

static int transfer(void *context, const uint8_t *command, size_t size,
                      uint8_t *response, size_t count)
{
    (void)context;
    if (count) {
        ++model.reads;
        if (model.fail_read) return KSHIM_BUS_NACK;
        if (command[0] == 0x85) {
            assert(size == 1 && count == 8);
            if (model.head < model.count)
                memcpy(response, model.fifo[model.head++], 8);
            else
                memset(response, 0, 8);
        } else {
            assert(size == 3 && command[0] == 0xb6 && command[1] == 0 &&
                   command[2] == 4 && count == 3);
            response[0] = 0xaa;
            response[1] = 0x36;
            response[2] = model.wrong_id ? 0x39 : 0x70;
        }
    } else {
        ++model.writes;
        /* Only volatile IRQ/sensing commands may be sent. */
        if (command[0] == 0xb6) {
            assert(size == 4 && command[1] == 0 && command[2] == 0x2c &&
                   (command[3] == 0 || command[3] == 0x41));
            if (command[3] == 0 && model.fail_irq_off) return KSHIM_BUS_TIMEOUT;
        } else {
            assert(size == 1 && (command[0] == 0x92 || command[0] == 0x93));
            if (command[0] == 0x93 && model.fail_sense) return KSHIM_BUS_TIMEOUT;
            if (command[0] == 0x92 && model.fail_stop) return KSHIM_BUS_TIMEOUT;
        }
    }
    return 0;
}

static void delay_us(void *context, uint32_t us)
{
    (void)context;
    assert(us == 10000);
    ++model.delays;
}

int kshim_i2c_transfer(struct kshim_qcom_bus *bus,
                       const struct kshim_i2c_msg *messages, size_t count,
                       uint32_t timeout_us)
{
    assert(bus && (count == 1 || count == 2) && timeout_us == 20000);
    assert(messages[0].address == 0x49 && !messages[0].read);
    if (count == 2) assert(messages[1].address == 0x49 && messages[1].read);
    ++model.bus_calls;
    return transfer(NULL, messages[0].data, messages[0].length,
                    count == 2 ? messages[1].data : NULL,
                    count == 2 ? messages[1].length : 0);
}

static void setup(kshim_touch_t *touch)
{
    memset(&model, 0, sizeof(model));
    const kshim_touch_config_t config = {
        .source_width = 1440, .source_height = 2880,
        .output_width = 1440, .output_height = 2880, .emit = event_sink,
    };
    assert(kshim_touch_init(touch, &config) == 0);
    push(0x10, 0, 0, 0);
}

int main(void)
{
    kshim_touch_t touch;
    kshim_ftm4_t device = {0};
    const kshim_ftm4_transport_t transport = {NULL, transfer, delay_us};
    setup(&touch);
    assert(kshim_ftm4_init(&device, &transport, &touch, true, true) == 0);
    push(0x03, 2, 100, 200);
    push(0x03, 4, 300, 400);
    push(0x05, 2, 200, 500);
    assert(kshim_ftm4_poll(&device) == 0);
    assert(model.presses == 1 && model.moves == 1 && model.x == 1240 && model.y == 2380);
    push(0x04, 2, 0, 0);
    assert(kshim_ftm4_poll(&device) == 0);
    assert(model.releases == 1 && model.presses == 2 && model.x == 1140 && model.y == 2480);
    model.fail_read = 1;
    assert(kshim_ftm4_poll(&device) == KSHIM_BUS_NACK && model.releases == 1 && model.cancels == 1);
    model.fail_read = 0; model.fail_stop = 1;
    assert(kshim_ftm4_quiesce(&device) == KSHIM_BUS_TIMEOUT && device.scanning);
    model.fail_stop = 0;
    assert(kshim_ftm4_quiesce(&device) == 0 && !device.scanning);
    unsigned writes = model.writes;
    assert(kshim_ftm4_quiesce(&device) == 0 && writes == model.writes);

    setup(&touch); model.wrong_id = 1;
    assert(kshim_ftm4_init(&device, &transport, &touch, false, false) == KSHIM_BUS_UNSUPPORTED);
    assert(!model.writes); /* FTM5 at the same address must never be configured. */
    setup(&touch); model.count = 0;
    assert(kshim_ftm4_init(&device, &transport, &touch, false, false) == KSHIM_BUS_TIMEOUT);
    assert(model.delays == 50 && model.reads == 51 && !model.writes);
    setup(&touch); model.count = 0;
    for (unsigned i = 0; i < 45; ++i) push(0, 0, 0, 0);
    push(0x10, 0, 0, 0); /* A valid 450 ms controller boot must not time out. */
    assert(kshim_ftm4_init(&device, &transport, &touch, false, false) == 0);
    assert(model.delays == 45);
    model.fail_irq_off = 1;
    assert(kshim_ftm4_quiesce(&device) == KSHIM_BUS_TIMEOUT && !device.quiesced);
    model.fail_irq_off = 0;
    assert(kshim_ftm4_quiesce(&device) == 0 && device.quiesced);
    setup(&touch); model.fail_sense = 1;
    assert(kshim_ftm4_init(&device, &transport, &touch, false, false) == KSHIM_BUS_TIMEOUT);
    assert(!device.scanning && !device.initialized);

    setup(&touch);
    struct kshim_qcom_bus bus = {.initialized = true,
        .config = {.protocol = KSHIM_GENI_I2C, .speed_hz = 400000},
        .io = {.delay_us = delay_us}};
    assert(kshim_ftm4_init_i2c(&device, &bus, &touch, true, true) == 0);
    push(0x05, 0, 0, 0); /* Recover a contact whose ENTER was missed. */
    assert(kshim_ftm4_poll(&device) == 0 && model.presses == 1);
    assert(model.x == 1439 && model.y == 2879);
    push(0x03, 15, 0, 0);
    assert(kshim_ftm4_poll(&device) == KSHIM_BUS_INVALID && model.cancels == 1 && model.releases == 0);
    assert(model.bus_calls > 0);
    assert(kshim_ftm4_quiesce(&device) == 0);

    setup(&touch);
    assert(kshim_ftm4_init(&device, &transport, &touch, false, false) == 0);
    for (unsigned i = 0; i < 70; ++i) push(0xec, 0, 0, 0);
    unsigned before = model.reads;
    assert(kshim_ftm4_poll(&device) == 0 && model.reads - before == 64);
    assert(kshim_ftm4_poll(&device) == 0);
    push(0x03, 1, 100, 200); push(0x10, 0, 0, 0);
    assert(kshim_ftm4_poll(&device) == KSHIM_BUS_IO && model.cancels == 1 && model.releases == 0);
    assert(kshim_ftm4_quiesce(&device) == 0);
    puts("FTM4: identity, dummy byte, bounded FIFO, flips, contacts, errors and quiesce pass");
    return 0;
}
