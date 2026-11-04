/* SPDX-License-Identifier: MIT */
/*
 * Touch controller framework (kshim/touch_driver.h): DT selection over the
 * driver table, common resources (bus client, reset/IRQ GPIOs, supplies,
 * pinctrl, touchscreen.yaml properties), the power sequence, the deferred
 * TLMM interrupt with its polling fallback, coordinate transform through the
 * tracker (touch.h) and the idempotent stop/quiesce. One active device.
 *
 * The IRQ handler only masks the pin and marks the device pending; the bus
 * is touched only from kshim_touch_framework_poll() (thread context, boot
 * CPU), as docs/io-stack.md "Deferred interrupt model" requires.
 */
#include <config.h>
#include <kshim/touch_driver.h>
#include <kshim/io.h>
#include <kshim/gpio.h>
#include <kshim/pinctrl.h>
#include <kshim/regulator.h>
#include <kshim/time.h>
#include <dt.h>

#include <stdio.h>
#include <string.h>

#if CONFIG_QCOM_IRQCHIP
#include <qcom_irq.h>
#endif

#define TOUCH_STUCK_MS 100U        /* docs/io-stack.md polling fallback */
#define TOUCH_HINT_PERIOD_MS 10U   /* driver->pending hint rate in IRQ mode */
#define TOUCH_PROVISIONAL_RANGE 4096U
#define TOUCH_MAX_RANGE 65536U

/* touchscreen.yaml trigger values (kshim/irq.h enum kshim_irq_trigger),
 * spelled out so the file builds without the interrupt core. */
#define TRIGGER_NONE 0U
#define TRIGGER_EDGE_RISING 1U
#define TRIGGER_EDGE_FALLING 2U
#define TRIGGER_LEVEL_HIGH 4U
#define TRIGGER_LEVEL_LOW 8U

static const char *const GenericReset[] = {"reset-gpios", NULL};
static const char *const GenericIrq[] = {"irq-gpios", NULL};
/* Android touch nodes name their active state "pmx_ts_active". */
static const char *const PinctrlStates[] = {"default", "pmx_ts_active", NULL};

static struct kshim_touch_dev Dev;
static uint64_t Priv[KSHIM_TOUCH_PRIV_MAX / sizeof(uint64_t)];

static struct {
    const void *fdt;
    kshim_touch_event_fn emit;
    void *emit_context;
    uint32_t out_w, out_h;
    bool active;            /* Dev powered and probed */
    bool failed;            /* a handle error disabled event processing */
    bool reset_held, irq_gpio_held;
    uint8_t supplies_on;    /* bit i: supplies[i] enabled by us */
    int tlmm;               /* TLMM node of the interrupt, or -1 */
    bool irq_tried;
    volatile uint32_t irq_count;
    uint32_t stuck_count;
    uint64_t stuck_since, hint_at;
    bool stuck_armed, hint_armed;
} T = {.tlmm = -1};

const struct kshim_touch_dev *kshim_touch_active(void)
{
    return T.active ? &Dev : NULL;
}

static const char *node_name(const struct kshim_touch_dev *dev)
{
    const char *name = dev->fdt ? fdt_get_name(dev->fdt, dev->node, NULL) : NULL;
    return name ? name : "?";
}

/* ---- Bus helpers --------------------------------------------------------- */

int kshim_touch_i2c_write_read(struct kshim_touch_dev *dev, const void *tx, size_t tx_length,
                               void *rx, size_t rx_length)
{
    if (!dev || dev->bus != KSHIM_TOUCH_BUS_I2C || !tx || !tx_length ||
        (rx_length && !rx)) return ERROR_INVALID_PARAMETER;
#if CONFIG_KSHIM_I2C
    if (!rx_length) return kshim_i2c_write(&dev->i2c, tx, tx_length, 0);
    return kshim_i2c_write_read(&dev->i2c, tx, tx_length, rx, rx_length, 0);
#else
    return ERROR_NOT_SUPPORTED;
#endif
}

int kshim_touch_spi_sync(struct kshim_touch_dev *dev, const struct kshim_spi_xfer *xfers,
                         size_t count)
{
    if (!dev || dev->bus != KSHIM_TOUCH_BUS_SPI || !xfers || !count)
        return ERROR_INVALID_PARAMETER;
#if CONFIG_KSHIM_SPI
    return kshim_spi_sync(&dev->spi, xfers, count, 0);
#else
    return ERROR_NOT_SUPPORTED;
#endif
}

