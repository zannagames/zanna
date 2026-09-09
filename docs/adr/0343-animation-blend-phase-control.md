---
status: accepted
audience: contributors
last-verified: 2026-09-09
---

# ADR 0343: Animation Blend Phase Control

## Context

Baseball's walk, jog and sprint loops have different durations and contact
landmarks. Equal playback rates do not synchronize their feet. The existing
blender owns one clock per state but exposes only speed and weight.

## Decision

Add AnimBlend3D.SetPhase(state: Integer, phase: Float) and
GetPhase(state: Integer) -> Float, with runtime functions
rt_anim_blend3d_set_phase(void*, int64_t, double) and
rt_anim_blend3d_get_phase(void*, int64_t) -> double.

Phase is normalized clip time. Looping states wrap to [0,1), including
negative inputs; nonlooping states clamp to [0,1]. Nonfinite phase becomes
zero. Invalid receivers/indices/clips are no-ops or return zero; invalid
clip duration yields zero without altering the shared clip. The setter
changes only the selected state's clock. It neither advances another clock
nor evaluates a pose, emits events, changes speed/weight, or invalidates
previous bone palettes. The existing controller's next Update performs
normal evaluation and motion-history capture. Callers own contact offsets
and cadence; no universal gait markers are embedded in the engine.

No payload fields or ownership changes. Disabled-graphics stubs retain the
usual safe no-op/zero behavior. Registry delta: two functions, two methods.

## Validation

Test unequal-duration clips at matching phase, positive/negative wrap,
nonlooping endpoints, invalid/nonfinite input, state independence, and
pose evaluation through the existing update path. Baseball must integrate
measured contact offsets and test real foot sliding and seeks; this API
alone does not complete locomotion work.
