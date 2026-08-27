---
status: active
audience: contributors
last-verified: 2026-08-27
---

# ADR 0298: Evaluate Procedural Water Through the Morph Pipeline

## Status

Accepted (2026-08-27)

## Context

`Water3D.Update` rewrites every vertex and analytic normal in its grid on every
simulation tick. At the maximum resolution this evaluates more than 65,000
vertices per surface even on Metal, D3D11, and OpenGL, although those backends
already provide vertex-stage morph deformation. Adding a procedural producer to
that animation/render boundary changes the internal runtime C ABI and creates a
new cross-layer dependency, so ADR 0006 requires this decision record.

Every supported water wave is sinusoidal. The identity
`sin(theta - phase) = sin(theta) cos(phase) - cos(theta) sin(phase)` allows each
wave, including its analytic normal derivative, to be represented by two
immutable morph shapes and two time-varying scalar weights.

## Decision

Add the internal constructor
`rt_morphtarget3d_new_packed_internal(vertex_count, shape_count,
position_deltas, normal_deltas)`. It takes ownership only on success and lets
immutable procedural systems share one shape-major payload between GPU upload
and software blending without per-vertex setter calls or a duplicate packed
copy.

Water3D builds a flat grid and two position/normal morph bases per wave only
when its topology, placement, or wave parameters change. Normal updates advance
time and change only the sine/cosine weights. Its draw path uses the established
morph pipeline:

- Metal, D3D11, and OpenGL blend the bases in their existing vertex shaders.
- The software backend uses the existing CPU morph fallback during draw.
- Isolated or reduced builds without MorphTarget3D retain the former direct CPU
  grid deformation.

## Consequences

- GPU-backed water no longer rewrites or invalidates its mesh each tick.
- Wave motion automatically participates in existing previous-weight motion
  history and backend payload caches.
- Maximum-resolution, eight-wave surfaces retain a larger immutable morph
  payload in exchange for eliminating continuous CPU grid work and uploads.
- Water provides vertically expanded conservative bounds and remains excluded
  from CPU occluder writes while deforming.
- No scripting registry, IL, grammar, verifier, serialized format, or external
  dependency changes.

## Alternatives Considered

- **Add water-specific uniforms and shader code to every backend.** Rejected
  because it duplicates deformation upload, motion-history, and software
  fallback machinery already supplied by morph targets.
- **Continue CPU deformation with distance gating.** Rejected because visible
  water still pays the full grid cost and forces dynamic geometry uploads.
- **Use a normal map without analytic normal deformation.** Rejected because it
  changes the existing lighting result for untextured water.
