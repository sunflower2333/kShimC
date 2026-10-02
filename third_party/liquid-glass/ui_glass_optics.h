#pragma once

#include <stdint.h>

#define UI_GLASS_OPTIC_RING_COUNT 3
#define UI_GLASS_ANIM_PROGRESS_MAX 1024

typedef enum {
    UI_GLASS_MATERIAL_REGULAR = 0,
    UI_GLASS_MATERIAL_CLEAR,
    UI_GLASS_MATERIAL_CONTRAST,
    UI_GLASS_MATERIAL_CHROME,
} ui_glass_material_t;

typedef struct {
    uint8_t fill_opacity;
    // Per-card fill used when three equal material layers can overlap. This is
    // precompensated so the combined stack remains legible without assigning
    // different transmittance to front, middle, and back cards.
    uint8_t stack_fill_opacity;
    uint8_t edge_strength;
    uint8_t ring_opacity[UI_GLASS_OPTIC_RING_COUNT];
    uint8_t top_specular_opacity;
    uint8_t bottom_refraction_opacity;
    uint8_t glint_opacity;
    uint8_t glint_width_percent;
} ui_glass_optics_t;

// Returns the precomputed optical profile for a material. The profile is small
// enough to stay in flash and avoids blur kernels or per-pixel shader state.
ui_glass_optics_t ui_glass_optics_for_material(ui_glass_material_t material);

// Scales an optical opacity by the animated edge strength. A strength of 86 is
// the nominal front-window value; larger values are used only for brief pulses.
uint8_t ui_glass_scale_opacity(uint8_t opacity, uint8_t edge_strength);

// Integer RGB mixing and background sampling keep edge colors deterministic and
// independently host-testable. mix=0 returns first, mix=255 returns second.
uint32_t ui_glass_mix_rgb(uint32_t first, uint32_t second, uint8_t mix);
uint32_t ui_glass_background_at_y(int16_t y);

// Maps an animation progress to a horizontal coordinate. Values outside
// 0..1024 intentionally place the glint beyond the visible edge.
int16_t ui_glass_glint_center(int32_t progress, int16_t left, int16_t right);
