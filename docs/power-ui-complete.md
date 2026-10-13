# Power UI integration: all 29 presets

This supersedes the limited four-family visual-review scope. Power actions no
longer reserve a strip at the physical screen bottom. Every supported design
family has an explicit profile in `menu_power_profile.h`; light/dark variants
share structure but use their own paired color roles. A missing profile fails the
compiled contract and registry tests instead of falling back to a generic bar.

| Presets | Entry within original chrome | Mode menu and confirmation |
| --- | --- | --- |
| FLAT | Small transparent title actions; original backdrop and boot list retained | Anchored compact surface; content-sized confirmation |
| CLASSIC | Framed secondary controls in window action row | Square system window, title rule, outlined rows |
| CARDS | Rounded secondary utility tiles beside primary Boot | Three independent mode tiles; local card confirmation |
| TERMINAL | Bracketed text commands | Numbered fixed-advance command list, square frame, inverse focus |
| MINIMAL | Plain text actions aligned with existing action row | Borderless text menu, no ornamental icons or shadow |
| HIGH_CONTRAST | Explicit labelled, thick-outline controls | Solid surfaces, clear borders and independent focus outline |
| FLUENT / FLUENT_DARK | Rectangular header actions | Square-edged anchored list, restrained confirmation |
| FLUENT2 / FLUENT2_DARK | Small titlebar glyphs | Compact flyout, small-radius confirmation |
| MATERIAL2 / MATERIAL2_DARK | Action-bar glyphs with the bar's foreground | Raised continuous menu, text actions, 4-unit corner radius |
| MATERIAL3 | Tonal secondary action chips | Rounded anchored surface, distinct cancel/execute hierarchy |
| SURFACE / SURFACE_DARK | Detail-column actions on wide screens; header on narrow ones | Detail-area flyout; conventional confirmation |
| CUPERTINO | Secondary utilities framing the startup-disk action row | Centered short dialog with aligned actions |
| IOS_HIG | Blue transparent navigation actions | Bottom action sheet and centered destructive confirmation |
| HARMONYOS | Rounded utility controls, separate from image cards | Rounded icon tiles; restrained centered confirmation |
| CLOVER | Small system tools below boot-media icons | Icon reset-method picker; direct boot on entry activation, no Boot object |
| GLASS | Subtle translucent header controls | Local translucent flyout/highlight rim; reduced-effects fallback |
| AURORA | Tonal rounded utilities in the existing action row | Rounded mode tiles; background remains the visual emphasis |
| SOFT_UI | Same-surface raised utilities | Paired light/dark edges and pressed feedback, not just a flat gradient |
| BENTO | Utility blocks within the card composition | One spanning reset tile plus two smaller tiles, stable ordering |
| NEON | Sharp-corner system controls | Local corner strokes and focus glow, never a full-screen glow effect |
| NORD | Compact neutral framed text actions | Frost focus, Polar Night surfaces, no invented hardware telemetry |
| METRO | Lowercase secondary text in the page | Large type on an opaque page; no rounded floating card |
| ADWAITA | Header-bar symbolic controls | Bordered menu rows and content-sized dialog |
| HOLO | Flat action-bar glyphs | Cyan title rule, continuous separated options, rectangular actions |
| LVGL_DEFAULT | Original panel's action row, native symbol buttons | Native widgets and fonts; only geometry fitted to the content |

These are embedded LVGL interpretations, not claims of native vendor toolkit
conformance. Tiny viewports prioritize short labels and a scrollable mode list;
power menus still have a separately reachable Cancel. Menu samples use synthetic
capabilities, never actual hardware power operations. No unavailable system
settings, battery readings or manufacturer assets are invented.

## Verification contract

`power_menu_test` checks all presets at five viewport sizes: panel-contained
controls, independent boot list, nonoverlap, mode visibility, visible focus,
Cancel-first modal navigation, real pointer cancellation, capability masks,
autoboot cancellation and 49-entry rebuilds. It rejects execution text with
identical foreground and surface colors. `power_preview_test` additionally
checks real-font labels before exporting the four full-resolution scenarios.
The registry and compiled contract cover every family explicitly.

The standard CI matrix retains all 29 host suites and 28 non-FLAT ARM64 builds.
Reduced-effects / explicit-scale host coverage includes Metro, Adwaita, Holo,
LVGL_DEFAULT, Clover, Glass, Surface and iOS. Ordinary power-disabled rendering
retains its baseline comparison; Clover retains its intentional direct-picker
contract rather than being compared with the obsolete separate-Boot layout.

This change is presentation-only: Qualcomm DT parsing, PON/SCM/PS_HOLD sequencing,
touch drivers and payload ownership are unchanged. Host rendering and build
success do not establish physical target size, frame rate, firmware authority,
or successful hardware reboot/shutdown/EDL. Read `power.md` for those boundaries.
