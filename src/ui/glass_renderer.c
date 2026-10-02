#include <glass_renderer.h>

#include <limits.h>

#include "liquid_glass_compositor_core.h"
#include "ui_glass_motion.h"
#include "ui_glass_optics.h"

#define KSHIM_GLASS_TEXTURE_SHORT_EDGE 384U
#define KSHIM_GLASS_WAVE_PERIOD 1024U

static uint32_t min_u32(uint32_t first, uint32_t second)
{
    return first < second ? first : second;
}

static uint32_t max_u32(uint32_t first, uint32_t second)
{
    return first > second ? first : second;
}

static int32_t abs_i32(int32_t value)
{
    return value < 0 ? -value : value;
}

static uint8_t smooth_weight(int32_t distance, int32_t radius)
{
    if (distance >= radius || radius <= 0)
        return 0;
    if (distance < 0)
        distance = -distance;
    int32_t progress = ((radius - distance) * UI_GLASS_MOTION_PROGRESS_MAX) /
                       radius;
    int32_t eased = ui_glass_ease_in_out_cubic(progress);
    return (uint8_t)((eased * 255 + UI_GLASS_MOTION_PROGRESS_MAX / 2) /
                     UI_GLASS_MOTION_PROGRESS_MAX);
}

/* Smooth 0..1023 periodic wave returned as -512..512. */
static int32_t flowing_wave(int32_t phase)
{
    uint32_t wrapped = (uint32_t)phase & (KSHIM_GLASS_WAVE_PERIOD - 1U);
    uint32_t triangle = wrapped < 512U ? wrapped * 2U :
                        (1024U - wrapped) * 2U;
    int32_t eased = ui_glass_ease_in_out_cubic((int32_t)triangle);
    return eased - 512;
}

static uint32_t background_pixel(const kshim_glass_layout_t *layout,
                                 int32_t x, int32_t y, uint32_t time_ms,
                                 bool dark)
{
    const uint32_t dark_top = 0x07111FU;
    const uint32_t dark_bottom = 0x21162FU;
    const uint32_t light_top = 0xEAF8FCU;
    const uint32_t light_bottom = 0xF2EAF8U;
    const uint32_t accent_cyan = dark ? 0x168DA9U : 0x8DDDE1U;
    const uint32_t accent_mint = dark ? 0x1F8C78U : 0xA6E3C9U;
    const uint32_t accent_lilac = dark ? 0x7154B4U : 0xCDB8EEU;
    const uint32_t accent_warm = dark ? 0xA45F57U : 0xF3B6A8U;
    int32_t width = (int32_t)layout->texture_width;
    int32_t height = (int32_t)layout->texture_height;
    int32_t nx;
    int32_t ny;
    uint32_t color;
    uint8_t weight;

    if (x < 0)
        x = 0;
    else if (x >= width)
        x = width - 1;
    if (y < 0)
        y = 0;
    else if (y >= height)
        y = height - 1;
    nx = width > 1 ? x * 1024 / (width - 1) : 0;
    ny = height > 1 ? y * 1024 / (height - 1) : 0;
    color = ui_glass_mix_rgb(dark ? dark_top : light_top,
                             dark ? dark_bottom : light_bottom,
                             (uint8_t)(ny * 255 / 1024));

    /* Broad connected ribbons keep the background flowing without discrete
     * decorative blobs. All phase arithmetic is deterministic and integer. */
    int32_t center = 250 + flowing_wave(nx * 2 + (int32_t)(time_ms / 7U)) / 3;
    weight = smooth_weight(abs_i32(ny - center), 310);
    color = ui_glass_mix_rgb(color, accent_cyan,
                             (uint8_t)((uint16_t)weight * (dark ? 105U : 92U) /
                                       255U));

    center = 760 + flowing_wave(ny * 2 - (int32_t)(time_ms / 9U)) / 3;
    weight = smooth_weight(abs_i32(nx - center), 280);
    color = ui_glass_mix_rgb(color, accent_lilac,
                             (uint8_t)((uint16_t)weight * (dark ? 112U : 86U) /
                                       255U));

    int32_t diagonal = (nx + ny / 2 + (int32_t)(time_ms / 11U)) & 1023;
    weight = smooth_weight(abs_i32(diagonal - 512), 220);
    color = ui_glass_mix_rgb(color, accent_mint,
                             (uint8_t)((uint16_t)weight * 82U / 255U));

    int32_t warm = (ny - nx / 3 - (int32_t)(time_ms / 17U)) & 1023;
    weight = smooth_weight(abs_i32(warm - 512), 150);
    return ui_glass_mix_rgb(color, accent_warm,
                            (uint8_t)((uint16_t)weight * (dark ? 50U : 45U) /
                                      255U));
}

