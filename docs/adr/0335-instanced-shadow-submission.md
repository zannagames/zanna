# ADR 0335: Instanced shadow submission

Status: Implemented; native Metal/software verified, native OpenGL/D3D11 acceptance pending

## Context

Legacy Baseball's repeated stadium geometry batches in the main pass but emits
thousands of separate shadow draws. Even explicit instance batches expand to
one backend call per instance. Measured classic shadow encoding costs exceed
30 ms with two spot banks and 60 ms with four after frustum culling.

## Decision

Add an optional private backend `shadow_draw_instanced(ctx, cmd, matrices, count)`
hook. Matrices are borrowed row-major 4×4 transforms, consumed/copied during the
call; no ownership escapes without a backend copy. Geometry/material are shared.
GPU backends implement static instanced depth drawing with the same alpha-mask,
UV, sidedness, bias and compact-geometry behavior as individual shadow draws.
Software and unavailable hooks retain per-mesh expansion. Deformed/particle
payloads keep the established path; no pose approximation is allowed.

The shared renderer groups compatible stable mesh casters by normalized command
state (excluding current/previous model transforms and motion-only state).
A stable hash/index sort is only a grouping aid; exact normalized command
comparison prevents hash collisions from merging different state. Sorting occurs
once in the frame eligibility list; each light's existing conservative culling
retains that order. Cache signatures still consume individual eligible casters.
Matrix scratch is reused and released with the canvas. Allocation failure retains
individual submission. Main-pass ordering, light power and shadow selection do
not change. No public runtime registration, IL, save or simulation change.

Telemetry counts an instanced shadow dispatch as one draw and N instances.
Native acceptance must demonstrate fewer stadium shadow calls with preserved
images/poses and no dropped work. Required coverage includes compatible/non-
compatible materials, interleaved geometry, explicit instances, fallback and
alpha masks. All backend implementations and platform checks are required;
macOS execution alone does not prove native Windows/Linux acceptance.

Explicit instance batches must not discard camera-invisible potential casters
before shadow processing. With shadows enabled and ShadowMode other than None,
retain their complete transform list (including camera-relative submission).
This conservative shared list may submit extra clipped main-view instances;
separate per-view/per-light instance visibility remains a performance follow-up.
The no-shadow / explicit ShadowMode.None path retains camera culling.

## Validation

Shared GPU-path regressions pass, including interleaved alpha-compatible groups,
explicit off-camera instances and deformation fallback. Native Metal/software
opaque and alpha-mask reference comparisons preserve every sampled code value;
Metal reduces three depth submissions to two. The full native baseball image
and ball gate passes. Stadium measurements and the still-failing 16-request /
12-tile coverage case are recorded in
`baseball/analysis/plan96/shadow-batches-14/README.md`. This implementation does
not establish the commercial frame-time SLA or native Windows/Linux acceptance.
