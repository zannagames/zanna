---
status: accepted
audience: contributors
last-verified: 2026-09-17
---

# ADR 0368: RenderTarget3D.AsDisplayPixels (Display-Referred Offscreen Readback)

## Status

Accepted. Adds one method to `Zanna.Graphics3D.RenderTarget3D`. The
scene-referred `AsPixels` / `CopyTo` contract of
[ADR 0299](0299-native-render-target-sampling.md) and the material-mirror
resolve of [ADR 0301](0301-display-referred-render-target-sampling.md) are
unchanged; this exposes the latter to scripts.

## Context

A script that renders a scene into an offscreen `RenderTarget3D` and reads it back
with `AsPixels` gets the SCENE-REFERRED frame: clamped linear colour for an LDR
target, a bare range-compressed copy for an HDR one, with no tone curve, no
exposure and no display gamma (ADR 0299 §3). That is the right contract for
scopes and for callers that composite linear data, and it is the wrong one for
anything a person looks at: a face texel at linear 0.25 is written as 64/255
where the same scene presented on screen through an ACES chain at exposure 1.10
reads ~170/255, with 8-bit-linear banding through the shadows.

The engine already resolves a target display-referred: when a frame into an
offscreen target ends on a canvas with a usable post chain, the canvas records
that chain on the target (`rt_canvas3d_render_pass.inc`), and the target's
material mirror (`rt_rendertarget3d_material_pixels`) is encoded through it once
per completed frame (ADR 0301) — tone curve + exposure + gamma, colour grade,
LUT, FXAA and sharpen, in chain order; bloom, SSAO, DOF, motion blur, TAA, SSR,
auto-exposure, sun shafts and vignette are skipped. That mirror is only reachable
when the target is sampled as a texture. Legacy Baseball's portrait studio
(plan 126 L6) needs exactly those bytes as a `Pixels` it can save and draw.

## Decision

- `Zanna.Graphics3D.RenderTarget3D.AsDisplayPixels() -> Zanna.Graphics.Pixels`
  (runtime `rt_rendertarget3d_as_display_pixels`, owned result) returns a NEW
  `Pixels` copy of the target's material mirror, i.e. the last completed frame
  resolved through the post-FX chain it was rendered under.
- A target whose last frame ended on a canvas without a usable chain returns the
  same bytes as `AsPixels` (the mirror keeps the historical scene-referred copy).
- The mirror is resolved at most once per completed frame (ADR 0301); repeated
  calls copy the same bytes. `AsPixels` and `CopyTo` stay scene-referred.
- The graphics-disabled stub returns null, like `AsPixels`.

## Consequences

- Offscreen renders can be saved or composited the way the same scene presents,
  on every backend, deterministically (the resolve is the serial CPU path).
- The subset of effects is the ADR 0301 subset: a caller that wants bloom or a
  vignette on the copy adds them on the `Pixels` (or draws the target as a
  texture into a second, presented frame).
- Tests: `test_rt_canvas3d` (the ADR 0301 mirror test asserts the copy equals
  the display-encoded mirror under a tonemap chain and equals `AsPixels` without
  one; null safety). The graphics-disabled stub sits beside `AsPixels` in
  `rt_canvas3d_stubs.c`.
