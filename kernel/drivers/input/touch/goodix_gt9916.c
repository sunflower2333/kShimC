/* SPDX-License-Identifier: MIT */
/*
 * Goodix GT9916 ("Berlin-D" family) touch controller on SPI
 * (CONFIG_KSHIM_TOUCH_GT9916), a driver on the touch framework
 * (kshim/touch_driver.h). Board: Qualcomm QRD8650 (&spi4 chip select 0,
 * 1 MHz, IRQ TLMM 162 level low, reset TLMM 161 active low, avdd from
 * vreg_l14b_3p2, 1080x2400). Owner: touch-goodix. Written from protocol
 * facts only; no GPL code was used. See docs/touch-goodix.md.
 *
 * Protocol facts. These are the Goodix Berlin host-interface facts that the
 * mainline Linux goodix_berlin_{core,spi} drivers and Goodix's own Berlin
 * Android driver rely on (both GPL-2.0: consulted for register addresses,
 * framing and layouts only; no code was taken). The DT binding
 * goodix,gt9916.yaml (GPL-2.0-only OR BSD-2-Clause) gives the supplies
 * (avdd-supply required, vddio-supply optional) and makes
 * touchscreen-size-x/y required.
 *  - SPI mode 0, 8-bit words. Registers are 32-bit addresses, big-endian
 *    on the wire. Write: 0xF0, addr[4], data. Read: 0xF1, addr[4], three
 *    dummy bytes (0xFF), then the device clocks the data out. One chip
 *    select assertion per access.
 *  - 0x10000 boot option: after reset the host writes eight 0xAA bytes and
 *    reads them back; an echo confirms the firmware is up.
 *  - 0x10014 firmware version, 28 bytes: rom_pid[6], rom_vid[3], reserved,
 *    patch_pid[8], patch_vid[4], reserved, sensor_id, reserved[2],
 *    checksum (LE16).
 *  - 0x10070 IC information: LE16 total length (< 1024), version block
 *    (16 bytes), feature block (10 bytes), drv/sen/button/force counts
 *    (4 bytes), five counted LE16 lists (active scan rates, mutual
 *    frequencies, self TX, self RX, stylus frequencies), then the "misc"
 *    block whose LE32 at +44 is the touch data address; LE16 checksum last.
 *  - Checksums: the LE16 in the last two bytes equals the 16-bit sum of
 *    every byte before it.
 *  - Touch data (at the address the IC information gives): 8-byte header
 *    (status, reserved, count/request code, reserved[3], LE16 checksum),
 *    then 8-byte points (bits [7:4] track ID, [3:0] type with 1 = stylus
 *    hover and 3 = stylus; X, Y, W as LE16), then an LE16 checksum over
 *    the points.
 *    Status bit 7 = touch event (count in byte 2 [3:0], at most 10),
 *    bit 6 = request (code in byte 2; 3 = the firmware asks for a reset).
 *    Writing one 0 byte at the touch data address releases the event and
 *    deasserts INT.
 *  - Power: vddio, then 3 ms, avdd, then 15 ms, reset released, 4 ms until
 *    the boot-option handshake, 100 ms until the firmware is fully up; a
 *    reset request is served with reset asserted for 2 ms, then 100 ms.
 */
#include <stdio.h>
#include <string.h>

#include "goodix_touch.h"

#define BRL_SPI_WRITE          0xf0U
#define BRL_SPI_READ           0xf1U
#define BRL_SPI_WRITE_PREFIX   5U   /* flag + address */
#define BRL_SPI_READ_PREFIX    8U   /* flag + address + 3 dummy bytes */

#define BRL_REG_BOOTOPTION     0x10000U
#define BRL_REG_FW_VERSION     0x10014U
#define BRL_REG_IC_INFO        0x10070U

#define BRL_CONFIRM_VALUE      0xaaU
#define BRL_CONFIRM_BYTES      8U
#define BRL_FW_VERSION_BYTES   28U
#define BRL_FW_PATCH_PID       10U  /* offset of patch_pid[8] */
#define BRL_FW_PATCH_PID_LEN   8U
#define BRL_FW_SENSOR_ID       23U
#define BRL_IC_INFO_MAX        1024U
#define BRL_IC_INFO_FIXED      (2U + 16U + 10U + 4U)
#define BRL_IC_INFO_LISTS      5U
#define BRL_MISC_TOUCH_ADDR    44U

