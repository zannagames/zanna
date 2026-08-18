---
status: active
audience: contributors
last-verified: 2026-08-17
---

# ADR 0213: Render Runtime-Backed Water Preview Layers

## Status

Accepted (2026-07-27)

## Context

ADR 0212 lets a project compose independently matched preview prefabs beside a
canonical 3D scene. That is appropriate for ordinary runtime-built scenery,
but a static mesh is not an accurate substitute for a procedural surface.
Ashfall mission 05 constructs a `Water3D` pool from canonical root metadata,
binds the production water texture and normal map, and registers two Gerstner
waves. Its first Studio preview had the correct footprint but remained a flat,
uniform plane.

The editor also submitted the canonical scene and disposable project-preview
graph through separate implicit `Canvas3D` frames. Each implicit frame resets
the deferred draw queue and depth state. That prevents transparent project
surfaces from sorting and depth-testing with canonical geometry exactly as
they do in a game, even when both images appear plausible in isolation.

Studio must improve this fidelity without executing project startup code,
adding project-specific keys to the product, making procedural presentation
canonical, or introducing a dependency.

## Decision

### Component-schema version 15

`scene-components.json` version 15 adds an optional
`scenePreview3D.waterLayers` array containing one through eight definitions.
Each definition has:

- the same required exact `matchProperty` and Boolean, integer, or string
  `matchValue` contract as an additive environment layer;
- optional static `positionX`, `positionY`, and `positionZ` values, each of
  which may be replaced by a configured float metadata property;
- a positive static `width` and `depth` (default one), each of which may be
  replaced by a configured float metadata property;
- optional positive `widthMultiplier` and `depthMultiplier`;
- an optional complete normalized `colorR/G/B/A` group;
- optional safe project-relative `texture` and `normalMap` raster paths;
- a bounded grid `resolution` from 8 through 64;
- an optional `animate` flag; and
- zero through eight complete wave records containing finite `dirX`, `dirZ`,
  `speed`, nonnegative `amplitude`, and positive `wavelength` values.

Configured position/size properties must exist as float metadata. Resolved
width and depth must remain finite and positive after multiplication. Missing,
wrong-typed, nonpositive, unreadable, oversized, or undecodable inputs omit
that layer and publish truthful preview status. Match metadata that is absent,
wrong-typed, or unequal simply leaves the layer inactive.

Schemas older than version 15 that contain `waterLayers`, empty or oversized
arrays, incomplete color or wave groups, unsafe paths, fractional integer
matches, invalid property keys, out-of-range numbers, or orphan multipliers
reject atomically.

### Runtime-backed disposable surfaces

For each matched definition, Studio creates the existing public runtime
`Water3D`, resolves its dimensions and position, applies the optional color,
texture, normal map, resolution, and waves, and advances it once at zero time
so the initial frame already contains the true generated mesh and normals.
The surface is retained only by the active scene editor. It does not enter the
canonical `SceneGraph`, hierarchy, picking, selection, VSCN serialization,
revision, dirty state, or history.

Animated layers advance at most 30 times per second while their document is
active. Elapsed time is capped before reaching `Water3D.Update`, preventing a
long suspended editor interval from producing an unbounded simulation step.
Each definition is capped at a 64-by-64 grid and eight waves; at most eight
water layers may be retained.

### One composited 3D frame

The Studio viewport explicitly opens one `Canvas3D.Begin`/`End` frame, submits
the canonical scene, the disposable project-preview scene, and runtime-backed
water surfaces, then closes and finalizes that frame once. Shaded, wireframe,
and shaded-plus-wire submissions snapshot their intended mode inside the same
deferred queue.

This makes opaque and transparent sorting, depth testing, sky, fog, lighting,
post-processing, and frame statistics operate on the complete authored view.
It also fulfills ADR 0212's existing same-depth-buffer contract rather than
merely drawing several independently finalized images onto one target.

### Ashfall

Ashfall maps `af.wantWater` to one runtime water layer. `af.watX/Y/Z` supplies
its center; `af.watW/D` supplies half extents multiplied by two to match the
game's `Water3D.New(w * 2, d * 2)` call. The project preview generator writes
the exact 256-pixel production water texture and concrete normal map from the
same in-tree `TextureLib`; the schema registers the same two wave records used
by `world/level_base.zia`.

The old flat `environment-water.scene3d` proxy is removed.

## Consequences

- Authored water has the same runtime mesh class, wave geometry, material
  inputs, footprint, camera, depth, atmosphere, and post-processing as the
  game.
- Canonical and project-derived geometry now occlude and blend in one render
  frame, improving every existing project preview rather than only water.
- Version 15 is declarative and portable. Other projects can preview water
  without importing Ashfall code or teaching Studio their metadata vocabulary.
- Versions 1–14 and projects without water layers preserve their schema and
  canonical behavior.
- The feature reuses the existing public graphics runtime. It adds no runtime C
  ABI, IL, VSCN, dependency, or platform-adapter change.
- Probes must cover schema bounds and version gates, authoring preservation,
  exact Ashfall dimensions/assets/waves, animated pixel changes, one-frame
  composition, and canonical content/history isolation.

## Alternatives Considered

- **Keep the flat prefab.** Rejected because correct bounds alone do not satisfy
  the requested scene-versus-game visual fidelity.
- **Serialize `Water3D` into VSCN now.** Deferred because canonical procedural
  water needs a broader component/file-format authoring decision; this preview
  contract does not force a game to serialize a runtime-owned effect.
- **Execute the game's water builder in Studio.** Rejected because arbitrary
  project execution is not a safe, bounded authoring contract.
- **Bake one deformed water mesh.** Rejected because it loses the production
  material and animation path and drifts as soon as wave parameters change.
- **Render water in a second implicit frame.** Rejected because it cannot sort
  or depth-test truthfully with the canonical and prefab geometry.
