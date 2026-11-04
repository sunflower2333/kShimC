/* SPDX-License-Identifier: MIT */
/*
 * ST FingerTip FTM4 on the touch framework (Qualcomm SM8250 HDK/QRD, "st,fts"
 * with the st,* property family), I2C only.
 *
 * kShimC's own implementation, written from protocol facts. Fact sources
 * (GPL-2.0 evidence only, nothing copied; pinned OnePlus
 * android_kernel_oneplus_sm8250 cad09e061ef6, hashes in
 * configs/hdk8250-touch-provenance.json), under
 * drivers/input/touchscreen/st/:
 *   fts_lib/ftsHardware.h  FTM4 (non-FTM3) values: chip ID 0x36 0x70 at HW
 *                          register 0x0004, one dummy byte before HW register
 *                          data (DUMMY_HW_REG 1), system reset 0x80 at 0x0028,
 *                          interrupt enable 0x41 / disable 0x00 at 0x002C,
 *                          8-byte FIFO events, FIFO depth 64, FIFO "read one"
 *                          0x85, FIFO flush 0xA1, HW register op code 0xB6
 *   fts_lib/ftsSoftware.h  sense off 0x92 / on 0x93; event IDs: none 0x00,
 *                          enter 0x03, leave 0x04, motion 0x05, error 0x0F,
 *                          controller ready 0x10, echo 0xEC
 *   fts_lib/ftsIO.c        HW register read framing: <op, addr[15:8],
 *                          addr[7:0]>, repeated START, read (dummy + data)
 *   fts_lib/ftsTool.h      poll resolution 10 ms, general timeout 500 ms
 *   fts_lib/ftsTool.c      reset with a GPIO: low 20 ms, high, then wait for
 *                          the controller-ready event; without one, the
 *                          system-reset register write; touch enable = sense
 *                          on, then interrupt enable
 *   fts.c                  pointer events: ID = byte1[3:0],
 *                          X = byte2 << 4 | byte4[7:4],
 *                          Y = byte3 << 4 | byte4[3:0]; the interrupt work
 *                          reads single events until ID 0x00, at most the
 *                          FIFO depth
 *   fts.h                  X_AXIS_MAX 1440, Y_AXIS_MAX 2880, 10 touch IDs
 * Board facts (kona-qrd.dtsi/kona-hdk.dtsi, provenance JSON): st_fts@49 on
 * qupv3_se13_i2c, IRQ GPIO39 level low, reset GPIO38, avdd = pm8150_l13
 * (ldoa13) before vdd = pm8150a_l1 (ldoc1), HDK flips both axes.
 */
#include <kshim/touch_driver.h>
#include <kshim/io.h>
#include <kshim/time.h>

#include <stdio.h>
#include <string.h>

#include "st_fts.h"

#define FTM4_OP_HW_REG          0xb6U
#define FTM4_OP_FIFO_READ_ONE   0x85U
#define FTM4_OP_FIFO_FLUSH      0xa1U
#define FTM4_OP_SENSE_OFF       0x92U
#define FTM4_OP_SENSE_ON        0x93U

#define FTM4_REG_CHIP_ID        0x0004U
#define FTM4_REG_FW_VERSION     0x0008U
#define FTM4_REG_SYSTEM_RESET   0x0028U
#define FTM4_REG_IER            0x002cU
#define FTM4_SYSTEM_RESET       0x80U
#define FTM4_IER_ENABLE         0x41U
#define FTM4_IER_DISABLE        0x00U
#define FTM4_CHIP_ID0           0x36U
#define FTM4_CHIP_ID1           0x70U

#define FTM4_EVENT_SIZE         8U
#define FTM4_FIFO_DEPTH         64U
#define FTM4_EV_NONE            0x00U
#define FTM4_EV_ENTER           0x03U
#define FTM4_EV_LEAVE           0x04U
#define FTM4_EV_MOTION          0x05U
#define FTM4_EV_ERROR           0x0fU
#define FTM4_EV_READY           0x10U

#define FTM4_POLL_US            10000U
#define FTM4_READY_ATTEMPTS     50U       /* 500 ms / 10 ms */
#define FTM4_RESET_LOW_US       20000U
#define FTM4_WIDTH              1440U
#define FTM4_HEIGHT             2880U
#define FTM4_CONTACTS           10U
/* Spontaneous controller resets survived before touch is given up. */
#define FTM4_MAX_RECOVERIES     3U

