---
status: active
audience: contributors
last-verified: 2026-07-11
---

# ADR 0081: Game3D Dialogue System (Dialogue3D + Camera3D.WorldToScreen)

Date: 2026-07-10

## Status

Accepted

## Context

Adventure games spend much of their runtime in conversations. Zanna had no
dialogue primitive: games hand-rolled text reveal, input latching, and choice
menus per project, and there was no supported way to project a world-space
point into screen space for speech-bubble anchoring (the math lived only
inside the renderer).

## Decision

`Camera3D.WorldToScreen(x, y, z, screenW, screenH)` exposes the projection ×
view column-vector transform as a public method returning `Vec3(sx, sy,
visible)` where `visible` is `clip.w > 0` — behind-camera points report a
position but are flagged, letting callers choose fallback placement rather
than trapping.

`Dialogue3D` is a world-scoped typewriter queue (bounded at 32 pending
lines): `say`/`sayVoiced` enqueue, `advance` implements two-stage skip (first
press completes the reveal at the configured characters-per-second, second
press advances), and a revealed line auto-advances after a 1.2 s hold when
auto mode is on. Strings are resolved through an optional bound
`MessageBundle` — if the string exists as a key it localizes, otherwise the
literal is displayed, so plain-text prototypes upgrade to localized shipping
text without an API change. Voiced lines route through the existing 2D audio
path. `askChoice` blocks advance until `moveChoice`/`confirmChoice`; results
are one-shot (`choiceMade`) plus a sticky `lastChoice`, matching the polled
event idiom used across Game3D.

The overlay draws in `render_once` after user overlay callbacks and before
the timeline overlay: a bottom panel by default, or a compact bubble anchored
via `WorldToScreen` above a bound speaker entity with automatic panel
fallback when the anchor is off-screen or behind the camera. Dialogue ticks
on *unscaled* time inside the sim step (before controllers) so conversations
keep revealing while gameplay is paused for a cutscene conversation.

## Consequences

- Input stays game-mapped: the runtime never claims keys; games call
  `advance`/`moveChoice`/`confirmChoice` from their own bindings and gate
  interactions on `Dialogue3D.Active`.
- Missing localization keys degrade to literals — never a trap — which is the
  right failure mode for shipping text.
- `WorldToScreen` is generally useful beyond dialogue (markers, nameplates,
  minimap hand-off) and is deliberately a Camera3D method, not a Dialogue3D
  helper.
- Tests: `test_rt_game3d_dialogue_facial` (projection cases, reveal/two-stage
  skip/queue order, choice latching, localization fallback).

## Amendment (2026-09-15): near-plane visibility

`visible` is now `clip.w >= near` for perspective cameras (`clip.w > 0` for
orthographic). A point between the eye and the near plane is clipped by every
renderer, and its perspective divide is unbounded — a torso a hair in front of
the eye projected to ~1e10 pixels while still flagged visible, and Legacy
Baseball's per-frame actor pick squared that as a 64-bit Integer and trapped
"integer overflow" mid-game. Screen coordinates are still written on 0 so
callers that ignore the flag keep receiving finite values, but they are only
bounded when the flag is 1. Test: `test_rt_game3d_dialogue_facial`
(inside-near-plane case).