/* ---- Power-sequence building blocks -------------------------------------- */

int kshim_touch_power_supplies(struct kshim_touch_dev *dev, bool on)
{
    if (!dev) return ERROR_INVALID_PARAMETER;
    if (on) {
        for (unsigned i = 0; i < dev->supply_count; ++i) {
            if (T.supplies_on & (1U << i)) continue;
            int status = kshim_regulator_enable(dev->supplies[i]);
            if (status) {
                printf("Touch: supply %s: %s\n", kshim_regulator_name(dev->supplies[i]),
                       kshim_io_strerror(status));
                (void)kshim_touch_power_supplies(dev, false);
                return status;
            }
            T.supplies_on |= (uint8_t)(1U << i);
        }
        return 0;
    }
    int first = 0;
    for (unsigned i = dev->supply_count; i-- > 0;) {
        if (!(T.supplies_on & (1U << i))) continue;
        int status = kshim_regulator_disable(dev->supplies[i]);
        if (status && !first) first = status;
        /* The reference is dropped either way; a retry would unbalance it. */
        T.supplies_on &= (uint8_t)~(1U << i);
    }
    return first;
}

int kshim_touch_set_reset(struct kshim_touch_dev *dev, bool asserted)
{
    if (!dev) return ERROR_INVALID_PARAMETER;
    if (!dev->has_reset) return ERROR_NOT_FOUND;
    if (!T.reset_held) {
        int status = kshim_gpio_request(&dev->reset_gpio, "touch-reset");
        if (status) return status;
        T.reset_held = true;
    }
    return kshim_gpio_direction_output(&dev->reset_gpio, asserted);
}

static int irq_gpio_hold(struct kshim_touch_dev *dev)
{
    if (!dev->has_irq_gpio) return ERROR_NOT_FOUND;
    if (T.irq_gpio_held) return 0;
    int status = kshim_gpio_request(&dev->irq_gpio, "touch-irq");
    if (!status) T.irq_gpio_held = true;
    return status;
}

int kshim_touch_irq_gpio_output(struct kshim_touch_dev *dev, bool level_high)
{
    if (!dev) return ERROR_INVALID_PARAMETER;
    int status = irq_gpio_hold(dev);
    if (status) return status;
    /* Physical level to the logical value the GPIO API takes. */
    return kshim_gpio_direction_output(&dev->irq_gpio, level_high != dev->irq_gpio.active_low);
}

int kshim_touch_irq_gpio_input(struct kshim_touch_dev *dev)
{
    if (!dev) return ERROR_INVALID_PARAMETER;
    int status = irq_gpio_hold(dev);
    if (status) return status;
    return kshim_gpio_direction_input(&dev->irq_gpio, KSHIM_GPIO_PULL_KEEP);
}

/* ---- Reports ------------------------------------------------------------- */

int kshim_touch_report(struct kshim_touch_dev *dev, uint8_t contact_id,
                       kshim_touch_contact_action_t action, uint32_t x, uint32_t y)
{
    if (!dev || !dev->tracker.initialized || contact_id >= KSHIM_TOUCH_MAX_CONTACTS ||
        (dev->max_contacts && contact_id >= dev->max_contacts))
        return ERROR_INVALID_PARAMETER;
    /* Swap and inversion are the tracker's (tracker_setup()). */
    if (action == KSHIM_TOUCH_CONTACT_MOVE && !dev->tracker.contacts[contact_id].active)
        action = KSHIM_TOUCH_CONTACT_ENTER;
    return kshim_touch_update(&dev->tracker, contact_id, action, x, y) ? ERROR_INVALID_PARAMETER : 0;
}

void kshim_touch_report_release_all(struct kshim_touch_dev *dev)
{
    if (!dev || !dev->tracker.initialized) return;
    for (uint8_t id = 0; id < KSHIM_TOUCH_MAX_CONTACTS; ++id)
        if (dev->tracker.contacts[id].active)
            (void)kshim_touch_update(&dev->tracker, id, KSHIM_TOUCH_CONTACT_LEAVE, 0, 0);
}

