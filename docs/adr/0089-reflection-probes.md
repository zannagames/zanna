---
status: active
audience: contributors
last-verified: 2026-08-17
---

# ADR 0089: Local Reflection Probes (Capture Core)

Date: 2026-07-10

## Status

Accepted

## Context

Interiors reflect only the global skybox IBL. The plan called for captured
local probes with parallax-corrected box projection, canvas slots, and SSR
fallback — a large surface whose shader half belongs with the deferred
LIT-batch shader work (alongside the plan-14 GPU lightmap term).

## Decision

Ship the capture core now: `ReflectionProbe3D.New(position, boxMin, boxMax)`
renders six 90-degree faces of a scene from the probe position through an
off-screen `RenderTarget3D` (`Capture(canvas, scene)`, explicit/scripted
only), assembles them with `CubeMap3D.New` face ordering, and retains the
result (`Cubemap`). `InfluenceScale` grows the box for blending;
`Contains(position)` answers influence queries for game-side probe selection;
`CaptureDirty` is the scripted re-capture request flag (plan 16's
time-of-day driver sets it). Games apply probes today through the existing
`Material3D.SetEnvMap` reflection machinery per room.

## Consequences

- Deferred to the follow-up shader batch (recorded): canvas probe slots with
  per-draw two-probe blending, parallax box projection in the four shader
  sources, and SSR-miss fallback. The probe object, capture path, influence
  math, and cubemap output land now and are the stable authoring surface.
- Test: `g3d_test_reflection_probe_capture` (capture succeeds against a
  colored-wall scene, dirty-flag lifecycle, influence containment).
