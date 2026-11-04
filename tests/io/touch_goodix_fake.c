/* SPDX-License-Identifier: MIT */
/* Host fake of the touch framework helpers for the Goodix tests; see
 * touch_goodix_fake.h for the contracts it implements. */
#include "touch_goodix_fake.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <kshim/gpio.h>
#include <kshim/time.h>

struct fake_world fake;

void fake_world_reset(void)
{
    memset(&fake, 0, sizeof(fake));
}

void fake_dev_init(struct kshim_touch_dev *dev, const struct kshim_touch_driver *driver,
                   unsigned bus, void *priv)
{
    memset(dev, 0, sizeof(*dev));
    memset(priv, 0, driver->priv_size);
    dev->driver = driver;
    dev->node = -1;
    dev->bus = bus;
    dev->priv = priv;
    dev->reset_gpio.slot = -1;
    dev->irq_gpio.slot = -1;
    if (bus == KSHIM_TOUCH_BUS_SPI) {
        /* As the SPI core would resolve a node without spi-cpha/cpol. */
        dev->spi.max_speed_hz = 1000000;
        dev->spi.bits_per_word = 0; /* the driver must set 8 */
    }
}

void fake_dev_set_reset(struct kshim_touch_dev *dev, bool present, bool active_low)
{
    dev->has_reset = present;
    dev->reset_gpio.chip = KSHIM_GPIO_CHIP_TLMM;
    dev->reset_gpio.pin = FAKE_RESET_PIN;
    dev->reset_gpio.active_low = active_low;
    dev->reset_gpio.dt_flags = active_low ? KSHIM_GPIO_DT_ACTIVE_LOW : 0;
}

void fake_dev_set_irq_gpio(struct kshim_touch_dev *dev, bool present, bool active_low)
{
    dev->has_irq_gpio = present;
    dev->irq_gpio.chip = KSHIM_GPIO_CHIP_TLMM;
    dev->irq_gpio.pin = FAKE_IRQ_PIN;
    dev->irq_gpio.active_low = active_low;
    dev->has_irq = true;
    dev->irq_pin = FAKE_IRQ_PIN;
}

void fake_dev_add_supply(struct kshim_touch_dev *dev, struct kshim_regulator *regulator)
{
    assert(dev->supply_count < KSHIM_TOUCH_MAX_SUPPLIES);
    dev->supplies[dev->supply_count++] = regulator;
}

void fake_events_clear(void)
{
    fake.event_count = 0;
}

void fake_expect_event(unsigned index, uint8_t id, kshim_touch_contact_action_t action,
                       uint32_t x, uint32_t y)
{
    assert(index < fake.event_count);
    const struct fake_touch_event *e = &fake.events[index];
    if (e->id != id || e->action != action || e->x != x || e->y != y) {
        fprintf(stderr, "event %u: got id %u action %d (%u,%u), want id %u action %d (%u,%u)\n",
                index, e->id, (int)e->action, e->x, e->y, id, (int)action, x, y);
        assert(0);
    }
}

/* ---- Clock ---------------------------------------------------------------- */

uint64_t delay(uint64_t micro_seconds)
{
    fake.now_us += micro_seconds;
    return micro_seconds;
}

uint64_t get_current_time(void)
{
    return fake.now_us / 1000U;
}

const char *kshim_io_strerror(int status)
{
    static char text[16];
    snprintf(text, sizeof(text), "code %d", status);
    return text;
}

/* ---- Regulators and GPIOs --------------------------------------------------- */

bool kshim_regulator_is_enabled(const struct kshim_regulator *regulator)
{
    return regulator->on;
}

static bool any_supply_on(const struct kshim_touch_dev *dev)
{
    for (unsigned i = 0; i < dev->supply_count; ++i)
        if (dev->supplies[i]->on)
            return true;
    return dev->supply_count == 0;
}

int kshim_touch_power_supplies(struct kshim_touch_dev *dev, bool on)
{
    bool before = any_supply_on(dev);
    if (on) {
        ++fake.supply_calls_on;
        for (unsigned i = 0; i < dev->supply_count; ++i) {
            struct kshim_regulator *r = dev->supplies[i];
            if (!r->refs++ && !r->on) {
                r->on = true;
                ++r->enables;
            }
        }
    } else {
        ++fake.supply_calls_off;
        for (unsigned i = dev->supply_count; i-- > 0;) {
            struct kshim_regulator *r = dev->supplies[i];
            assert(r->refs > 0);
            if (!--r->refs && !r->inherited_on)
                r->on = false;
        }
    }
    dev->powered = on;
    bool after = any_supply_on(dev);
    if (before != after && fake.chip.power)
        fake.chip.power(fake.chip.ctx, after);
    return 0;
}

