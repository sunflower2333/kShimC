/* SPDX-License-Identifier: MIT */
/*
 * Goodix GT911 capacitive touch controller on I2C (CONFIG_KSHIM_TOUCH_GT911),
 * a driver on the touch framework (kshim/touch_driver.h). Board: AYANEO
 * Pocket S2 / S2 Pro (&i2c4 at 400 kHz, touchscreen@5d, IRQ TLMM 162 falling
 * edge, reset TLMM 161, VDDIO and AVDD28 from vreg_l14b_3p2, 1440x2560).
 * Owner: touch-goodix. Written from hardware facts only; no GPL code was
 * used. See docs/touch-goodix.md for the full provenance.
 *
 * Facts and where they come from:
 *  - GT911 datasheet (Goodix), "I2C" and "power-on timing" sections:
 *    7-bit slave address 0x5d (0xBA/0xBB) or 0x14 (0x28/0x29). The chip
 *    samples INT when RSTB (active low) rises: INT low selects 0x5d, INT
 *    high selects 0x14. Timings: RSTB held low > 10 ms after the rails are
 *    up (T2), INT set up > 100 us before RSTB rises (T3) and held > 5 ms
 *    after (T4), then INT driven low for 50 ms (T5) before the host turns
 *    INT into an input.
 *  - GT911 Programming Guide (Goodix), register map. Registers are 16-bit,
 *    sent high byte first; multi-byte values are little-endian.
 *      0x8040        command
 *      0x8047-0x80FE configuration (184 bytes): +0 Config_Version,
 *                    +1/+2 X output max, +3/+4 Y output max,
 *                    +5 Touch_Number [3:0], +6 Module_Switch1 (INT trigger
 *                    [1:0]: 0 rising, 1 falling, 2 low level, 3 high level)
 *      0x80FF        Config_Chksum: two's complement of the byte sum of
 *                    0x8047-0x80FE
 *      0x8140-0x8143 product ID (ASCII, "911"), 0x8144 firmware version
 *                    (LE16), 0x8146 X resolution, 0x8148 Y resolution,
 *                    0x814A vendor ID
 *      0x814E        status: bit 7 buffer ready, [3:0] number of points;
 *                    the host writes 0 after reading the points
 *      0x814F        points, 8 bytes each: track ID, X (LE16), Y (LE16),
 *                    size (LE16), reserved
 *  - DT binding goodix.yaml (facts only): reg is 0x5d or 0x14; supplies
 *    AVDD28-supply and VDDIO-supply. All 18 goodix,gt911 nodes in mainline
 *    arch/arm and arch/arm64 (2026-09) write reset-gpios as GPIO_ACTIVE_HIGH
 *    (38 of the binding's 40 nodes do): for this binding the GPIO's logical
 *    value is the RSTB level (0 = held in reset). The Pocket S2 RFC node
 *    follows that convention, and its pinctrl pulls GPIO161 down, which
 *    holds the chip in reset while the pad is an input. The RFC's
 *    GPIO_ACTIVE_HIGH is therefore consistent; it is not verified on
 *    hardware. Because of that pull-down, RSTB stays driven high while the
 *    controller is in use (never released to an input).
 */
#include <stdio.h>
#include <string.h>

#include <dt.h>
#include <kshim/irq.h>
#include "goodix_touch.h"

#define GT911_ADDR_LOW_INT  0x5dU /* INT low at reset release */
#define GT911_ADDR_HIGH_INT 0x14U /* INT high at reset release */

#define GT911_REG_CONFIG     0x8047U
#define GT911_REG_PRODUCT_ID 0x8140U
#define GT911_REG_STATUS     0x814eU
#define GT911_REG_POINTS     0x814fU

#define GT911_CONFIG_BYTES   184U   /* 0x8047..0x80FE, checksum at 0x80FF */
#define GT911_INFO_BYTES     11U    /* 0x8140..0x814A */
#define GT911_CFG_X_MAX      1U
#define GT911_CFG_Y_MAX      3U
#define GT911_CFG_TOUCH_NUM  5U
#define GT911_CFG_SWITCH1    6U
#define GT911_SWITCH1_TRIGGER 0x3U

#define GT911_STATUS_READY   0x80U
#define GT911_STATUS_COUNT   0x0fU
#define GT911_POINT_SIZE     8U
#define GT911_MAX_POINTS     KSHIM_TOUCH_MAX_CONTACTS
#define GT911_DEFAULT_POINTS 5U

