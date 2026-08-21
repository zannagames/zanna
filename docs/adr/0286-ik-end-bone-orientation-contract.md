---
status: active
audience: contributors
last-verified: 2026-08-21
---

# ADR 0286: IK End-Bone Orientation Contract

## Status

Accepted (2026-08-21).

## Context

`IKSolver3D.SetGroundNormal` oriented a chain's end (foot) bone by replacing
its local rotation with an absolute basis whose +Y column equals the ground
normal. That bakes one rig convention (+Y = sole-up, +Z = toe-forward) into the
runtime. Imported humanoid rigs overwhelmingly use the bone-axis convention
instead: +Y runs along the bone, ankle toward toe. On such a rig the pass
rotates the foot ~90 degrees toes-up, and it does so even when the positional
solve is a no-op, because the orientation pass is unconditional once a normal
is set. The baseball demo's stationary foot plants exposed this: every planted
idle foot pitched skyward while moving poses (weight 0) stayed correct. The
absolute basis also discards authored foot animation (heel raise, toe pivot)
on flat ground, where the correct answer is "change nothing."

Separately, the positional chain solve never orients the end bone at all: it
aims each parent link and pins the child translation, so an end effector
reaches the right position with an arbitrary inherited rotation. Constraints
that need a specific end orientation (an off hand wrapping a bat) had no
runtime surface to express it.

## Decision

1. Ground-normal alignment becomes a delta, not an absolute basis.
   `ik3d_apply_foot_orientation` computes the shortest-arc rotation from model
   +Y to the supplied normal and composes it onto the end bone's current
   (animated, post-solve) global rotation, converts to parent-local, and slerps
   by solver weight. On flat ground the pass is an exact no-op; on a slope the
   authored pose leans with the surface. The pass is rig-axis-agnostic.
2. Add an explicit end-effector orientation goal:
   `IKSolver3D.SetTargetRotation(rotation)` stores a model-space quaternion
   goal for the chain's end bone, applied after the positional solve by
   slerping the end bone toward the goal by solver weight (same parent-local
   conversion as the ground pass). `IKSolver3D.ClearTargetRotation()` removes
   the goal. Non-Quat inputs are ignored; components are sanitized and the
   quaternion is normalized (identity when degenerate).
3. Precedence: when both a target rotation and a ground normal are set, the
   target rotation wins and the ground delta is skipped. An explicit goal is
   strictly stronger than a surface hint.
4. Look-at solvers are chain-count-1 and have no end segment; both passes
   apply only to positional chains (two-bone and FABRIK).

## Consequences

- Rigs keep their authored end-bone animation on flat ground; ground response
  becomes purely terrain-driven. Callers that relied on the absolute +Y basis
  to *flatten* feet (heel-strike planting) must now express that with
  `SetTargetRotation` or authored clips.
- Two new registry functions/methods extend the reviewed Graphics3D surface;
  the ABI manifest and generated reference are re-pinned.
- The end bone's authored twist survives a zero-correction solve, which the
  previous contract destroyed.

## Tests

`test_rt_animcontroller3d` re-pins the ground contract as a delta: a foot on a
flat normal keeps its authored rotation bit-exactly through a full-weight
solve; a tilted normal tilts the animated rotation by exactly the shortest-arc
delta (verified against the rotated-parent case). New coverage drives
`SetTargetRotation` through apply, weight blending, precedence over a ground
normal, sanitization, and `ClearTargetRotation` restoring the positional-only
result. ABI-surface tests pin both PascalCase methods.
