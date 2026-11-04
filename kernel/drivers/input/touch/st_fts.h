/* SPDX-License-Identifier: MIT */
#pragma once
/*
 * Shared DT handling of the Android ST FingerTip nodes ("st,fts") for the
 * FTM4 and FTM5 match hooks. Both generations bind the same compatible; the
 * vendor property family tells them apart:
 *   FTM5 (andromeda-sm8150.dtsi, ST 5.2.x driver): fts,irq-gpio,
 *        fts,reset-gpio, fts,x-max/y-max, fts,x-flip/y-flip
 *   FTM4 (kona-qrd.dtsi/kona-hdk.dtsi, ST 4.2.x driver): st,irq-gpio,
 *        st,reset-gpio, st,x-flip/y-flip, no size property
 * Board selection (qcom,i2c-touch-active, panel lists) is
 * kshim_touch_dt_selected() in kernel/dt/touch.c.
 */
#include <kshim/touch_driver.h>
#include <dt.h>

/* Low nibble of an Android IRQ flags cell (dt-bindings irq.h values). */
#define ST_FTS_IRQ_TYPE_EDGE_RISING 0x1U
#define ST_FTS_IRQ_TYPE_LEVEL_HIGH 0x4U

/* Accept the node for the family whose IRQ GPIO property is `own` and not
 * `other`, selected on this board. 0 or ERROR_NOT_FOUND. */
static inline int st_fts_family(const void *fdt, int node, const char *own, const char *other)
{
    if (!fdt_getprop(fdt, node, own, NULL) || fdt_getprop(fdt, node, other, NULL))
        return ERROR_NOT_FOUND;
    return kshim_touch_dt_selected(fdt, node, "st,fts") ? 0 : ERROR_NOT_FOUND;
}

/* The Android properties carry raw levels, whatever their flags cell says:
 * the ST drivers pulse reset low (gpio_set_value 0, then 1), and the IRQ
 * GPIO's flags cell is the interrupt type (0x2008 = IRQF_ONESHOT |
 * IRQ_TYPE_LEVEL_LOW in both board DTs), active low unless it names a
 * high level or rising edge. A generic reset-gpios/irq-gpios keeps its
 * GPIO_ACTIVE_* flag. */
static inline void st_fts_android_gpios(struct kshim_touch_dev *dev, const char *reset_prop,
                                        const char *irq_prop)
{
    const void *f = dev->fdt;
    if (dev->has_reset && !fdt_getprop(f, dev->node, "reset-gpios", NULL) &&
        fdt_getprop(f, dev->node, reset_prop, NULL))
        dev->reset_gpio.active_low = true;
    if (dev->has_irq_gpio && !fdt_getprop(f, dev->node, "irq-gpios", NULL) &&
        fdt_getprop(f, dev->node, irq_prop, NULL)) {
        uint32_t type = dev->irq_gpio.dt_flags & 0xfU;
        dev->irq_gpio.active_low = type != ST_FTS_IRQ_TYPE_LEVEL_HIGH &&
                                   type != ST_FTS_IRQ_TYPE_EDGE_RISING;
    }
}

/* Vendor axis flips OR the touchscreen-inverted-* properties. */
static inline int st_fts_flips(struct kshim_touch_dev *dev, const char *x_prop, const char *y_prop)
{
    bool x, y;
    if (dt_bool(dev->fdt, dev->node, x_prop, &x) || dt_bool(dev->fdt, dev->node, y_prop, &y))
        return ERROR_INVALID_PARAMETER;
    dev->props.invert_x = dev->props.invert_x || x;
    dev->props.invert_y = dev->props.invert_y || y;
    return 0;
}