#define BRL_HEAD_BYTES         8U
#define BRL_POINT_BYTES        8U
#define BRL_CHECKSUM_BYTES     2U
#define BRL_MAX_POINTS         10U
/* Header, two points and their checksum: one read covers most frames. */
#define BRL_PRE_READ           (BRL_HEAD_BYTES + 2U * BRL_POINT_BYTES + BRL_CHECKSUM_BYTES)
#define BRL_EVENT_MAX \
    (BRL_HEAD_BYTES + BRL_MAX_POINTS * BRL_POINT_BYTES + BRL_CHECKSUM_BYTES)

#define BRL_STATUS_TOUCH       0x80U
#define BRL_STATUS_REQUEST     0x40U
#define BRL_REQUEST_RESET      3U
#define BRL_POINT_STYLUS_HOVER 1U
#define BRL_POINT_STYLUS       3U

#define BRL_SUPPLY_DELAY_US    18000U  /* vddio 3 ms + avdd 15 ms */
#define BRL_FW_INIT_US         4000U   /* reset release -> handshake */
#define BRL_FW_BOOT_US         100000U /* handshake -> fully up */
#define BRL_RESET_PULSE_US     2000U
#define BRL_RETRY_US           5000U
#define BRL_ATTEMPTS           3U

struct gt9916 {
    struct goodix_contacts contacts;
    uint64_t last_poll_ms;
    uint32_t touch_data_addr;
    char patch_pid[BRL_FW_PATCH_PID_LEN + 1];
    uint8_t sensor_id;
    bool inherited;
};
_Static_assert(sizeof(struct gt9916) <= KSHIM_TOUCH_PRIV_MAX, "GT9916 private state");

/* The binding fixes the wire format; the DT carries only the speed. */
static void brl_spi_setup(struct kshim_touch_dev *dev)
{
    dev->spi.mode = KSHIM_SPI_MODE_0;
    dev->spi.bits_per_word = 8;
}

static int brl_read(struct kshim_touch_dev *dev, uint32_t addr, void *data, size_t length)
{
    const uint8_t prefix[BRL_SPI_READ_PREFIX] = {
        BRL_SPI_READ, (uint8_t)(addr >> 24), (uint8_t)(addr >> 16), (uint8_t)(addr >> 8),
        (uint8_t)addr, 0xff, 0xff, 0xff,
    };
    const struct kshim_spi_xfer xfers[2] = {
        { .tx = prefix, .rx = NULL, .len = sizeof(prefix) },
        { .tx = NULL, .rx = data, .len = (uint32_t)length },
    };
    return kshim_touch_spi_sync(dev, xfers, 2);
}

static int brl_write(struct kshim_touch_dev *dev, uint32_t addr, const void *data, size_t length)
{
    const uint8_t prefix[BRL_SPI_WRITE_PREFIX] = {
        BRL_SPI_WRITE, (uint8_t)(addr >> 24), (uint8_t)(addr >> 16), (uint8_t)(addr >> 8),
        (uint8_t)addr,
    };
    const struct kshim_spi_xfer xfers[2] = {
        { .tx = prefix, .rx = NULL, .len = sizeof(prefix) },
        { .tx = data, .rx = NULL, .len = (uint32_t)length },
    };
    return kshim_touch_spi_sync(dev, xfers, 2);
}

static bool brl_checksum_ok(const uint8_t *data, size_t length)
{
    uint16_t sum = 0;
    if (length < BRL_CHECKSUM_BYTES)
        return false;
    for (size_t i = 0; i < length - BRL_CHECKSUM_BYTES; ++i)
        sum = (uint16_t)(sum + data[i]);
    return sum == goodix_le16(&data[length - BRL_CHECKSUM_BYTES]);
}

/* A checksummed block that is not the bus idle pattern. */
static int brl_read_block(struct kshim_touch_dev *dev, uint32_t addr, uint8_t *data, size_t length)
{
    int status = brl_read(dev, addr, data, length);
    if (status)
        return status;
    if (goodix_is_bus_idle(data, length))
        return KSHIM_IO_ERR_NO_DEVICE;
    return brl_checksum_ok(data, length) ? 0 : KSHIM_IO_ERR_PROTOCOL;
}

