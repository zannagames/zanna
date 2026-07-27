---
status: active
audience: contributors
last-verified: 2026-07-26
---

# ADR 0200: Add Project-Owned 3D Sky and Light Preview Rigs

## Status

Accepted (2026-07-26)

## Context

ADR 0198 lets a project map scene-root metadata to Studio's first gameplay
camera, ambient floor, clear color, and distance fog. That makes large scenes
open at a meaningful scale, but many games derive most of their visual identity
from a procedural sky and a key/fill directional-light rig. A solid horizon
color plus Studio's generic default light turns those scenes into flat
silhouettes even when their canonical meshes, materials, and metadata are
correct.

Studio must not recognize one game's metadata vocabulary or execute project
environment code. Baking derived sky and light nodes into every canonical scene
would duplicate runtime state and could change game loading. A useful preview
therefore needs another project-owned, deterministic, read-only mapping.

## Decision

### Component-schema version 6

`scene-components.json` version 6 extends `scenePreview3D` with these optional
metadata-key groups:

- `zenithRProperty`, `zenithGProperty`, `zenithBProperty`,
  `groundRProperty`, `groundGProperty`, and `groundBProperty`. Together with
  the existing complete clear-color triple (the horizon color), these form one
  complete sky-palette group.
- `sunXProperty`, `sunYProperty`, `sunZProperty`, `sunRProperty`,
  `sunGProperty`, `sunBProperty`, and `sunIntensityProperty`.
- `fillXProperty`, `fillYProperty`, `fillZProperty`, `fillRProperty`,
  `fillGProperty`, `fillBProperty`, and `fillIntensityProperty`.
- Optional `cloudProperty` and `starsProperty` keys refine a complete sky
  palette when their scene metadata is respectively a float and a boolean.

Every key uses the existing portable 128-character metadata-key contract.
Sky, sun, and fill groups are independently all-or-none; cloud/stars require
the complete sky palette. Supplying any version-6 member in an older schema,
or supplying a partial, malformed, or oversized group, rejects the complete
component schema under the existing fail-closed policy.

### Bounded read-only presentation

For each shaded viewport render, Studio resolves only correctly typed root
metadata. A complete sky palette builds a cached 32-pixel-per-face
`CubeMap3D`: zenith blends through the horizon to the ground, with bounded
haze, optional clouds/stars, and a sun glow when a complete sun group exists.
The fixed face size bounds construction to 6,144 texels. A canonical authored
`env.skybox` remains authoritative; the generated project sky is a fallback.

Complete sun and fill groups create transient `Light3D.Directional` values in
Studio-owned canvas slots. Direction vectors, colors, and intensities are
sanitized and bounded before use. Scenes without those groups retain Studio's
default editor light. Canonical `SceneNode.Light` components continue to render
as scene lights alongside the preview rig.

The generated cubemap and lights belong only to the viewport. They never join
the canonical `SceneGraph`, VSCN serializer, hierarchy, history, revision,
dirty state, session payload, or runtime scene. Studio does not import or run
project code.

## Consequences

- Metadata-driven scenes can reproduce their game's broad sky palette,
  silhouette lighting, fog, and gameplay camera instead of showing a flat
  solid-color backdrop.
- The convention remains project-owned, source-controlled, zero-dependency,
  deterministic, and cross-platform.
- Existing version 1–5 schemas and incomplete/wrong-typed scene metadata retain
  their current safe fallback behavior.
- Studio probes must pin schema version gating, complete-group validation,
  typed render-state resolution, bounded color/distance/intensity clamping,
  generated sky/light application, cache invalidation, and the no-dirty
  invariant.

## Alternatives Considered

- **Recognize Ashfall's `lenv.*` keys in Studio.** Rejected because project
  vocabulary cannot become editor policy.
- **Run the project's sky and lighting modules.** Rejected because arbitrary
  game code is not a bounded, deterministic authoring contract.
- **Store generated skyboxes and lights in every VSCN.** Rejected because
  derived preview state would become canonical and could affect runtime parity.
- **Keep a solid clear color and stronger ambient only.** Rejected because it
  cannot communicate horizon composition, key direction, material response, or
  the game's dominant environment palette.
