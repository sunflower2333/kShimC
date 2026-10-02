#pragma once

#include <stdint.h>

#define UI_GLASS_MOTION_PROGRESS_MAX 1024

typedef struct {
    int16_t x;
    int16_t y;
    int16_t width;
    int16_t height;
    int16_t radius;
    uint8_t opacity;
} ui_glass_morph_frame_t;

int32_t ui_glass_motion_clamp(int32_t progress);
int32_t ui_glass_ease_out_cubic(int32_t progress);
int32_t ui_glass_ease_in_out_cubic(int32_t progress);

// A deterministic, lightly under-damped curve for focus and morph motion.
// The returned value intentionally overshoots 1024 before settling at 1024.
int32_t ui_glass_spring(int32_t progress);

int32_t ui_glass_interpolate(int32_t start, int32_t target,
                             int32_t progress);
ui_glass_morph_frame_t ui_glass_morph_interpolate(
    ui_glass_morph_frame_t start,
    ui_glass_morph_frame_t target,
    int32_t progress);
