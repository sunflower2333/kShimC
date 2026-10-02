#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct kshim_glass_layout {
    uint32_t screen_width;
    uint32_t screen_height;
    uint16_t texture_width;
    uint16_t texture_height;
    int32_t panel_x;
    int32_t panel_y;
    uint32_t panel_width;
    uint32_t panel_height;
    uint32_t panel_radius;
} kshim_glass_layout_t;

/* Pick a bounded texture size and the centered glass panel geometry. */
int kshim_glass_layout(uint32_t screen_width, uint32_t screen_height,
                       size_t max_texture_pixels,
                       kshim_glass_layout_t *layout);

/* Pick the same geometry for a menu with a known number of rows.  The
 * compatibility entry point above uses the normal three-row menu. */
int kshim_glass_layout_for_entries(uint32_t screen_width,
                                   uint32_t screen_height,
                                   size_t max_texture_pixels,
                                   size_t entry_count,
                                   kshim_glass_layout_t *layout);

/* Render one opaque 0xAARRGGBB keyframe. The panel samples a displaced,
 * multi-tap blur of the animated background rather than using a flat tint. */
int kshim_glass_render(uint32_t *pixels, size_t capacity_pixels,
                       const kshim_glass_layout_t *layout,
                       uint32_t time_ms, bool dark, bool reduced_quality);