/* Power-on timing with margin over the datasheet minimums. */
#define GT911_T2_US          20000U /* RSTB low after power, > 10 ms */
#define GT911_T3_US          200U   /* INT setup before RSTB rises, > 100 us */
#define GT911_T4_US          6000U  /* INT hold after RSTB rises, > 5 ms */
#define GT911_T5_US          50000U /* INT low before it becomes an input */
#define GT911_RETRY_US       10000U
#define GT911_ATTEMPTS       3U

struct gt911 {
    struct goodix_contacts contacts;
    uint64_t last_poll_ms;
    uint16_t fw_version;
    char product_id[5];
    bool inherited;
    bool config_valid;
};
_Static_assert(sizeof(struct gt911) <= KSHIM_TOUCH_PRIV_MAX, "GT911 private state");

static int gt911_read(struct kshim_touch_dev *dev, uint16_t reg, void *data, size_t length)
{
    const uint8_t address[2] = { (uint8_t)(reg >> 8), (uint8_t)reg };
    return kshim_touch_i2c_write_read(dev, address, sizeof(address), data, length);
}

static int gt911_write_u8(struct kshim_touch_dev *dev, uint16_t reg, uint8_t value)
{
    const uint8_t frame[3] = { (uint8_t)(reg >> 8), (uint8_t)reg, value };
    return kshim_touch_i2c_write_read(dev, frame, sizeof(frame), NULL, 0);
}

/* RSTB level. Per the goodix,gt9xx binding convention (file header) the
 * reset GPIO's logical value is the RSTB level; the driver declares
 * KSHIM_TOUCH_RESET_LEVEL, so the framework maps "held in reset" to RSTB
 * low and its stop never pulses the reset of an inherited controller. */
static int gt911_rstb(struct kshim_touch_dev *dev, bool high)
{
    return dev->has_reset ? kshim_touch_set_reset(dev, !high) : 0;
}

/* Answers at its DT address with a product ID: the previous bootloader left
 * it powered, out of reset and addressed. Read-only. */
static bool gt911_running(struct kshim_touch_dev *dev)
{
    uint8_t id[4];
    bool rstb_high;

    if (!goodix_supplies_on(dev))
        return false;
    if (dev->has_reset && (kshim_gpio_get(&dev->reset_gpio, &rstb_high) || !rstb_high))
        return false;
    return !gt911_read(dev, GT911_REG_PRODUCT_ID, id, sizeof(id)) &&
           !goodix_is_bus_idle(id, sizeof(id));
}

/* Rails on with RSTB low, then select the DT address with INT at the RSTB
 * rising edge (datasheet power-on timing). Without a reset GPIO the chip
 * powers up on its own and the address follows the INT pull; probe() then
 * tries the other address. */
static int gt911_cold_power_on(struct kshim_touch_dev *dev)
{
    const bool int_high = dev->i2c.addr == GT911_ADDR_HIGH_INT;
    int status;

    if ((status = gt911_rstb(dev, false)))
        return status;
    if (dev->has_irq_gpio && (status = kshim_touch_irq_gpio_output(dev, false)))
        return status;
    if ((status = kshim_touch_power_supplies(dev, true)))
        return status;
    if (!dev->has_reset) {
        delay(GT911_T5_US);
        return 0;
    }
    delay(GT911_T2_US);
    if (dev->has_irq_gpio && int_high && (status = kshim_touch_irq_gpio_output(dev, true)))
        return status;
    delay(GT911_T3_US);
    if ((status = gt911_rstb(dev, true)))
        return status;
    delay(GT911_T4_US);
    if (dev->has_irq_gpio) {
        if ((status = kshim_touch_irq_gpio_output(dev, false)))
            return status;
        delay(GT911_T5_US);
        return kshim_touch_irq_gpio_input(dev);
    }
    delay(GT911_T5_US);
    return 0;
}

static int gt911_power_on(struct kshim_touch_dev *dev)
{
    struct gt911 *g = dev->priv;
    int status;

    g->inherited = false;
    if (!gt911_running(dev))
        return gt911_cold_power_on(dev);
    /* Inherit: take the rail references (inherited-on rails are not written)
     * and keep RSTB driven high so the pad cannot fall back to its pull. */
    if ((status = kshim_touch_power_supplies(dev, true)))
        return status;
    if ((status = gt911_rstb(dev, true)))
        return status;
    g->inherited = true;
    printf("GT911: inherited running controller at 0x%02x\n", (unsigned)dev->i2c.addr);
    return 0;
}

static int gt911_match(struct kshim_touch_dev *dev, const void *fdt)
{
    uint32_t reg;

    if (dt_u32(fdt, dev->node, "reg", &reg))
        return ERROR_INVALID_PARAMETER;
    if (reg != GT911_ADDR_LOW_INT && reg != GT911_ADDR_HIGH_INT) {
        printf("GT911: reg 0x%x is not 0x5d or 0x14\n", (unsigned)reg);
        return ERROR_NOT_SUPPORTED;
    }
    return 0;
}