struct ftm4_priv {
    bool scanning_maybe;     /* sense on may have reached the controller */
    uint8_t recoveries;
    uint8_t fw_version[2];
};

static int ftm4_write(struct kshim_touch_dev *dev, const uint8_t *data, size_t length)
{
    return kshim_touch_i2c_write_read(dev, data, length, NULL, 0);
}

static int ftm4_command(struct kshim_touch_dev *dev, uint8_t op)
{
    return ftm4_write(dev, &op, 1);
}

static int ftm4_reg_write(struct kshim_touch_dev *dev, uint16_t reg, uint8_t value)
{
    const uint8_t frame[] = {FTM4_OP_HW_REG, (uint8_t)(reg >> 8), (uint8_t)reg, value};
    return ftm4_write(dev, frame, sizeof(frame));
}

/* HW register read: the first byte returned is a dummy. */
static int ftm4_reg_read(struct kshim_touch_dev *dev, uint16_t reg, uint8_t *data, size_t length)
{
    uint8_t raw[1 + 8];
    const uint8_t frame[] = {FTM4_OP_HW_REG, (uint8_t)(reg >> 8), (uint8_t)reg};
    if (length + 1 > sizeof(raw)) return ERROR_INVALID_PARAMETER;
    int status = kshim_touch_i2c_write_read(dev, frame, sizeof(frame), raw, length + 1);
    if (!status) memcpy(data, raw + 1, length);
    return status;
}

static int ftm4_read_event(struct kshim_touch_dev *dev, uint8_t event[FTM4_EVENT_SIZE])
{
    const uint8_t op = FTM4_OP_FIFO_READ_ONE;
    return kshim_touch_i2c_write_read(dev, &op, 1, event, FTM4_EVENT_SIZE);
}

/* Controller-ready event after reset. A NACK while it boots counts as not
 * ready yet; error events are skipped. */
static int ftm4_wait_ready(struct kshim_touch_dev *dev)
{
    uint8_t event[FTM4_EVENT_SIZE];
    int status = ERROR_TIMEOUT;
    for (unsigned attempt = 0; attempt < FTM4_READY_ATTEMPTS; ++attempt) {
        status = ftm4_read_event(dev, event);
        if (!status && event[0] == FTM4_EV_READY) return 0;
        if (status && status != KSHIM_IO_ERR_NACK) return status;
        if (!status && event[0] == FTM4_EV_ERROR)
            printf("Touch: FTM4 error event %02x %02x %02x\n", event[1], event[2], event[3]);
        status = ERROR_TIMEOUT;
        (void)delay(FTM4_POLL_US);
    }
    return status;
}

/* Touch enable: sense on, then the controller's interrupt output. */
static int ftm4_enable(struct kshim_touch_dev *dev)
{
    struct ftm4_priv *p = dev->priv;
    p->scanning_maybe = true;
    int status = ftm4_command(dev, FTM4_OP_SENSE_ON);
    if (!status) status = ftm4_reg_write(dev, FTM4_REG_IER, FTM4_IER_ENABLE);
    return status;
}

static int ftm4_match(struct kshim_touch_dev *dev, const void *fdt)
{
    int status = st_fts_family(fdt, dev->node, "st,irq-gpio", "fts,irq-gpio");
    if (status) return status;
    status = st_fts_flips(dev, "st,x-flip", "st,y-flip");
    if (status) return status;
    st_fts_android_gpios(dev, "st,reset-gpio", "st,irq-gpio");
    return 0;
}

static int ftm4_probe(struct kshim_touch_dev *dev)
{
    struct ftm4_priv *p = dev->priv;
    uint8_t id[2];
    int status;
    /* With a reset GPIO the framework's pulse was the reset. */
    if (!dev->has_reset && (status = ftm4_reg_write(dev, FTM4_REG_SYSTEM_RESET, FTM4_SYSTEM_RESET)))
        return status;
    if ((status = ftm4_wait_ready(dev))) return status;
    if ((status = ftm4_reg_read(dev, FTM4_REG_CHIP_ID, id, sizeof(id)))) return status;
    if (id[0] != FTM4_CHIP_ID0 || id[1] != FTM4_CHIP_ID1) {
        printf("Touch: FTM4 chip ID %02x %02x, expected 36 70\n", id[0], id[1]);
        return KSHIM_IO_ERR_NO_DEVICE;
    }
    if (ftm4_reg_read(dev, FTM4_REG_FW_VERSION, p->fw_version, sizeof(p->fw_version)))
        memset(p->fw_version, 0, sizeof(p->fw_version));
    if ((status = ftm4_enable(dev))) return status;
    dev->max_x = FTM4_WIDTH;
    dev->max_y = FTM4_HEIGHT;
    dev->max_contacts = FTM4_CONTACTS;
    printf("Touch: FTM4 chip 3670 fw %02x%02x\n", p->fw_version[0], p->fw_version[1]);
    return 0;
}