/* Controller range: the DT size wins, then probe's max_x/max_y. */
static int tracker_setup(struct kshim_touch_dev *dev, bool provisional)
{
    uint32_t w = dev->props.size_x ? dev->props.size_x : dev->max_x;
    uint32_t h = dev->props.size_y ? dev->props.size_y : dev->max_y;
    if (provisional) {
        if (!w) w = TOUCH_PROVISIONAL_RANGE;
        if (!h) h = TOUCH_PROVISIONAL_RANGE;
    }
    if (!w || !h || w > TOUCH_MAX_RANGE || h > TOUCH_MAX_RANGE) return ERROR_INVALID_PARAMETER;
    /* touchscreen.yaml: invert in controller space, then swap. The tracker
     * flips first and rotates after, and a 90 degree rotation of a Y-flipped
     * point is the swap: (x, H-1-y) -> (H-1-(H-1-y), x) = (y, x). So a swap
     * is rotation 90 with the Y flip toggled. Controllers and the FTM5 core
     * may therefore also feed dev->tracker directly. */
    bool swap = dev->props.swap_xy;
    kshim_touch_config_t config = {
        .source_width = w, .source_height = h,
        .output_width = T.out_w, .output_height = T.out_h,
        .rotation = swap ? KSHIM_TOUCH_ROTATION_90 : KSHIM_TOUCH_ROTATION_0,
        .flip_x = dev->props.invert_x,
        .flip_y = dev->props.invert_y != swap,
        .emit = T.emit, .emit_context = T.emit_context,
    };
    return kshim_touch_init(&dev->tracker, &config) ? ERROR_INVALID_PARAMETER : 0;
}

/* ---- DT resources (no device access) ------------------------------------- */

static int read_props(struct kshim_touch_dev *dev)
{
    const void *f = dev->fdt;
    struct kshim_touch_props *p = &dev->props;
    if ((fdt_getprop(f, dev->node, "touchscreen-size-x", NULL) &&
         dt_u32(f, dev->node, "touchscreen-size-x", &p->size_x)) ||
        (fdt_getprop(f, dev->node, "touchscreen-size-y", NULL) &&
         dt_u32(f, dev->node, "touchscreen-size-y", &p->size_y)) ||
        dt_bool(f, dev->node, "touchscreen-swapped-x-y", &p->swap_xy) ||
        dt_bool(f, dev->node, "touchscreen-inverted-x", &p->invert_x) ||
        dt_bool(f, dev->node, "touchscreen-inverted-y", &p->invert_y) ||
        p->size_x > TOUCH_MAX_RANGE || p->size_y > TOUCH_MAX_RANGE)
        return ERROR_INVALID_PARAMETER;
    return 0;
}

static int read_gpio(struct kshim_touch_dev *dev, const char *const *names,
                     struct kshim_gpio *gpio, bool *present)
{
    for (; *names; ++names) {
        int status = kshim_gpio_from_dt(dev->fdt, dev->node, *names, 0, gpio);
        if (!status) { *present = true; return 0; }
        if (status != ERROR_NOT_FOUND) return status;
    }
    return 0;
}

static int read_supplies(struct kshim_touch_dev *dev)
{
    const char *const *names = dev->driver->supplies;
    for (; names && *names; ++names) {
        struct kshim_regulator *r;
        int status = kshim_regulator_get(dev->fdt, dev->node, *names, &r);
        if (status == ERROR_NOT_FOUND) continue;
        if (status == ERROR_NOT_SUPPORTED) {
            /* Inherit first: a rail we cannot drive may well be on. */
            printf("Touch: %s-supply of %s not controllable, left as inherited\n",
                   *names, node_name(dev));
            continue;
        }
        if (status) return status;
        if (dev->supply_count == KSHIM_TOUCH_MAX_SUPPLIES) return ERROR_NOT_SUPPORTED;
        dev->supplies[dev->supply_count++] = r;
    }
    return 0;
}

static void read_irq(struct kshim_touch_dev *dev)
{
    T.tlmm = -1;
#if CONFIG_QCOM_IRQCHIP
    struct kshim_dt_irq spec;
    if (!kshim_dt_irq(dev->fdt, dev->node, 0, &spec) && spec.count == 2 &&
        kshim_dt_irq_kind(dev->fdt, spec.parent) == KSHIM_DT_IRQ_TLMM) {
        dev->has_irq = true;
        dev->irq_pin = spec.cells[0];
        dev->irq_trigger = kshim_dt_irq_trigger(spec.cells[1]);
        T.tlmm = spec.parent;
    }
#else
    (void)dev;   /* without the cascade only the IRQ GPIO is used */
#endif
}