/* Product information block, retried at the DT address, then once at the
 * other address (address select not possible or not taken). */
static int gt911_identify(struct kshim_touch_dev *dev, uint8_t *info)
{
    int status = ERROR_DEVICE_ERROR;

    for (unsigned attempt = 0; attempt < GT911_ATTEMPTS; ++attempt) {
        if (attempt)
            delay(GT911_RETRY_US);
        status = gt911_read(dev, GT911_REG_PRODUCT_ID, info, GT911_INFO_BYTES);
        if (!status && goodix_is_bus_idle(info, 4))
            status = KSHIM_IO_ERR_NO_DEVICE;
        if (!status)
            return 0;
    }
    uint16_t dt_address = dev->i2c.addr;
    dev->i2c.addr = dt_address == GT911_ADDR_LOW_INT ? GT911_ADDR_HIGH_INT : GT911_ADDR_LOW_INT;
    if (!gt911_read(dev, GT911_REG_PRODUCT_ID, info, GT911_INFO_BYTES) &&
        !goodix_is_bus_idle(info, 4)) {
        printf("GT911: answers at 0x%02x, not the DT address 0x%02x\n",
               (unsigned)dev->i2c.addr, (unsigned)dt_address);
        return 0;
    }
    dev->i2c.addr = dt_address;
    return status;
}

static bool gt911_config_valid(const uint8_t *config)
{
    uint8_t sum = 0;
    for (unsigned i = 0; i <= GT911_CONFIG_BYTES; ++i)
        sum = (uint8_t)(sum + config[i]);
    /* The checksum byte is the two's complement of the sum before it. */
    return sum == 0 && (config[GT911_CFG_X_MAX] | config[GT911_CFG_X_MAX + 1]) != 0;
}

static unsigned gt911_trigger(uint8_t switch1)
{
    static const uint8_t triggers[4] = {
        KSHIM_IRQ_TYPE_EDGE_RISING, KSHIM_IRQ_TYPE_EDGE_FALLING,
        KSHIM_IRQ_TYPE_LEVEL_LOW, KSHIM_IRQ_TYPE_LEVEL_HIGH,
    };
    return triggers[switch1 & GT911_SWITCH1_TRIGGER];
}

static int gt911_probe(struct kshim_touch_dev *dev)
{
    struct gt911 *g = dev->priv;
    uint8_t info[GT911_INFO_BYTES];
    uint8_t config[GT911_CONFIG_BYTES + 1];
    uint32_t x_max, y_max;
    int status;

    if ((status = gt911_identify(dev, info))) {
        printf("GT911: no answer at 0x%02x (%s)\n", (unsigned)dev->i2c.addr,
               kshim_io_strerror(status));
        return status;
    }
    for (unsigned i = 0; i < 4; ++i)
        g->product_id[i] = info[i] >= 0x20 && info[i] < 0x7f ? (char)info[i] : '\0';
    g->product_id[4] = '\0';
    g->fw_version = goodix_le16(&info[4]);
    if (strncmp(g->product_id, "911", 3) != 0)
        printf("GT911: unexpected product ID \"%s\"\n", g->product_id);

    /* The configuration only refines what the product block gives. */
    g->config_valid = !gt911_read(dev, GT911_REG_CONFIG, config, sizeof(config)) &&
                      gt911_config_valid(config);
    if (g->config_valid) {
        x_max = goodix_le16(&config[GT911_CFG_X_MAX]);
        y_max = goodix_le16(&config[GT911_CFG_Y_MAX]);
        unsigned points = config[GT911_CFG_TOUCH_NUM] & 0x0fU;
        dev->max_contacts = (uint8_t)(points && points <= GT911_MAX_POINTS ? points
                                                                          : GT911_DEFAULT_POINTS);
        unsigned trigger = gt911_trigger(config[GT911_CFG_SWITCH1]);
        /* The INT waveform is whatever the chip's configuration says. The
         * framework re-derives the IRQ GPIO's active level from it. */
        if (dev->has_irq && dev->irq_trigger != trigger) {
            printf("GT911: DT IRQ type %u, controller configured for %u: using %u\n",
                   dev->irq_trigger, trigger, trigger);
            dev->irq_trigger = trigger;
        }
    } else {
        printf("GT911: configuration unreadable or bad checksum, using the product resolution\n");
        x_max = goodix_le16(&info[6]);
        y_max = goodix_le16(&info[8]);
        dev->max_contacts = GT911_DEFAULT_POINTS;
    }
    if (dev->props.size_x && dev->props.size_y &&
        (dev->props.size_x != x_max || dev->props.size_y != y_max))
        printf("GT911: DT size %ux%u, controller %ux%u: using the DT size\n",
               (unsigned)dev->props.size_x, (unsigned)dev->props.size_y,
               (unsigned)x_max, (unsigned)y_max);
    if (!dev->props.size_x)
        dev->max_x = x_max;
    if (!dev->props.size_y)
        dev->max_y = y_max;
    if (!dev->max_x || !dev->max_y) {
        printf("GT911: no resolution\n");
        return KSHIM_IO_ERR_PROTOCOL;
    }

    /* Drop a frame latched before we listened. */
    memset(&g->contacts, 0, sizeof(g->contacts));
    if ((status = gt911_write_u8(dev, GT911_REG_STATUS, 0)))
        return status;
    g->last_poll_ms = get_current_time();
    printf("GT911: product %s firmware 0x%04x at 0x%02x, %ux%u, %u points%s\n",
           g->product_id, (unsigned)g->fw_version, (unsigned)dev->i2c.addr,
           (unsigned)dev->max_x, (unsigned)dev->max_y, (unsigned)dev->max_contacts,
           g->inherited ? ", inherited" : "");
    return 0;
}

