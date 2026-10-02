# Liquid Glass core

This directory contains the platform-independent optics, motion, and RGB565
compositor core from
[`EnglandLobster/ai-passport-liquid-glass-ui`](https://github.com/EnglandLobster/ai-passport-liquid-glass-ui)
at commit `6e9bbd9d402fe9a1a9c58cbb4009210cd0a01ef1`.

The files are imported without the ESP-IDF runtime, device drivers, demo
screens, and assets. kShimC's `glass_renderer` uses the upstream coverage,
optics, color mixing, and nonlinear motion functions while adapting rendering
to a resolution-independent ARGB8888 framebuffer. See `UPSTREAM.json` and
`LICENSE` for provenance and licensing.

The compositor carries one local, backward-compatible extension:
`liquid_glass_coverage_spans_radius()` accepts the corner radius calculated by
the resolution-independent adapter. The original fixed-radius API is retained.
