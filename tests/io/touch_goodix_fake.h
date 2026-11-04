/* SPDX-License-Identifier: MIT */
#pragma once
/*
 * Host fake of the touch framework (kshim/touch_driver.h helpers), the
 * regulator and GPIO calls, and the clock, for the Goodix driver tests.
 * The framework itself belongs to touch-st and is not on this branch; this
 * fake implements the documented helper contracts:
 *  - kshim_touch_set_reset(dev, asserted) drives the reset GPIO to the
 *    logical value `asserted` (DT flags applied: physical = asserted ^
 *    active_low);
 *  - kshim_touch_irq_gpio_output(dev, level_high) drives the physical level;
 *  - kshim_touch_power_supplies() enables every dev->supplies[] in order
 *    (refcounted; an inherited-on rail stays on when disabled);
 *  - kshim_touch_i2c_write_read() is one write then a repeated-START read
 *    on dev->i2c.addr; kshim_touch_spi_sync() is one chip-select assertion.
 * A chip model plugs in through struct fake_chip and sees physical pin
 * levels, rail state and bus traffic with the fake time.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kshim/io.h>
#include <kshim/regulator.h>
#include <kshim/time.h>
#include <kshim/touch_driver.h>

#define FAKE_TOUCH_EVENTS 128U
#define FAKE_RESET_PIN 161U
#define FAKE_IRQ_PIN 162U

struct kshim_regulator {
    const char *name;
    bool on;
    bool inherited_on;
    unsigned refs;
    unsigned enables;     /* off -> on transitions programmed */
};

struct fake_chip {
    void *ctx;
    /* Physical pin levels seen by the chip. */
    void (*power)(void *ctx, bool on);
    void (*rstb)(void *ctx, bool level);
    void (*int_drive)(void *ctx, bool driven, bool level);
    /* INT level the chip drives (host not driving). */
    bool (*int_level)(void *ctx);
    /* Bus. */
    int (*i2c)(void *ctx, uint16_t addr, const uint8_t *tx, size_t tx_length,
               uint8_t *rx, size_t rx_length);
    int (*spi)(void *ctx, const uint8_t *mosi, uint8_t *miso, size_t length);
};

struct fake_touch_event {
    uint8_t id;
    kshim_touch_contact_action_t action;
    uint32_t x, y;
};

struct fake_world {
    uint64_t now_us;
    struct fake_chip chip;
    /* Reset pad: driven by the host, else pulled to reset_pull_high. */
    bool reset_driven, reset_physical, reset_pull_high;
    bool int_driven, int_physical;
    struct fake_touch_event events[FAKE_TOUCH_EVENTS];
    unsigned event_count;
    unsigned release_all_calls;
    unsigned i2c_transfers, spi_messages;
    int fail_bus;          /* non-zero: a bus access returns it ... */
    unsigned fail_skip;    /* ... after this many more succeed */
    unsigned supply_calls_on, supply_calls_off;
};

extern struct fake_world fake;

/* Clear the world (time 0, no chip, pads undriven). */
void fake_world_reset(void);
/* A device as the framework would hand it to the driver after selection:
 * bus and client/device, reset GPIO 161 and IRQ GPIO 162 (active_low per
 * argument), supplies, props. priv must hold driver->priv_size bytes. */
void fake_dev_init(struct kshim_touch_dev *dev, const struct kshim_touch_driver *driver,
                   unsigned bus, void *priv);
void fake_dev_set_reset(struct kshim_touch_dev *dev, bool present, bool active_low);
void fake_dev_set_irq_gpio(struct kshim_touch_dev *dev, bool present, bool active_low);
void fake_dev_add_supply(struct kshim_touch_dev *dev, struct kshim_regulator *regulator);

void fake_events_clear(void);
void fake_expect_event(unsigned index, uint8_t id, kshim_touch_contact_action_t action,
                       uint32_t x, uint32_t y);
