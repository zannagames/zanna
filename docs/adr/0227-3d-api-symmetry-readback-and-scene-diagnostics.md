---
status: active
audience: contributors
last-verified: 2026-07-29
---

# ADR 0227: 3D API Symmetry, Readback, and Scene Diagnostics

## Status

Accepted (2026-07-29)

## Context

The 3D runtime surface grew to 128 registered classes across ADRs 0142–0226,
and a systematic review of that surface against its C implementation found
three recurring shapes of gap. None of them is a missing subsystem; all of
them are places where the registered Zia surface stops short of state the
runtime already retains.

**Write-only authoring state.** Studio and games can set state they can
never read back: `Terrain3D` has fourteen setters and three getters,
`Water3D` has eleven setters and none, `Skeleton3D` accepts bones but cannot
report a bone's parent or bind pose even though
`rt_skeleton3d_get_bone_parent_raw` and `rt_skeleton3d_get_bone_bind_pose`
exist. `TextureAtlas3D` can `Add` an image but never return its packed UV
rect (`rt_texatlas3d_get_uv_rect` is unregistered), which leaves the class
unusable for its single purpose. `InstanceBatch3D` cannot read a transform
back. Three registered properties are outright write-only
(`Canvas3D.Wireframe`, `Particles3D.Additive`, `Path3D.Looping`), an
accessor shape nothing else in the registry has.

**Missing inverses on core math and load paths.** `Quat.FromEuler` has no
`ToEuler`, so Zanna Studio hand-rolls the decomposition in Zia
(`scene_editor_3d_inspector_hierarchy.zia`), duplicating a convention that
must match `rt_quat_from_euler` exactly and silently breaking if either side
drifts. `Quat.ToMat4` has no `FromMat4`. `SceneGraph.SaveToText`
(ADR 0190) has no text-loading peer on the scene side, and `SceneGraph.Load`
returns a bare nullable object while `SceneAsset` carries six `*Result`
loaders — the scene document path, the one Studio and scene-driven games
actually use, is the only loader without a diagnostic channel. `Camera3D`
retains its view matrix, projection matrix, eye, and aspect in the payload
struct yet exposes none of them; with reversed-Z active on three of four
backends, hand-reconstructed projection math in Zia is exactly the kind of
duplication that breaks silently.

**Silent failure and silent loss.** VSCN v7 prefab grafting (ADR 0187)
handles every failure — cycle, depth past 8, instance budget exhausted,
unjoinable or missing path — by leaving an empty placeholder and returning,
with no warning through the `rt_asset_error` channel and no queryable count.
A game cannot distinguish a broken prefab reference from an authored empty
node. Separately, merging an imported scene into an existing one moves the
children but not the source scene's `baked_animations`, so importing an
animated model into a non-empty Studio document silently drops its clips;
they survive only when the document was empty (the import becomes the
document and keeps its carrier).

Importer surfaces also diverged without a design reason: `Fbx` registers
fifteen members while `Gltf` registers six, even though `rt_gltf.h` already
implements the skeleton, animation, node-animation, camera, and scene
accessors. And the 3D geometry helpers (`Ray3D`, `AABB3D`, `Sphere3D`,
`Segment3D`, `Capsule3D`) are registered as bare `RT_FUNC` namespaces with
no `RT_CLASS_BEGIN` block, unlike `Zanna.Math` and every other function
group, so they get no class entry in `--dump-runtime-api`, no hover, and no
completion.

Runtime C ABI surface additions are ADR-gated; this document batches the
whole symmetry pass so it is reviewed once and lands as one def-batch.

## Decision

All additions are strictly additive: no existing registration changes
signature or semantics, and no VSCN format change is involved.

### Scene merge animation adoption

`Zanna.Graphics3D.SceneGraph` gains:

- `AdoptAnimations(source: SceneGraph) -> i64` — retain-append every baked
  animation clip carried by `source` onto the receiver, skipping handles the
  receiver already carries, and return the number adopted. The source keeps
  its references (adoption is copy-retain, not move) so the caller's
  teardown path stays unchanged.