int kshim_touch_set_reset(struct kshim_touch_dev *dev, bool asserted)
{
    assert(dev->has_reset);
    bool physical = asserted != dev->reset_gpio.active_low;
    bool before = fake.reset_driven ? fake.reset_physical : fake.reset_pull_high;
    fake.reset_driven = true;
    fake.reset_physical = physical;
    if (before != physical && fake.chip.rstb)
        fake.chip.rstb(fake.chip.ctx, physical);
    return 0;
}

int kshim_touch_irq_gpio_output(struct kshim_touch_dev *dev, bool level_high)
{
    assert(dev->has_irq_gpio);
    fake.int_driven = true;
    fake.int_physical = level_high;
    if (fake.chip.int_drive)
        fake.chip.int_drive(fake.chip.ctx, true, level_high);
    return 0;
}

int kshim_touch_irq_gpio_input(struct kshim_touch_dev *dev)
{
    assert(dev->has_irq_gpio);
    fake.int_driven = false;
    if (fake.chip.int_drive)
        fake.chip.int_drive(fake.chip.ctx, false, false);
    return 0;
}

int kshim_gpio_get(const struct kshim_gpio *gpio, bool *active)
{
    bool physical;
    if (gpio->pin == FAKE_RESET_PIN)
        physical = fake.reset_driven ? fake.reset_physical : fake.reset_pull_high;
    else if (gpio->pin == FAKE_IRQ_PIN)
        physical = fake.int_driven ? fake.int_physical
                                   : (fake.chip.int_level ? fake.chip.int_level(fake.chip.ctx)
                                                          : true);
    else
        return ERROR_NOT_FOUND;
    *active = physical != gpio->active_low;
    return 0;
}

/* ---- Bus ---------------------------------------------------------------------- */

static int take_failure(void)
{
    if (fake.fail_bus && fake.fail_skip) {
        --fake.fail_skip;
        return 0;
    }
    int status = fake.fail_bus;
    fake.fail_bus = 0;
    return status;
}

int kshim_touch_i2c_write_read(struct kshim_touch_dev *dev, const void *tx, size_t tx_length,
                               void *rx, size_t rx_length)
{
    struct kshim_i2c_adapter *adapter = dev->i2c.adapter;
    assert(dev->bus == KSHIM_TOUCH_BUS_I2C);
    assert(adapter && adapter->ops && adapter->ops->xfer);
    assert(tx && tx_length && tx_length <= UINT16_MAX && rx_length <= UINT16_MAX);
    assert(rx || !rx_length);
    const struct kshim_i2c_msg msgs[2] = {
        { .addr = dev->i2c.addr, .flags = dev->i2c.flags, .len = (uint16_t)tx_length,
          .buf = (uint8_t *)(uintptr_t)tx },
        { .addr = dev->i2c.addr, .flags = (uint16_t)(dev->i2c.flags | KSHIM_I2C_M_RD),
          .len = (uint16_t)rx_length, .buf = rx },
    };
    ++fake.i2c_transfers;
    int status = take_failure();
    if (status)
        return status;
    return adapter->ops->xfer(adapter, msgs, rx_length ? 2 : 1, KSHIM_I2C_DEFAULT_TIMEOUT_US);
}

int kshim_touch_spi_sync(struct kshim_touch_dev *dev, const struct kshim_spi_xfer *xfers,
                         size_t count)
{
    struct kshim_spi_controller *controller = dev->spi.controller;
    assert(dev->bus == KSHIM_TOUCH_BUS_SPI);
    assert(controller && controller->ops && controller->ops->transfer);
    assert(xfers && count >= 1);
    ++fake.spi_messages;
    int status = take_failure();
    if (status)
        return status;
    return controller->ops->transfer(controller, &dev->spi, xfers, count,
                                     KSHIM_SPI_DEFAULT_TIMEOUT_US);
}

/* ---- Reports ------------------------------------------------------------------ */

int kshim_touch_report(struct kshim_touch_dev *dev, uint8_t contact_id,
                       kshim_touch_contact_action_t action, uint32_t x, uint32_t y)
{
    (void)dev;
    assert(contact_id < KSHIM_TOUCH_MAX_CONTACTS);
    assert(fake.event_count < FAKE_TOUCH_EVENTS);
    fake.events[fake.event_count++] = (struct fake_touch_event){ contact_id, action, x, y };
    return 0;
}

void kshim_touch_report_release_all(struct kshim_touch_dev *dev)
{
    (void)dev;
    ++fake.release_all_calls;
}
