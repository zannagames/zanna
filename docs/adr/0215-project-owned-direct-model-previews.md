---
status: active
audience: contributors
last-verified: 2026-07-28
---

# ADR 0215: Render Project-Owned Direct Model Previews

## Status

Accepted (2026-07-28)

## Context

ADR 0199 lets a project replace metadata-only 3D marker nodes with transient
`.scene3d` or `.vscn` prefab art. That makes gameplay markers recognizable, but
it still requires a project to maintain an editor-only copy of every asset.

Ashfall exposes the practical cost. Its game loads optional glTF enemy rigs and
uses procedural meshes only when the imported art is unavailable. Studio
previously showed the procedural fallback for every spawn even when the exact
project rig was present. Camera, terrain, water, materials, and post-processing
could match the game while its most prominent actors still did not.

Studio needs to load the same project-owned model formats through the public
runtime asset surface. A project also needs exact typed matches because numeric
archetype discriminators must not be confused with their textual
representation, plus a bounded fixed transform for model-coordinate and
presentation differences. Missing optional art must retain a deterministic
fallback without weakening schema validation or canonical scene isolation.

## Decision

### Component-schema version 17

`scene-components.json` version 17 extends each ordered `nodePreviews3D` rule.
Its `matchValue` may now be a Boolean, integer, or string. Studio compares both
the canonical metadata kind and value. Fractional numbers are invalid, and a
duplicate property/kind/value identity rejects the complete schema. Versions
5–16 retain their string-only node-match contract.

The rule's `prefab` and `variantPrefabs` may reference safe project-relative:

- `.scene3d` and `.vscn` scene assets; or
- `.gltf`, `.glb`, `.fbx`, `.obj`, and `.stl` model assets.

Direct model paths in node-preview rules require version 17. The shared preview
asset validator may also admit these formats for scene-level environment
assets, whose own version and transform contracts remain unchanged. Absolute
paths, backslashes, drive or URI separators, and parent traversal reject the
complete schema.

Version 17 also adds optional fixed `scale`, `yawDegrees`, and
`offsetX/Y/Z`. Scale stays in `[0.001, 1000]`, yaw in
`[-36000, 36000]`, and each offset in `[-1000000, 1000000]`.
Omitted values remain identity transforms. The fixed transform composes after
the canonical source-node world transform and before the existing
metadata-driven yaw and scale mappings:

`source × translate(offset) × rotateY(fixed + mapped yaw) × scale(fixed × mapped)`

These members require version 17 even when the referenced asset is a VSCN
scene. Structured component edits retain typed matches and fixed transforms and
write version 17 whenever any direct-model member is present.

### Load the first successful matching rule

Rules remain ordered, but selection advances past a matching rule whose asset
cannot load. The first matching rule that loads completely owns the node. This
lets a project place an exact typed direct-model rule before a broader
procedural scene rule. If optional imported art is absent or invalid, Studio
uses the later matching fallback and reports a fallback count instead of
silently dropping the node.

No rule that fails to load contributes transform state. Studio retains the
exact successful rule beside each transient wrapper so later canonical
transform or metadata edits continue to use the correct recipe. A node for
which at least one rule matches but none loads increments the missing count.
Direct-model and fallback instance counts remain separately visible for probes
and viewport status.

### Runtime boundary and ownership

Studio resolves each path from the owning schema directory and calls the public
`SceneAsset.LoadResult` API. It does not duplicate glTF, GLB, FBX, OBJ, or STL
parsers. The parent asset must remain at or below the existing 64 MB preview
source limit and have a stable size across the load. The runtime loader owns
format validation and bounded dependency traversal.

Loaded children move into Studio's disposable project-preview graph under one
wrapper per canonical source node. The wrapper participates in rendering,
visual bounds, surface placement, and picking remapping, but never enters the
canonical `SceneGraph`, hierarchy, selection identity, VSCN serialization,
dirty state, revision, or undo/redo history. Loading another scene or schema
discards the graph and asset cache.

### Ashfall

Ashfall declares typed rules for every archetype with shipped imported art,
using the same glTF paths and presentation scales as its runtime asset registry.
Flying archetypes add the same authored vertical presentation offset and all
imported rigs use the model-facing yaw correction. The existing generated
enemy scene prefabs remain later fallback rules for optional-art failures and
for procedural-only archetypes.

The canonical mission continues to store only marker nodes and typed gameplay
metadata. Studio now shows the game's actual enemy rigs when available without
making those derived instances part of the mission.

## Consequences

- A scene can resemble the running game without maintaining editor-only copies
  of imported actor models.
- Exact typed matches avoid collisions between Boolean, integer, and string
  discriminators.
- Ordered successful-load selection makes optional art honest and resilient
  while preserving deterministic procedural fallback.
- Fixed transforms handle model coordinate conventions without changing
  canonical marker transforms.
- Version 17 is bounded, cross-platform, zero-dependency, and additive.
- The decision adds no runtime C ABI, IL, VSCN, workflow, dependency, or
  platform-adapter change. It consumes the existing public `SceneAsset`
  result API and model loaders.
- Probes must cover version gates, typed identity, safe format/path rules,
  numeric bounds, structured-authoring preservation, ordered fallback,
  successful-rule transform retention, direct Ashfall rig loading, status
  counts, and canonical content/history isolation.

## Alternatives Considered

- **Bake every imported rig into an editor VSCN.** Rejected because it
  duplicates project assets and drifts from runtime loading.
- **Execute the project's asset registry inside Studio.** Rejected because
  arbitrary project code is not a bounded, deterministic preview contract.
- **Teach Studio Ashfall's archetype table.** Rejected because metadata names,
  model paths, scales, and fallback policy belong to the project.
- **Stop at the first matching rule even when loading fails.** Rejected because
  optional runtime art needs the same deterministic fallback available in the
  game.
- **Apply a failed preferred rule's transform to the fallback.** Rejected
  because transforms are part of one asset recipe and cannot safely leak
  between rules.
