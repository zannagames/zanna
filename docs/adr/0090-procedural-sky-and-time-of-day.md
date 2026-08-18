---
status: active
audience: contributors
last-verified: 2026-08-17
---

# ADR 0090: Procedural Sky and Time-of-Day Clock

Date: 2026-07-10

## Status

Accepted

## Context

The skybox was a static cubemap and every environment a fixed lighting rig.
The plan called for an analytic sky, a clock driving sun/ambient/sky/IBL, and
weather presets.

## Decision

- **`Sky3D`** renders a CPU-generated cubemap (default 64x64 faces,
  sub-millisecond) through the existing skybox path, so IBL, fog, and all
  four backends follow with zero shader work. The radiance model is the
  documented simplified formulation (plan section 8 fallback, recorded here):
  a zenith/horizon gradient keyed to sun elevation with turbidity haze, a
  sunset band pooling toward the sun azimuth, a Mie-inspired halo plus sun
  disc, a near-black night fade, and a ground-albedo hemisphere. Deterministic
  in its inputs; `Update(canvas)` regenerates only while `Dirty`.
- **`TimeOfDay3D`** is a deterministic clock: `Advance(dt, canvas)` is the
  only input (call with the world's scaled dt — pause and hit-stop compose),
  `Hours`/`DayLengthSeconds` (0 = paused)/`LatitudeDegrees` shape the sun
  arc. It drives a bound sun `Light3D` (direction; warm-horizon to white-noon
  color curve; intensity 0 below the horizon), a bound `Sky3D` (sun direction
  + regeneration), and flags a bound `ReflectionProbe3D` dirty — all
  throttled by `RefreshDegrees` (default 2) so IBL/capture cost is bounded.

## Consequences

- Weather presets (rain/snow particles, wetness material term, world wind
  plumbing) are deferred: particles belong with the effects registry work and
  wetness with the LIT shader batch; a vegetation registry does not exist to
  push wind into. Recorded as the follow-up scope.
- Explicit `Advance` (rather than an embedded world ticker) keeps the clock
  deterministic, testable, and free of world-struct changes; Game3D worlds
  call it once per frame with `world.Dt`.
- Tests: `g3d_test_timeofday3d_clock` (noon/midnight sun state, running-clock
  rate, sky refresh) and `g3d_test_sky3d_procedural` (dirty lifecycle,
  generation, install).