static bool trigger_active_low(unsigned trigger)
{
    return trigger == TRIGGER_LEVEL_LOW || trigger == TRIGGER_EDGE_FALLING;
}

/* Step 4 of the contract: one pin serves as both the interrupt and the
 * polled level. */
static void reconcile_irq(struct kshim_touch_dev *dev)
{
    uint32_t gpio_pin;
    bool gpio_tlmm = dev->has_irq_gpio && !kshim_gpio_tlmm_pin(&dev->irq_gpio, &gpio_pin);
    if (dev->has_irq && dev->has_irq_gpio) {
        if (!gpio_tlmm || gpio_pin != dev->irq_pin) {
            printf("Touch: %s interrupt pin %u is not its IRQ GPIO, polling\n",
                   node_name(dev), (unsigned)dev->irq_pin);
            dev->has_irq = false;
        } else if (dev->irq_trigger != TRIGGER_NONE) {
            dev->irq_gpio.active_low = trigger_active_low(dev->irq_trigger);
        }
    } else if (dev->has_irq && T.tlmm >= 0) {
        bool low = dev->irq_trigger == TRIGGER_NONE || trigger_active_low(dev->irq_trigger);
        if (!kshim_gpio_from_tlmm_pin(dev->fdt, T.tlmm, dev->irq_pin, low, &dev->irq_gpio))
            dev->has_irq_gpio = true;
    } else if (!dev->has_irq && gpio_tlmm) {
        dev->has_irq = true;
        dev->irq_pin = gpio_pin;
        dev->irq_trigger = TRIGGER_NONE;
    }
    if (dev->has_irq && dev->irq_trigger == TRIGGER_NONE)
        dev->irq_trigger = !dev->has_irq_gpio || dev->irq_gpio.active_low ?
                           TRIGGER_LEVEL_LOW : TRIGGER_LEVEL_HIGH;
}

static int bind_bus(struct kshim_touch_dev *dev)
{
    int parent = fdt_parent_offset(dev->fdt, dev->node);
    int status = ERROR_NOT_FOUND;
    if (parent < 0) return ERROR_NOT_FOUND;
#if CONFIG_KSHIM_I2C
    if (dev->driver->bus_mask & KSHIM_TOUCH_BUS_I2C) {
        struct kshim_i2c_adapter *adapter;
        status = kshim_i2c_adapter_get(dev->fdt, parent, &adapter);
        if (!status) {
            status = kshim_i2c_client_from_node(dev->fdt, dev->node, &dev->i2c);
            if (!status) dev->bus = KSHIM_TOUCH_BUS_I2C;
            return status;
        }
        if (status != ERROR_NOT_FOUND) return status;
    }
#endif
#if CONFIG_KSHIM_SPI
    if (dev->driver->bus_mask & KSHIM_TOUCH_BUS_SPI) {
        struct kshim_spi_controller *controller;
        status = kshim_spi_controller_get(dev->fdt, parent, &controller);
        if (!status) {
            status = kshim_spi_device_from_node(dev->fdt, dev->node, &dev->spi);
            if (!status) dev->bus = KSHIM_TOUCH_BUS_SPI;
            return status;
        }
    }
#endif
    return status;
}

/* ---- Power --------------------------------------------------------------- */

static void apply_pinctrl(struct kshim_touch_dev *dev)
{
    for (const char *const *state = PinctrlStates; *state; ++state) {
        int status = kshim_pinctrl_apply(dev->fdt, dev->node, *state);
        if (status == ERROR_NOT_FOUND) continue;
        /* PMIC states and bad tables leave the pads as inherited. */
        if (status) printf("Touch: pinctrl %s of %s: %s\n", *state, node_name(dev),
                           kshim_io_strerror(status));
        return;
    }
}