static uint32_t blur_background(const kshim_glass_layout_t *layout,
                                int32_t x, int32_t y, int32_t refraction_x,
                                int32_t refraction_y, uint32_t time_ms,
                                bool dark, bool reduced_quality)
{
    static const int8_t offsets[][2] = {
        {0, 0}, {-3, 0}, {3, 0}, {0, -3}, {0, 3},
        {-2, -2}, {2, -2}, {-2, 2}, {2, 2},
    };
    uint32_t red = 0;
    uint32_t green = 0;
    uint32_t blue = 0;
    size_t count = reduced_quality ? 5U :
                   sizeof(offsets) / sizeof(offsets[0]);

    for (size_t index = 0; index < count; index++) {
        uint32_t sample = background_pixel(
            layout, x + refraction_x + offsets[index][0],
            y + refraction_y + offsets[index][1], time_ms, dark);
        red += (sample >> 16) & 0xffU;
        green += (sample >> 8) & 0xffU;
        blue += sample & 0xffU;
    }
    return ((red / count) << 16) | ((green / count) << 8) |
           (blue / count);
}

int kshim_glass_layout(uint32_t screen_width, uint32_t screen_height,
                       size_t max_texture_pixels,
                       kshim_glass_layout_t *layout)
{
    return kshim_glass_layout_for_entries(screen_width, screen_height,
                                          max_texture_pixels, 3U, layout);
}

int kshim_glass_layout_for_entries(uint32_t screen_width,
                                   uint32_t screen_height,
                                   size_t max_texture_pixels,
                                   size_t entry_count,
                                   kshim_glass_layout_t *layout)
{
    uint32_t short_edge;
    uint32_t texture_width;
    uint32_t texture_height;
    uint32_t panel_height;

    if (layout == NULL || screen_width < 64U || screen_height < 48U ||
        screen_width > INT32_MAX || screen_height > INT32_MAX ||
        max_texture_pixels < 64U * 48U || entry_count == 0U)
        return -1;

    short_edge = min_u32(screen_width, screen_height);
    uint32_t texture_short = min_u32(short_edge,
                                     KSHIM_GLASS_TEXTURE_SHORT_EDGE);
    if (screen_width <= screen_height) {
        texture_width = texture_short;
        texture_height = (uint32_t)(((uint64_t)screen_height * texture_short +
                                     screen_width / 2U) / screen_width);
    } else {
        texture_height = texture_short;
        texture_width = (uint32_t)(((uint64_t)screen_width * texture_short +
                                    screen_height / 2U) / screen_height);
    }
    while ((uint64_t)texture_width * texture_height > max_texture_pixels &&
           texture_short > 64U) {
        texture_short--;
        if (screen_width <= screen_height) {
            texture_width = texture_short;
            texture_height = (uint32_t)(((uint64_t)screen_height * texture_short +
                                         screen_width / 2U) / screen_width);
        } else {
            texture_height = texture_short;
            texture_width = (uint32_t)(((uint64_t)screen_width * texture_short +
                                        screen_height / 2U) / screen_height);
        }
    }
    if (texture_width == 0U || texture_height == 0U ||
        texture_width > UINT16_MAX || texture_height > UINT16_MAX ||
        (uint64_t)texture_width * texture_height > max_texture_pixels)
        return -2;

    uint32_t panel_width = short_edge * 4U / 5U;
    uint32_t padding = short_edge / 28U;
    if (padding < 2U)
        padding = 2U;
    else if (padding > 36U)
        padding = 36U;

    /* Keep the frame compact for the common two-entry UEFI menu while still
     * capping long manifests to a scrollable panel.  These line metrics match
     * the LVGL chrome in lvgl_port.c and are deliberately integer-only. */
    uint32_t title_height = short_edge >= 720U ? 58U :
                            short_edge >= 320U ? 34U : 24U;
    uint32_t status_height = short_edge >= 160U ? 17U : 0U;
    uint32_t row_height = short_edge >= 720U ? 72U :
                          short_edge >= 320U ? 52U : 24U;
    uint32_t row_gap = max_u32(2U, padding / 4U);
    uint32_t boot_height = short_edge / 11U;
    boot_height = max_u32(16U, min_u32(boot_height, 88U));
    size_t visible_count = entry_count > 49U ? 49U : entry_count;
    uint64_t rows = (uint64_t)row_height * visible_count;
    if (visible_count > 1U)
        rows += (uint64_t)row_gap * (visible_count - 1U);
    uint64_t fixed = (uint64_t)padding + title_height +
                    (status_height != 0U ? status_height + padding / 3U : 0U) +
                    padding + boot_height + (uint64_t)padding * 2U;
    uint64_t natural_height = fixed + rows;
    uint32_t panel_cap = screen_height * 7U / 10U;
    if (screen_width > screen_height)
        panel_cap = short_edge * 7U / 10U;
    panel_height = (uint32_t)min_u32(
        (uint32_t)min_u32(natural_height > UINT32_MAX ? UINT32_MAX :
                          (uint32_t)natural_height, panel_cap),
        UINT32_MAX);
    uint32_t minimum_height = fixed + row_height;
    if (panel_height < minimum_height)
        panel_height = minimum_height;
    if (panel_height > screen_height)
        panel_height = screen_height;
    panel_width = max_u32(panel_width, min_u32(screen_width, 56U));
    panel_height = max_u32(panel_height, min_u32(screen_height, 44U));

    *layout = (kshim_glass_layout_t){
        .screen_width = screen_width,
        .screen_height = screen_height,
        .texture_width = (uint16_t)texture_width,
        .texture_height = (uint16_t)texture_height,
        .panel_x = (int32_t)((screen_width - panel_width) / 2U),
        .panel_y = (int32_t)((screen_height - panel_height) / 2U),
        .panel_width = panel_width,
        .panel_height = panel_height,
        .panel_radius = max_u32(8U, short_edge * 3U / 100U),
    };
    return 0;
}

