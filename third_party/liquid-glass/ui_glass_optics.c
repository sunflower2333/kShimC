#include "ui_glass_optics.h"

// Average colors from a narrow center strip of the embedded wallpaper. The
// edge renderer interpolates this tiny LUT instead of retaining or reading a
// framebuffer, so a lens rim can still borrow plausible displaced color.
static const uint32_t WALLPAPER_CENTER_SAMPLES[] = {
    0xB0CBD7u, 0xB3CDD8u, 0xA2C9D8u, 0x3F8DC4u, 0x033580u,
    0x011345u, 0x010E2Eu, 0x01102Eu, 0x011537u, 0x011B3Eu,
    0x082240u, 0x12233Bu, 0x0F1A2Bu, 0x0A1223u, 0x0A1325u,
    0x09182Fu, 0x060D1Eu, 0x050B1Bu, 0x09294Bu, 0x042B53u,
};

ui_glass_optics_t ui_glass_optics_for_material(ui_glass_material_t material)
{
    static const ui_glass_optics_t profiles[] = {
        [UI_GLASS_MATERIAL_REGULAR] = {
            .fill_opacity = 158,
            .stack_fill_opacity = 67,
            .edge_strength = 86,
            .ring_opacity = { 72, 26, 7 },
            .top_specular_opacity = 76,
            .bottom_refraction_opacity = 34,
            .glint_opacity = 218,
            .glint_width_percent = 28,
        },
        [UI_GLASS_MATERIAL_CLEAR] = {
            .fill_opacity = 92,
            .stack_fill_opacity = 52,
            .edge_strength = 102,
            .ring_opacity = { 92, 34, 9 },
            .top_specular_opacity = 88,
            .bottom_refraction_opacity = 42,
            .glint_opacity = 236,
            .glint_width_percent = 34,
        },
        [UI_GLASS_MATERIAL_CONTRAST] = {
            .fill_opacity = 210,
            .stack_fill_opacity = 84,
            .edge_strength = 76,
            .ring_opacity = { 60, 20, 6 },
            .top_specular_opacity = 64,
            .bottom_refraction_opacity = 30,
            .glint_opacity = 184,
            .glint_width_percent = 24,
        },
        [UI_GLASS_MATERIAL_CHROME] = {
            .fill_opacity = 30,
            .stack_fill_opacity = 30,
            .edge_strength = 30,
            .ring_opacity = { 46, 16, 4 },
            .top_specular_opacity = 48,
            .bottom_refraction_opacity = 22,
            .glint_opacity = 0,
            .glint_width_percent = 0,
        },
    };

    if (material < UI_GLASS_MATERIAL_REGULAR ||
        material > UI_GLASS_MATERIAL_CHROME) {
        material = UI_GLASS_MATERIAL_REGULAR;
    }
    return profiles[material];
}

uint8_t ui_glass_scale_opacity(uint8_t opacity, uint8_t edge_strength)
{
    uint16_t scaled = ((uint16_t)opacity * edge_strength + 43u) / 86u;
    return (uint8_t)(scaled > 255u ? 255u : scaled);
}

uint32_t ui_glass_mix_rgb(uint32_t first, uint32_t second, uint8_t mix)
{
    uint16_t inverse = (uint16_t)(255u - mix);
    uint8_t red = (uint8_t)((((first >> 16) & 0xFFu) * inverse +
                             ((second >> 16) & 0xFFu) * mix + 127u) / 255u);
    uint8_t green = (uint8_t)((((first >> 8) & 0xFFu) * inverse +
                               ((second >> 8) & 0xFFu) * mix + 127u) / 255u);
    uint8_t blue = (uint8_t)(((first & 0xFFu) * inverse +
                              (second & 0xFFu) * mix + 127u) / 255u);
    return ((uint32_t)red << 16) | ((uint32_t)green << 8) | blue;
}

uint32_t ui_glass_background_at_y(int16_t y)
{
    if (y < 0) y = 0;
    if (y > 319) y = 319;

    const uint32_t last =
        (sizeof(WALLPAPER_CENTER_SAMPLES) /
         sizeof(WALLPAPER_CENTER_SAMPLES[0])) - 1u;
    uint32_t position = ((uint32_t)y * last * 256u) / 319u;
    uint32_t index = position >> 8;
    if (index >= last) return WALLPAPER_CENTER_SAMPLES[last];

    return ui_glass_mix_rgb(WALLPAPER_CENTER_SAMPLES[index],
                            WALLPAPER_CENTER_SAMPLES[index + 1u],
                            (uint8_t)(position & 0xFFu));
}

int16_t ui_glass_glint_center(int32_t progress, int16_t left, int16_t right)
{
    int32_t width = (int32_t)right - left;
    return (int16_t)(left + (width * progress) / UI_GLASS_ANIM_PROGRESS_MAX);
}
