#include "liquid_glass_motion.h"

static int32_t clamp_progress(int32_t progress)
{
    if (progress < 0) return 0;
    if (progress > LIQUID_GLASS_MOTION_PROGRESS_MAX) {
        return LIQUID_GLASS_MOTION_PROGRESS_MAX;
    }
    return progress;
}

static int32_t linear(int32_t start, int32_t target, int32_t progress)
{
    return start + ((target - start) * progress) /
                   LIQUID_GLASS_MOTION_PROGRESS_MAX;
}

static int32_t quadratic(int32_t start, int32_t control, int32_t target,
                         int32_t progress)
{
    const int64_t maximum = LIQUID_GLASS_MOTION_PROGRESS_MAX;
    const int64_t remaining = maximum - progress;
    const int64_t denominator = maximum * maximum;
    int64_t numerator = (int64_t)start * remaining * remaining +
                        (int64_t)2 * control * remaining * progress +
                        (int64_t)target * progress * progress;
    return (int32_t)((numerator + denominator / 2) / denominator);
}

static uint8_t channel(int32_t value)
{
    if (value < 0) return 0;
    if (value > 255) return 255;
    return (uint8_t)value;
}

uint8_t liquid_glass_window_rank(uint8_t window_index, uint8_t selected_index)
{
    return (uint8_t)((window_index + LIQUID_GLASS_WINDOW_COUNT - selected_index) %
                     LIQUID_GLASS_WINDOW_COUNT);
}

liquid_glass_frame_t liquid_glass_frame_for_rank(uint8_t rank)
{
    // These are one information deck, not three perspective objects. Keep the
    // scale delta subordinate to occlusion and edge exposure: each depth rank
    // changes only 4 px in width, 2 px in height, and 10 px vertically.
    static const uint8_t border_opacity[LIQUID_GLASS_WINDOW_COUNT] = {
        86, 60, 38,
    };
    if (rank >= LIQUID_GLASS_WINDOW_COUNT) {
        rank = LIQUID_GLASS_WINDOW_COUNT - 1;
    }
    return (liquid_glass_frame_t) {
        .x = (int16_t)(14 + rank * 2),
        .y = (int16_t)(68 + rank * 10),
        .width = (int16_t)(212 - rank * 4),
        .height = (int16_t)(172 - rank * 2),
        .surface_opacity = 64,
        .border_opacity = border_opacity[rank],
        .content_opacity = rank == 0 ? 255 : 0,
    };
}

liquid_glass_frame_t liquid_glass_transition_frame(
    liquid_glass_frame_t start,
    liquid_glass_frame_t target,
    bool wraps_depth,
    int32_t progress)
{
    progress = clamp_progress(progress);
    if (!wraps_depth) {
        return (liquid_glass_frame_t) {
            .x = (int16_t)linear(start.x, target.x, progress),
            .y = (int16_t)linear(start.y, target.y, progress),
            .width = (int16_t)linear(start.width, target.width, progress),
            .height = (int16_t)linear(start.height, target.height, progress),
            .surface_opacity = channel(linear(start.surface_opacity,
                                              target.surface_opacity,
                                              progress)),
            .border_opacity = channel(linear(start.border_opacity,
                                             target.border_opacity,
                                             progress)),
            .content_opacity = channel(linear(start.content_opacity,
                                              target.content_opacity,
                                              progress)),
        };
    }

    // Keep every keyframe centered at x=120. The wrapping card lifts above the
    // stack as its scale changes, so depth changes never read as a card sliding
    // in from a screen edge. Reusing the control point makes the backward path
    // the exact geometric inverse of the forward path.
    const liquid_glass_frame_t control = {
        .x = 15, .y = 54, .width = 210, .height = 170,
        .surface_opacity = 0, .border_opacity = 54,
        .content_opacity = 0,
    };
    int32_t material_opacity =
        (start.surface_opacity + target.surface_opacity + 1) / 2;
    return (liquid_glass_frame_t) {
        .x = (int16_t)quadratic(start.x, control.x, target.x, progress),
        .y = (int16_t)quadratic(start.y, control.y, target.y, progress),
        .width = (int16_t)quadratic(start.width, control.width,
                                   target.width, progress),
        .height = (int16_t)quadratic(start.height, control.height,
                                    target.height, progress),
        .surface_opacity = channel(quadratic(start.surface_opacity,
                                             material_opacity,
                                             target.surface_opacity,
                                             progress)),
        .border_opacity = channel(quadratic(start.border_opacity,
                                            control.border_opacity,
                                            target.border_opacity,
                                            progress)),
        .content_opacity = channel(quadratic(start.content_opacity,
                                             control.content_opacity,
                                             target.content_opacity,
                                             progress)),
    };
}
