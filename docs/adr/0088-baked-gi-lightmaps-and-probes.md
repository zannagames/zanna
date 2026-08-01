---
status: active
audience: contributors
last-verified: 2026-07-11
---

# ADR 0088: Baked GI — LightBaker3D and LightProbeGrid3D

Date: 2026-07-10

## Status

Accepted

## Context

Ambient lighting was flat or skybox-IBL only: interiors glowed uniformly or
needed forests of point lights. The plan called for offline-baked global
illumination — a from-scratch CPU lightmap baker plus an SH-9 probe grid —
with strict determinism and zero dependencies.

## Decision

- **`LightBaker3D.New(scene)`** gathers `SceneNode.SetStatic(true)` mesh nodes
  (new bake-hint flag), builds a median-split BVH over world-space triangles
  (via the plan-12 raw mesh readback accessors), and path-traces per-texel
  irradiance: explicit direct lighting with shadow rays per registered light
  (`AddLight` — explicit and deterministic rather than implicitly scraping
  canvas slots), cosine-weighted bounces (`Bounces`, default 2) with albedo
  modulation and emissive pickup, and a `SetSkyColor` escape term. Sampling
  uses per-texel seeded LCGs — the same scene and options bake byte-identical
  atlases. Geometric normals orient to authored vertex normals so winding
  conventions can never flip a bake into darkness.
- **Charts:** per-triangle rectangles on a shelf-packed 1024 atlas, texel
  density from `TexelsPerUnit`; chart corners write TEXCOORD_1 directly into
  the source meshes; a one-ring dilation pass guards bleed. Radiance stores
  with 2x headroom (texel 255 = 2.0).
- **`Apply()`** installs the atlas on each baked node through
  `Material3D.MakeInstance` + the new **`Material3D.SetLightmap`** slot, so
  shared materials stay clean.
- **Shading:** the software rasterizer (the correctness baseline) replaces the
  flat-ambient term with `lightmap x albedo x ao` for lightmapped draws,
  sampling raw TEXCOORD_1. Scenes without lightmaps render bit-identically.
- **`LightProbeGrid3D.New(min, max, spacing)`** bakes SH-9 RGB per probe with
  the same tracer/BVH (uniform-sphere radiance projection), marks probes
  buried inside geometry invalid (close backface heuristic) and in-fills from
  neighbors, samples by trilinear interpolation + cosine-convolved SH
  evaluation (`Sample(position, normal)` → Vec3), and round-trips through
  versioned little-endian `.vlpg` files.

## Consequences (v1 boundaries, recorded deliberately)

- **GPU lightmap sampling is deferred:** lightmapped rendering is
  SW-reference in v1; GPU backends currently keep the flat-ambient term for
  lightmapped draws. The term joins the next shader-batch pass alongside the
  probe per-draw ambient hook (which needs per-command ambient plumbing
  across four backends).
- `.vlm` atlas serialization is deferred; `Apply` is in-session and `Atlas`
  exposes the Pixels for games that persist it through existing image paths.
  `.vlpg` (the runtime-critical half) ships now.
- The atlas is one 1024 page; all charts are capacity-checked before sampling,
  and overflow fails the complete bake without publishing a partial atlas or
  chart UVs (density and scene size are the knobs).
- Tests: `test_rt_lightbaker3d` — direct bake lights the atlas and Apply
  installs lightmap instances; a red wall skews bounced floor light toward
  red vs a white wall and identical bakes hash identically; probe gradients
  follow light falloff and `.vlpg` round-trips sample-identically.
