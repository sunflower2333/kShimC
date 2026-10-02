#include "liquid_glass_compositor_core.h"

#include <stdbool.h>
#include <string.h>

#define DIRTY_TILE_SIZE 8
#define DIRTY_TILE_COLUMNS \
    (LIQUID_GLASS_COMPOSITOR_WIDTH / DIRTY_TILE_SIZE)
#define DIRTY_TILE_ROWS \
    (LIQUID_GLASS_COMPOSITOR_HEIGHT / DIRTY_TILE_SIZE)

typedef struct {
    int16_t x;
    uint8_t card_bit;
} coverage_event_t;

static uint8_t mix_to_five_bits(uint8_t opacity)
{
    return (uint8_t)(((uint16_t)opacity + 4u) >> 3);
}

static uint8_t blend_channel(uint8_t foreground, uint8_t background,
                             uint8_t opacity)
{
    uint8_t mix = mix_to_five_bits(opacity);
    return (uint8_t)(((uint16_t)foreground * mix +
                      (uint16_t)background * (32u - mix)) >> 5);
}

static uint8_t card_channel_for_mask(uint8_t background, uint8_t foreground,
                                     uint8_t mask,
                                     const uint8_t opacity[
                                         LIQUID_GLASS_WINDOW_COUNT])
{
    uint8_t result = background;
    for (uint8_t card = 0; card < LIQUID_GLASS_WINDOW_COUNT; ++card) {
        if (mask & (uint8_t)(1u << card)) {
            result = blend_channel(foreground, result, opacity[card]);
        }
    }
    return result;
}

uint16_t liquid_glass_rgb888_to_rgb565(uint32_t color)
{
    return (uint16_t)((((color >> 16) & 0xFFu) >> 3) << 11 |
                      (((color >> 8) & 0xFFu) >> 2) << 5 |
                      ((color & 0xFFu) >> 3));
}

void liquid_glass_rgb565_lut_build(
    liquid_glass_rgb565_lut_t *lut,
    uint16_t tint,
    const uint8_t opacity[LIQUID_GLASS_WINDOW_COUNT])
{
    if (!lut || !opacity) return;

    uint8_t tint_red = (uint8_t)((tint >> 11) & 0x1Fu);
    uint8_t tint_green = (uint8_t)((tint >> 5) & 0x3Fu);
    uint8_t tint_blue = (uint8_t)(tint & 0x1Fu);
    for (uint8_t mask = 0; mask < LIQUID_GLASS_COVERAGE_STATES; ++mask) {
        for (uint8_t value = 0; value < 32; ++value) {
            lut->red[mask][value] = card_channel_for_mask(
                value, tint_red, mask, opacity);
            lut->blue[mask][value] = card_channel_for_mask(
                value, tint_blue, mask, opacity);
        }
        for (uint8_t value = 0; value < 64; ++value) {
            lut->green[mask][value] = card_channel_for_mask(
                value, tint_green, mask, opacity);
        }
    }
}

uint16_t liquid_glass_rgb565_lut_apply(
    const liquid_glass_rgb565_lut_t *lut,
    uint16_t background,
    uint8_t coverage_mask)
{
    coverage_mask &= LIQUID_GLASS_COVERAGE_STATES - 1u;
    return (uint16_t)(
        (uint16_t)lut->red[coverage_mask][background >> 11] << 11 |
        (uint16_t)lut->green[coverage_mask][(background >> 5) & 0x3Fu] << 5 |
        lut->blue[coverage_mask][background & 0x1Fu]);
}

