/* SPDX-License-Identifier: MIT */
/*
 * LVGL stage of the QEMU SMP self-test (CONFIG_KSHIM_SMP_SELFTEST): render
 * through the LVGL OS layer on the PSCI workers and check that every draw
 * unit ran and shut down cleanly. Called by the kernel self-test through
 * kshim_user_smp_selftest() while the workers are online.
 */
#include <kshim/user.h>

#include <config.h>
#ifndef CONFIG_KSHIM_SMP_SELFTEST
#define CONFIG_KSHIM_SMP_SELFTEST 0
#endif
#ifndef KSHIM_LVGL_TEST_FORCE_DELETE_FAILURE
#define KSHIM_LVGL_TEST_FORCE_DELETE_FAILURE 0
#endif

#if CONFIG_KSHIM_SMP_SELFTEST
#include <kshim_lvgl_os.h>
#include <lvgl.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SELFTEST_LVGL_WIDTH 64U
#define SELFTEST_LVGL_HEIGHT 48U
#define SELFTEST_LVGL_DRAW_UNITS (LV_DRAW_SW_DRAW_UNIT_CNT - 1U)

typedef struct {
    size_t pixels;
    uint32_t flushes;
    uint16_t first_pixel;
    uint8_t have_first;
    uint8_t varied;
} lvgl_flush_context_t;

static uint16_t mLvglBuffer[SELFTEST_LVGL_WIDTH * SELFTEST_LVGL_HEIGHT]
    __attribute__((aligned(8)));

static size_t bit_count(uint32_t value)
{
    size_t count = 0U;

    while (value != 0U) {
        count += value & 1U;
        value >>= 1;
    }
    return count;
}

static void lvgl_flush(
    lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    lvgl_flush_context_t *context = lv_display_get_user_data(display);
    int32_t width;
    int32_t height;
    uint32_t stride;

    if (context == NULL || area == NULL || pixels == NULL) {
        lv_display_flush_ready(display);
        return;
    }
    width = area->x2 - area->x1 + 1;
    height = area->y2 - area->y1 + 1;
    if (width <= 0 || height <= 0) {
        lv_display_flush_ready(display);
        return;
    }
    stride = lv_draw_buf_width_to_stride(
        (uint32_t)width, LV_COLOR_FORMAT_RGB565);
    for (int32_t y = 0; y < height; y++) {
        const uint16_t *row = (const uint16_t *)(pixels + (size_t)y * stride);

        for (int32_t x = 0; x < width; x++) {
            uint16_t pixel = row[x];

            if (context->have_first == 0U) {
                context->first_pixel = pixel;
                context->have_first = 1U;
            }
            else if (pixel != context->first_pixel) {
                context->varied = 1U;
            }
            context->pixels++;
        }
    }
    context->flushes++;
    lv_display_flush_ready(display);
}

int kshim_user_smp_selftest(size_t worker_count)
{
    lvgl_flush_context_t flush = {0};
    lv_display_t *display = NULL;
    lv_obj_t *screen;
    lv_obj_t *box;
    size_t expected = worker_count;
    size_t active = 0U;
    size_t peak = 0U;
    uint32_t mask = 0U;
    int failure = 0;

    if (expected > SELFTEST_LVGL_DRAW_UNITS)
        expected = SELFTEST_LVGL_DRAW_UNITS;
    if (kshim_lvgl_os_reset_stats() != 0)
        return -120;

    memset(mLvglBuffer, 0x5a, sizeof(mLvglBuffer));
    lv_init();
    active = kshim_lvgl_os_active_threads();
    peak = kshim_lvgl_os_peak_threads();
    mask = kshim_lvgl_os_worker_mask();
    if (active != expected || peak != expected || bit_count(mask) != expected)
        failure = -121;

    if (failure == 0) {
        display = lv_display_create(
            (int32_t)SELFTEST_LVGL_WIDTH, (int32_t)SELFTEST_LVGL_HEIGHT);
        if (display == NULL)
            failure = -122;
    }
    if (failure == 0) {
        lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
        lv_display_set_buffers(
            display, mLvglBuffer, NULL, sizeof(mLvglBuffer),
            LV_DISPLAY_RENDER_MODE_FULL);
        lv_display_set_user_data(display, &flush);
        lv_display_set_flush_cb(display, lvgl_flush);

        screen = lv_screen_active();
        box = screen != NULL ? lv_obj_create(screen) : NULL;
        if (screen == NULL || box == NULL) {
            failure = -123;
        }
        else {
            lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
            lv_obj_remove_style_all(box);
            lv_obj_set_size(box, 24, 16);
            lv_obj_center(box);
            lv_obj_set_style_bg_color(box, lv_color_white(), 0);
            lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
            lv_obj_invalidate(screen);
            lv_refr_now(display);
            if (flush.flushes == 0U ||
                flush.pixels < SELFTEST_LVGL_WIDTH * SELFTEST_LVGL_HEIGHT ||
                flush.varied == 0U)
                failure = -124;
        }
    }

#if KSHIM_LVGL_TEST_FORCE_DELETE_FAILURE
    printf("LVGL_FAILSTOP: delete failure armed\n");
#endif
    lv_deinit();
#if KSHIM_LVGL_TEST_FORCE_DELETE_FAILURE
    printf("LVGL_FAILSTOP: ERROR deinit returned\n");
#endif
    if ((kshim_lvgl_os_active_threads() != 0U ||
         kshim_lvgl_os_worker_mask() != 0U ||
         kshim_lvgl_os_prepare_shutdown() != 0) && failure == 0)
        failure = -125;
    printf("LVGL_SMP: expected=%u active=%u peak=%u mask=0x%x "
           "flushes=%u pixels=%u varied=%u shutdown=%s\n",
           (unsigned)expected, (unsigned)active, (unsigned)peak, mask,
           flush.flushes, (unsigned)flush.pixels, flush.varied,
           kshim_lvgl_os_prepare_shutdown() == 0 ? "clean" : "dirty");
    return failure;
}
#endif