static int power_on(struct kshim_touch_dev *dev)
{
    const struct kshim_touch_driver *drv = dev->driver;
    int status;
    if (drv->power_on) {
        apply_pinctrl(dev);
        status = drv->power_on(dev);
        if (!status && dev->has_irq_gpio) status = kshim_touch_irq_gpio_input(dev);
        return status;
    }
    if (dev->has_reset && (status = kshim_touch_set_reset(dev, true))) return status;
    if ((status = kshim_touch_power_supplies(dev, true))) return status;
    apply_pinctrl(dev);
    if (dev->has_irq_gpio && (status = kshim_touch_irq_gpio_input(dev))) return status;
    if (drv->power_on_delay_us) (void)delay(drv->power_on_delay_us);
    if (dev->has_reset && (status = kshim_touch_set_reset(dev, false))) return status;
    if (drv->post_reset_delay_us) (void)delay(drv->post_reset_delay_us);
    return 0;
}

/* Back to the inherited state: reset asserted while the rails go down, then
 * the GPIO registry restores both pins (reset ends as it was found). */
static int release(struct kshim_touch_dev *dev)
{
    int first = 0, status;
    if (T.reset_held) (void)kshim_gpio_direction_output(&dev->reset_gpio, true);
    if ((status = kshim_touch_power_supplies(dev, false)) && !first) first = status;
    if (T.irq_gpio_held) {
        if ((status = kshim_gpio_free(&dev->irq_gpio)) && !first) first = status;
        T.irq_gpio_held = false;
    }
    if (T.reset_held) {
        if ((status = kshim_gpio_free(&dev->reset_gpio)) && !first) first = status;
        T.reset_held = false;
    }
    dev->powered = false;
    return first;
}

/* ---- Interrupt ----------------------------------------------------------- */

#if CONFIG_KSHIM_INPUT_IRQ
static void touch_irq(uint32_t pin, void *context)
{
    struct kshim_touch_dev *dev = context;
    /* Deferred model: mask (a level line stays asserted until the bus read
     * in thread context) and record the work. */
    kshim_gpio_irq_mask(pin, true);
    dev->irq_pending = true;
    ++T.irq_count;
}
#endif

static void irq_start(struct kshim_touch_dev *dev)
{
#if CONFIG_KSHIM_INPUT_IRQ
    if (T.irq_tried || !dev->has_irq || !kshim_irq_ready()) return;
    T.irq_tried = true;
    T.irq_count = 0;
    T.stuck_armed = T.hint_armed = false;
    int status = kshim_tlmm_irq_init(dev->fdt);
    if (!status) status = kshim_gpio_irq_request(dev->irq_pin, dev->irq_trigger, touch_irq, dev);
    dev->irq_live = !status;
    /* One pass for anything latched before the interrupt existed. */
    dev->irq_pending = dev->irq_live;
    printf("Touch: %s, pin %u\n", dev->irq_live ? "interrupt-driven" : "polled",
           (unsigned)dev->irq_pin);
#else
    (void)dev;
#endif
}

static void irq_stop(struct kshim_touch_dev *dev)
{
#if CONFIG_KSHIM_INPUT_IRQ
    if (!dev->irq_live) return;
    int status = kshim_gpio_irq_free(dev->irq_pin);
    /* The pin is restored either way; a summary that failed to release is
     * the irqchip restore's (handoff step 3b) to report. */
    if (status && status != ERROR_NOT_FOUND)
        printf("Touch: interrupt free: %s\n", kshim_io_strerror(status));
#endif
    dev->irq_live = false;
    dev->irq_pending = false;
}

/* The controller says (or the IRQ GPIO shows) that events are waiting. */
static bool pending_hint(struct kshim_touch_dev *dev)
{
    if (dev->driver->pending) return dev->driver->pending(dev);
    if (!dev->has_irq_gpio) return true;
    bool active;
    return kshim_gpio_get(&dev->irq_gpio, &active) || active;
}

#if CONFIG_KSHIM_INPUT_IRQ
/* In IRQ mode the IRQ GPIO level costs no bus access; a driver hint is only
 * asked every TOUCH_HINT_PERIOD_MS when there is no GPIO to sample. */
static int irq_level_hint(struct kshim_touch_dev *dev, uint64_t now)
{
    if (dev->has_irq_gpio) {
        bool active;
        return !kshim_gpio_get(&dev->irq_gpio, &active) && active;
    }
    if (!dev->driver->pending) return 0;
    if (T.hint_armed && now - T.hint_at < TOUCH_HINT_PERIOD_MS) return -1;
    T.hint_armed = true;
    T.hint_at = now;
    return dev->driver->pending(dev);
}
#endif

