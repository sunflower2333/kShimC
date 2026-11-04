/* SPDX-License-Identifier: MIT */
/*
 * ST FTM5 on the touch framework (Xiaomi Mi MIX 3 5G "andromeda", SM8150).
 *
 * The protocol is the existing kShimC core in kernel/drivers/st_ftm5.c
 * (ready wait, chip ID 36 39, system info, scan enable, FIFO decode, scan
 * disable; provenance MiCode Xiaomi_Kernel_OpenSource andromeda-p-oss, ST
 * 5.2.4, see its header and configs/andromeda-provenance.json). This file
 * binds it to kshim/touch_driver.h:
 *  - transport: the device's I2C client (write, repeated START, read),
 *  - reports: the core updates the framework's tracker (dev->tracker), whose
 *    configuration carries the DT swap/invert and the scaling,
 *  - status: the core's negative errno values become kShimC codes; bus
 *    codes from the transport pass through unchanged.
 * Board facts (DT andromeda-sm8150.dtsi, provenance JSON): fts@49 on
 * qupv3_se17_i2c, IRQ GPIO122 level low, reset GPIO54 held low >= 10 ms,
 * avdd = GPIO37 regulator-fixed (4 ms), vdd = RPMh XOB ldoc1, 1080x2340.
 */
#include <kshim/touch_driver.h>
#include <kshim/io.h>
#include <kshim/time.h>
#include <st_ftm5.h>

#include <stdio.h>

#include "st_fts.h"

#define FTM5_RESET_LOW_US 10000U   /* andromeda-provenance.json gpio.reset.low_us */
/* The core's own ready budget (KSHIM_FTM5_READY_ATTEMPTS x 2 ms) also bounds
 * the time an I2C NACK from a still-booting controller is retried. */
#define FTM5_BOOT_BUDGET_US (KSHIM_FTM5_READY_ATTEMPTS * KSHIM_FTM5_READY_DELAY_US)
#define FTM5_MAX_AXIS 8192U

struct ftm5_priv {
    kshim_ftm5_t core;
};

static int ftm5_code(int status)
{
    switch (status) {
    case KSHIM_FTM5_OK: return 0;
    case KSHIM_FTM5_ERR_NO_DEVICE: return KSHIM_IO_ERR_NO_DEVICE;
    case KSHIM_FTM5_ERR_INVALID: return ERROR_INVALID_PARAMETER;
    case KSHIM_FTM5_ERR_PROTOCOL: return KSHIM_IO_ERR_PROTOCOL;
    case KSHIM_FTM5_ERR_UNSUPPORTED: return ERROR_NOT_SUPPORTED;
    case KSHIM_FTM5_ERR_TIMEOUT: return ERROR_TIMEOUT;
    default: return status > 0 ? status : ERROR_DEVICE_ERROR;
    }
}

static int ftm5_transfer(void *context, const uint8_t *command, size_t command_size,
                         uint8_t *response, size_t response_size)
{
    return kshim_touch_i2c_write_read(context, command, command_size, response, response_size);
}

static void ftm5_delay(void *context, uint32_t delay_us)
{
    (void)context;
    (void)delay(delay_us);
}

static int ftm5_match(struct kshim_touch_dev *dev, const void *fdt)
{
    int status = st_fts_family(fdt, dev->node, "fts,irq-gpio", "st,irq-gpio");
    if (status) return status;
    /* fts,x-max/y-max are the reported range (andromeda: 1080 x 2340); the
     * common touchscreen-size-* properties win when both are present. */
    uint32_t x = 0, y = 0;
    if ((fdt_getprop(fdt, dev->node, "fts,x-max", NULL) && dt_u32(fdt, dev->node, "fts,x-max", &x)) ||
        (fdt_getprop(fdt, dev->node, "fts,y-max", NULL) && dt_u32(fdt, dev->node, "fts,y-max", &y)) ||
        x > FTM5_MAX_AXIS || y > FTM5_MAX_AXIS)
        return ERROR_INVALID_PARAMETER;
    if (!dev->props.size_x) dev->props.size_x = x;
    if (!dev->props.size_y) dev->props.size_y = y;
    status = st_fts_flips(dev, "fts,x-flip", "fts,y-flip");
    if (status) return status;
    st_fts_android_gpios(dev, "fts,reset-gpio", "fts,irq-gpio");
    return 0;
}

static int ftm5_probe(struct kshim_touch_dev *dev)
{
    struct ftm5_priv *p = dev->priv;
    const kshim_ftm5_transport_t transport = {
        .context = dev, .transfer = ftm5_transfer, .delay_us = ftm5_delay,
        .bus_kind = KSHIM_FTM5_BUS_I2C,
    };
    int status;
    for (uint32_t waited = 0;; waited += KSHIM_FTM5_READY_DELAY_US) {
        status = kshim_ftm5_init(&p->core, &transport, &dev->tracker);
        if (status != KSHIM_IO_ERR_NACK || waited >= FTM5_BOOT_BUDGET_US) break;
        (void)delay(KSHIM_FTM5_READY_DELAY_US);
    }
    if (status) return ftm5_code(status);
    dev->max_x = p->core.info.resolution_x;
    dev->max_y = p->core.info.resolution_y;
    dev->max_contacts = KSHIM_FTM5_MAX_CONTACTS;
    printf("Touch: FTM5 chip %02x%02x fw %04x config %04x, %ux%u\n",
           p->core.info.chip_id_low, p->core.info.chip_id_high,
           p->core.info.firmware_version, p->core.info.config_version,
           (unsigned)dev->max_x, (unsigned)dev->max_y);
    return 0;
}

/* One FIFO snapshot (at most 32 events); a level IRQ stays asserted while
 * more are queued, so the next IRQ or poll reads the rest. */
static int ftm5_handle(struct kshim_touch_dev *dev)
{
    struct ftm5_priv *p = dev->priv;
    return ftm5_code(kshim_ftm5_poll(&p->core));
}

static int ftm5_quiesce(struct kshim_touch_dev *dev)
{
    struct ftm5_priv *p = dev->priv;
    return ftm5_code(kshim_ftm5_quiesce(&p->core));
}

static const char *const Ftm5Compatibles[] = {"st,fts", NULL};
static const char *const Ftm5Reset[] = {"reset-gpios", "fts,reset-gpio", NULL};
static const char *const Ftm5Irq[] = {"irq-gpios", "fts,irq-gpio", NULL};
/* fts,pwr-reg-name = "avdd", fts,bus-reg-name = "vdd"; AVDD first. */
static const char *const Ftm5Supplies[] = {"avdd", "vdd", NULL};

const struct kshim_touch_driver kshim_touch_driver_ftm5 = {
    .name = "ftm5",
    .compatibles = Ftm5Compatibles,
    .bus_mask = KSHIM_TOUCH_BUS_I2C,
    .reset_gpio_names = Ftm5Reset,
    .irq_gpio_names = Ftm5Irq,
    .supplies = Ftm5Supplies,
    .power_on_delay_us = FTM5_RESET_LOW_US,
    .post_reset_delay_us = 0,   /* the core polls for the READY event */
    .priv_size = sizeof(struct ftm5_priv),
    .match = ftm5_match,
    .probe = ftm5_probe,
    .handle = ftm5_handle,
    .quiesce = ftm5_quiesce,
};
