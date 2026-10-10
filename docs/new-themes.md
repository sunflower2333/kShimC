# Metro, One UI, Adwaita and Holo

Four additional compile-time presets extend the existing 25 choices. They use
the real pinned LVGL renderer, existing CJK font and shared boot/input state
machine. No native toolkit, proprietary font, OS logo or inactive control is
imported. These are design-language adaptations, not official implementations.

| Choice | Structural identity |
| --- | --- |
| METRO | Top-anchored unframed black page, 44-unit title / 26-unit row type, accent text and drawn selection check, local outlined action |
| ONE_UI | Portrait viewing region above a bottom-anchored selection group and Boot action; more than seven entries expand the list upward; landscape falls back to a regular group |
| ADWAITA | Centered header surface, continuous boxed list, symbolic media icons, neutral separators and independent suggested action |
| HOLO | Dark action-bar surface with cyan bottom rule, flat continuous separated rows, radio marks and restrained rectangular action |

Use one `CONFIG_KSHIM_MENU_STYLE_<NAME>=y`. Existing choices retain their
colors, numeric descriptor values, motion and geometry. FLAT continues to bypass
the new design layer. The editable descriptor generator now reproduces the actual
committed old descriptors, including Terminal's 14-unit status text (the previous
generator had drifted to 13); enum values are preserved and new kinds appended.

`menu_new_themes_render.h` contains presentation-only hooks. One UI's layout is
computed from viewport and entry count, not selected index. Focus movement never
moves the fixed Boot control. All four use opaque surfaces and no new background
cache. Explicit design scale and reduced-effects settings remain available.

## Validation

The CI matrix covers 29 default host builds and 28 non-FLAT ARM64 builds. The
four new themes also run the full host suite at explicit scale 256 with reduced
effects. New pure geometry tests cover ten sizes, five entry counts and five
scales; real LVGL tests verify CJK, selection pixels, long labels, fixed chrome,
49-item rebuilds, independent confirmation and framebuffer guards. Existing
keyboard, touch-cancellation and lifecycle assertions are not removed.

All 25 pre-existing presets render both original resolutions and four stages
again using the previous 5167858 presentation headers. CI compares every PPM
byte to the new build's output. FLAT additionally retains its older renderer
comparison. A workflow configuration is not a passing result: consult the actual
run and `previews/compatibility.txt` artifacts for completed comparisons.

The original seven-entry preview fixture is unchanged. A successful matrix
produces 232 authentic PNGs and an offline `theme-review-html` artifact. Current
artifacts carry their exact checkout SHA; no image is relabelled with another
commit. The old/new gallery's optional baseline mode still requires equal theme
sets: a 25-theme historical matrix cannot pretend to contain the four new themes.

## Boundaries

The reference scale is a design unit, not measured physical DPI. Compact displays
may hide title/status and clip long text to keep the Boot control usable. This
work does not measure device frame rate, startup latency, physical touch-target
size or whole-product accessibility. Native appearance and target performance
remain subject to visual and hardware review.

Reference directions: Microsoft's typography/content-led Windows Phone Metro,
Samsung One UI's viewing/interaction regions, GNOME HIG boxed lists and header
bars, and Android's original Holo theme family. The new preset names do not
imply vendor certification or native API use.