static int16_t rounded_interval_inset_for_radius(int16_t local_y,
                                                 int16_t height,
                                                 int16_t radius)
{
    if (radius * 2 > height) radius = height / 2;
    if (radius <= 0) return 0;

    int16_t edge_y;
    if (local_y < radius) {
        edge_y = (int16_t)(radius - 1 - local_y);
    } else if (local_y >= height - radius) {
        edge_y = (int16_t)(local_y - (height - radius));
    } else {
        return 0;
    }

    // The fused renderer needs the outer fill plus three nested one-pixel
    // outlines. Exact integer-circle tables avoid square roots in every row.
    static const uint8_t inset[4][LIQUID_GLASS_CARD_RADIUS] = {
        { 0, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 3, 3,
          4, 5, 5, 6, 7, 8, 9, 10, 11, 13, 14, 16, 19 },
        { 0, 1, 1, 1, 1, 1, 1, 1, 2, 2, 3, 3, 4,
          4, 5, 5, 6, 7, 8, 9, 10, 12, 14, 16, 18 },
        { 0, 1, 1, 1, 1, 1, 1, 2, 2, 2, 3, 3, 4,
          4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 18 },
        { 0, 1, 1, 1, 1, 1, 1, 2, 2, 2, 3, 3, 4,
          5, 5, 6, 7, 8, 9, 11, 12, 14, 17 },
    };
    int16_t table = LIQUID_GLASS_CARD_RADIUS - radius;
    if (table < 0) table = 0;
    if (table > 3) table = 3;
    if (edge_y >= LIQUID_GLASS_CARD_RADIUS) {
        edge_y = LIQUID_GLASS_CARD_RADIUS - 1;
    }
    return inset[table][edge_y];
}

static bool card_interval_radius(const liquid_glass_frame_t *frame, int16_t y,
                                 int16_t radius, int16_t *x1, int16_t *x2)
{
    if (frame->width <= 0 || frame->height <= 0 ||
        y < frame->y || y >= frame->y + frame->height) {
        return false;
    }

    int16_t local_y = (int16_t)(y - frame->y);
    int16_t inset = rounded_interval_inset_for_radius(
        local_y, frame->height, radius);
    int16_t half_width = frame->width / 2;
    if (inset > half_width) inset = half_width;
    *x1 = (int16_t)(frame->x + inset);
    *x2 = (int16_t)(frame->x + frame->width - 1 - inset);
    return *x1 <= *x2;
}

static bool inset_card_interval(const liquid_glass_frame_t *frame,
                                int16_t y,
                                int16_t inset,
                                int16_t *x1,
                                int16_t *x2)
{
    int16_t width = (int16_t)(frame->width - inset * 2);
    int16_t height = (int16_t)(frame->height - inset * 2);
    int16_t top = (int16_t)(frame->y + inset);
    int16_t radius = (int16_t)(LIQUID_GLASS_CARD_RADIUS - inset);
    if (width <= 0 || height <= 0 || y < top || y >= top + height) {
        return false;
    }

    int16_t curve = rounded_interval_inset_for_radius(
        (int16_t)(y - top), height, radius);
    int16_t half_width = width / 2;
    if (curve > half_width) curve = half_width;
    *x1 = (int16_t)(frame->x + inset + curve);
    *x2 = (int16_t)(frame->x + inset + width - 1 - curve);
    return *x1 <= *x2;
}

static void sort_events(coverage_event_t *events, size_t count)
{
    for (size_t i = 1; i < count; ++i) {
        coverage_event_t value = events[i];
        size_t position = i;
        while (position > 0 && events[position - 1].x > value.x) {
            events[position] = events[position - 1];
            --position;
        }
        events[position] = value;
    }
}

size_t liquid_glass_coverage_spans(
    const liquid_glass_frame_t frames[LIQUID_GLASS_WINDOW_COUNT],
    int16_t y,
    int16_t clip_x1,
    int16_t clip_x2,
    liquid_glass_span_t spans[LIQUID_GLASS_MAX_ROW_SPANS])
{
    return liquid_glass_coverage_spans_radius(
        frames, y, clip_x1, clip_x2, LIQUID_GLASS_CARD_RADIUS, spans);
}

