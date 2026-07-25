---
status: active
audience: contributors
last-verified: 2026-07-25
---

# ADR 0191: GPU-Accelerated Offscreen Editor Rendering

## Status

Accepted (2026-07-25)

## Context

ADR 0168 gave Zanna Studio a windowless `Canvas3D.NewOffscreen(RenderTarget3D)`
whose backend is deliberately pinned to the deterministic software rasterizer:
"Offscreen canvases deliberately use the deterministic software backend."
That choice made editor probes reproducible, but it also makes the editor
viewport the only 3D surface in the product that cannot use the platform GPU
backends (Metal, D3D11, OpenGL) that every game window selects by default.
The Studio 3D viewport therefore renders on one CPU core, is capped well below
pane resolution, and turns orbit/fly/gizmo drags on production scenes
(ashfall-scenes missions) into slideshow interactions. The editor-quality
program (2026-07-25) requires Unity-grade viewport feel; Stephen approved
reversing the software-only rule with a software fallback for probes and CI.

## Decision

### Runtime surface (additive)

- `Zanna.Graphics3D.Canvas3D.NewOffscreenAccelerated(target) -> Canvas3D` —
  construct an offscreen canvas that requests the platform-default GPU
  backend and falls back to software through the existing
  `backend_fallback` / fallback-reason plumbing when no GPU backend can
  initialize. The existing `NewOffscreen(target)` keeps its deterministic
  software behavior unchanged, so every current caller (probes, bakes,
  thumbnails) is untouched.
- `Canvas3D.get_BackendName` already reports the active backend; offscreen
  callers use it plus `get_IsBackendFallback` to surface truth in the UI.
- GPU offscreen frames read back through the existing
  `RenderTarget3D.AsPixels()` contract: after `Draw`, the backend resolves the
  target into CPU pixels. Shared-surface/zero-copy presentation is future
  work and out of scope here.

### Backend contract

A backend advertising offscreen support must render into the bound
`RenderTarget3D` at target resolution with the same pass semantics the
windowed path uses (shadow, main, postfx) and must implement target readback.
Backends that cannot honor readback report unavailability so construction
falls back to software rather than producing blank frames. Determinism is NOT
promised across backends: GPU output may differ from software within normal
rasterization tolerances, which is why probes and goldens stay on the
software constructor.

### Editor contract

- Studio's `SceneEditor3D` requests the accelerated constructor for its
  viewport and the camera-preview inset; on fallback it keeps the current
  truthful `IsViewportRenderFallback` reporting.
- Probes and CI keep constructing the deterministic software path; the
  accelerated constructor is exercised by an opt-in parity smoke (skipped
  headless) that renders a fixture scene on both backends and asserts a
  bounded difference, not byte equality.
- Interim, backend-independent wins land with this ADR: the viewport cap
  rises to the pane size, and interactive drags render at reduced resolution
  with a full-resolution re-render on release.

## Consequences

- The editor viewport can finally use the same GPU path games use, removing
  the single-core ceiling from orbit/fly/drag interactions.
- Editor rendering is no longer bit-deterministic when accelerated; every
  determinism-sensitive consumer (probes, bake previews, goldens) must and
  does stay on the software constructor.
- Readback keeps one copy per frame on the CPU; if profiling shows it
  dominating, a shared-texture presentation path becomes its own ADR.
- Windows/Linux/macOS each exercise their platform backend through one new
  constructor, so cross-platform smoke gains a GPU-offscreen leg.