static int brl_read_version(struct kshim_touch_dev *dev, uint8_t *version)
{
    int status = ERROR_DEVICE_ERROR;
    for (unsigned attempt = 0; attempt < BRL_ATTEMPTS; ++attempt) {
        if (attempt)
            delay(BRL_RETRY_US);
        if (!(status = brl_read_block(dev, BRL_REG_FW_VERSION, version, BRL_FW_VERSION_BYTES)))
            return 0;
    }
    return status;
}

/* Walk the variable-length IC information to the touch data address. */
static int brl_parse_ic_info(const uint8_t *info, size_t length, uint32_t *touch_data_addr)
{
    size_t offset = BRL_IC_INFO_FIXED;

    for (unsigned list = 0; list < BRL_IC_INFO_LISTS; ++list) {
        if (offset >= length)
            return KSHIM_IO_ERR_PROTOCOL;
        offset += 1U + (size_t)info[offset] * 2U;
    }
    if (offset + BRL_MISC_TOUCH_ADDR + 4U > length - BRL_CHECKSUM_BYTES)
        return KSHIM_IO_ERR_PROTOCOL;
    *touch_data_addr = goodix_le32(&info[offset + BRL_MISC_TOUCH_ADDR]);
    return *touch_data_addr ? 0 : KSHIM_IO_ERR_PROTOCOL;
}

static int brl_read_ic_info(struct kshim_touch_dev *dev, uint32_t *touch_data_addr)
{
    uint8_t info[BRL_IC_INFO_MAX];
    int status = ERROR_DEVICE_ERROR;

    for (unsigned attempt = 0; attempt < BRL_ATTEMPTS; ++attempt) {
        uint8_t raw[2];
        if (attempt)
            delay(BRL_RETRY_US);
        if ((status = brl_read(dev, BRL_REG_IC_INFO, raw, sizeof(raw))))
            continue;
        uint16_t length = goodix_le16(raw);
        if (length <= BRL_IC_INFO_FIXED || length >= BRL_IC_INFO_MAX) {
            status = goodix_is_bus_idle(raw, sizeof(raw)) ? KSHIM_IO_ERR_NO_DEVICE
                                                          : KSHIM_IO_ERR_PROTOCOL;
            continue;
        }
        if ((status = brl_read_block(dev, BRL_REG_IC_INFO, info, length)))
            continue;
        if (!(status = brl_parse_ic_info(info, length, touch_data_addr)))
            return 0;
    }
    return status;
}

/* Boot-option handshake after a reset. */
static int brl_confirm(struct kshim_touch_dev *dev)
{
    uint8_t tx[BRL_CONFIRM_BYTES], rx[BRL_CONFIRM_BYTES];
    int status;

    memset(tx, BRL_CONFIRM_VALUE, sizeof(tx));
    for (unsigned attempt = 0; attempt < BRL_ATTEMPTS; ++attempt) {
        if (attempt)
            delay(BRL_RETRY_US);
        if ((status = brl_write(dev, BRL_REG_BOOTOPTION, tx, sizeof(tx))) ||
            (status = brl_read(dev, BRL_REG_BOOTOPTION, rx, sizeof(rx))))
            return status;
        if (!memcmp(tx, rx, sizeof(tx)))
            return 0;
    }
    return KSHIM_IO_ERR_NO_DEVICE;
}

/* Powered, out of reset and reporting a valid firmware version: the
 * previous bootloader left it running. Read-only. */
static bool brl_running(struct kshim_touch_dev *dev)
{
    uint8_t version[BRL_FW_VERSION_BYTES];
    bool asserted;

    if (!goodix_supplies_on(dev))
        return false;
    if (dev->has_reset && (kshim_gpio_get(&dev->reset_gpio, &asserted) || asserted))
        return false;
    return !brl_read_block(dev, BRL_REG_FW_VERSION, version, sizeof(version));
}

