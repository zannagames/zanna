# ADR 0332: Authored range for point and spot lights

Status: Accepted and implemented. Date: 2026-09-06.

## Problem and scope

`Light3D.Range` is public but currently ignores point/spot writes. Live shading
ignores their stored ranges; flattened parameters even replace zero with one.
The baker already applies positive punctual range. Cluster bounds use only an
attenuation threshold, and local shadow cameras use `1/sqrt(attenuation)`, which
can clip stadium fixture shadows long before the illuminated field.

Extend the existing property across live shading, clustering and shadow
projection. No new C symbol, dependency, opcode or configuration key is needed.
The per-light property is the explicit authoring control.

## Contract

- Point (type 1) and spot (type 3) lights default to range zero: retain the
  existing distance attenuation with no additional authored cutoff.
- Positive finite range is world-unit distance from the emitter. Multiply live
  punctual attenuation by `t*t*(3-2*t)`, where `t=clamp(1-distance/range,0,1)`.
  Contribution is zero at and beyond range. This matches the existing baker.
- Their setter/getter clamp finite values to `[0, LIGHT3D_PARAM_MAX]`; negative,
  NaN and infinity become zero. Repeated writes of the same normalized value do
  not invalidate retained light caches. Zero restores the legacy curve.
- Area/volume positive-range sanitization is unchanged. Directional/ambient and
  invalid receiver behavior remain unchanged (zero query, ignored write).
- Preserve zero through render-parameter flattening. Metal, D3D11, OpenGL and
  software apply the same fade in PBR and legacy material paths.
- Positive punctual range bounds cluster coverage conservatively; use that full
  sphere, rather than a brightness approximation which might omit its interior.
  Zero-range lights retain the current attenuation-derived bounds.
- Local shadow projections honor positive point/spot range as their far reach,
  retaining existing fallback rules for zero range. Cone and cubemap projection
  logic remain unchanged. Range mutation invalidates normal retained snapshots.
- Baking retains its existing positive-range fade and copied-input behavior.
  Existing punctual decay behavior is outside this change.

## Verification

Given either punctual type, exercise default, positive, zero reset, invalid
numbers, repeated writes and flattened parameters. Given a white receiver,
positive range must dim it inside the range and remove contribution outside;
zero must restore the original image. Verify both PBR and legacy shading,
software and host native GPU, plus compile/check the other shader sources.
Given a small range and distant clusters, omit the light there while retaining
cells inside the range. Verify projection far reach and mutation rebuilds.
Keep existing area/volume, shadow, cluster and baker tests green. Re-run the
Baseball native broadcast gates because its previously ignored mast ranges now
become effective. Actual Windows/Linux GPU execution remains a release gate.

## Implementation evidence

Shared property/flattening/sanitizer code, all four live shader paths, cluster
bounds and local shadow projection are updated. Regression tests cover zero
compatibility and positive cutoffs through shading, baking and shadow reach.
Canonical graphics build passes 163 tests plus audit/smoke stages; native Metal
and software authoring probes pass. Baseball's rebuilt 14-image/42-ball gate
also passes. OpenGL request on this host fell back and was rejected by the probe;
actual OpenGL/D3D11 platform execution remains an explicit acceptance limitation.
