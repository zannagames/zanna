---
status: active
audience: contributors
last-verified: 2026-08-25
---

# ADR 0294: Venue-Kit Engine Surface — Mesh3D.BendArc, glTF `extras` Metadata, SceneAsset.FlattenStatic

## Status

Accepted

## Context

Legacy Baseball is moving from one licensed stadium scan (scaled to fit every
generated park) to a hybrid venue: a procedural bowl driven by the sim's own
`Ballpark` record, dressed with a small library of modular glTF pieces that are
composed and tinted per club. Three engine gaps stood between the runtime and
that kit-of-parts workflow:

1. **Arc pieces.** Seating tiers, deck-front walls, facades and roofs are most
   naturally authored as *straight* 20 ft chords, but a bowl radius runs from
   ~80 ft behind the plate to ~430 ft in centre field. Rigidly placing straight
   chords leaves radial gaps that grow with the segment angle (the exact defect
   the game's lofted `fencePanel` exists to prevent). `Mesh3D.Transform` is
   affine and cannot wrap a chord onto a circle; `Mesh3D` exposes no triangle
   readback, so a caller cannot rebuild the mesh vertex-by-vertex either.
2. **Self-describing pieces.** glTF `extras` (Blender custom properties) are the
   standard place to author a piece's semantics — chord length, rake, tint role,
   socket names. The importer parsed `extras` as an opaque value and only read
   `mesh.extras.targetNames`; every piece needed a sidecar manifest instead.
   `SceneNode` already carries typed metadata (ADR 0159, persisted by VSCN v6
   and deep-copied on `Instantiate`), so only the import hop was missing.
3. **Static batching at stage build.** A composed park is several hundred
   placements of a few dozen pieces. Merging them into one draw per material
   is possible today with `Mesh3D.Clone` → `Transform` → `Append` from Zia,
   but that walks every node subtree through the VM, clones per node, and
   churns temporary meshes — measurable against the game's 10 s balanced
   startup budget. The runtime already has every primitive; it lacked the
   one-call C-side flatten.

## Decision

Three additions to the `Zanna.Graphics3D` surface. None touches the IL, the
verifier, or existing signatures.

### 1. `Mesh3D.BendArc(radius, arcDegrees)` — `rt_mesh3d_bend_arc(obj, f64, f64)`

The mesh's authoritative X extent `[xmin, xmax]` is mapped onto the angle span
`[-arc/2, +arc/2]` of a circle of radius `radius` whose centre lies on the
mesh's local **+Z** side, `radius` units from the chord midpoint
`(xmid, *, 0)`. A vertex at depth `z` (toward the centre) lands on the
concentric circle of radius `radius - z`; the chord midpoint and its depth
stay fixed, so the piece bends *about* the point the author placed at the
origin. Y is untouched. Normals and tangents are rotated by the local frame
rotation at each vertex's angle (the standard bend-deformer approximation)
and renormalized; tangent handedness (`t.w`) is preserved. Because every
depth is required to stay below the radius the Jacobian is positive
everywhere, the map is orientation-preserving, and triangle winding is
unchanged.

Two pieces bent to the same radius and arc and placed at adjacent angles
share their end sections exactly by construction — this is the zero-gap
property the composer relies on. Validation runs before any mutation, so a
trap leaves the mesh untouched:

- `Mesh3D.BendArc: radius must be finite and positive`
- `Mesh3D.BendArc: arc must be finite and within (0, 360] degrees`
- `Mesh3D.BendArc: skinned and morph-target meshes cannot be bent`
- `Mesh3D.BendArc: mesh must span a positive X extent`
- `Mesh3D.BendArc: vertex positions must be finite`
- `Mesh3D.BendArc: every vertex must lie closer than the radius to the bend axis`

The opposite bend direction is obtained by placing the piece rotated 180°
about Y; a signed arc was rejected because it is a reflection, not a rotation,
and would silently flip winding.

`rt_mesh3d_transform` gained an internal sibling,
`rt_mesh3d_transform_components(obj, const double[16])`, sharing one core so
the flatten below never re-implements the affine path.

### 2. glTF node `extras` → SceneNode metadata (importer only; no new API)

`gltf_import_node` now reads the node's `extras` object after its name.
JSON strings become `string` metadata, integral numbers `int` (the JSON tree
boxes every number as a double, so `20` and `20.0` both import as int — the
Blender int-property case), fractional numbers `float`, booleans `bool`, and
JSON `null` a `null` entry. Nested objects and arrays,
keys longer than `RT_SCENE_NODE3D_MAX_METADATA_KEY_BYTES`, strings over
`RT_SCENE_NODE3D_MAX_METADATA_STRING_BYTES`, non-finite numbers, and entries
past `RT_SCENE_NODE3D_MAX_METADATA_ENTRIES` are skipped and reported once per
node as an asset warning (`glTF node '<name>' extras: N entries not imported`)
so a kit author sees the drop in `zanna asset validate`. A non-object
`extras` value is ignored (the spec allows any JSON value). The existing
persistence contract does the rest: `zanna asset bake` writes VSCN v6 with the
metadata, `Instantiate` deep-copies it, and `SceneNode.MetadataGet*` reads it.

### 3. `SceneAsset.FlattenStatic(rootName, transform) -> Seq[SceneNode]` — `rt_model3d_flatten_static(obj, str, obj)`

Walks the immutable template subtree rooted at the node named `rootName`
(empty = the whole model), depth-first in authored child order, and merges
every visible static mesh into one `Mesh3D` per material, in first-seen
material order. Each mesh is transformed by `transform × nodeWorld`, where
`nodeWorld` is the node's matrix in model space (the synthetic template root
is the identity) and `transform` is the caller's placement (`null` =
identity). Every group is returned as a fresh transform-only `SceneNode`
named after its first contributing source node (`flatten_<index>` when that
node is unnamed) — so a name-based role convention on the kit's nodes
(`tint_seat_*`, `emissive_*`) survives flattening — carrying the merged mesh
and the *shared* material,
so a caller spawns one entity per returned node and the source materials
(including their textures and tints) are reused rather than copied.
Skinned meshes, morph-target meshes, and nodes whose composed matrix has a
singular upper 3×3 are skipped and reported once as an asset warning. An
unknown `rootName` returns an empty sequence. The template is never mutated.