static int gt9916_power_on(struct kshim_touch_dev *dev)
{
    struct gt9916 *g = dev->priv;
    int status;

    brl_spi_setup(dev);
    g->inherited = brl_running(dev);
    if (g->inherited) {
        /* Take the rail references (inherited-on rails are not written)
         * and drive reset explicitly released. */
        if ((status = kshim_touch_power_supplies(dev, true)))
            return status;
        printf("GT9916: inherited running controller\n");
        return dev->has_reset ? kshim_touch_set_reset(dev, false) : 0;
    }
    if (dev->has_reset && (status = kshim_touch_set_reset(dev, true)))
        return status;
    if ((status = kshim_touch_power_supplies(dev, true)))
        return status;
    delay(BRL_SUPPLY_DELAY_US);
    if (dev->has_reset && (status = kshim_touch_set_reset(dev, false)))
        return status;
    delay(BRL_FW_INIT_US);
    return 0;
}

static int gt9916_probe(struct kshim_touch_dev *dev)
{
    struct gt9916 *g = dev->priv;
    uint8_t version[BRL_FW_VERSION_BYTES];
    int status;

    brl_spi_setup(dev);
    /* The Berlin interface has no resolution register; the binding makes
     * touchscreen-size-x/y required. */
    if (!dev->props.size_x || !dev->props.size_y) {
        printf("GT9916: touchscreen-size-x/y missing from the DT\n");
        return ERROR_INVALID_PARAMETER;
    }
    if (!g->inherited) {
        if ((status = brl_confirm(dev))) {
            printf("GT9916: boot-option handshake failed (%s)\n", kshim_io_strerror(status));
            return status;
        }
        delay(BRL_FW_BOOT_US);
    }
    if ((status = brl_read_version(dev, version))) {
        printf("GT9916: firmware version unreadable (%s)\n", kshim_io_strerror(status));
        return status;
    }
    for (unsigned i = 0; i < BRL_FW_PATCH_PID_LEN; ++i) {
        uint8_t c = version[BRL_FW_PATCH_PID + i];
        g->patch_pid[i] = c >= 0x20 && c < 0x7f ? (char)c : '\0';
    }
    g->patch_pid[BRL_FW_PATCH_PID_LEN] = '\0';
    g->sensor_id = version[BRL_FW_SENSOR_ID];
    if ((status = brl_read_ic_info(dev, &g->touch_data_addr))) {
        printf("GT9916: IC information invalid (%s)\n", kshim_io_strerror(status));
        return status;
    }
    dev->max_contacts = BRL_MAX_POINTS;
    dev->max_x = dev->props.size_x;
    dev->max_y = dev->props.size_y;

    /* Drop an event latched before we listened. */
    memset(&g->contacts, 0, sizeof(g->contacts));
    const uint8_t zero = 0;
    if ((status = brl_write(dev, g->touch_data_addr, &zero, 1)))
        return status;
    g->last_poll_ms = get_current_time();
    printf("GT9916: firmware %s sensor %u, touch data 0x%x%s\n", g->patch_pid,
           (unsigned)g->sensor_id, (unsigned)g->touch_data_addr,
           g->inherited ? ", inherited" : "");
    return 0;
}

/* The firmware asked for a hardware reset (Berlin request code 3). */
static void gt9916_reset_request(struct kshim_touch_dev *dev, struct gt9916 *g)
{
    goodix_release_all(dev, &g->contacts);
    if (!dev->has_reset) {
        printf("GT9916: reset requested, no reset GPIO\n");
        return;
    }
    if (kshim_touch_set_reset(dev, true) == 0) {
        delay(BRL_RESET_PULSE_US);
        kshim_touch_set_reset(dev, false);
        delay(BRL_FW_BOOT_US);
    }
}