static bool irq_stuck(struct kshim_touch_dev *dev)
{
#if CONFIG_KSHIM_INPUT_IRQ
    if (!kshim_gpio_irq_live(dev->irq_pin)) return true;   /* sealed by a storm */
    uint64_t now = get_current_time();
    int hint = irq_level_hint(dev, now);
    if (hint < 0) return false;   /* not sampled this time */
    if (!hint) { T.stuck_armed = false; return false; }
    if (!T.stuck_armed) {
        T.stuck_armed = true;
        T.stuck_since = now;
        T.stuck_count = T.irq_count;
        return false;
    }
    return now - T.stuck_since >= TOUCH_STUCK_MS && T.irq_count == T.stuck_count;
#else
    (void)dev;
    return false;
#endif
}

/* ---- Selection ----------------------------------------------------------- */

static void dev_reset(struct kshim_touch_dev *dev, const struct kshim_touch_driver *drv,
                      const void *fdt, int node)
{
    memset(dev, 0, sizeof(*dev));
    memset(Priv, 0, sizeof(Priv));
    dev->driver = drv;
    dev->fdt = fdt;
    dev->node = node;
    dev->priv = Priv;
    dev->max_contacts = KSHIM_TOUCH_MAX_CONTACTS;
    unsigned mask = drv->bus_mask & (KSHIM_TOUCH_BUS_I2C | KSHIM_TOUCH_BUS_SPI);
    dev->bus = mask == KSHIM_TOUCH_BUS_I2C || mask == KSHIM_TOUCH_BUS_SPI ? mask : 0;
    T.reset_held = T.irq_gpio_held = false;
    T.supplies_on = 0;
}

/* 0: active. ERROR_NOT_FOUND: not a candidate (nothing touched). */
static int try_node(const struct kshim_touch_driver *drv, const void *fdt, int node)
{
    struct kshim_touch_dev *dev = &Dev;
    dev_reset(dev, drv, fdt, node);
    if (drv->priv_size > KSHIM_TOUCH_PRIV_MAX || !drv->probe || !drv->handle)
        return ERROR_INVALID_PARAMETER;
    /* Resolution errors only count once the driver accepts the node. */
    int deferred = read_props(dev);
    int status = read_gpio(dev, drv->reset_gpio_names ? drv->reset_gpio_names : GenericReset,
                           &dev->reset_gpio, &dev->has_reset);
    if (status && !deferred) deferred = status;
    status = read_gpio(dev, drv->irq_gpio_names ? drv->irq_gpio_names : GenericIrq,
                       &dev->irq_gpio, &dev->has_irq_gpio);
    if (status && !deferred) deferred = status;
    read_irq(dev);
    if (drv->match) {
        status = drv->match(dev, fdt);
        if (status == ERROR_NOT_FOUND) return status;
        if (status) {
            printf("Touch: %s rejects %s: %s\n", drv->name, node_name(dev), kshim_io_strerror(status));
            return status;
        }
    }
    if (!deferred) deferred = read_supplies(dev);
    if (deferred) {
        printf("Touch: %s %s: %s\n", drv->name, node_name(dev), kshim_io_strerror(deferred));
        return deferred;
    }
    reconcile_irq(dev);
    /* probe sees the DT size as the default range (it may replace it). */
    dev->max_x = dev->props.size_x;
    dev->max_y = dev->props.size_y;
    status = bind_bus(dev);
    if (status) {
        if (status != ERROR_NOT_FOUND)
            printf("Touch: %s %s bus: %s\n", drv->name, node_name(dev), kshim_io_strerror(status));
        return status;
    }
    status = power_on(dev);
    dev->powered = true;
    if (!status) status = tracker_setup(dev, true);
    if (!status) {
        status = drv->probe(dev);
        if (!status) status = tracker_setup(dev, false);
        if (status && drv->quiesce) (void)drv->quiesce(dev);
    }
    if (status) {
        (void)release(dev);
        printf("Touch: %s %s: %s\n", drv->name, node_name(dev), kshim_io_strerror(status));
        return status;
    }
    dev->probed = true;
    return 0;
}