size_t liquid_glass_coverage_spans_radius(
    const liquid_glass_frame_t frames[LIQUID_GLASS_WINDOW_COUNT],
    int16_t y,
    int16_t clip_x1,
    int16_t clip_x2,
    int16_t radius,
    liquid_glass_span_t spans[LIQUID_GLASS_MAX_ROW_SPANS])
{
    if (!frames || !spans || clip_x1 > clip_x2) return 0;
    if (radius < 0) radius = 0;

    coverage_event_t events[LIQUID_GLASS_WINDOW_COUNT * 2];
    size_t event_count = 0;
    for (uint8_t card = 0; card < LIQUID_GLASS_WINDOW_COUNT; ++card) {
        int16_t x1;
        int16_t x2;
        if (!card_interval_radius(&frames[card], y, radius, &x1, &x2)) {
            continue;
        }
        events[event_count++] = (coverage_event_t) {
            .x = x1,
            .card_bit = (uint8_t)(1u << card),
        };
        events[event_count++] = (coverage_event_t) {
            .x = (int16_t)(x2 + 1),
            .card_bit = (uint8_t)(1u << card),
        };
    }
    if (event_count == 0) return 0;

    sort_events(events, event_count);
    uint8_t mask = 0;
    int16_t segment_start = events[0].x;
    size_t span_count = 0;
    size_t event = 0;
    while (event < event_count) {
        int16_t event_x = events[event].x;
        if (mask != 0 && segment_start < event_x) {
            int16_t x1 = segment_start > clip_x1 ? segment_start : clip_x1;
            int16_t x2 = (int16_t)(event_x - 1);
            if (x2 > clip_x2) x2 = clip_x2;
            if (x1 <= x2 && span_count < LIQUID_GLASS_MAX_ROW_SPANS) {
                spans[span_count++] = (liquid_glass_span_t) {
                    .x1 = x1,
                    .x2 = x2,
                    .coverage_mask = mask,
                };
            }
        }

        do {
            mask ^= events[event].card_bit;
            ++event;
        } while (event < event_count && events[event].x == event_x);
        segment_start = event_x;
    }
    return span_count;
}

void liquid_glass_composite_row(
    const liquid_glass_rgb565_lut_t *lut,
    const liquid_glass_frame_t frames[LIQUID_GLASS_WINDOW_COUNT],
    const uint16_t *wallpaper_row,
    int16_t y,
    int16_t x1,
    int16_t x2,
    uint16_t *output)
{
    if (!lut || !frames || !wallpaper_row || !output || x1 > x2) return;

    size_t width = (size_t)(x2 - x1 + 1);
    if (output != wallpaper_row + x1) {
        memcpy(output, wallpaper_row + x1, width * sizeof(uint16_t));
    }

    liquid_glass_span_t spans[LIQUID_GLASS_MAX_ROW_SPANS];
    size_t count = liquid_glass_coverage_spans(frames, y, x1, x2, spans);
    for (size_t span = 0; span < count; ++span) {
        for (int16_t x = spans[span].x1; x <= spans[span].x2; ++x) {
            size_t output_index = (size_t)(x - x1);
            output[output_index] = liquid_glass_rgb565_lut_apply(
                lut, output[output_index], spans[span].coverage_mask);
        }
    }
}

typedef struct {
    bool valid[LIQUID_GLASS_EDGE_RING_COUNT + 1];
    int16_t left[LIQUID_GLASS_EDGE_RING_COUNT + 1];
    int16_t right[LIQUID_GLASS_EDGE_RING_COUNT + 1];
} edge_row_shape_t;

static uint16_t blend_rgb565(uint16_t foreground, uint16_t background,
                             uint8_t opacity)
{
    uint8_t mix = mix_to_five_bits(opacity);
    uint8_t inverse = (uint8_t)(32u - mix);
    uint8_t red = (uint8_t)((((foreground >> 11) & 0x1Fu) * mix +
                             ((background >> 11) & 0x1Fu) * inverse) >> 5);
    uint8_t green = (uint8_t)((((foreground >> 5) & 0x3Fu) * mix +
                               ((background >> 5) & 0x3Fu) * inverse) >> 5);
    uint8_t blue = (uint8_t)(((foreground & 0x1Fu) * mix +
                              (background & 0x1Fu) * inverse) >> 5);
    return (uint16_t)((uint16_t)red << 11 |
                      (uint16_t)green << 5 | blue);
}

static void blend_span(uint16_t *output,
                       int16_t output_x1,
                       int16_t clip_x2,
                       int16_t left,
                       int16_t right,
                       uint16_t color,
                       uint8_t opacity)
{
    if (!output || opacity == 0) return;
    if (left < output_x1) left = output_x1;
    if (right > clip_x2) right = clip_x2;
    for (int16_t x = left; x <= right; ++x) {
        size_t index = (size_t)(x - output_x1);
        output[index] = blend_rgb565(color, output[index], opacity);
    }
}