static int gt9916_handle(struct kshim_touch_dev *dev)
{
    struct gt9916 *g = dev->priv;
    uint8_t event[BRL_EVENT_MAX];
    struct goodix_point points[BRL_MAX_POINTS];
    const uint8_t zero = 0;
    unsigned count = 0;
    int status, clear;

    g->last_poll_ms = get_current_time();
    if (!g->touch_data_addr)
        return ERROR_NOT_READY;
    if ((status = brl_read(dev, g->touch_data_addr, event, BRL_PRE_READ)))
        return status;
    if (!event[0])
        return 0;
    if (!brl_checksum_ok(event, BRL_HEAD_BYTES)) {
        status = KSHIM_IO_ERR_PROTOCOL;
    } else if (event[0] & BRL_STATUS_TOUCH) {
        count = event[2] & 0x0fU;
        if (count > BRL_MAX_POINTS)
            status = KSHIM_IO_ERR_PROTOCOL;
        else if (count > 2)
            status = brl_read(dev, g->touch_data_addr + BRL_PRE_READ, &event[BRL_PRE_READ],
                              (count - 2U) * BRL_POINT_BYTES);
        if (!status && count) {
            const uint8_t *data = &event[BRL_HEAD_BYTES];
            uint8_t type = data[0] & 0x0fU;
            /* A stylus frame carries two point slots whatever the count. */
            unsigned covered = type == BRL_POINT_STYLUS || type == BRL_POINT_STYLUS_HOVER
                                   ? 2U : count;
            if (!brl_checksum_ok(data, covered * BRL_POINT_BYTES + BRL_CHECKSUM_BYTES))
                status = KSHIM_IO_ERR_PROTOCOL;
        }
        if (!status) {
            unsigned fingers = 0;
            for (unsigned i = 0; i < count; ++i) {
                const uint8_t *p = &event[BRL_HEAD_BYTES + i * BRL_POINT_BYTES];
                uint8_t type = p[0] & 0x0fU;
                if (type == BRL_POINT_STYLUS || type == BRL_POINT_STYLUS_HOVER)
                    continue;
                points[fingers].id = p[0] >> 4;
                points[fingers].x = goodix_le16(&p[2]);
                points[fingers].y = goodix_le16(&p[4]);
                ++fingers;
            }
            status = goodix_report_frame(dev, &g->contacts, points, fingers);
        }
    }
    /* Release the event (deasserts the level IRQ) before serving a reset. */
    clear = brl_write(dev, g->touch_data_addr, &zero, 1);
    if (status == 0 && (event[0] & BRL_STATUS_REQUEST) && !(event[0] & BRL_STATUS_TOUCH)) {
        if (event[2] == BRL_REQUEST_RESET)
            gt9916_reset_request(dev, g);
        else
            printf("GT9916: request code %u ignored\n", (unsigned)event[2]);
    }
    return status ? status : clear;
}

/* Polling fallback: INT at its active level, else the report cadence (an
 * edge-configured firmware pulses INT). */
static bool gt9916_pending(struct kshim_touch_dev *dev)
{
    const struct gt9916 *g = dev->priv;
    bool active;

    if (dev->has_irq_gpio && !kshim_gpio_get(&dev->irq_gpio, &active) && active)
        return true;
    return goodix_poll_due(g->last_poll_ms);
}

/* Handoff: release the event buffer and stop tracking. The controller is
 * not put to sleep: one we powered returns to its inherited rail and reset
 * state through the framework, one inherited running stays as it was.
 * Best effort; nothing here needs restoring. */
static int gt9916_quiesce(struct kshim_touch_dev *dev)
{
    struct gt9916 *g = dev->priv;
    const uint8_t zero = 0;

    memset(&g->contacts, 0, sizeof(g->contacts));
    if (dev->probed && g->touch_data_addr && brl_write(dev, g->touch_data_addr, &zero, 1))
        printf("GT9916: event not released at quiesce\n");
    return 0;
}

static const char *const gt9916_compatibles[] = { "goodix,gt9916", NULL };
/* vddio, then avdd (Berlin power-up order); QRD8650 wires only avdd. */
static const char *const gt9916_supplies[] = { "vddio", "avdd", NULL };

const struct kshim_touch_driver kshim_touch_driver_gt9916 = {
    .name = "Goodix GT9916",
    .compatibles = gt9916_compatibles,
    .bus_mask = KSHIM_TOUCH_BUS_SPI,
    .supplies = gt9916_supplies,
    /* Used by power_on's cold path; listed for the framework's logs. */
    .power_on_delay_us = BRL_SUPPLY_DELAY_US,
    .post_reset_delay_us = BRL_FW_INIT_US,
    .priv_size = sizeof(struct gt9916),
    .power_on = gt9916_power_on,
    .probe = gt9916_probe,
    .handle = gt9916_handle,
    .pending = gt9916_pending,
    .quiesce = gt9916_quiesce,
};
