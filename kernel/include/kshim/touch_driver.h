/* SPDX-License-Identifier: MIT */
#pragma once
/*
 * Touch controller framework (CONFIG_KSHIM_TOUCH with any KSHIM_TOUCH_<chip>).
 * Owner: touch-st (kernel/drivers/input/touch/core.c, kernel/core/input.c
 * touch service, kernel/drivers/touch.c tracker). Drivers: touch-st (FTM4,
 * FTM5), touch-goodix (GT911, GT9916). See docs/io-stack.md.
 *
 * The framework implements kshim_input_touch_start/poll (include/kshim/input.h)
 * and kshim_input_touch_stop (platform.h):
 *  1. Selection: for each driver in kshim_touch_drivers[] order, each
 *     compatible, each available DT node: the parent must resolve as an I2C
 *     adapter (kshim_i2c_adapter_get) or SPI controller
 *     (kshim_spi_controller_get) allowed by bus_mask; driver->match may
 *     reject the node (vendor property family, panel selection). The first
 *     node that powers and probes successfully is the active device; one
 *     active device per boot.
 *  2. Common resources from DT: pinctrl "default"; reset GPIO from
 *     "reset-gpios" (driver->reset_gpio_names overrides, for Android
 *     "fts,reset-gpio"); IRQ from "interrupts"/"interrupts-extended" (TLMM
 *     parent: pin + trigger), else from the IRQ GPIO; IRQ GPIO from
 *     "irq-gpios" (driver->irq_gpio_names overrides), else the TLMM pin of
 *     the interrupt; supplies driver->supplies[] ("<name>-supply"; missing
 *     ones are skipped). Common binding properties touchscreen-size-x/y,
 *     touchscreen-swapped-x-y, touchscreen-inverted-x/y fill dev->props.
 *  3. Power on (driver->power_on, else the common sequence): reset asserted
 *     (when present), supplies on in table order, power_on_delay_us, reset
 *     released, post_reset_delay_us. Then driver->probe.
 *  4. Events: IRQ-driven when the TLMM interrupt resolves and is live
 *     (kshim_gpio_irq_request). The framework's handler masks the pin
 *     (kshim_gpio_irq_mask) and marks the device pending; it never touches
 *     the bus. kshim_input_touch_poll() (thread context) then calls
 *     driver->handle and unmasks. Polling fallback: no interrupt, or an
 *     active IRQ level held 100 ms without delivery -> driver->pending (or
 *     the IRQ GPIO level) decides when to call handle each poll.
 *  5. Reports: handle calls kshim_touch_report() in controller coordinates;
 *     the framework applies swap/invert, scales to the surface through the
 *     tracker (touch.h) and queues PRESS/MOVE/RELEASE for the emit callback.
 *  6. Stop/quiesce: free the IRQ, driver->quiesce (stop scanning, bounded),
 *     cancel the active contact, reset asserted only if it was asserted at
 *     entry, supplies disabled (back to inherited), pins restored by the GPIO
 *     registry. Idempotent; kshim_input_quiesce() (step 1) and
 *     kshim_io_quiesce() both call it.
 *
 * Return codes: 0 or a positive ERROR_ or KSHIM_IO_ERR_ code (kshim/io.h); a driver
 * wrapping legacy negative-errno code converts at its boundary.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "error_no.h"
#include <kshim/gpio.h>
#include <kshim/i2c.h>
#include <kshim/spi.h>
#include <touch.h>

#define KSHIM_TOUCH_BUS_I2C 0x1U
#define KSHIM_TOUCH_BUS_SPI 0x2U

#define KSHIM_TOUCH_MAX_SUPPLIES 4U
#define KSHIM_TOUCH_PRIV_MAX 1024U

struct kshim_regulator;
struct kshim_touch_dev;

/* DT common touchscreen binding (touchscreen.yaml). size 0 = not given. */
struct kshim_touch_props {
    uint32_t size_x, size_y;
    bool swap_xy, invert_x, invert_y;
};

