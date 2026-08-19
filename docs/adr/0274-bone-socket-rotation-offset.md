---
status: active
audience: contributors
last-verified: 2026-08-19
---

# ADR 0274: Bone-Socket Rotation Offset

## Status

Accepted (2026-08-19).

## Context

Bone sockets (`Entity3D.AttachToBone` / `AttachToBoneOffset`) drive a child
node's world transform from `parentWorld x bonePose x socketOffset` every
sync pass. The socket offset has always been position-only at the API
surface: the node struct carried a `socket_offset_quat` that the sync math
already composed (`quat_mul_local(bone_quat, socket_offset_quat, ...)` in
`scene_node_apply_bone_socket`), but nothing could ever set it — both attach
paths unconditionally reset it to identity, and `scene_node_set_world_transform`
overwrites the child's own local rotation every sync pass, so rotating the
socketed child directly never worked either.

Consumers therefore baked orientation into prop meshes. The baseball demo's
bat is the canonical case: a "grip tilt" rotation baked into the mesh at
load (`registry3d.ensureBat`), re-baked whenever the grip convention
changed, with a second mirrored mesh bake required the moment the tilt axis
left the local YZ plane (plan 64). Mesh re-bakes as a stand-in for a socket
parameter is the wrong altitude: the same prop mesh should be attachable at
different orientations per hand, per role, or per tuning pass.

## Decision

Expose the already-composed rotation through two additions:

1. **`SceneNode3D` C ABI**: `rt_scene_node3d_set_bone_socket_rotation(node,
   qx, qy, qz, qw)` — validates finite, non-zero; normalizes; stores into
   `socket_offset_quat`. Attach continues to reset the rotation to identity,
   so the setter is called after attaching. Setting it on an unsocketed node
   is allowed (call order within a frame is free).
2. **Zia surface**: `Zanna.Game3D.Entity3D.AttachToBoneOffsetRotated(child,
   boneName, ox, oy, oz, rxDeg, ryDeg, rzDeg)` — the offset attach plus a
   bone-space Euler-degrees rotation (the `SetRotationEuler` convention and
   clamp), converted through `rt_quat_from_euler` and applied to the child's
   socket. On attach failure the socket and rotation are left unset.

The composition point is unchanged (this ADR adds no new math): the
rotation multiplies bone-locally after the bone pose and does NOT affect
the positional offset, which continues to rotate by the bone pose alone.

## Consequences

- Held props can cock/roll in the hand per attach site with one shared
  mesh; orientation retunes stop being mesh re-bakes.
- The baseball demo's bat keeps its measured-axis mesh bake for now (it is
  landed, gated by G-BAT-CLEAR/G-BAT-GRIP, and visually verified); the next
  grip retune should migrate to this op and delete the LH mirror bake
  (tracked in `baseball/plans/64-bat-grip.md`).
- Rejected alternative: exposing a public per-bone pose-override
  (`rt_anim_controller3d_apply_pose_override` stays internal to
  ragdoll/cloth) — a socket parameter, not a skeleton mutation, is the
  right tool for prop orientation.

## Tests

`test_rt_scene3d_bindings` (`test_bone_socket_rotation_offset`): rotation
composes onto the bone pose, leaves the socket position alone, normalizes
non-unit input, and resets to identity on re-attach.
