---
status: active
audience: contributors
last-verified: 2026-07-26
---

# ADR 0199: Add Project-Owned 3D Node Prefab Previews

## Status

Accepted (2026-07-26)

## Context

Many scene-driven games keep gameplay placements as empty VSCN nodes with
typed metadata. At runtime, project code turns a `prop.kind`, enemy archetype,
pickup kind, or similar value into a procedural mesh, imported model, or
composite entity. Studio deliberately does not execute that code, so these
placements appear only as tiny diagnostic markers—or disappear entirely when
authors hide overlays to inspect the game-like shaded view.

Writing preview meshes into every marker node would duplicate derived data,
make metadata edits stale, inflate canonical scenes, and risk changing runtime
loading. Hard-coding project metadata names in Studio would couple the editor
to individual games. Per-user configuration would make the same source scene
look different across a team.

## Decision

### Component-schema version 5

`scene-components.json` version 5 adds an optional top-level
`nodePreviews3D` array. Each rule contains:

- `matchProperty` and `matchValue`, which match one exact string metadata value
  on a VSCN node;
- an optional project-relative `prefab` fallback ending in `.scene3d` or
  `.vscn`;
- an optional `variantProperty` naming integer metadata and a bounded
  `variantPrefabs` array whose integer index selects a project-relative prefab;
- optional `yawProperty` plus explicit `yawUnit` (`degrees` or `radians`) and
  optional `scaleProperty` float metadata that compose a presentation-only
  Y rotation and uniform scale after the source node's world transform.

A rule must provide either a fallback prefab or a non-empty variant array.
`variantProperty` and `variantPrefabs`, and `yawProperty` and `yawUnit`, are
supplied in complete pairs. Match pairs are unique. Keys use the existing
portable 128-character metadata-key contract.
Prefab references are non-empty, at most 1,024 characters, use portable `/`
separators, stay beneath the project root, and cannot be absolute or contain
parent traversal. The schema supports at most 128 rules, 256 variants per
rule, and 4,096 instantiated preview nodes per editor document.

Wrong versions, malformed groups, duplicate matches, unsafe paths, or
oversized data reject the complete component schema. A missing or invalid
prefab is a presentation error: Studio skips that instance and reports a
bounded preview warning without rejecting or changing the scene.

### Transient prefab graph

Studio resolves matching rules after loading the canonical VSCN. It loads each
distinct prefab asset at most once, instantiates bounded copies beneath a
separate transient `SceneGraph`, and places each copy at the matched source
node's complete world transform. The transient graph renders after canonical
geometry through the same `Canvas3D`, camera, atmosphere, and material path.

Preview nodes never join the canonical graph, hierarchy, serializer, history,
revision, dirty state, session payload, or runtime scene. Rebuilding previews
after a scene or component-schema edit discards the old transient graph.

Viewport ray picking maps a preview hit back to its source marker node.
Selection, transforms, metadata edits, duplication, deletion, and undo still
operate only on that source node. Missing previews retain ordinary marker
fallback behavior.

## Consequences

- Metadata-driven props, pickups, enemies, and interaction points can resemble
  their runtime counterparts in Studio without running project code.
- Variant changes update from project-owned prefabs instead of stale embedded
  visualization data.
- Preview assets are ordinary zero-dependency VSCN files and work identically
  on macOS, Windows, and Linux.
- Large preview libraries remain bounded and cached per editor document.
- Studio probes must pin schema validation, variant fallback, path safety,
  transient rendering/picking, missing-asset behavior, and the no-dirty
  invariant.

## Alternatives Considered

- **Attach generated meshes to every marker.** Rejected because derived
  visualization would become canonical, stale, and potentially runtime-visible.
- **Execute the game factory in Studio.** Rejected because arbitrary project
  code is not deterministic, bounded, or safe editor presentation.
- **Recognize Ashfall metadata names.** Rejected because project vocabulary
  belongs in project-owned configuration.
- **Keep marker icons only.** Rejected because icons communicate placement but
  cannot provide silhouette, scale, material, occlusion, or composition.
