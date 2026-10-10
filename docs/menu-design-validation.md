# Design-layer validation notes

The initial integrated revision exposed a root-screen scroll displacement at
small sizes. The follow-up disables root scrolling only for redesigned themes;
entry lists continue to scroll. Exact panel/entry/label bounds and root-scroll
invariants remain in the tests.

The next matrix rendered 24/25 theme configurations. GLASS failed the exact
Boot-region restoration check after its confirmation transform. Its translucent
parent exposes the transformed cached background in partial repaint areas.
The current candidate expands actual pending GLASS damage to the full viewport
at REFR_START, before drawing or partial-buffer strip calculation. This keeps
background sampling and clipping consistent. INVALIDATE_AREA is not rounded:
LVGL also sends that event while finding a strip height that fits the existing
partial render buffers. A pending-request flag prevents repeated idle redraws.

The ordinary LVGL frame timer still schedules drawing; the test does not force a
refresh or relax pixel comparisons. The original partial render buffers and
58,080-byte cache are retained; there is no new full-frame allocation or
per-frame blur. This policy increases redraw work for GLASS and needs device
performance evaluation. Reduced-effects GLASS excludes the cache and these
refresh callbacks.

Use the latest matching CI run as the authority on whether this candidate fixes
the failure. Earlier green jobs and configured matrices are not substitutes for
a fresh successful run. The baseline FLAT frames are also compared independently
against the existing renderer; unchanged source paths alone do not prove pixel
identity. Design-language visual review and target hardware measurement remain
separate acceptance steps.