### Deferred: per-instance tint on `InstanceBatch3D`

The venue plan also proposed a per-instance colour for instanced draws. It
changes the instance record and vertex stage of all four backends (Metal,
D3D11, OpenGL, software); only two are verifiable on the authoring machine,
and the game's draw budget (20–23 draws today against a 240 ceiling) makes
one batch per (mesh, tinted material instance) — the `Material3D.MakeInstance`
+ `SetColor` idiom already used for the crowd — an adequate substitute. It
stays a candidate for a later ADR once a Windows/Linux verification pass is
scheduled.

## Consequences

- Registered in `src/il/runtime/defs/graphics3d/rendering.def` (`Mesh3D`)
  and `lighting.def` (`SceneAsset`); the public 3D ABI manifest
  (`test_graphics3d_runtime_manifest`) is re-pinned for +2 functions / +2
  methods. Graphics-disabled builds gain matching stubs.
- `docs/generated/runtime/graphics3d.md` is regenerated from the live binary.
- Unit coverage: `test_rt_canvas3d` (bend geometry, shared end sections,
  normal rotation, bounds/revision refresh, every trap with no mutation);
  `test_rt_model3d` (extras kinds and skips, VSCN round-trip of imported
  metadata, flatten grouping/order/transform/subtree filter/unknown root).
- VM and native paths are identical by construction (pure C runtime math).
- `Mesh3D.BendArc` and `FlattenStatic` deliberately refuse skinned and
  morph-target meshes rather than silently baking a bind pose.
