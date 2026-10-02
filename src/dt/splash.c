/* SPDX-License-Identifier: MIT */
#include <qcom_dt.h>
#include <Library/cr_splash.h>
#include <string.h>
static int offsets(const void *f, int n, const char *name, uint32_t *out, unsigned capacity)
{
    int len;
    const fdt32_t *p = fdt_getprop(f, n, name, &len);
    if (!p || len <= 0 || len % 4 || (unsigned)len / 4 > capacity) return -1;
    for (int i = 0; i < len / 4; ++i) out[i] = fdt32_to_cpu(p[i]);
    return len / 4;
}
int kshim_dt_splash(const void *f, const struct CrIo *io,
                    kshim_framebuffer_config_t *fb, kshim_mmu_region_t *mmio)
{
    if (!fb || !mmio) return -1;
    memset(fb, 0, sizeof(*fb)); memset(mmio, 0, sizeof(*mmio));
    int reserved = fdt_path_offset(f, "/reserved-memory"), node, found = -1;
    struct dt_range memory = {0};
    if (reserved < 0 || !dt_available(f, reserved)) return -1;
    fdt_for_each_subnode(node, f, reserved) {
        const char *label = dt_string(f, node, "label");
        const char *name = fdt_get_name(f, node, NULL);
        if (!dt_available(f, node) ||
            !((label && !strcmp(label, "cont_splash_region")) ||
              (name && !strncmp(name, "cont_splash_region@", 19)))) continue;
        if (found >= 0 || dt_reg(f, node, 0, &memory)) return -1;
        found = node;
    }
    if (found < 0) return -1;
    int sde = -1;
    for (node = fdt_node_offset_by_compatible(f, -1, "qcom,sde-kms"); node >= 0;
         node = fdt_node_offset_by_compatible(f, node, "qcom,sde-kms")) {
        if (!dt_available(f, node)) continue;
        if (sde >= 0) return -1;
        sde = node;
    }
    struct dt_range reg;
    if (sde < 0 || dt_reg_named(f, sde, "mdp_phys", &reg)) return -1;
    struct CrSplashResources resources = {.base = reg.base, .size = reg.size,
        .reserved_base = memory.base, .reserved_size = memory.size};
    int pipes = offsets(f, sde, "qcom,sde-sspp-off", resources.pipe, 8);
    int ctls = offsets(f, sde, "qcom,sde-ctl-off", resources.ctl, 8);
    int count = offsets(f, sde, "qcom,sde-mixer-off", resources.mixer, 6);
    if (pipes < 1 || ctls < 1 || count < 1) return -1;
    resources.pipe_count = pipes; resources.ctl_count = ctls; resources.mixer_count = count;
    unsigned vig = 0, dma = 0;
    for (int i = 0; i < pipes; ++i) {
        int len;
        const char *type = fdt_stringlist_get(f, sde, "qcom,sde-sspp-type", i, &len);
        if (!type) return -1;
        if (len == 3 && !memcmp(type, "vig", 3) && vig < 4) resources.pipe_id[i] = vig++;
        else if (len == 3 && !memcmp(type, "dma", 3) && dma < 4) resources.pipe_id[i] = 4 + dma++;
        else return -1;
    }
    struct CrSplash scanout;
    int status = CrSplashRead(&resources, io, &scanout);
    if (status) return status;
    static const kshim_fb_format_t formats[] = {KSHIM_FB_FORMAT_RGB565, KSHIM_FB_FORMAT_XRGB8888,
        KSHIM_FB_FORMAT_ARGB8888, KSHIM_FB_FORMAT_XBGR8888, KSHIM_FB_FORMAT_ABGR8888};
    *fb = (kshim_framebuffer_config_t){.render_address = scanout.address,
        .width = scanout.width, .height = scanout.height, .stride = scanout.stride,
        .format = formats[scanout.format], .buffer_size = scanout.size,
        .bpp = scanout.format == CR_SPLASH_RGB565 ? 16 : 32,
        .foreground = UINT32_MAX, .background = scanout.format == CR_SPLASH_RGB565 ? 0 : 0xff000000};
    *mmio = (kshim_mmu_region_t){reg.base, reg.size, KSHIM_MMU_DEVICE, KSHIM_MMU_READ};
    return 0;
}
