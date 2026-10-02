#include "ui_glass_motion.h"

static int32_t smooth_segment(int32_t start, int32_t end,
                              int32_t local, int32_t span)
{
    int64_t t = local;
    int64_t maximum = span;
    int64_t smooth = t * t * (3 * maximum - 2 * t) /
                     (maximum * maximum);
    return start + (int32_t)(((int64_t)(end - start) * smooth) / maximum);
}

int32_t ui_glass_motion_clamp(int32_t progress)
{
    if (progress < 0) return 0;
    if (progress > UI_GLASS_MOTION_PROGRESS_MAX) {
        return UI_GLASS_MOTION_PROGRESS_MAX;
    }
    return progress;
}

int32_t ui_glass_ease_out_cubic(int32_t progress)
{
    const int64_t maximum = UI_GLASS_MOTION_PROGRESS_MAX;
    int64_t remaining = maximum - ui_glass_motion_clamp(progress);
    return (int32_t)(maximum -
                     remaining * remaining * remaining /
                     (maximum * maximum));
}

int32_t ui_glass_ease_in_out_cubic(int32_t progress)
{
    const int64_t maximum = UI_GLASS_MOTION_PROGRESS_MAX;
    int64_t value = ui_glass_motion_clamp(progress);
    if (value < maximum / 2) {
        return (int32_t)(4 * value * value * value /
                         (maximum * maximum));
    }
    int64_t remaining = maximum - value;
    return (int32_t)(maximum -
                     4 * remaining * remaining * remaining /
                     (maximum * maximum));
}

int32_t ui_glass_spring(int32_t progress)
{
    static const int16_t time[] = { 0, 512, 768, 896, 1024 };
    static const int16_t value[] = { 0, 870, 1072, 1012, 1024 };
    int32_t clamped = ui_glass_motion_clamp(progress);

    for (unsigned i = 0; i + 1 < sizeof(time) / sizeof(time[0]); ++i) {
        if (clamped <= time[i + 1]) {
            return smooth_segment(value[i], value[i + 1],
                                  clamped - time[i],
                                  time[i + 1] - time[i]);
        }
    }
    return UI_GLASS_MOTION_PROGRESS_MAX;
}

int32_t ui_glass_interpolate(int32_t start, int32_t target,
                             int32_t progress)
{
    return start + (int32_t)(((int64_t)(target - start) * progress) /
                             UI_GLASS_MOTION_PROGRESS_MAX);
}

ui_glass_morph_frame_t ui_glass_morph_interpolate(
    ui_glass_morph_frame_t start,
    ui_glass_morph_frame_t target,
    int32_t progress)
{
    int32_t opacity = ui_glass_interpolate(start.opacity, target.opacity,
                                           progress);
    if (opacity < 0) opacity = 0;
    if (opacity > 255) opacity = 255;
    return (ui_glass_morph_frame_t) {
        .x = (int16_t)ui_glass_interpolate(start.x, target.x, progress),
        .y = (int16_t)ui_glass_interpolate(start.y, target.y, progress),
        .width = (int16_t)ui_glass_interpolate(start.width, target.width,
                                               progress),
        .height = (int16_t)ui_glass_interpolate(start.height, target.height,
                                                progress),
        .radius = (int16_t)ui_glass_interpolate(start.radius, target.radius,
                                                progress),
        .opacity = (uint8_t)opacity,
    };
}
