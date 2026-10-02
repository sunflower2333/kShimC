/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <qcom_bus.h>
#include <touch.h>

#define KSHIM_FTM4_WIDTH 1440U
#define KSHIM_FTM4_HEIGHT 2880U
#define KSHIM_FTM4_ADDRESS 0x49U

typedef struct {
    void *context;
    int (*transfer)(void *context, const uint8_t *command, size_t command_size,
                    uint8_t *response, size_t response_size);
    void (*delay_us)(void *context, uint32_t us);
} kshim_ftm4_transport_t;

typedef struct {
    kshim_ftm4_transport_t transport;
    kshim_touch_t *touch;
    bool flip_x, flip_y, identified, scanning, initialized, quiesced;
} kshim_ftm4_t;

/* Board code supplies power and a 20 ms hardware reset pulse first. FTM4 is
 * distinct from FTM5: 16-bit HW addresses, a dummy HW byte and 0x85 FIFO. */
int kshim_ftm4_init(kshim_ftm4_t *device,
                    const kshim_ftm4_transport_t *transport,
                    kshim_touch_t *touch, bool flip_x, bool flip_y);
int kshim_ftm4_init_i2c(kshim_ftm4_t *device, struct kshim_qcom_bus *bus,
                        kshim_touch_t *touch, bool flip_x, bool flip_y);
int kshim_ftm4_poll(kshim_ftm4_t *device);
int kshim_ftm4_quiesce(kshim_ftm4_t *device);
