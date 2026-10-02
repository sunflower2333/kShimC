#pragma once

#include "liquid_glass_motion.h"

#include <stddef.h>
#include <stdint.h>

#define LIQUID_GLASS_COMPOSITOR_WIDTH  240
#define LIQUID_GLASS_COMPOSITOR_HEIGHT 320
#define LIQUID_GLASS_CARD_RADIUS       26
#define LIQUID_GLASS_EDGE_RING_COUNT   3
#define LIQUID_GLASS_COVERAGE_STATES   (1u << LIQUID_GLASS_WINDOW_COUNT)
#define LIQUID_GLASS_MAX_ROW_SPANS     7
#define LIQUID_GLASS_MAX_DIRTY_RECTS   12

typedef struct {
    int16_t x1;
    int16_t x2;
    uint8_t coverage_mask;
} liquid_glass_span_t;

typedef struct {
    int16_t x1;
    int16_t y1;
    int16_t x2;
    int16_t y2;
} liquid_glass_dirty_rect_t;

// One lookup per native RGB565 channel replaces up to three source-over
// blends. The coverage key is a card bitmask, so startup frames with unequal
// card opacity remain exact as well.
typedef struct {
    uint8_t red[LIQUID_GLASS_COVERAGE_STATES][32];
    uint8_t green[LIQUID_GLASS_COVERAGE_STATES][64];
    uint8_t blue[LIQUID_GLASS_COVERAGE_STATES][32];
} liquid_glass_rgb565_lut_t;

// Precomputed native-color edge treatment for one card. Opacities already
// include material and depth exposure, keeping the hot row renderer free of
// divisions and color-space conversion.
typedef struct {
    uint16_t ring_color[LIQUID_GLASS_EDGE_RING_COUNT];
    uint8_t ring_opacity[LIQUID_GLASS_EDGE_RING_COUNT];
    uint16_t top_specular_color;
    uint16_t bottom_refraction_color;
    uint16_t glint_color;
    uint8_t top_specular_opacity;
    uint8_t bottom_refraction_opacity;
    uint8_t glint_opacity;
    uint8_t glint_width_percent;
    int16_t glint_progress;
} liquid_glass_edge_style_t;

uint16_t liquid_glass_rgb888_to_rgb565(uint32_t color);

void liquid_glass_rgb565_lut_build(
    liquid_glass_rgb565_lut_t *lut,
    uint16_t tint,
    const uint8_t opacity[LIQUID_GLASS_WINDOW_COUNT]);

uint16_t liquid_glass_rgb565_lut_apply(
    const liquid_glass_rgb565_lut_t *lut,
    uint16_t background,
    uint8_t coverage_mask);

// Produces non-overlapping inclusive spans for a rounded-rectangle card set.
// Spans with no card coverage are omitted.
size_t liquid_glass_coverage_spans(
    const liquid_glass_frame_t frames[LIQUID_GLASS_WINDOW_COUNT],
    int16_t y,
    int16_t clip_x1,
    int16_t clip_x2,
    liquid_glass_span_t spans[LIQUID_GLASS_MAX_ROW_SPANS]);

/* kShimC extension: retain the upstream span planner while allowing the
 * resolution-independent adapter to supply its scaled corner radius. */
size_t liquid_glass_coverage_spans_radius(
    const liquid_glass_frame_t frames[LIQUID_GLASS_WINDOW_COUNT],
    int16_t y,
    int16_t clip_x1,
    int16_t clip_x2,
    int16_t radius,
    liquid_glass_span_t spans[LIQUID_GLASS_MAX_ROW_SPANS]);

// Composites one clipped row from the immutable wallpaper. Input and output
// may alias, which is useful for an in-place scanline decoder.
void liquid_glass_composite_row(
    const liquid_glass_rgb565_lut_t *lut,
    const liquid_glass_frame_t frames[LIQUID_GLASS_WINDOW_COUNT],
    const uint16_t *wallpaper_row,
    int16_t y,
    int16_t x1,
    int16_t x2,
    uint16_t *output);

// Blends only the short optical edge spans in physical back-to-front order.
// The ordinary fill path retains its single combined LUT lookup, so this adds
// work proportional to perimeter pixels rather than invalidated area.
void liquid_glass_composite_edges_row(
    const liquid_glass_frame_t frames[LIQUID_GLASS_WINDOW_COUNT],
    const uint8_t draw_order[LIQUID_GLASS_WINDOW_COUNT],
    const liquid_glass_edge_style_t styles[LIQUID_GLASS_WINDOW_COUNT],
    int16_t y,
    int16_t x1,
    int16_t x2,
    uint16_t *output);

// Finds the changed coverage tiles between two coherent deck states, then
// greedily coalesces them to stay below LVGL's invalid-area fallback limit.
// Returned rectangles are inclusive and never omit a changed pixel.
size_t liquid_glass_plan_dirty_rects(
    const liquid_glass_frame_t previous[LIQUID_GLASS_WINDOW_COUNT],
    const liquid_glass_frame_t next[LIQUID_GLASS_WINDOW_COUNT],
    liquid_glass_dirty_rect_t rects[LIQUID_GLASS_MAX_DIRTY_RECTS]);

// Extends the coverage diff with exact visual regions owned by ordinary LVGL
// children (text, bars, and optical edges). All regions share one tile map and
// one rectangle budget, preventing independent invalidations from overflowing
// LVGL's fixed 32-entry invalid-area buffer.
size_t liquid_glass_plan_visual_dirty_rects(
    const liquid_glass_frame_t previous[LIQUID_GLASS_WINDOW_COUNT],
    const liquid_glass_frame_t next[LIQUID_GLASS_WINDOW_COUNT],
    const liquid_glass_dirty_rect_t *visual_regions,
    size_t visual_region_count,
    liquid_glass_dirty_rect_t rects[LIQUID_GLASS_MAX_DIRTY_RECTS]);