struct kshim_touch_driver {
    const char *name;
    const char *const *compatibles;     /* NULL-terminated */
    unsigned bus_mask;                  /* KSHIM_TOUCH_BUS_* */
    /* Optional overrides of the GPIO property names (NULL-terminated lists,
     * first present wins); NULL = the generic names. */
    const char *const *reset_gpio_names;
    const char *const *irq_gpio_names;
    /* Supply names in power-on order, NULL-terminated; NULL = none. */
    const char *const *supplies;
    uint32_t power_on_delay_us;         /* supplies on -> reset release */
    uint32_t post_reset_delay_us;       /* reset release -> probe */
    size_t priv_size;                   /* zeroed bytes at dev->priv, <= KSHIM_TOUCH_PRIV_MAX */
    /* Optional, no bus I/O: 0 accepts the node, ERROR_NOT_FOUND skips it,
     * any other code skips it with a log line. */
    int (*match)(struct kshim_touch_dev *dev, const void *fdt);
    /* Optional replacement of the common power-on sequence (GT911 drives the
     * IRQ GPIO during reset to select its I2C address). Use
     * kshim_touch_power_supplies(), kshim_touch_set_reset(),
     * kshim_touch_irq_gpio_output()/_input(). */
    int (*power_on)(struct kshim_touch_dev *dev);
    /* Identify the chip, read its resolution/config into dev->max_x/max_y
     * (when the DT gives no size) and enable reporting. Bus I/O allowed. */
    int (*probe)(struct kshim_touch_dev *dev);
    /* Read and report everything pending, then clear the controller's
     * interrupt so a level IRQ deasserts. Thread context. */
    int (*handle)(struct kshim_touch_dev *dev);
    /* Optional polling hint; NULL = the IRQ GPIO is at its active level. */
    bool (*pending)(struct kshim_touch_dev *dev);
    /* Stop scanning / enter sleep; bounded; bus I/O allowed. */
    int (*quiesce)(struct kshim_touch_dev *dev);
};

struct kshim_touch_dev {
    const struct kshim_touch_driver *driver;
    const void *fdt;
    int node;
    unsigned bus;                       /* KSHIM_TOUCH_BUS_I2C or _SPI */
    struct kshim_i2c_client i2c;
    struct kshim_spi_device spi;
    struct kshim_gpio reset_gpio, irq_gpio;
    bool has_reset, has_irq_gpio;
    struct kshim_regulator *supplies[KSHIM_TOUCH_MAX_SUPPLIES];
    unsigned supply_count;
    struct kshim_touch_props props;
    uint32_t irq_pin;                   /* TLMM pin, valid with has_irq */
    unsigned irq_trigger;               /* enum kshim_irq_trigger */
    bool has_irq, irq_live;
    volatile bool irq_pending;
    uint32_t max_x, max_y;              /* controller coordinate range (exclusive) */
    uint8_t max_contacts;               /* <= KSHIM_TOUCH_MAX_CONTACTS */
    kshim_touch_t tracker;
    void *priv;                         /* driver state, priv_size bytes */
    bool powered, probed;
};

/* Registry: the framework walks this NULL-terminated table
 * (kernel/drivers/input/touch/registry.c, one #if per driver). */
extern const struct kshim_touch_driver *const kshim_touch_drivers[];
extern const struct kshim_touch_driver kshim_touch_driver_ftm4;
extern const struct kshim_touch_driver kshim_touch_driver_ftm5;
extern const struct kshim_touch_driver kshim_touch_driver_gt911;
extern const struct kshim_touch_driver kshim_touch_driver_gt9916;

/* ---- Helpers for drivers (thread context) -------------------------------- */

/* Report one contact in controller coordinates (0..max-1). */
int kshim_touch_report(struct kshim_touch_dev *dev, uint8_t contact_id,
                       kshim_touch_contact_action_t action, uint32_t x, uint32_t y);
/* Release every contact the controller no longer reports (lift-off without
 * per-contact events). */
void kshim_touch_report_release_all(struct kshim_touch_dev *dev);

/* Bus helpers on the device's own client. I2C: write then read with a
 * repeated START (rx_length 0 = write only). SPI: kshim_spi_sync on dev->spi. */
int kshim_touch_i2c_write_read(struct kshim_touch_dev *dev, const void *tx, size_t tx_length,
                               void *rx, size_t rx_length);
int kshim_touch_spi_sync(struct kshim_touch_dev *dev, const struct kshim_spi_xfer *xfers,
                         size_t count);

/* Power-sequence building blocks for driver->power_on. */
int kshim_touch_power_supplies(struct kshim_touch_dev *dev, bool on);
int kshim_touch_set_reset(struct kshim_touch_dev *dev, bool asserted);
/* Drive / release the IRQ GPIO before the interrupt is requested. */
int kshim_touch_irq_gpio_output(struct kshim_touch_dev *dev, bool level_high);
int kshim_touch_irq_gpio_input(struct kshim_touch_dev *dev);

/* Handoff (kshim_io_quiesce, after PCI): stop the active device. Idempotent. */
int kshim_touch_quiesce_all(void);
