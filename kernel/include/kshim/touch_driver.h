/* SPDX-License-Identifier: MIT */
#pragma once
/*
 * Touch controller framework (CONFIG_KSHIM_TOUCH with any KSHIM_TOUCH_<chip>).
 * Owner: touch-st (kernel/drivers/input/touch/core.c, kernel/core/input.c
 * touch service, kernel/drivers/touch.c tracker). Drivers: touch-st (FTM4,
 * FTM5), touch-goodix (GT911, GT9916). See docs/io-stack.md.
 *
 * The framework (kernel/drivers/input/touch/core.c) implements the touch
 * service behind kshim_input_touch_start/poll (include/kshim/input.h) and
 * kshim_input_touch_stop (platform.h), which kernel/core/input.c forwards to
 * kshim_touch_framework_start/poll/stop below:
 *  1. Selection: for each driver in kshim_touch_drivers[] order, each
 *     compatible, each available DT node, in DT order. The first node that
 *     matches, powers and probes is the active device; one per boot.
 *  2. DT resources, no device access: touchscreen-size-x/y,
 *     touchscreen-swapped-x-y and touchscreen-inverted-x/y fill dev->props;
 *     reset GPIO from "reset-gpios" (driver->reset_gpio_names overrides, for
 *     Android "fts,reset-gpio"); IRQ GPIO from "irq-gpios"
 *     (driver->irq_gpio_names overrides); supplies driver->supplies[]
 *     ("<name>-supply"; absent ones are skipped, an unsupported provider is
 *     logged and left as inherited); the interrupt from
 *     "interrupts"/"interrupts-extended" when its parent is the TLMM.
 *  3. driver->match(dev, fdt): no bus I/O. It sees the resolved resources
 *     and may adjust them (vendor size/flip properties into dev->props, the
 *     polarity of Android raw-level GPIO properties) or reject the node
 *     (ERROR_NOT_FOUND: vendor property family, panel selection). dev->bus is
 *     the driver's bus when bus_mask names exactly one, else 0.
 *  4. IRQ reconciliation: without an "interrupts" entry the TLMM pin of the
 *     IRQ GPIO is the interrupt (level, at the GPIO's active level); without
 *     an IRQ GPIO the interrupt's TLMM pin is the IRQ GPIO. When both name
 *     the same pin the interrupt trigger decides the GPIO's active level
 *     (LEVEL_LOW/EDGE_FALLING = active low); a trigger of NONE follows the
 *     GPIO. Different pins: the interrupt is dropped and the GPIO polled.
 *  5. Bus: the parent must resolve as an I2C adapter (kshim_i2c_adapter_get)
 *     or SPI controller (kshim_spi_controller_get) allowed by bus_mask; this
 *     is the first device access (the controller is probed on demand).
 *  6. Power on. Common sequence: reset asserted (when present), supplies on
 *     in table order, the pinctrl state "default" (Android "pmx_ts_active"
 *     when there is no "default"), applied only once the supplies are on so
 *     no pull drives an unpowered IC, IRQ GPIO as input, power_on_delay_us,
 *     reset released, post_reset_delay_us. With driver->power_on the
 *     pinctrl state is applied first, then power_on runs, then the IRQ GPIO
 *     is made an input. The tracker is set up (provisional range), then
 *     driver->probe, which finds dev->max_x/max_y preset to the DT size (0
 *     when absent), then the tracker gets the final range: dev->props size
 *     when the DT gives one, else dev->max_x/max_y from probe. A failed
 *     probe runs driver->quiesce (bounded) and releases everything again.
 *  7. Events: IRQ-driven when CONFIG_KSHIM_INPUT_IRQ and the TLMM interrupt
 *     can be requested (kshim_gpio_irq_request). The framework's handler
 *     masks the pin (kshim_gpio_irq_mask) and marks the device pending; it
 *     never touches the bus. kshim_input_touch_poll() (thread context) then
 *     calls driver->handle and unmasks. One handle pass runs right after the
 *     interrupt is requested (anything latched before it). Polling fallback:
 *     no interrupt, a TLMM cascade that sealed the pin, or the IRQ GPIO held
 *     at its active level for 100 ms without a delivery; then
 *     driver->pending (or the IRQ GPIO level; no GPIO = always) decides when
 *     each poll calls handle. A handle error disables touch (contact
 *     cancelled, interrupt freed); the device is still quiesced at stop.
 *  8. Reports: handle calls kshim_touch_report() in controller coordinates
 *     (or updates dev->tracker directly). The tracker (touch.h) is set up
 *     with the touchscreen.yaml transform, inversion in controller space and
 *     then the swap (as its flip plus a 90 degree rotation), scales to the
 *     surface and emits PRESS/MOVE/RELEASE synchronously to the emit
 *     callback. kshim_touch_report() also counts a MOVE for a contact that is
 *     not down as ENTER, so frame-based controllers may report every contact
 *     present as MOVE.
 *  9. Stop/quiesce: free the IRQ, driver->quiesce (stop scanning, bounded;
 *     if it fails and a reset GPIO exists, holding reset for
 *     power_on_delay_us stops the controller instead, otherwise the error
 *     is returned and the next call retries), cancel the active contact,
 *     reset asserted (KSHIM_TOUCH_RESET_LEVEL: see that enum; never driven
 *     to the reset level), supplies disabled newest first (back to the
 *     inherited state), IRQ and reset GPIOs freed (the GPIO registry
 *     restores their snapshots, so reset ends as it was found; a pad that
 *     also has a pinctrl state is restored with the pinctrl snapshots at
 *     handoff). Idempotent; kshim_input_quiesce() (step 1) and
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

/* Meaning of the reset GPIO's logical value (driver->reset_convention). */
enum kshim_touch_reset_convention {
    /* Generic *-reset-gpios: logical 1 = held in reset (the DT flags give
     * the physical level). Stop asserts reset while the rails go down. */
    KSHIM_TOUCH_RESET_ASSERTED = 0,
    /* goodix,gt9xx: the logical value is the RSTB level, 0 = held in reset
     * (all mainline goodix,gt911 nodes write GPIO_ACTIVE_HIGH).
     * kshim_touch_set_reset(dev, asserted) drives the logical !asserted.
     * Stop never drives RSTB low: a controller kShimC never held in reset
     * (inherited running) keeps RSTB as it was; one kShimC reset and powered
     * itself has its RSTB pad turned into an input before the rails drop. */
    KSHIM_TOUCH_RESET_LEVEL,
};