int kshim_glass_render(uint32_t *pixels, size_t capacity_pixels,
                       const kshim_glass_layout_t *layout,
                       uint32_t time_ms, bool dark, bool reduced_quality)
{
    liquid_glass_frame_t frames[LIQUID_GLASS_WINDOW_COUNT] = {{0}};
    ui_glass_optics_t optics;
    uint32_t width;
    uint32_t height;
    uint32_t tint;
    uint32_t edge_color;

    if (pixels == NULL || layout == NULL || layout->texture_width == 0U ||
        layout->texture_height == 0U)
        return -1;
    width = layout->texture_width;
    height = layout->texture_height;
    if ((uint64_t)width * height > capacity_pixels)
        return -2;

    for (uint32_t y = 0; y < height; y++)
        for (uint32_t x = 0; x < width; x++)
            pixels[(size_t)y * width + x] = 0xff000000U |
                background_pixel(layout, (int32_t)x, (int32_t)y,
                                 time_ms, dark);

    frames[0] = (liquid_glass_frame_t){
        .x = (int16_t)((int64_t)layout->panel_x * width /
                       layout->screen_width),
        .y = (int16_t)((int64_t)layout->panel_y * height /
                       layout->screen_height),
        .width = (int16_t)((uint64_t)layout->panel_width * width /
                          layout->screen_width),
        .height = (int16_t)((uint64_t)layout->panel_height * height /
                           layout->screen_height),
        .surface_opacity = 92,
        .border_opacity = 102,
        .content_opacity = 255,
    };
    if (frames[0].width <= 0 || frames[0].height <= 0)
        return -3;

    optics = ui_glass_optics_for_material(UI_GLASS_MATERIAL_CLEAR);
    /* The LVGL content already supplies the panel's material fill.  Keep the
     * rasterized rim subtle so the window reads as soft glass instead of a
     * one-pixel white rectangle after texture upscaling. */
    optics.edge_strength = 46U;
    optics.ring_opacity[0] = 38U;
    optics.ring_opacity[1] = 17U;
    optics.ring_opacity[2] = 5U;
    optics.top_specular_opacity = 24U;
    optics.bottom_refraction_opacity = 24U;
    optics.glint_opacity = 82U;
    optics.glint_width_percent = 26U;
    tint = dark ? 0x20374CU : 0xF2FBFFU;
    edge_color = dark ? 0xBCEEFFU : 0xFFFFFFU;
    int32_t right = frames[0].x + frames[0].width - 1;
    int32_t bottom = frames[0].y + frames[0].height - 1;
    int32_t radius = (int32_t)((uint64_t)layout->panel_radius * width /
                               layout->screen_width);
    if (radius < 2)
        radius = 2;
    int32_t glint = ui_glass_glint_center(
        (int32_t)((time_ms * 3U) % 1536U) - 256,
        frames[0].x, (int16_t)right);

    for (int32_t y = frames[0].y; y <= bottom; y++) {
        liquid_glass_span_t spans[LIQUID_GLASS_MAX_ROW_SPANS];
        size_t span_count = liquid_glass_coverage_spans_radius(
            frames, (int16_t)y, 0, (int16_t)(width - 1U),
            (int16_t)radius, spans);
        for (size_t span_index = 0; span_index < span_count; span_index++) {
            if ((spans[span_index].coverage_mask & 1U) == 0U)
                continue;
            for (int32_t x = spans[span_index].x1;
                 x <= spans[span_index].x2; x++) {
                int32_t left_distance = x - frames[0].x;
                int32_t right_distance = right - x;
                int32_t top_distance = y - frames[0].y;
                int32_t bottom_distance = bottom - y;
                int32_t edge_distance = left_distance;
                int32_t refraction_x = 0;
                int32_t refraction_y = 0;
                if (right_distance < edge_distance)
                    edge_distance = right_distance;
                if (top_distance < edge_distance)
                    edge_distance = top_distance;
                if (bottom_distance < edge_distance)
                    edge_distance = bottom_distance;
                if (edge_distance < 9) {
                    int32_t strength = 9 - edge_distance;
                    if (left_distance == edge_distance)
                        refraction_x = strength;
                    else if (right_distance == edge_distance)
                        refraction_x = -strength;
                    if (top_distance == edge_distance)
                        refraction_y = strength;
                    else if (bottom_distance == edge_distance)
                        refraction_y = -strength;
                }

                uint32_t color = blur_background(
                    layout, x, y, refraction_x, refraction_y, time_ms,
                    dark, reduced_quality);
                color = ui_glass_mix_rgb(
                    color, tint,
                    dark ? (uint8_t)(optics.fill_opacity * 2U / 3U) :
                           (uint8_t)(optics.fill_opacity * 3U / 4U));
                if (edge_distance < 3) {
                    uint8_t opacity = ui_glass_scale_opacity(
                        optics.ring_opacity[(uint32_t)edge_distance],
                        optics.edge_strength);
                    color = ui_glass_mix_rgb(color, edge_color, opacity);
                }
                if (top_distance < 7) {
                    uint8_t opacity = (uint8_t)(
                        (7 - top_distance) * optics.top_specular_opacity / 7U);
                    color = ui_glass_mix_rgb(color, 0xFFFFFFU, opacity);
                }
                if (bottom_distance < 8) {
                    uint8_t opacity = (uint8_t)(
                        (8 - bottom_distance) *
                        optics.bottom_refraction_opacity / 8U);
                    color = ui_glass_mix_rgb(
                        color, ui_glass_background_at_y((int16_t)(
                                   y * 319 / (height > 1U ? height - 1U : 1U))),
                        opacity);
                }
                int32_t glint_distance = abs_i32(x - glint);
                int32_t glint_width = frames[0].width *
                    optics.glint_width_percent / 100;
                if (glint_distance < glint_width && top_distance < 5) {
                    uint8_t opacity = smooth_weight(glint_distance,
                                                    glint_width);
                    opacity = (uint8_t)((uint16_t)opacity *
                                        optics.glint_opacity / 255U);
                    color = ui_glass_mix_rgb(color, 0xFFFFFFU, opacity);
                }
                pixels[(size_t)y * width + (size_t)x] =
                    0xff000000U | color;
            }
        }
    }
    return 0;
}