static int gt911_handle(struct kshim_touch_dev *dev)
{
    struct gt911 *g = dev->priv;
    uint8_t frame[1 + GT911_POINT_SIZE * GT911_MAX_POINTS];
    struct goodix_point points[GT911_MAX_POINTS];
    int status, clear;

    g->last_poll_ms = get_current_time();
    /* Status and the first point in one read; most frames have one. */
    if ((status = gt911_read(dev, GT911_REG_STATUS, frame, 1 + GT911_POINT_SIZE)))
        return status;
    if (!(frame[0] & GT911_STATUS_READY))
        return 0;
    unsigned count = frame[0] & GT911_STATUS_COUNT;
    unsigned limit = dev->max_contacts ? dev->max_contacts : GT911_DEFAULT_POINTS;
    if (count > limit || count > GT911_MAX_POINTS) {
        status = KSHIM_IO_ERR_PROTOCOL;
    } else if (count > 1) {
        status = gt911_read(dev, (uint16_t)(GT911_REG_POINTS + GT911_POINT_SIZE),
                            &frame[1 + GT911_POINT_SIZE], (count - 1U) * GT911_POINT_SIZE);
    }
    if (!status) {
        for (unsigned i = 0; i < count; ++i) {
            const uint8_t *p = &frame[1 + i * GT911_POINT_SIZE];
            points[i].id = p[0] & 0x0fU;
            points[i].x = goodix_le16(&p[1]);
            points[i].y = goodix_le16(&p[3]);
        }
        status = goodix_report_frame(dev, &g->contacts, points, count);
    }
    /* Hand the buffer back to the controller in every case. */
    clear = gt911_write_u8(dev, GT911_REG_STATUS, 0);
    return status ? status : clear;
}

/* Polling fallback: the INT pulse is short, so read the status register at
 * the report cadence instead of sampling the pin. */
static bool gt911_pending(struct kshim_touch_dev *dev)
{
    const struct gt911 *g = dev->priv;
    return goodix_poll_due(g->last_poll_ms);
}

/* Handoff: release the report buffer and stop tracking. No sleep command: a
 * controller we powered goes back to its inherited rail and reset state with
 * the framework's restore, and one inherited running stays running for the
 * payload. Best effort; nothing here needs restoring. */
static int gt911_quiesce(struct kshim_touch_dev *dev)
{
    struct gt911 *g = dev->priv;

    memset(&g->contacts, 0, sizeof(g->contacts));
    if (dev->probed && gt911_write_u8(dev, GT911_REG_STATUS, 0))
        printf("GT911: status not cleared at quiesce\n");
    return 0;
}

static const char *const gt911_compatibles[] = { "goodix,gt911", NULL };
/* AVDD28, then VDDIO; both are vreg_l14b_3p2 on the Pocket S2. */
static const char *const gt911_supplies[] = { "AVDD28", "VDDIO", NULL };

const struct kshim_touch_driver kshim_touch_driver_gt911 = {
    .name = "Goodix GT911",
    .compatibles = gt911_compatibles,
    .bus_mask = KSHIM_TOUCH_BUS_I2C,
    .reset_convention = KSHIM_TOUCH_RESET_LEVEL,
    .supplies = gt911_supplies,
    .priv_size = sizeof(struct gt911),
    .match = gt911_match,
    .power_on = gt911_power_on,
    .probe = gt911_probe,
    .handle = gt911_handle,
    .pending = gt911_pending,
    .quiesce = gt911_quiesce,
};
