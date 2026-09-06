# ADR 0333: Preserve authored local-light attenuation

Status: Accepted; implemented. Date: 2026-09-06.

## Problem

The default attenuation coefficient `0.001` is also enforced as a lower bound.
An explicitly authored coefficient `0.0001` therefore changes silently, moving
the inverse-quadratic half-intensity distance from 100 world units to about
31.6. Stadium lighting written in feet cannot express its intended falloff.
GPU area-light shaders repeat the clamp even when software accepts a smaller
positive coefficient. Importers also clamp range-derived coefficients.

## Contract

Separate the fallback coefficient from the numerical minimum:

- `RT_LIGHT3D_DEFAULT_ATTENUATION` remains `0.001` for zero, negative and
  nonfinite input. Default constructors and invalid-input behavior stay stable.
- `RT_LIGHT3D_MIN_ATTENUATION` is `1e-12`. Finite positive coefficients clamp
  to `[1e-12, 1e6]`. This retains a positive float-representable coefficient and
  prevents accidental infinite/no-falloff lights without replacing valid art.
- Point, spot and area constructors/accessors use this normalization. Repeated
  normalized writes remain mutation no-ops. Global lights remain unaffected.
- Live GPU area-light decay accepts positive coefficients like the software
  reference. Zero/invalid fallback remains `0.001`; native parameter sanitizers
  already keep shader inputs finite. Punctual and baker equations are unchanged.
- glTF and FBX range-to-attenuation conversion uses the numerical minimum,
  retaining the existing `1/range²` mapping and invalid-range fallback. FBX's
  existing explicit range storage is unchanged. glTF cutoff import is separate
  existing behavior and is not silently altered by this coefficient correction.
- No new public C symbol, opcode, dependency, feature flag or environment key.
  Positive finite `Range` from ADR 0332 bounds wide fixture coverage explicitly.

## Verification and game integration

Tests require exact small coefficients through constructors/setters/getters,
invalid fallback, numerical limits, mutation caching and import conversion.
Compare rendered small-coefficient and default-coefficient emitters in software
and host-native GPU paths, including area lights. Keep existing invalid-input,
import, shadow, cluster and bake tests green. Run the canonical graphics build
and required platform checks; retain honest limitations for unavailable GPUs.

For Legacy Baseball, first capture unchanged rig values with corrected falloff.
Then isolate fixture-only illumination before calibrating power and any broad
fill. Preserve exposure and visibility gates; do not compensate by simply
brightening the whole image or lowering the acceptance bands. Record chosen
values and limitations in Plan 96's execution log with unedited captures.

## Discovered cluster-bound correction

The native 100-unit fixture probe found that cluster binning clamps a light's
radius to the camera far plane **before** testing its sphere against the camera.
A light behind the camera can still illuminate the visible receiver, but this
clamp shrinks its actual sphere and drops it. Remove the radius clamp; intersect
the true sphere, then clamp resulting cluster indices normally. Existing finite
radius validation and conservative projection fallback stay in force. Regression:
an emitter at view depth -120 with radius 150 must cover a receiver at depth 10
under a 100-unit far plane. Native fixture probes then verify real contribution.


The canonical macOS graphics build passes 163 tests and its audit/smoke stages.
Native Metal and software probes verify the coefficient and rendered reach for
point, spot, rectangle and sphere lights. Platform policy lint and host smoke
pass. Native D3D11/OpenGL and Windows/Linux execution remain unverified.
Game fixture calibration and visual acceptance are tracked independently in
Baseball Plan 96, increment 6.