struct kshim_touch_driver {
    const char *name;
    const char *const *compatibles;     /* NULL-terminated */
    unsigned bus_mask;                  /* KSHIM_TOUCH_BUS_* */
    unsigned reset_convention;          /* enum kshim_touch_reset_convention */
    /* Optional overrides of the GPIO property names (NULL-terminated lists,
     * first present wins); NULL = the generic names. */
    const char *const *reset_gpio_names;
    const char *const *irq_gpio_names;
    /* Supply names in power-on order, NULL-terminated; NULL = none. */
    const char *const *supplies;
    uint32_t power_on_delay_us;         /* supplies on -> reset release */
    uint32_t post_reset_delay_us;       /* reset release -> probe */
    size_t priv_size;                   /* zeroed bytes at dev->priv, <= KSHIM_TOUCH_PRIV_MAX */
    /* Optional, no bus I/O, runs after the DT resources are resolved (step
     * 3): 0 accepts the node, ERROR_NOT_FOUND skips it, any other code skips
     * it with a log line. May adjust dev->props and the GPIO descriptors. */
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

/* Power-sequence building blocks for driver->power_on. set_reset takes
 * "held in reset" and maps it through driver->reset_convention. */
int kshim_touch_power_supplies(struct kshim_touch_dev *dev, bool on);
int kshim_touch_set_reset(struct kshim_touch_dev *dev, bool asserted);
/* Drive / release the IRQ GPIO before the interrupt is requested. */
int kshim_touch_irq_gpio_output(struct kshim_touch_dev *dev, bool level_high);
int kshim_touch_irq_gpio_input(struct kshim_touch_dev *dev);

/* Handoff (kshim_io_quiesce, after PCI): stop the active device. Idempotent. */
int kshim_touch_quiesce_all(void);

/* ---- Touch service (kernel/core/input.c), additive to the architect API -- */

/* Select, power and probe the first matching controller of `fdt` and scale
 * its reports to an output_width x output_height surface; events go to emit
 * from kshim_touch_framework_poll(). 0 = ready (calling it again only
 * updates the surface and callback); ERROR_NOT_FOUND = no node matched any
 * driver (no device was touched); any other code = every candidate failed
 * and was released again. Thread context. */
int kshim_touch_framework_start(const void *fdt, uint32_t output_width, uint32_t output_height,
                                kshim_touch_event_fn emit, void *context);
/* Service the active device once (deferred IRQ work or polling). 0, or the
 * handle error after which touch is disabled until stop. Boot CPU only;
 * other CPUs return 0. */
int kshim_touch_framework_poll(void);
/* Same as kshim_touch_quiesce_all(). */
int kshim_touch_framework_stop(void);
/* The active device, or NULL (diagnostics, host tests). */
const struct kshim_touch_dev *kshim_touch_active(void);

/* Android board selection shared by drivers' match hooks (kernel/dt/touch.c):
 * false when the controller node's bus names another touch controller in
 * "qcom,i2c-touch-active" (an empty string selects none), or when the node
 * lists "panel" phandles and none of them is the panel of an active DSI
 * display ("qcom,dsi-display-active" + "qcom,dsi-panel"). */
bool kshim_touch_dt_selected(const void *fdt, int node, const char *compatible);
