/* SPDX-License-Identifier: GPL-2.0-only
 * ST FTM4 protocol subset based on STMicroelectronics' 2016-2019 driver.
 * Source: OnePlusOSS/android_kernel_oneplus_sm8250,
 * cad09e061ef6cf689a4a1e54d27562e0f042236a, drivers/input/touchscreen/st.
 * New freestanding implementation: no update, calibration or flash commands.
 */
#include <st_ftm4.h>

#define FTM4_FIFO 0x85U
#define FTM4_HW 0xb6U
#define FTM4_SENSE_OFF 0x92U
#define FTM4_SENSE_ON 0x93U
#define FTM4_READY 0x10U
#define FTM4_ERROR 0x0fU
#define FTM4_ENTER 0x03U
#define FTM4_LEAVE 0x04U
#define FTM4_MOVE 0x05U
#define FTM4_READY_POLL_US 10000U
#define FTM4_READY_POLLS 50U /* Vendor GENERAL_TIMEOUT is 500 ms. */

static int transfer(kshim_ftm4_t *d, const uint8_t *cmd, size_t size,
                     uint8_t *response, size_t count)
{
    return d->transport.transfer(d->transport.context, cmd, size, response, count);
}

static int irq_enable(kshim_ftm4_t *d, bool enabled)
{
    const uint8_t command[] = {FTM4_HW, 0x00, 0x2c, enabled ? 0x41 : 0x00};
    return transfer(d, command, sizeof(command), NULL, 0);
}

static int read_event(kshim_ftm4_t *d, uint8_t event[8])
{
    const uint8_t command = FTM4_FIFO;
    return transfer(d, &command, 1, event, 8);
}

static int fail(kshim_ftm4_t *d, int status)
{
    kshim_touch_cancel(d->touch);
    d->initialized = false;
    return status;
}

int kshim_ftm4_quiesce(kshim_ftm4_t *d)
{
    if (!d)
        return KSHIM_BUS_INVALID;
    kshim_touch_cancel(d->touch);
    d->initialized = false;
    if (!d->identified || d->quiesced)
        return 0;
    /* A failed write may have reached the controller, so preserve scanning
     * until both commands succeed and permit a later bounded retry. */
    const uint8_t command = FTM4_SENSE_OFF;
    int status = transfer(d, &command, 1, NULL, 0);
    if (status != 0)
        return status;
    status = irq_enable(d, false);
    if (status == 0) {
        d->scanning = false;
        d->quiesced = true;
    }
    return status;
}

int kshim_ftm4_init(kshim_ftm4_t *d, const kshim_ftm4_transport_t *transport,
                    kshim_touch_t *touch, bool flip_x, bool flip_y)
{
    if (!d || !transport || !transport->transfer || !transport->delay_us ||
        !touch || !touch->initialized ||
        touch->config.source_width != KSHIM_FTM4_WIDTH ||
        touch->config.source_height != KSHIM_FTM4_HEIGHT)
        return KSHIM_BUS_INVALID;
    *d = (kshim_ftm4_t){.transport = *transport, .touch = touch,
                         .flip_x = flip_x, .flip_y = flip_y};
    uint8_t event[8];
    bool ready = false;
    for (unsigned i = 0; i <= FTM4_READY_POLLS; ++i) {
        int status = read_event(d, event);
        if (status != 0)
            return fail(d, status);
        if (event[0] == FTM4_READY) {
            ready = true;
            break;
        }
        if (event[0] == FTM4_ERROR)
            return fail(d, KSHIM_BUS_IO);
        if (i < FTM4_READY_POLLS)
            transport->delay_us(transport->context, FTM4_READY_POLL_US);
    }
    if (!ready)
        return fail(d, KSHIM_BUS_TIMEOUT);

    const uint8_t id_command[] = {FTM4_HW, 0x00, 0x04};
    uint8_t identity[3]; /* HW register reads include a leading dummy byte. */
    int status = transfer(d, id_command, sizeof(id_command), identity, sizeof(identity));
    if (status != 0)
        return fail(d, status);
    if (identity[1] != 0x36 || identity[2] != 0x70)
        return fail(d, KSHIM_BUS_UNSUPPORTED);
    d->identified = true;
    status = irq_enable(d, false);
    if (status == 0) {
        const uint8_t command = FTM4_SENSE_ON;
        d->scanning = true;
        status = transfer(d, &command, 1, NULL, 0);
    }
    if (status == 0)
        status = irq_enable(d, true);
    if (status != 0) {
        (void)kshim_ftm4_quiesce(d);
        return fail(d, status);
    }
    d->initialized = true;
    return 0;
}