void liquid_glass_composite_edges_row(
    const liquid_glass_frame_t frames[LIQUID_GLASS_WINDOW_COUNT],
    const uint8_t draw_order[LIQUID_GLASS_WINDOW_COUNT],
    const liquid_glass_edge_style_t styles[LIQUID_GLASS_WINDOW_COUNT],
    int16_t y,
    int16_t x1,
    int16_t x2,
    uint16_t *output)
{
    if (!frames || !draw_order || !styles || !output || x1 > x2) {
        return;
    }

    edge_row_shape_t shapes[LIQUID_GLASS_WINDOW_COUNT] = { 0 };
    for (uint8_t card = 0; card < LIQUID_GLASS_WINDOW_COUNT; ++card) {
        for (uint8_t inset = 0;
             inset <= LIQUID_GLASS_EDGE_RING_COUNT;
             ++inset) {
            shapes[card].valid[inset] = inset_card_interval(
                &frames[card], y, inset,
                &shapes[card].left[inset], &shapes[card].right[inset]);
        }
    }

    for (uint8_t depth = 0; depth < LIQUID_GLASS_WINDOW_COUNT; ++depth) {
        uint8_t card = draw_order[depth];
        if (card >= LIQUID_GLASS_WINDOW_COUNT) card = depth;
        const liquid_glass_edge_style_t *style = &styles[card];
        for (uint8_t ring = 0;
             ring < LIQUID_GLASS_EDGE_RING_COUNT;
             ++ring) {
            if (style->ring_opacity[ring] == 0 ||
                !shapes[card].valid[ring]) {
                continue;
            }
            if (!shapes[card].valid[ring + 1]) {
                blend_span(output, x1, x2,
                           shapes[card].left[ring],
                           shapes[card].right[ring],
                           style->ring_color[ring],
                           style->ring_opacity[ring]);
            } else {
                blend_span(output, x1, x2,
                           shapes[card].left[ring],
                           (int16_t)(shapes[card].left[ring + 1] - 1),
                           style->ring_color[ring],
                           style->ring_opacity[ring]);
                blend_span(output, x1, x2,
                           (int16_t)(shapes[card].right[ring + 1] + 1),
                           shapes[card].right[ring],
                           style->ring_color[ring],
                           style->ring_opacity[ring]);
            }
        }

        int16_t safe_left = (int16_t)(frames[card].x +
                                      LIQUID_GLASS_CARD_RADIUS);
        int16_t safe_right = (int16_t)(frames[card].x +
                                       frames[card].width - 1 -
                                       LIQUID_GLASS_CARD_RADIUS);
        int16_t safe_width = (int16_t)(safe_right - safe_left);
        if (safe_width <= 0) continue;
        if (style->top_specular_opacity > 0 &&
            y == frames[card].y + 1) {
            blend_span(output, x1, x2, safe_left,
                       (int16_t)(safe_left + safe_width * 42 / 100),
                       style->top_specular_color,
                       style->top_specular_opacity);
        }
        if (style->bottom_refraction_opacity > 0 &&
            y == frames[card].y + frames[card].height - 2) {
            blend_span(output, x1, x2,
                       (int16_t)(safe_right - safe_width * 36 / 100),
                       safe_right, style->bottom_refraction_color,
                       style->bottom_refraction_opacity);
        }
        if (style->glint_opacity > 0 && style->glint_width_percent > 0 &&
            style->glint_progress > -256 &&
            style->glint_progress < 1280 && y == frames[card].y + 2) {
            int16_t center = (int16_t)(safe_left +
                ((int32_t)safe_width * style->glint_progress) / 1024);
            int16_t half = (int16_t)(safe_width *
                                     style->glint_width_percent / 200);
            int16_t glint_left = (int16_t)(center - half);
            int16_t glint_right = (int16_t)(center + half);
            if (glint_left < safe_left) glint_left = safe_left;
            if (glint_right > safe_right) glint_right = safe_right;
            blend_span(output, x1, x2, glint_left, glint_right,
                       style->glint_color, style->glint_opacity);
        }
    }
}