Backed by new `rt_scene3d_adopt_baked_animations(dst, src)`. Studio's 3D
import merge calls it before grafting an instantiated scene's children
into an existing document, which fixes the clip-loss defect and lets a
subsequent `Save` serialize the rig (the v3 `animations` section already
round-trips scene-carried clips). Cross-document paste is deliberately
excluded: paste re-parses the clipboard document into fresh handles, so
pointer-identity adoption would duplicate clips on every repeat paste;
extending adoption to paste needs name-aware deduplication and is
deferred until that contract is designed.

### Prefab resolution diagnostics

The VSCN prefab grafter reports every placeholder it leaves behind:

- Each unresolved reference appends one bounded warning through
  `rt_asset_error_add_warning` naming the referencing node, the portable
  path, and the reason (`missing`, `cycle`, `depth`, `budget`, `invalid`).
  Warnings surface through the existing
  `Zanna.Graphics3D.AssetDiagnostics3D` accessors.
- `rt_scene3d` records the count, exposed as a new read-only property
  `SceneGraph.UnresolvedPrefabCount -> i64` (zero for scenes with no
  prefab references). The count survives until the scene is released, so
  a game can gate startup on `UnresolvedPrefabCount == 0`.

Resolution behavior itself is unchanged: placeholders still load, still
retain their reference for round-trip, and still render nothing.

### Result-based scene loading

`Zanna.Graphics3D.SceneGraph` gains two static loaders mirroring the
`SceneAsset` result contract (ADR 0190):

- `LoadResult(path: str) -> Zanna.Result` — ok carries the loaded
  `SceneGraph`; err carries the loader's diagnostic text.
- `LoadTextResult(virtualPath: str, text: str) -> Zanna.Result` — the
  inverse of `SaveToText`, promoting `rt_scene3d_load_from_memory` from
  policy-internal to a registered wrapper. `virtualPath` names the base
  directory for relative prefab references exactly as in
  `SceneAsset.LoadTextResult`.

`Load` and `Save` remain for compatibility.

### Quaternion and matrix inverses

`Zanna.Math.Quat` gains:

- `ToEuler() -> Vec3` — radians, exact algebraic inverse of `FromEuler`
  (pitch about X, yaw about Y, roll about Z), with the asin argument
  clamped and gimbal poles resolving yaw exactly as the reference
  decomposition Studio ships today.
- `FromMat4(m: Mat4) -> Quat` — normalized rotation extracted from the
  upper-left 3×3 via the standard trace/branch method; shear or scale in
  the input is tolerated by column-normalizing first.

`Zanna.Math.Vec2.X/Y` become read-write, matching `Vec3`. `Quat`
components stay read-only: quaternions are normalized on every authoring
path and mutable components would break that invariant. `Mat4.Row`/`Col`
are deliberately not added: `Mat3.Row` returns a `Vec3`, a `Mat4` row has
four components, and no four-component value type exists — a truncated
`Vec3` row would silently drop the translation/projection column that
callers most often want. `Mat4.Get(row, col)` remains the element reader.

Studio deletes `QuaternionToEulerDegrees` and converts through
`Quat.ToEuler` at the inspector edge.

### Camera readback

`Zanna.Graphics3D.Camera3D` gains read-only properties backed by the
retained payload: `ViewMatrix -> Mat4`, `ProjectionMatrix -> Mat4`
(the cached render projection, reversed-Z exactly as submitted),
`AspectRatio -> f64`, and `Up -> Vec3` (second row of the view basis,
matching the existing `Forward`/`Right` accessors).

### Readback symmetry batch

- `TextureAtlas3D`: `GetUvMin(id) -> Vec2`, `GetUvMax(id) -> Vec2`
  wrapping `rt_texatlas3d_get_uv_rect`.
- `InstanceBatch3D`: `GetTransform(index) -> Mat4`, plus `Mesh` and
  `Material` read-only properties over the retained handles.
- `Skeleton3D`: `GetBoneParent(index) -> i64`,
  `GetBoneBindPose(index) -> Mat4`.
- `Terrain3D` and `Water3D`: every setter over retained state gains its
  read peer (scale, layer texture/scale, material, LOD distances,
  hysteresis, skirt depth, hole enumeration `GetHole(index)`, and the
  water surface parameters). Setters whose input is consumed rather than
  retained (`SetHeightmap` pixels, `SetSplatMap` pixels) stay write-only
  and are documented as such.
- Write-only properties gain getters: `Canvas3D.Wireframe`,
  `Particles3D.Additive`, `Path3D.Looping`.
