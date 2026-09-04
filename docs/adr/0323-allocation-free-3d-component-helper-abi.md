---
status: accepted
audience: contributors
last-verified: 2026-09-04
---

# ADR 0323: Add Allocation-Free 3D Component Helper ABI

## Context

Game3D controllers run once or twice per simulation frame. Several controller
paths allocated temporary Vec3 or Quat runtime objects merely to pass numeric
components between the Game3D, Physics3D, and Camera3D layers. Character
movement, node/body synchronization, first-person camera placement, and the
third-person boom sweep therefore created garbage every frame even though all
values already existed in native arrays.

Adding cross-layer runtime C entry points changes the internal runtime ABI and
requires a decision record under ADR 0006.

## Decision

Physics3D and Camera3D provide internal component-oriented counterparts for
character movement/position, body orientation, sphere-sweep input, and camera
position. These functions accept scalar values or borrowed fixed-size arrays,
perform the same validation and sanitization as their boxed public wrappers,
and never retain caller storage. Existing scripting-visible functions remain
unchanged and delegate to the component cores.

Hot Game3D controller paths use the component helpers directly. Query result
objects such as PhysicsHit3D remain boxed because they escape the call and are
part of the public ownership contract.

`RuntimeSurfacePolicy.inc` classifies the component symbols as implementation-
only so runtime surface auditing cannot accidentally promote them into the
frontend API.

## Consequences

- Normal character and camera updates allocate no temporary Vec3/Quat objects.
- Public runtime signatures and scripting behavior remain compatible.
- Component helpers are internal cross-layer ABI: callers must treat all array
  arguments as borrowed for the duration of the call.
- Disabled-graphics builds do not expose these private helpers because the
  calling Game3D implementation is disabled with the same feature boundary.

## Alternatives Considered

- Stack-fabricate runtime Vec3 objects: rejected because managed object headers
  and validation require genuine runtime allocations.
- Cache mutable Vec3 objects per controller: rejected because it increases
  retained state, corruption surface, and reentrancy hazards.
- Keep allocating and rely on collection: rejected because the work occurs in
  latency-sensitive per-frame loops.
