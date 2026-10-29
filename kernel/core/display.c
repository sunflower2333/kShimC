/* SPDX-License-Identifier: MIT */
/* Kernel display services behind kshim/display.h. */
#include <kshim/display.h>
#include <qcom_dt.h>
#include <config.h>

int kshim_display_get(kshim_framebuffer_config_t *config)
{
    struct kshim_resources *r = kshim_resources_get();
#if CONFIG_KSHIM_QEMU_PLATFORM
    /* Explicit virtual test surface; physical platforms require scanout evidence. */
    r->framebuffer = (kshim_framebuffer_config_t){.render_address = CONFIG_KSHIM_FB_RENDER_ADDRESS,
        .width = CONFIG_KSHIM_FB_WIDTH, .height = CONFIG_KSHIM_FB_HEIGHT,
        .stride = CONFIG_KSHIM_FB_STRIDE, .format = CONFIG_KSHIM_FB_FORMAT,
        .bpp = kshim_fb_format_bpp(CONFIG_KSHIM_FB_FORMAT), .foreground = UINT32_MAX};
#endif
    *config = r->framebuffer;
    return config->render_address ? 0 : -1;
}

void kshim_display_scratch(uint64_t *base, uint64_t *size)
{
    struct kshim_resources *r = kshim_resources_get();
    *base = r->ui_scratch.base;
    *size = r->ui_scratch.size;
}
