---
status: active
audience: contributors
last-verified: 2026-07-27
---

# ADR 0203: Add Project-Owned 3D Scene Environment Previews

## Status

Accepted (2026-07-27)

## Context

Studio can render a canonical VSCN, project-owned node prefab previews, and a
metadata-driven gameplay camera, sky, light, and fog rig. Some games still add
large visual structure only after loading that VSCN. Ashfall, for example,
constructs its exterior `Terrain3D` from root metadata while the saved scene
contains the editable arena and gameplay markers. The editor therefore showed
the right nodes from the right eye but omitted the canyon enclosing them.

Studio cannot safely execute a project's level builder, startup loop, or
renderer. Teaching the editor game-specific metadata keys would couple product
code to one example, while copying a large derived mesh into every canonical
scene would make runtime-generated presentation part of gameplay authoring and
create noisy scene history.

## Decision

### Component-schema version 9

`scene-components.json` version 9 extends `scenePreview3D` with an optional
scene-level environment mapping:

- `environmentPrefab` is an optional fallback `.scene3d` or `.vscn` asset.
- `environmentVariantProperty` and `environmentVariantPrefabs` are an optional
  pair. Studio reads that exact integer from the canonical scene root and uses
  it to select one of 1–64 prefab assets.
- `environmentVariantOffset` is an optional integer in
  `[-1,000,000, 1,000,000]` added before indexing the variant list. It requires
  the variant pair.

A fallback may accompany the variant pair. Missing, wrong-typed, or
out-of-range metadata selects the fallback; without one, Studio simply omits
the environment preview.

Paths are at most 1,024 characters, use forward slashes, and resolve relative
to the project root containing `scene-components.json`. Absolute paths,
drive/URI syntax, backslashes, and parent traversal are rejected. Wrong-version,
malformed, incomplete, unsafe, or oversized declarations reject the complete
component schema without publishing partial rules.

### Transient scene composition

Studio loads the resolved asset through its bounded `SceneAsset` path and
places its top-level nodes beneath an identity wrapper in the existing
disposable project-preview graph. The environment shares the 64 MB scene-read,
256 cached-preview-asset, and 4,096 preview-node limits. A missing, invalid, or
over-budget asset produces preview status rather than changing the canonical
scene.

The preview graph draws through the same retained camera, depth buffer, sky,
lights, fog, shaded/wireframe mode, and responsive viewport as the canonical
graph. The environment wrapper has no canonical owner: it does not appear in
the hierarchy, cannot become a saved selection, and cannot intercept picking
from editable nodes. Rebuilding, rendering, or applying Gameplay View never
changes scene bytes, revision, dirty state, or undo history.

Projects with procedural runtime environments bake deterministic,
self-contained preview prefabs using their own tooling. Studio consumes only
the ordinary scene asset and does not import or execute that tooling.

## Consequences

- A scene can show runtime-generated terrain, modular shells, or other large
  visual context while its canonical hierarchy remains concise and editable.
- Preview truth travels with the project and behaves identically on macOS,
  Windows, and Linux without dependencies or runtime ABI changes.
- Versions 1–8 and projects without an environment mapping retain their prior
  behavior.
- ADR 0212 extends this single base slot with independently matched additive
  layers; it does not change the version-9 fallback or variant contract.
- ADR 0213 adds bounded runtime-backed water surfaces and makes every
  canonical/project preview share one explicit canvas frame.
- Probes must pin schema version/path/variant rejection, real-project loading,
  exact gameplay-camera retention across layout changes, and canonical
  content/history isolation.

## Alternatives Considered

- **Execute the owning game inside Studio.** Rejected because startup side
  effects, input loops, saves, timing, and arbitrary project code are not a
  deterministic editor contract.
- **Recognize Ashfall's `af.terrain` key in Studio.** Rejected because project
  vocabulary belongs to the project schema.
- **Bake derived terrain into every mission VSCN.** Rejected because it would
  duplicate runtime-generated data and pollute hierarchy, saves, and history.
- **Add a new runtime terrain serialization ABI.** Deferred because ordinary
  self-contained scene meshes satisfy authoring preview needs without changing
  the runtime surface or canonical scene format.
