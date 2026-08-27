---
status: active
audience: contributors
last-verified: 2026-08-27
---

# ADR 0296: Add Allocation-Free 3D Query Readbacks

## Status

Accepted (2026-08-27)

## Context

Frame-rate gameplay systems need physics overlap results and spatial-audio source
positions every simulation step. The existing APIs materialize managed `Vec3`,
`PhysicsHit3D`, and `PhysicsHitList3D` objects even when native callers only need
borrowed bodies or three scalar coordinates. Target locking and audio occlusion
therefore create avoidable GC traffic in proportion to query frequency and hit
count.

Adding C entry points changes the runtime C ABI and therefore requires this ADR
under ADR 0006. The new helpers are native-runtime fast paths, not additional
language-visible methods or properties.

## Decision

Add these C-internal runtime entry points:

- `int32_t rt_world3d_overlap_sphere_bodies_raw(void *world, double cx,
  double cy, double cz, double radius, int64_t mask, void **out_bodies,
  int32_t capacity)` writes borrowed `Body3D` handles in deterministic physics
  broadphase order. It is capped by both caller capacity and the world's query
  limit, allocates no managed result objects, and returns `-1` for invalid input
  or transient-query setup failure.
- `int8_t rt_soundsource3d_get_position_components(void *source, double *x,
  double *y, double *z)` synchronizes any scene-node binding and writes the
  source position into caller-owned scalar outputs without allocating a `Vec3`.

The boxed overlap and position APIs remain unchanged. The raw sphere overlap
shares the exact collision traversal used by `World3D.OverlapSphere`; the raw
source getter shares the same binding synchronization used by
`SoundSource3D.Position`.

## Consequences

- Target acquisition and audio occlusion no longer allocate managed query or
  vector objects per sampled candidate/source.
- Borrowed bodies must not be retained past mutations or teardown of their
  physics world unless the caller explicitly retains them.
- Graphics-disabled builds provide matching fail-closed stubs.
- No scripting registry, IL opcode, grammar, verifier rule, or external
  dependency changes.

## Alternatives Considered

- **Keep boxed APIs and pool managed results.** Rejected because pooling adds
  object-lifetime complexity and still requires managed handles on native-only
  paths.
- **Expose internal physics/source layouts to Game3D.** Rejected because that
  creates brittle cross-translation-unit structure coupling.
- **Add language-visible overloads.** Rejected because these are ownership-
  sensitive implementation fast paths rather than safe scripting contracts.