static uint32_t rect_area(const liquid_glass_dirty_rect_t *rect)
{
    return (uint32_t)(rect->x2 - rect->x1 + 1) *
           (uint32_t)(rect->y2 - rect->y1 + 1);
}

static liquid_glass_dirty_rect_t rect_union(
    liquid_glass_dirty_rect_t first,
    liquid_glass_dirty_rect_t second)
{
    if (second.x1 < first.x1) first.x1 = second.x1;
    if (second.y1 < first.y1) first.y1 = second.y1;
    if (second.x2 > first.x2) first.x2 = second.x2;
    if (second.y2 > first.y2) first.y2 = second.y2;
    return first;
}

static void merge_least_costly_pair(
    liquid_glass_dirty_rect_t rects[LIQUID_GLASS_MAX_DIRTY_RECTS],
    size_t *count)
{
    if (*count < 2) return;

    size_t best_first = 0;
    size_t best_second = 1;
    uint32_t best_cost = UINT32_MAX;
    for (size_t first = 0; first + 1 < *count; ++first) {
        for (size_t second = first + 1; second < *count; ++second) {
            liquid_glass_dirty_rect_t joined = rect_union(
                rects[first], rects[second]);
            uint32_t cost = rect_area(&joined) - rect_area(&rects[first]) -
                            rect_area(&rects[second]);
            if (cost < best_cost) {
                best_cost = cost;
                best_first = first;
                best_second = second;
            }
        }
    }

    rects[best_first] = rect_union(rects[best_first], rects[best_second]);
    rects[best_second] = rects[*count - 1];
    --*count;
}

static void add_dirty_run(
    liquid_glass_dirty_rect_t rects[LIQUID_GLASS_MAX_DIRTY_RECTS],
    size_t *count,
    int16_t tile_x1,
    int16_t tile_x2,
    int16_t tile_y)
{
    liquid_glass_dirty_rect_t run = {
        .x1 = (int16_t)(tile_x1 * DIRTY_TILE_SIZE),
        .y1 = (int16_t)(tile_y * DIRTY_TILE_SIZE),
        .x2 = (int16_t)((tile_x2 + 1) * DIRTY_TILE_SIZE - 1),
        .y2 = (int16_t)((tile_y + 1) * DIRTY_TILE_SIZE - 1),
    };

    for (size_t index = 0; index < *count; ++index) {
        if (rects[index].x1 == run.x1 && rects[index].x2 == run.x2 &&
            rects[index].y2 + 1 == run.y1) {
            rects[index].y2 = run.y2;
            return;
        }
    }

    if (*count == LIQUID_GLASS_MAX_DIRTY_RECTS) {
        merge_least_costly_pair(rects, count);
    }
    rects[(*count)++] = run;
}

static void mark_dirty_rect(uint32_t dirty_tiles[DIRTY_TILE_ROWS],
                            liquid_glass_dirty_rect_t rect)
{
    if (rect.x2 < 0 || rect.y2 < 0 ||
        rect.x1 >= LIQUID_GLASS_COMPOSITOR_WIDTH ||
        rect.y1 >= LIQUID_GLASS_COMPOSITOR_HEIGHT) {
        return;
    }
    if (rect.x1 < 0) rect.x1 = 0;
    if (rect.y1 < 0) rect.y1 = 0;
    if (rect.x2 >= LIQUID_GLASS_COMPOSITOR_WIDTH) {
        rect.x2 = LIQUID_GLASS_COMPOSITOR_WIDTH - 1;
    }
    if (rect.y2 >= LIQUID_GLASS_COMPOSITOR_HEIGHT) {
        rect.y2 = LIQUID_GLASS_COMPOSITOR_HEIGHT - 1;
    }
    if (rect.x1 > rect.x2 || rect.y1 > rect.y2) return;

    int16_t tile_x1 = rect.x1 / DIRTY_TILE_SIZE;
    int16_t tile_x2 = rect.x2 / DIRTY_TILE_SIZE;
    uint32_t width = (uint32_t)(tile_x2 - tile_x1 + 1);
    uint32_t mask = width == 32
        ? UINT32_MAX
        : (((uint32_t)1u << width) - 1u) << tile_x1;
    for (int16_t tile_y = rect.y1 / DIRTY_TILE_SIZE;
         tile_y <= rect.y2 / DIRTY_TILE_SIZE; ++tile_y) {
        dirty_tiles[tile_y] |= mask;
    }
}