- `SceneNode` and `Transform3D`: `Position` and `Scale` stay read-only
  properties. The surface audit enforces one canonical mutation path per
  property — a writable property may not coexist with a `Set<Prop>`
  method — and the scalar-triple `SetPosition`/`SetScale` methods are that
  path (the same rule explains why `Rotation`, whose only mutation is the
  Quat-valued property, is read-write). What the review read as accidental
  asymmetry is an enforced convention; this ADR documents it instead of
  fighting it. `SceneNode` does gain
  `TrySetWorldPosition(x, y, z) -> i1` — a translation-only world edit
  that recomputes the local offset through the parent's world inverse and
  rejects (returns false) when that inverse does not exist, consistent
  with ADR 0166's exact-or-reject contract.
- `SceneNode.SetStatic`/`GetStatic` stay method-shaped: the registry
  requires distinct `get_`/`set_`-qualified functions for a property, and
  re-exporting the same C symbols under second qualified names is exactly
  the duplicate-export shape the surface audit rejects. The methods remain
  the canonical flag accessors.

### Importer parity

`Zanna.Graphics3D.Gltf` registers the accessors its loader already
implements, mirroring `Fbx`: `SkeletonCount`/`GetSkeleton`,
`AnimationCount`/`GetAnimation`, `NodeAnimationCount`/`GetNodeAnimation`,
`CameraCount`/`GetCamera`, `SceneCount`/`GetSceneName`/`GetSceneRootAt`,
`NodeCount`, and `GetSceneRoot`. An `Fbx.LoadAsset` peer is deferred:
`Gltf.LoadAsset` is the asset-manager-resolving load variant, and the FBX
loader has no asset-manager staging path yet, so a wrapper would
misrepresent its semantics. `SceneAsset.Load` already covers `.fbx`
through the shared dispatch.

### Geometry class blocks

`Ray3D`, `AABB3D`, `Sphere3D`, `Segment3D`, and `Capsule3D` re-register
as proper class blocks (`RT_CLASS_BEGIN(..., "obj", none)` with static
methods, the `Zanna.Math` pattern). The flat function names remain valid;
the class blocks add the missing `--dump-runtime-api`, hover, and
completion entries.

## Consequences

- One def-batch touches `src/il/runtime/defs/` (graphics3d + math blocks),
  `RuntimeSurfacePolicy.inc` (symbols leaving the internal list), the ABI
  surface tests (`test_graphics3d_abi_surface`,
  `test_graphics3d_runtime_manifest`, `test_runtime_registry`), and the
  generated reference docs via rtgen.
- New C entry points are thin wrappers over retained state
  (`rt_quat_to_euler`, `rt_quat_from_mat4`, camera/atlas/batch/skeleton
  readback, `rt_scene3d_adopt_baked_animations`, the prefab counter) —
  no new invariants, no allocation beyond the returned handles.
- Every addition ships with a unit test that fails before registration;
  the quaternion inverses additionally ship a round-trip sweep test
  (`FromEuler(ToEuler(q))` and `FromMat4(ToMat4(q))` recover `q` up to
  sign) so convention drift is structurally impossible.
- Studio loses one hand-rolled math function and gains truthful import
  diagnostics; games gain a startup gate for broken prefab references.
- `--dump-runtime-api` output grows; consumers keying on class counts
  (docs, audits) regenerate in the same change.

## Alternatives considered

- **Per-feature mini-ADRs.** Rejected: the additions share one rationale
  (surface symmetry over already-retained state) and one migration (def
  batch + surface tests + rtgen); ten documents would review the same
  decision ten times.
- **Transferring animations implicitly inside `SceneGraph.TryAdd`.**
  Rejected: `TryAdd` is a node-graph operation; making it mutate
  scene-level carriers on a *different* scene handle is action at a
  distance, and non-Studio callers moving nodes between scenes they
  manage themselves would pay for retains they did not ask for.
- **Failing the VSCN load on unresolved prefabs.** Rejected: ADR 0187
  deliberately made placeholders non-fatal so an author can open a scene
  whose dependency moved. Diagnostics make the state visible without
  changing that contract.
- **Making `Quat` components mutable for full Vec3 parity.** Rejected:
  every quaternion produced by the runtime is normalized; component
  mutation would make that invariant caller-maintained for no authoring
  gain.
