/* SPDX-License-Identifier: MIT */
#pragma once
/*
 * Helpers shared by the Goodix GT911 (goodix_gt911.c) and GT9916
 * (goodix_gt9916.c) drivers on the touch framework (kshim/touch_driver.h).
 * Owner: touch-goodix. See docs/touch-goodix.md.
 *
 * Both controllers report frames: every frame lists the contacts that are
 * down now, each with a track ID. goodix_report_frame() turns two
 * consecutive frames into the framework's ENTER/MOVE/LEAVE contact events.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kshim/io.h>
#include <kshim/regulator.h>
#include <kshim/time.h>
#include <kshim/touch_driver.h>

/* Polling fallback cadence: the controllers refresh at about 100 Hz. */
#define GOODIX_POLL_INTERVAL_MS 10U

struct goodix_point {
    uint8_t id;
    uint32_t x, y;
};

/* Contacts reported down by the previous frame. */
struct goodix_contacts {
    uint16_t down;                          /* bit per track ID */
    uint32_t x[KSHIM_TOUCH_MAX_CONTACTS];
    uint32_t y[KSHIM_TOUCH_MAX_CONTACTS];
};

static inline uint16_t goodix_le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static inline uint32_t goodix_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* Bus default line state (0x00 or 0xff everywhere): nothing drove the data. */
static inline bool goodix_is_bus_idle(const uint8_t *data, size_t length)
{
    bool zeros = true, ones = true;
    for (size_t i = 0; i < length; ++i) {
        zeros = zeros && data[i] == 0x00U;
        ones = ones && data[i] == 0xffU;
    }
    return zeros || ones;
}

/*
 * Report one frame. points[] holds the contacts down now (IDs >= the
 * framework limit and repeated IDs are dropped). New IDs are ENTER,
 * known IDs MOVE, IDs missing from this frame LEAVE at their last
 * position. Returns the first framework error, after reporting the rest.
 */
static inline int goodix_report_frame(struct kshim_touch_dev *dev, struct goodix_contacts *c,
                                      const struct goodix_point *points, unsigned count)
{
    uint16_t now = 0;
    int status = 0;

    for (unsigned i = 0; i < count; ++i) {
        uint8_t id = points[i].id;
        if (id >= KSHIM_TOUCH_MAX_CONTACTS || (now & (1U << id)))
            continue;
        now |= (uint16_t)(1U << id);
        int r = kshim_touch_report(dev, id,
                                   (c->down & (1U << id)) ? KSHIM_TOUCH_CONTACT_MOVE
                                                          : KSHIM_TOUCH_CONTACT_ENTER,
                                   points[i].x, points[i].y);
        if (r && !status)
            status = r;
        c->x[id] = points[i].x;
        c->y[id] = points[i].y;
    }
    for (uint8_t id = 0; id < KSHIM_TOUCH_MAX_CONTACTS; ++id) {
        if (!(c->down & (1U << id)) || (now & (1U << id)))
            continue;
        int r = kshim_touch_report(dev, id, KSHIM_TOUCH_CONTACT_LEAVE, c->x[id], c->y[id]);
        if (r && !status)
            status = r;
    }
    c->down = now;
    return status;
}

/* Lift every contact (controller reset, quiesce). */
static inline void goodix_release_all(struct kshim_touch_dev *dev, struct goodix_contacts *c)
{
    goodix_report_frame(dev, c, NULL, 0);
}

/* Every resolved supply already reports on (inherited rails). */
static inline bool goodix_supplies_on(const struct kshim_touch_dev *dev)
{
    for (unsigned i = 0; i < dev->supply_count; ++i)
        if (!dev->supplies[i] || !kshim_regulator_is_enabled(dev->supplies[i]))
            return false;
    return true;
}

/* Polling fallback: due when GOODIX_POLL_INTERVAL_MS passed since *last_ms. */
static inline bool goodix_poll_due(uint64_t last_ms)
{
    return get_current_time() - last_ms >= GOODIX_POLL_INTERVAL_MS;
}