int kshim_ftm4_poll(kshim_ftm4_t *d)
{
    if (!d || !d->initialized)
        return KSHIM_BUS_INVALID;
    /* Match the vendor's bounded READONE loop; FTM4's 64-event FIFO does
     * not use FTM5's record layout or bulk-read command semantics. */
    for (unsigned i = 0; i < 64; ++i) {
        uint8_t event[8];
        int status = read_event(d, event);
        if (status != 0)
            return fail(d, status);
        if (event[0] == 0)
            return 0;
        if (event[0] == FTM4_ERROR || event[0] == FTM4_READY)
            return fail(d, KSHIM_BUS_IO);
        if (event[0] != FTM4_ENTER && event[0] != FTM4_MOVE && event[0] != FTM4_LEAVE)
            continue; /* Status, echo, and manufacturing reports are not touches. */
        uint8_t id = event[1] & 0x0f;
        if (id >= KSHIM_TOUCH_MAX_CONTACTS)
            return fail(d, KSHIM_BUS_INVALID);
        uint32_t x = ((uint32_t)event[2] << 4) | (event[4] >> 4);
        uint32_t y = ((uint32_t)event[3] << 4) | (event[4] & 0x0f);
        if (x > KSHIM_FTM4_WIDTH)
            x = KSHIM_FTM4_WIDTH;
        if (y > KSHIM_FTM4_HEIGHT)
            y = KSHIM_FTM4_HEIGHT;
        if (d->flip_x)
            x = KSHIM_FTM4_WIDTH - x;
        if (d->flip_y)
            y = KSHIM_FTM4_HEIGHT - y;
        kshim_touch_contact_action_t action = event[0] == FTM4_LEAVE ?
            KSHIM_TOUCH_CONTACT_LEAVE : (event[0] == FTM4_ENTER ||
            !d->touch->contacts[id].active ? KSHIM_TOUCH_CONTACT_ENTER : KSHIM_TOUCH_CONTACT_MOVE);
        status = kshim_touch_update(d->touch, id, action, x, y);
        if (status != 0)
            return fail(d, status);
    }
    return 0;
}

static int i2c_transfer(void *context, const uint8_t *command, size_t size,
                         uint8_t *response, size_t count)
{
    struct kshim_i2c_msg messages[] = {
        {KSHIM_FTM4_ADDRESS, false, (uint8_t *)(uintptr_t)command, size},
        {KSHIM_FTM4_ADDRESS, true, response, count},
    };
    return kshim_i2c_transfer(context, messages, count ? 2U : 1U, 20000);
}

static void i2c_delay(void *context, uint32_t us)
{
    struct kshim_qcom_bus *bus = context;
    bus->io.delay_us(bus->io.context, us);
}

int kshim_ftm4_init_i2c(kshim_ftm4_t *d, struct kshim_qcom_bus *bus,
                        kshim_touch_t *touch, bool flip_x, bool flip_y)
{
    if (!bus || !bus->initialized || !bus->io.delay_us ||
        bus->config.protocol != KSHIM_GENI_I2C || bus->config.speed_hz > 400000)
        return KSHIM_BUS_INVALID;
    const kshim_ftm4_transport_t transport = {bus, i2c_transfer, i2c_delay};
    return kshim_ftm4_init(d, &transport, touch, flip_x, flip_y);
}
