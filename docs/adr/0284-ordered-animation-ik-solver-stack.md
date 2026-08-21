---
status: active
audience: contributors
last-verified: 2026-08-20
---

# ADR 0284: Ordered Animation IK Solver Stack

## Status

Accepted (2026-08-20).

## Context

`AnimController3D` retained one post-pose `IKSolver3D`. `SetIKSolver` replaced
that slot, which made independent simultaneous constraints impossible: two
hands could not be held to one bat while a head tracker remained active, and
later glove/foot constraints would displace each other. A single FABRIK chain
cannot represent disjoint limbs because every chain link must be parented to
the previous link.

The baseball batter exposes the concrete failure. Its swing and bunt source
clips separate both wrists farther than one off-arm solve can recover. Moving
only the off arm either misses the bat or reaches the limb-length limit; moving
both arms toward a shared grip center is well-conditioned and preserves the
authored torso performance.

## Decision

1. Replace the controller's single retained solver with a fixed, ordered stack
   of at most four compatible `IKSolver3D` handles.
2. Preserve `SetIKSolver` behavior: a non-null call clears the stack and
   installs one solver; null clears the entire stack.
3. Add `AnimController3D.AddIKSolver(solver)` and the matching
   `Game3D.Animator3D.AddIKSolver` wrapper. The call validates the exact
   skeleton, is idempotent for a duplicate handle, and fails when full.
4. Apply constraints in insertion order after animation layers and before
   bone-count LOD/skinning. Each solver rebuilds globals from the pose left by
   its predecessor, so disjoint chains compose and overlapping chains have an
   explicit deterministic priority.

## Consequences

- Bat grip, gaze, glove, and foot constraints can coexist without an unbounded
  collection or a dependency.
- Four retained handles add a small constant amount of controller storage.
- Callers own ordering. Overlapping chains intentionally use last-applied
  precedence and should have a focused pose test.
- Existing callers and `SetIKSolver(NULL)` retain their old semantics.

## Tests

`test_rt_animcontroller3d` builds two independent arm chains, installs them
with `SetIKSolver` plus `AddIKSolver`, proves both endpoints solve, checks
duplicate idempotence and invalid-handle rejection, then proves null clears the
entire stack. ABI-surface tests pin both PascalCase methods.
