#pragma once

#include <stdbool.h>
#include <stdint.h>

#define LIQUID_GLASS_WINDOW_COUNT 3
#define LIQUID_GLASS_MOTION_PROGRESS_MAX 1024
#define LIQUID_GLASS_DEPTH_CROSS_PROGRESS 512

typedef struct {
    int16_t x;
    int16_t y;
    int16_t width;
    int16_t height;
    uint8_t surface_opacity;
    uint8_t border_opacity;
    uint8_t content_opacity;
} liquid_glass_frame_t;

uint8_t liquid_glass_window_rank(uint8_t window_index, uint8_t selected_index);
liquid_glass_frame_t liquid_glass_frame_for_rank(uint8_t rank);

// Returns one point on a card's continuous path between deck ranks. A wrapping
// card (front <-> back) follows the same reversible quadratic depth curve in
// both directions and remains visible for the entire transition.
liquid_glass_frame_t liquid_glass_transition_frame(
    liquid_glass_frame_t start,
    liquid_glass_frame_t target,
    bool wraps_depth,
    int32_t progress);
