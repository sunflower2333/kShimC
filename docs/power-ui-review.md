> Historical first-four-family review. Superseded by [all-preset integration](power-ui-complete.md); the earlier limited scope and generic fallback no longer describe current code.

# Power UI design review — first four visual families

This changes presentation, not Qualcomm sequencing. It is a visual-review
candidate on `feature/2610-design-language`, not an accepted replacement for all
29 themes. It covers Fluent 2 (including its dark palette), Material 3, Terminal
and Clover. Other themes deliberately retain the existing implementation until
the four directions are reviewed. No removed theme is restored.

## Entry / selection / confirmation

- **Fluent 2:** two subtle system glyphs in the panel header; a compact restart
  menu anchored to its trigger; a centered, content-sized confirmation. Existing
  boot selection remains the primary action. Keyboard focus is visible and
  modal focus starts on Cancel.
- **Material 3:** tonal secondary operations in the existing panel action row;
  a rounded, upward-opening mode surface; a rounded confirmation with a clear
  secondary Cancel and a specifically labelled execution action. These are not
  full-width controls attached to the physical framebuffer edge.
- **Terminal:** bracketed commands in the terminal action row; an ASCII-numbered
  reset menu with fixed-advance typography, square borders and inverse focus.
  It is not an interactive shell and the displayed command numbers do not
  promise number-key shortcuts. Confirmation retains the same terminal language.
- **Clover:** boot media form row one; small Restart and Power off utility
  glyphs are a centered second row; the selected item's description is below.
  No separate Boot widget exists. Tap or Enter activates the boot entry itself;
  focus changes, scrolling and cancelled touches do not launch an image. Power
  operations retain their distinct safety confirmations. Narrow viewports use
  short utility labels when there is insufficient space for glyphs.

Icons are drawn with LVGL primitives; no proprietary assets or icon-font package
is added. Dimensions are the existing project's design units, not guaranteed
physical dp. Fluent menu guidance and Material components inform the design;
this is an embedded LVGL interpretation, not either vendor's native toolkit.

https://fluent2.microsoft.design/components/web/react/core/menu/usage
https://fluent2.microsoft.design/components/web/react/core/button/usage/
https://m3.material.io/components/menus/guidelines
https://m3.material.io/components/dialogs/guidelines

## Contracts retained

The common menu still owns capabilities, typed requests, input-group isolation,
Cancel-first focus and confirmation. Unsupported modes cannot execute. Opening
an operation cancels automatic boot. Outside dismissal is cancel-only and
cannot activate the underlying boot list. Tool placement may adapt when the
entry count rebuilds the panel; moving selection within a panel does not move
these controls. Reduced-effects mode avoids the optional Fluent menu shadow.

Power-disabled legacy rendering is unchanged except for the explicitly revised
Clover boot/utility/description structure. Device-tree parsing, PMIC writes,
SMC/SCM calls, EDL cookies, shutdown and rollback are not modified by this review.
Neither host tests nor previews perform hardware power operations.

## Review artifacts

Render before and after with the same five-entry `power_preview_test` fixture.
Keep all eight frames per theme (two resolutions; controls, mode menu, EDL
confirmation and shutdown confirmation), plus decoded pixel/file hashes and
source identifiers. An offline HTML viewer displays only those genuine LVGL
frames; its controls do not reboot anything. Inspect original-size frames before
judging text clipping. A passing geometry test does not establish visual quality,
physical touch target size, accessibility, or firmware correctness on hardware.
