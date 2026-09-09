---
status: accepted
audience: contributors
last-verified: 2026-09-09
---

# ADR 0345: Pose-relative stride warp

## Context

Baseball's route clock owns actor translation. Retiming clips alone cannot
match every route speed while retaining a natural cadence. Reading a foot
matrix before the controller update targets the previous pose and introduces
one-frame feedback. A small correction must instead use the freshly blended
pose in the existing ordered IK pass.

## Decision

Add `IKSolver3D.SetStrideWarp(direction: Vec3, scale: Float,
maxOffset: Float)` and `ClearStrideWarp()`. Runtime entry points are
`rt_ik_solver3d_set_stride_warp(void*, void*, double, double)` and
`rt_ik_solver3d_clear_stride_warp(void*)`. Registry delta: two functions,
two methods. Provide disabled-graphics stubs.

For two-bone and FABRIK chains, derive an end target from the current
model-space pose: project the end-minus-chain-root displacement onto the
normalized horizontal direction, multiply by `scale - 1`, clamp to
`+/-maxOffset`, and add that directional offset to the animated endpoint.
Use the existing chain solver and contribution weight. Preserve animated
height, chain lengths, and the configured ground/rotation hints. Scale is
clamped to [0.5,1.5]; nonfinite scale becomes 1. Offset is in model units,
clamped to [0,1e12]; nonfinite offset becomes 0. Degenerate/nonfinite
horizontal directions and identity/zero corrections are pose no-ops.
No time, root motion, events, or route data are stored in the engine.

Configuration is copied into appended private scalar fields; no new
retained objects. Wrong receivers, non-Vec3 directions, and look-at solvers
ignore the setter. A valid SetTarget disables stride mode. ClearStrideWarp
restores the previously configured absolute target. Pose application must
not overwrite that target and must validate chain bounds before reading
endpoint matrices. Standalone Solve uses the same policy against bind pose.

## Validation

Test positive/negative correction, displacement cap, identity and invalid
numeric no-ops, current-pose evaluation, repeated evaluation without drift,
unchanged bone lengths, explicit target restoration, and unrelated state
independence. Retain existing IK corruption/ownership/ordered-stack tests.
Baseball will own calibrated stride lengths, bounded correction and contact
phase policy. Native foot-slip and transition acceptance remain necessary;
this engine primitive alone does not complete item 4.
