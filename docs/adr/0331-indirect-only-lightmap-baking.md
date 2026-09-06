# ADR 0331: Indirect-only lightmap baking

Status: Accepted and implemented. Date: 2026-09-05.

## Summary and scope

Legacy Baseball needs the same explicit fixture lights in live rendering and
baked GI. The baker currently includes direct illumination at a lightmap's
primary surface. Runtime materials use lightmaps as an ambient/GI replacement
and still evaluate analytic direct lights, so using that atlas alongside live
fixtures counts direct illumination twice.

Add an opt-in indirect-only lightmap mode. No shader, IL opcode, serialization,
dependency, or platform-specific change is required. Existing bake behavior is
preserved by default. This does not fix stale lightmaps after time/roof changes,
which remains the application's responsibility.

## Configuration and exact contract

- `Zanna.Graphics3D.LightBaker3D.IncludeDirect`: Boolean, default `true`.
- C accessors `rt_lightbaker3d_set_include_direct(void *, int8_t)` and
  `rt_lightbaker3d_get_include_direct(void *) -> int8_t`.
- When false, omit copied-light direct illumination **only at the lightmap's
  primary surface**. Preserve sky, emissive hits, and direct lighting reached
  through recursive bounces. Zero bounces therefore produces black lightmaps.
- Probe-grid baking is unchanged. Its rays gather radiance from surrounding
  surfaces; direct light at those surfaces is already indirect light at the
  receiving probe and must remain present.
- The option freezes with other baker inputs once scene gathering starts.
  Later writes are ignored, matching `Bounces` and `Samples`. C Boolean input
  normalizes nonzero to true. Invalid receivers use existing checked-handle
  behavior with exact diagnostics `LightBaker3D.set_IncludeDirect: invalid baker`
  and `LightBaker3D.get_IncludeDirect: invalid baker`.
- No feature flag beyond this explicit per-baker property; no environment keys.

## Verification

Given a lit floor and zero bounces, default mode produces lit texels and false
produces black texels. Given a sky and one bounce, false retains sky energy.
Given reflective geometry and explicit lights, false retains bounced fixture
energy. Given a probe grid, toggling the lightmap option leaves sampled probe
irradiance unchanged. Given an in-progress bake, later writes cannot change the
snapshot. Existing deterministic, direct-light, transactional and probe tests
must remain green. Check Zia property binding in VM and native execution.

## References

- `render/rt_lightbaker3d.c`: `baker_radiance`, lightmap sample tasks and probe rays.
- `backend/vgfx3d_backend_sw_raster.inc`: lightmap replaces ambient; analytic
  lights are still added. GPU material contracts likewise label it baked GI.
- `test_rt_lightbaker3d.cpp`: direct bake, color bleeding and frozen inputs.
- Existing `Samples` and `Bounces` properties provide binding and freeze patterns.

Subtracting direct energy after integration would add cancellation error and
waste primary-surface shadow rays. Removing live direct lights would break
dynamic players and moving objects. The explicit primary-surface switch avoids
both and keeps older standalone full-light bake consumers compatible.

## Implementation evidence

Implemented in the shared C baker and runtime registry. New unit coverage checks
primary-light exclusion, sky and fixture-bounce retention, frozen inputs,
Boolean normalization, receiver validation, and unchanged probe irradiance.
Public property binding passes in VM and native execution. The initial canonical
2027-test run exposed only the expected stale generated-reference and ABI-manifest
records; both were regenerated/reviewed. The four affected tests and a subsequent
canonical contract build pass, with platform lint and host cross-platform smoke
checks clean. Baseball retains logs in `analysis/plan96/environment-02/` in its
private repository. Windows/Linux native execution remains release validation
work; this change adds no platform-specific code.