size_t liquid_glass_plan_visual_dirty_rects(
    const liquid_glass_frame_t previous[LIQUID_GLASS_WINDOW_COUNT],
    const liquid_glass_frame_t next[LIQUID_GLASS_WINDOW_COUNT],
    const liquid_glass_dirty_rect_t *visual_regions,
    size_t visual_region_count,
    liquid_glass_dirty_rect_t rects[LIQUID_GLASS_MAX_DIRTY_RECTS])
{
    if (!previous || !next || !rects ||
        (!visual_regions && visual_region_count > 0)) {
        return 0;
    }

    uint32_t dirty_tiles[DIRTY_TILE_ROWS] = { 0 };
    uint8_t previous_row[LIQUID_GLASS_COMPOSITOR_WIDTH];
    uint8_t next_row[LIQUID_GLASS_COMPOSITOR_WIDTH];
    liquid_glass_span_t spans[LIQUID_GLASS_MAX_ROW_SPANS];
    for (int16_t y = 0; y < LIQUID_GLASS_COMPOSITOR_HEIGHT; ++y) {
        memset(previous_row, 0, sizeof(previous_row));
        memset(next_row, 0, sizeof(next_row));

        size_t count = liquid_glass_coverage_spans(
            previous, y, 0, LIQUID_GLASS_COMPOSITOR_WIDTH - 1, spans);
        for (size_t span = 0; span < count; ++span) {
            memset(previous_row + spans[span].x1,
                   spans[span].coverage_mask,
                   (size_t)(spans[span].x2 - spans[span].x1 + 1));
        }
        count = liquid_glass_coverage_spans(
            next, y, 0, LIQUID_GLASS_COMPOSITOR_WIDTH - 1, spans);
        for (size_t span = 0; span < count; ++span) {
            memset(next_row + spans[span].x1,
                   spans[span].coverage_mask,
                   (size_t)(spans[span].x2 - spans[span].x1 + 1));
        }

        for (int16_t x = 0; x < LIQUID_GLASS_COMPOSITOR_WIDTH; ++x) {
            if (previous_row[x] != next_row[x]) {
                dirty_tiles[y / DIRTY_TILE_SIZE] |=
                    (uint32_t)1u << (x / DIRTY_TILE_SIZE);
            }
        }
    }

    for (size_t region = 0; region < visual_region_count; ++region) {
        mark_dirty_rect(dirty_tiles, visual_regions[region]);
    }

    size_t count = 0;
    for (int16_t tile_y = 0; tile_y < DIRTY_TILE_ROWS; ++tile_y) {
        uint32_t row = dirty_tiles[tile_y];
        int16_t tile_x = 0;
        while (tile_x < DIRTY_TILE_COLUMNS) {
            while (tile_x < DIRTY_TILE_COLUMNS &&
                   (row & ((uint32_t)1u << tile_x)) == 0) {
                ++tile_x;
            }
            if (tile_x >= DIRTY_TILE_COLUMNS) break;
            int16_t start = tile_x;
            while (tile_x + 1 < DIRTY_TILE_COLUMNS &&
                   (row & ((uint32_t)1u << (tile_x + 1))) != 0) {
                ++tile_x;
            }
            add_dirty_run(rects, &count, start, tile_x, tile_y);
            ++tile_x;
        }
    }
    return count;
}

size_t liquid_glass_plan_dirty_rects(
    const liquid_glass_frame_t previous[LIQUID_GLASS_WINDOW_COUNT],
    const liquid_glass_frame_t next[LIQUID_GLASS_WINDOW_COUNT],
    liquid_glass_dirty_rect_t rects[LIQUID_GLASS_MAX_DIRTY_RECTS])
{
    return liquid_glass_plan_visual_dirty_rects(
        previous, next, NULL, 0, rects);
}
