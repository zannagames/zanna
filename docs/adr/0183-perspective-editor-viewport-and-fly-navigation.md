---
status: active
audience: contributors
last-verified: 2026-07-24
---

# ADR 0183: Add a Perspective Editor Viewport with Fly Navigation

## Status

Accepted (2026-07-24)

## Context

Studio's 3D viewport is orthographic-only. That was the right v1: the
retained overlay drawing (origin markers, hierarchy links, selection, gizmo
axes, rotation rings, plane handles) was written against an orthographic
projection whose exact alignment with the windowless Canvas3D render is a
pinned invariant. But level authoring at Ashfall scale needs a Unity-style
perspective view with fly navigation — depth perception, walking the space,
judging sightlines — and the runtime already has everything required:
`Camera3D` is mode-switchable (`IsOrtho`, `OrthoSize`, `Fov`) and exposes
`WorldToScreen` and `ScreenToRay`/`ScreenToRayOrigin`, so one retained camera
object can drive the render, every overlay, and picking in either projection.

## Decision

The 3D editor's retained viewport camera becomes projection-switchable.
Perspective is the default for new sessions; orthographic remains one toggle
away. Mode, camera pose, and the chrome states below are per-scene workspace
state that persists with the owning tab and session and never touches VSCN.

**One-projection rule:** every overlay and picking computation goes through
the shared retained camera — `WorldToScreen` for drawing, `ScreenToRay` for
picking and drags. No overlay may carry its own projection math. The existing
exact-alignment invariant between the shaded render, grid, markers, gizmos,
and hit tests now holds in both modes, and the ortho probes' exactness
assertions are re-pinned in perspective.

Navigation:

- **Fly:** holding right-mouse captures the pointer for mouse-look; WASD
  moves in the camera basis, Q/E move world-down/up, Shift is a fast
  multiplier. Speed is bounded and scroll-adjustable while flying.
- **Orbit/pan:** existing Shift+middle/right-drag pan is retained; orbit
  stays pointer-driven around the focus point.
- **Dolly-to-cursor:** the wheel dollies toward/away from the cursor ray
  (perspective) or zooms about the cursor (ortho).
- **F** frames the selection in both modes (existing Frame Selected math
  routed through the shared camera).
- Escape or releasing the captured button ends fly mode; keyboard capture
  never leaks into inspector text inputs (same focus rules the W/E/R tool
  shortcuts already follow).

Streamlined chrome, all workspace-only:

- A **perspective ground grid** on the world XZ plane, distance-faded,
  drawn through the shared projection (the ortho grid is unchanged).
- A **snap settings popover** exposing the move/rotate/scale snap increments
  the gizmos already honor.
- **Overlay visibility toggles** (markers, hierarchy links, colliders,
  routes, cameras, lights, grid) grouped in the view toolbar.
- A **viewport stats readout** (node/visible/culled counts from the live
  SceneGraph properties) toggleable in the same group.

Performance contract: the viewport repaints only when the camera, scene,
selection, or overlay state changes; an idle viewport performs no per-frame
render work. Continuous fly motion repaints per frame while active and stops
the instant input ends.

## Consequences

- Authors can fly Ashfall-scale levels with depth perception while every
  editing affordance (selection, gizmos, rings, plane handles, marquee)
  behaves identically to ortho mode.
- The overlay audit removes duplicated projection math, shrinking the editor
  and making future overlays (colliders, routes, frusta) one code path.
- A focused probe must pin: perspective pick/gizmo exactness (the same
  assertions the ortho probes make), mode and pose session round-trip,
  fly-capture focus safety, and idle-repaint neutrality.

## Alternatives Considered

- **A second, separate perspective camera object.** Rejected: two cameras
  invite drift between render and picking; `set_IsOrtho` on one retained
  camera keeps a single source of truth.
- **Perspective-only (drop ortho).** Rejected: precise axis-aligned layout
  work is strictly better orthographic, and the mode costs one toggle.
- **Editor-side projection matrices instead of Camera3D helpers.** Rejected:
  `WorldToScreen`/`ScreenToRay` are the exact transforms the renderer uses;
  reimplementing them in Zia is the drift the one-projection rule forbids.
