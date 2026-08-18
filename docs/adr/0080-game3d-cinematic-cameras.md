---
status: active
audience: contributors
last-verified: 2026-08-17
---

# ADR 0080: Game3D Cinematic Cameras (RailCamera3D + Timeline3D)

Date: 2026-07-10

## Status

Accepted

## Context

Third-person adventure games need dolly shots and multi-track cutscenes.
Path3D already carries ordered waypoints and `GetPositionAt`, but that
evaluator is a uniform Catmull-Rom over the raw parameter: on unevenly spaced
waypoints it overshoots and its speed varies wildly (a 10x spacing spread
produced a ~40% per-step speed spread), which reads as camera judder. The
Game3D world also already has a camera-controller registry with a
late-update slot that wins the frame's final camera write, so cinematic
cameras can slot into the existing dispatch instead of inventing a parallel
path.

## Decision

A new internal evaluator, `rt_path3d_eval_spline_raw(path, t, pos, tan)`,
implements *centripetal* Catmull-Rom (Barry-Goldman, alpha = 0.5) with
arclength normalization via a cached cumulative-length table (64 substeps per
segment, rebuilt lazily on the existing dirty flag). `Path3D.GetPositionAt`
keeps its historical uniform parameterization — goldens and existing gameplay
depend on it — so constant-speed evaluation is an additive contract used only
by the new consumers.

`RailCamera3D` becomes the sixth camera controller (class-ID switch in the
world sim, same is_valid/bind pattern as ThirdPersonController): damped
`Progress`/`Speed` along the spline, look modes (entity, point, second path,
tangent), and sorted FOV/roll key tracks with linear or smoothstep easing.
Roll rotates the up vector about the view axis (Rodrigues), with a +X fallback
when the view is parallel to +Y.

`Timeline3D` is a flat record sequencer: nine track types share one
`rt_game3d_tl_track` struct sorted at `play()`. Point tracks fire on playhead
crossing (`prev < t0 <= now`, plus `t0 == 0` on the first step) so firing is
step-size independent. While any camera track exists the installed controller
is suspended, not detached; the timeline writes the camera in the pre-step
slot and `stopTimeline` (or playback end) restores control. `skip()` applies
final animation states, silences audio instead of replaying it, and past-fires
pending markers. Timelines advance on the world's *scaled* clock (ADR 0079)
so pause and hit-stop compose. `World3D.SetDofFocus` mutates the live post-FX
DOF focus distance for rack-focus shots.

## Consequences

- Two spline parameterizations coexist by design; new cinematic code should
  use the raw evaluator, and `GetPositionAt` stays frozen for compatibility.
- Timeline camera tracks and camera controllers never fight: suspension is
  scoped to `has_camera_tracks`, so audio/subtitle-only timelines leave
  gameplay cameras running.
- Track storage is flat and bounded; marker/event delivery is polled
  (`EventsFiredCount`/`EventFiredId`), consistent with the no-VM-callback
  policy.
- Tests: `test_rt_game3d_cinematics` (spline constant-speed/continuity,
  rail camera, DOF focus, timeline firing/ownership/skip).
