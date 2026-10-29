/* SPDX-License-Identifier: MIT */
#pragma once

/*
 * Public kernel display API: the scanout surface the kernel discovered (DT
 * splash reservation and live SDE state, or the Kconfig QEMU window), and
 * spare RAM the kernel reserved for UI fonts and textures. Pixel writing
 * is userspace's job (user/include/framebuffer.h).
 */

#include <stddef.h>
#include <stdint.h>

typedef enum kshim_fb_format {
    /* One pixel per bit.  The first pixel in a byte is bit 0 or bit 7. */
    KSHIM_FB_FORMAT_MONO1_LSB = 1,
    KSHIM_FB_FORMAT_MONO1_MSB = 2,
    KSHIM_FB_FORMAT_GRAY8 = 3,
    KSHIM_FB_FORMAT_RGB565 = 4,
    KSHIM_FB_FORMAT_XRGB8888 = 5,
    KSHIM_FB_FORMAT_ARGB8888 = 6,
    KSHIM_FB_FORMAT_XBGR8888 = 7,
    KSHIM_FB_FORMAT_ABGR8888 = 8,
} kshim_fb_format_t;

typedef struct kshim_framebuffer_config {
    /* Physical or virtual address of the first byte of the first row. */
    uintptr_t render_address;
    uint32_t width;
    uint32_t height;
    /* Bytes between two rows.  Zero selects the minimum stride. */
    uint32_t stride;
    /* Bits per pixel.  It must match the selected format. */
    uint32_t bpp;
    kshim_fb_format_t format;
    /* Raw pixel values used by monochrome drawing primitives. */
    uint32_t foreground;
    uint32_t background;
    /* Optional bound for validation; zero means the mapping size is unknown. */
    size_t buffer_size;
} kshim_framebuffer_config_t;

/* Return the required bits per pixel for a format, or zero if unsupported. */
static inline uint32_t kshim_fb_format_bpp(kshim_fb_format_t format)
{
    switch (format) {
    case KSHIM_FB_FORMAT_MONO1_LSB:
    case KSHIM_FB_FORMAT_MONO1_MSB:
        return 1;
    case KSHIM_FB_FORMAT_GRAY8:
        return 8;
    case KSHIM_FB_FORMAT_RGB565:
        return 16;
    case KSHIM_FB_FORMAT_XRGB8888:
    case KSHIM_FB_FORMAT_ARGB8888:
    case KSHIM_FB_FORMAT_XBGR8888:
    case KSHIM_FB_FORMAT_ABGR8888:
        return 32;
    default:
        return 0;
    }
}

/* Copy the scanout description into *config. Returns 0 when a surface is
 * available (non-zero render address), -1 otherwise. */
int kshim_display_get(kshim_framebuffer_config_t *config);

/* Spare RAM outside the runtime slot for UI fonts and textures; zero size
 * when none was reserved. */
void kshim_display_scratch(uint64_t *base, uint64_t *size);