int kshim_touch_framework_start(const void *fdt, uint32_t output_width, uint32_t output_height,
                                kshim_touch_event_fn emit, void *context)
{
    if (!fdt || !output_width || !output_height) return ERROR_INVALID_PARAMETER;
    T.emit = emit;
    T.emit_context = context;
    T.out_w = output_width;
    T.out_h = output_height;
    if (T.active) return tracker_setup(&Dev, false);
    T.fdt = fdt;
    T.failed = false;
    int result = ERROR_NOT_FOUND;
    for (const struct kshim_touch_driver *const *d = kshim_touch_drivers; *d; ++d) {
        const struct kshim_touch_driver *drv = *d;
        for (const char *const *c = drv->compatibles; c && *c; ++c) {
            for (int n = fdt_node_offset_by_compatible(fdt, -1, *c); n >= 0;
                 n = fdt_node_offset_by_compatible(fdt, n, *c)) {
                if (!dt_available(fdt, n)) continue;
                int status = try_node(drv, fdt, n);
                if (!status) {
                    T.active = true;
                    T.irq_tried = false;
                    printf("Touch: %s at %s, %ux%u%s%s%s\n", drv->name, node_name(&Dev),
                           (unsigned)(Dev.props.size_x ? Dev.props.size_x : Dev.max_x),
                           (unsigned)(Dev.props.size_y ? Dev.props.size_y : Dev.max_y),
                           Dev.props.swap_xy ? " swapped" : "",
                           Dev.props.invert_x ? " inverted-x" : "",
                           Dev.props.invert_y ? " inverted-y" : "");
                    irq_start(&Dev);
                    return 0;
                }
                if (status != ERROR_NOT_FOUND || result == ERROR_NOT_FOUND) result = status;
            }
        }
    }
    /* Nothing stays half-claimed after a failed candidate. */
    memset(&Dev, 0, sizeof(Dev));
    return result;
}

static int poll_once(struct kshim_touch_dev *dev)
{
    return pending_hint(dev) ? dev->driver->handle(dev) : 0;
}

int kshim_touch_framework_poll(void)
{
    if (!T.active || T.failed || !kshim_io_on_boot_cpu()) return 0;
    struct kshim_touch_dev *dev = &Dev;
    irq_start(dev);   /* the GIC may have come up after start */
    int status = 0;
    if (dev->irq_live) {
        if (dev->irq_pending) {
            dev->irq_pending = false;
            T.stuck_armed = false;
            status = dev->driver->handle(dev);
#if CONFIG_KSHIM_INPUT_IRQ
            if (!status) kshim_gpio_irq_mask(dev->irq_pin, false);
#endif
        } else if (irq_stuck(dev)) {
            irq_stop(dev);
            printf("Touch: interrupt not delivered, polling\n");
            status = poll_once(dev);
        }
    } else {
        status = poll_once(dev);
    }
    if (status) {
        /* Disabled until stop, which still quiesces and releases it. */
        T.failed = true;
        irq_stop(dev);
        kshim_touch_cancel(&dev->tracker);
        printf("Touch: %s input error: %s\n", dev->driver->name, kshim_io_strerror(status));
    }
    return status;
}

int kshim_touch_quiesce_all(void)
{
    if (!T.active) return 0;
    struct kshim_touch_dev *dev = &Dev;
    irq_stop(dev);
    if (dev->probed && dev->driver->quiesce) {
        int status = dev->driver->quiesce(dev);
        if (status && dev->has_reset && !kshim_touch_set_reset(dev, true)) {
            /* A controller held in reset scans nothing. */
            uint32_t hold = dev->driver->power_on_delay_us ? dev->driver->power_on_delay_us : 10000U;
            (void)delay(hold);
            printf("Touch: %s stop: %s, held in reset\n", dev->driver->name,
                   kshim_io_strerror(status));
            status = 0;
        }
        if (status) {
            printf("Touch: %s stop: %s\n", dev->driver->name, kshim_io_strerror(status));
            return status;
        }
    }
    dev->probed = false;
    kshim_touch_cancel(&dev->tracker);
    int status = release(dev);
    if (status) printf("Touch: release: %s\n", kshim_io_strerror(status));
    T.active = T.failed = false;
    return 0;
}

int kshim_touch_framework_stop(void)
{
    return kshim_touch_quiesce_all();
}
