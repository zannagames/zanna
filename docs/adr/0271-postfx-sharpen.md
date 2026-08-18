---
status: active
audience: contributors
last-verified: 2026-08-18
---

# ADR 0271: PostFX3D.AddSharpen — Clamped Unsharp-Mask Pass

## Status

Accepted (2026-08-18).

## Context

The engine has no sharpening operator anywhere, while three separate
mechanisms soften the live image: the simplified FXAA (a box-blur mix on
every detected edge), TAA's 0.9 history weight, and the adaptive
render-scale ladder's bilinear upscale of 0.85/0.75-scale frames. Color
grading cannot recover acuity; the "vibrant clarity" ask (Legacy Baseball
plan 61) needs an edge-contrast pass that every backend implements
identically.

## Decision

New chain effect `Zanna.Graphics3D.PostFX3D.AddSharpen(f64 amount)`
(`rt_postfx3d_add_sharpen`), kind `PostFXEffectKind.Sharpen`:

1. **Math (identical on CPU and all three GPU backends).** Per channel:
   `out = c + amount * (c - avg4(N, S, E, W))`, then clamped to the local
   min/max of the five taps. The clamp suppresses ringing halos and leaves
   hard steps and flat regions untouched — only soft (blurred) edges
   steepen, which is exactly the FXAA/TAA/upscale damage profile. `amount`
   sanitizes to [0, 1]; border pixels pass through.
2. **Placement.** An ordered chain entry like every other effect;
   display-referred when authored after the tonemap (the intended
   position, after the AA pass).
3. **Enum/snapshot append.** `POSTFX_SHARPEN` /
   `VGFX3D_POSTFX_EFFECT_SHARPEN` append after SUN_SHAFTS (backends switch
   on raw values — append-safe); the backend snapshot appends
   `sharpen_enabled` / `sharpen_amount`; the shared sanitizer and both
   chain-usability range checks extend to the new kind.
4. **CPU reference** uses the FXAA copy-out band contract
   (order-independent, thread-count-invariant).

## Consequences

- ABI manifest re-reviewed: functions 2253 → 2256, properties 828 → 829,
  methods 1210 → 1212 (with ADR 0272's Material3D.SetTextureFilters), hash
  0x8d6129899e05da62; runtime reference docs regenerated.
- Tests: `test_rt_postfx3d_cpu` pins the soft-edge-steepening /
  no-undershoot behavior; the three backend shared suites pin the clamped
  unsharp-mask source in each shader.

## Links

- `src/runtime/graphics/3d/render/rt_postfx3d.{c,h}`
- `src/il/runtime/defs/graphics3d/lighting.def`
- ADR 0270 — PostFX snapshot COLOR_LUT payload (same landing family)
