---
status: accepted
audience: contributors
last-verified: 2026-09-09
---

# ADR 0341: Material Temporal History Weight

## Context

A small fast-moving subject can remain blurred by temporal history even
with depth validation. Reducing the entire scene's TAA blend gives up
stability on static rails and netting. Materials need a local control that
does not alter geometry, transparency, lighting, or motion-vector validity.

## Decision

Add `Zanna.Graphics3D.Material3D.TemporalWeight: Float`, backed by
`rt_material3d_set_temporal_weight(void *, double)` and
`rt_material3d_get_temporal_weight(void *) -> double`. Default is 1. Values
are clamped to [0,1]; non-finite values and invalid-handle getters return 1.
Invalid-handle setters are no-ops. Clone/MakeInstance preserve the value and
repair corrupt state. It is runtime presentation state; existing cooked
materials retain their default without a format change.

The weight multiplies the configured TAA history contribution after depth
validation. Zero uses the current surface sample; one retains existing
behavior. It does not change FXAA or motion blur. The deferred draw command
captures the value so later material edits cannot change a queued draw.

GPU backends carry the weight in a dedicated material-uniform scalar and
pack its quantized value into motion-target B alongside motion validity:
`(valid ? 128 : 0) + round(weight * 127)`, divided by 255. Existing validity
tests at B >= 0.5 continue to work. Clear B is 127/255 (static, full weight).
TAA reads the motion payload with a nearest texel fetch and decodes the low
seven bits as weight/127. Motion RG and SSR alpha retain their meanings.
Every mesh/instance path writes the same encoding on Metal, D3D11, OpenGL.

Software retains a matching per-pixel weight plane (0..127, default 127)
with the active scene/target, written by visible depth-writing opaque and
alpha-masked fragments. TAA consumes it alongside depth; absence means full
weight. Blended/additive draws retain their existing exclusion from the
motion target, so they use the underlying depth surface's history policy.
Storage follows the existing frame/target allocation limits and
ownership; target reservations include the extra byte (17 bytes per LDR
texel, 37 per HDR texel), while window mask capacity follows depth capacity; no external dependency or new configuration file is introduced.

## Validation and Integration

Test default, endpoints, clamp, non-finite/invalid receivers, cloning and
deferred snapshot isolation. Validate motion-validity/SSR independence,
software visible-fragment masking, and all backend shader paths. Existing
default-weight scenes retain their behavior. Legacy Baseball opts the
flight/possession baseball and bat materials out of history, with matched
moving captures before accepting the final values. A registered property
alone does not complete this feature; renderer consumption is required.

Registry delta: two functions and one property; no new class or method.
