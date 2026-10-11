# Additional presets and native LVGL

The supported additions are METRO, ADWAITA, HOLO and LVGL_DEFAULT. They use the
pinned LVGL renderer and shared boot/input state machine. No proprietary font,
OS logo or inactive control is imported.

| Choice | Structural identity |
| --- | --- |
| METRO | Top-anchored unframed black page, large title and row type, accent text, drawn selection check and local outlined action |
| ADWAITA | Centered header surface, continuous boxed list, symbolic media icons, neutral separators and independent suggested action |
| HOLO | Dark action bar with cyan bottom rule, flat separated rows, radio marks and rectangular action |
| LVGL_DEFAULT | Actual LVGL default theme with native list/buttons, focus and pressed feedback |

Select one `CONFIG_KSHIM_MENU_STYLE_<NAME>=y`. The former `ONE_UI` option and its
implementation have been removed, without a replacement alias. Regenerate older
configurations through Kconfig; when no supported choice is selected, the normal
FLAT default applies. Surface, iOS HIG, FLAT and LVGL_DEFAULT remain available.
Numeric design kind 22 stays unused so other kind IDs do not move.

`menu_new_themes_render.h` contains only presentation hooks for Metro, Adwaita
and Holo. These use opaque surfaces and no additional background cache. Selection,
physical keys, pointer cancellation and the independent Boot action stay shared.

## Validation and previews

The CI matrix covers 29 default host configurations and 28 non-FLAT ARM64 builds.
Metro, Adwaita, Holo and native LVGL also run the full host suite at explicit scale
256 with reduced effects. Pure geometry tests retain the viewport, entry-count,
scale and label-reservation checks. Real LVGL tests cover CJK, visible selection,
long labels, fixed chrome, 49-entry rebuilding and independent confirmation.

All 29 remaining themes, including native LVGL, compare both resolutions and all
four full-frame samples with `a154e1af2eb65514b15b8f30bb067fb934b6fcad`. FLAT also
retains its earlier renderer comparison. Only the retired theme's dedicated
assertions are removed; the remaining input/lifecycle and native-style checks
remain active. Registry tests keep Kconfig, generator, host choices, CI matrices
and HTML previews synchronized.

A successful run produces 232 original PNGs and `theme-review-html`. Images keep
their rendered checkout SHA. Old downloaded HTML files are immutable snapshots;
regenerate the gallery to remove the retired entry. Configuring a job is not
proof it passed: consult the run and retained compatibility/test logs.

## Native LVGL

`LVGL_DEFAULT` enables the actual default theme in the pinned LVGL library and
uses native container, list, list-button and button classes. It bypasses the
custom palette/drawing/layout and transform-animation paths. Geometry uses the
native flex layout; no copied palette, radius, border or shadow is installed.
The group selection activates the native FOCUS_KEY style, including for pointer
selection, rather than drawing a replacement highlight. The default Latin font
is Montserrat 14; a copy retains its glyphs and metrics and adds the existing CJK
font only as a missing-glyph fallback. With no scratch memory it uses the built-in
font alone. Native UI intentionally does not use the custom design-scale option.
At tiny framebuffer sizes, only container/Boot padding and height are reduced to
keep the input surface usable; native state colors/radius/borders remain intact.

Native preview entry and ready frames are identical because no custom entrance
animation is installed. Its confirmation sample uses a real pointer press and
native pressed feedback; it releases after sampling and verifies that only the
release confirms. Other presets retain the original preview input sequence.

The runtime native tests compare actual widget properties with newly-created,
untouched LVGL widgets across resting, focused and pressed states, exercise
2/49/2 entry rebuilding, CJK fallback, Boot position and confirmation. Existing
physical-key, touch-cancel, batched-tap, framebuffer-format, teardown and long-label
tests also run. Native behavior is checked explicitly instead of requiring a
custom animation that this preset intentionally does not have.

## Boundaries

Design scale is not measured physical DPI. Small displays may clip long text or
hide title/status to retain usable input. Host tests and screenshots do not
measure device frame rate, startup latency or physical touch-target size.
Metro, Adwaita and Holo are adaptations, not native platform toolkits.