static int ftm4_handle(struct kshim_touch_dev *dev)
{
    struct ftm4_priv *p = dev->priv;
    uint8_t e[FTM4_EVENT_SIZE];
    for (unsigned count = 0; count < FTM4_FIFO_DEPTH; ++count) {
        int status = ftm4_read_event(dev, e);
        if (status) return status;
        if (e[0] == FTM4_EV_NONE) break;
        uint8_t id = e[1] & 0x0fU;
        uint32_t x = ((uint32_t)e[2] << 4) | (e[4] >> 4);
        uint32_t y = ((uint32_t)e[3] << 4) | (e[4] & 0x0fU);
        switch (e[0]) {
        case FTM4_EV_ENTER:
        case FTM4_EV_MOTION:
            if (id < FTM4_CONTACTS)
                (void)kshim_touch_report(dev, id, e[0] == FTM4_EV_ENTER ?
                    KSHIM_TOUCH_CONTACT_ENTER : KSHIM_TOUCH_CONTACT_MOVE, x, y);
            break;
        case FTM4_EV_LEAVE:
            if (id < FTM4_CONTACTS)
                (void)kshim_touch_report(dev, id, KSHIM_TOUCH_CONTACT_LEAVE, 0, 0);
            break;
        case FTM4_EV_READY:
            /* The controller reset itself (watchdog, ESD): contacts are gone
             * and sensing is off. Enable it again, a bounded number of times. */
            kshim_touch_report_release_all(dev);
            if (++p->recoveries > FTM4_MAX_RECOVERIES) return KSHIM_IO_ERR_PROTOCOL;
            printf("Touch: FTM4 controller reset, sensing again\n");
            if ((status = ftm4_enable(dev))) return status;
            break;
        default:
            /* Error, echo, status, hover and key events carry no contact. */
            break;
        }
    }
    return 0;
}

/* Sense off and interrupt output off, then drop queued events (the level
 * IRQ line deasserts). Bounded: one attempt each. */
static int ftm4_quiesce(struct kshim_touch_dev *dev)
{
    struct ftm4_priv *p = dev->priv;
    if (!p->scanning_maybe) return 0;
    int status = ftm4_command(dev, FTM4_OP_SENSE_OFF);
    int irq = ftm4_reg_write(dev, FTM4_REG_IER, FTM4_IER_DISABLE);
    (void)ftm4_command(dev, FTM4_OP_FIFO_FLUSH);
    if (!status) status = irq;
    if (!status) p->scanning_maybe = false;
    return status;
}

static const char *const Ftm4Compatibles[] = {"st,fts", NULL};
static const char *const Ftm4Reset[] = {"reset-gpios", "st,reset-gpio", NULL};
static const char *const Ftm4Irq[] = {"irq-gpios", "st,irq-gpio", NULL};
/* st,regulator_avdd = "avdd", st,regulator_dvdd = "vdd"; AVDD first. */
static const char *const Ftm4Supplies[] = {"avdd", "vdd", NULL};

const struct kshim_touch_driver kshim_touch_driver_ftm4 = {
    .name = "ftm4",
    .compatibles = Ftm4Compatibles,
    .bus_mask = KSHIM_TOUCH_BUS_I2C,
    .reset_gpio_names = Ftm4Reset,
    .irq_gpio_names = Ftm4Irq,
    .supplies = Ftm4Supplies,
    .power_on_delay_us = FTM4_RESET_LOW_US,
    .post_reset_delay_us = 0,   /* the ready-event poll covers the boot */
    .priv_size = sizeof(struct ftm4_priv),
    .match = ftm4_match,
    .probe = ftm4_probe,
    .handle = ftm4_handle,
    .quiesce = ftm4_quiesce,
};
