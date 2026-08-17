# ADR 0257 — Sky3D night mode: deterministic star field, night gradient, moon disc

## Status

Accepted (2026-08-16).

## Context

`Sky3D` (ADR 0090) generates a procedural analytic cubemap: a
zenith/horizon gradient, a sun halo + disc, and a day factor derived
from sun elevation. Below the horizon the entire sky collapsed to one
flat near-black constant `{0.012, 0.014, 0.03}` — no stars, no moon, no
horizon separation. Any game rendering night scenes (Legacy Baseball's
night games are 40% of its schedule) had a featureless void behind the
stadium and effectively no environment image to feed IBL.

## Decision

1. **Night gradient.** The night base becomes a zenith→horizon blend
   (`{0.012, 0.014, 0.03}` → `{0.030, 0.035, 0.060}`) using the same
   horizon weight as the day gradient, so the skyline stays readable.
2. **Moon.** The existing sun-direction splat gains a cool dim moon
   term (`{0.90, 0.95, 1.00}`, disc 2.2×, tight 512-power halo, scaled
   by `1 − day` × 0.30). At night the caller simply points
   `SetSunDirection` at the moon; no new direction API.
3. **Deterministic star field.** New properties `Stars` (bool, default
   off) and `StarIntensity` (f64, clamped [0, 4], default 1.0). When
   enabled, ~600 stars from a hard-coded LCG catalog (three magnitude
   tiers; upper hemisphere) are splatted into the generated faces after
   the gradient pass — catalog-then-splat, so the field is identical at
   every face resolution — faded by `1 − day` so dusk transitions stay
   sane. Bright-tier stars get a faint 4-neighbor cross.
4. The sky remains a pure function of its authored parameters; the
   determinism invariant of ADR 0090 is unchanged. Both properties mark
   the cubemap dirty on change and are validated in `Update`.

## Consequences

- Registered in `src/il/runtime/defs/graphics3d/extras.def`
  (`set_Stars/get_Stars/set_StarIntensity/get_StarIntensity`); C ABI in
  `rt_sky3d.h`.
- `src/tests/fixtures/runtime/test_sky3d_procedural.zia` covers the
  dirty round-trip, night regeneration with stars, the intensity clamp,
  and cross-resolution stability.
- Day-path output is unchanged when stars are disabled (default), and
  at `day == 1` the star fade is zero, so existing day scenes are
  byte-identical.
