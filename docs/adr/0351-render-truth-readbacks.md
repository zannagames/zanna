---
status: accepted
audience: contributors
last-verified: 2026-09-11
---

# ADR 0351: Render-truth readbacks for skinned actors

## Context

A game's animation tripwire can read everything the controller knows — the
playing state, the blend-tree weight (ADR 0350), the evaluated bone matrices —
and still miss what the viewer sees. Three engine facts stay invisible from Zia:

- `scene3d_submit_node_draw` skins a node only when the **selected LOD mesh**
  carries bones and its bound skeleton is the controller's or none; a LOD mesh
  without skin data, or a mirrored copy bound to a foreign skeleton, draws as
  static geometry under a perfectly healthy controller, with no diagnostic.
- Which LOD a draw chose is per `(canvas, camera)` view state; the only public
  readbacks are the authored table (`LodCount`, `GetLodMesh`, `GetLodDistance`).
- `SetAnimationLod` rejects whole `Update` calls below its rate: the pose holds,
  the tree and the crossfades stand still. A caller that gates the throttle on
  its own camera distance cannot prove the gate never fired on a moving body.

Legacy Baseball's owner sees batters slide in the shipped game while every
controller-side gate is green (plan 114); the missing evidence is exactly these
three facts per frame.

## Decision

Add four read-only registry entries, no payload, ownership, IL or serialized
format change:

- `Mesh3D.BoneCount: Integer` — `rt_mesh3d_get_bone_count(void*) -> int64`,
  the `bone_count` the skinning gate reads; 0 means the mesh draws static.
- `Mesh3D.Skeleton: Object` (borrowed) — `rt_mesh3d_get_skeleton(void*) -> void*`,
  the bound Skeleton3D or `NULL` when unbound (an imported primitive that skins
  by raw joint index) or invalid.
- `SceneNode.SelectedLod(canvas, camera): Integer` —
  `rt_scene_node3d_get_selected_lod(void*, void*, void*) -> int64`, the index the
  last `Scene3D.Draw` chose for that view (0 = base mesh, `i` = `lod_levels[i-1]`),
  -1 when the view never drew the node, its slot was evicted, or a handle is
  invalid. It reads the per-view hysteresis cache without allocating a slot or
  touching the LRU clock, so observing never disturbs the selection.
- `AnimController3D.AnimationLodSkips: Integer` —
  `rt_anim_controller3d_get_animation_lod_skips(void*) -> int64`, a monotonic
  count of `Update` calls the rate gate rejected since construction; it survives
  every re-program and disable and evaluates nothing.

The disabled-graphics stubs return 0, `NULL`, -1 and 0.

## Validation

`test_rt_mesh3d_mirror` reads the bone count and skeleton of a skinned strip,
its mirror, a weightless box and an invalid handle; `test_rt_scene3d_bindings`
draws a node with one LOD from two camera distances through the mock backend
and reads 0, then 1, -1 for a never-used camera and invalid handles;
`test_rt_animcontroller3d` counts four rejected sub-interval steps at 2 Hz, the
accepted step and the disable; `RTGraphicsSurfaceLinkTests` links the four
symbols; `test_graphics3d_runtime_manifest` re-pins the counts and hash. The
Baseball rig consumes all four in its moving-body tripwire and gait ledger.
