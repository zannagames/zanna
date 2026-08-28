---
status: active
audience: contributors
last-verified: 2026-08-28
---

# ADR 0300: Enter an Animation State at a Time Offset

## Status

Accepted (2026-08-28)

## Context

`AnimController3D.Crossfade(state, blend)` always enters the destination clip
at time zero. A game that drives animation from a choreography clock (Legacy
Baseball's play scripts) notices a state mark on the first rendered frame at
or after the mark, so the clip starts up to one frame late relative to the
mark. The controller then reports a state time that is one frame behind
`playhead − mark` for the rest of the clip. At a bat barrel's ~100 ft/s that
is a foot of position per 10 ms; at 60 Hz the bat's contact frame lands
anywhere inside a 1.6-foot band, and the ball — compiled to the measured
contact point — visibly misses it. Every other one-shot action clip (throw
releases, glove closes) carries the same frame-phase error, which is why
probes had to seek at 10 ms steps to sample "at" a mark.

`AnimPlayer3D` already has a settable `Time`; the controller had no way to
combine a blend with an entry offset. Adding a registry method is a runtime
surface change (ADR 0006).

## Decision

Add `AnimController3D.CrossfadeAt(state, blendSeconds, startSeconds)`
(`rt_anim_controller3d_crossfade_at`). It performs exactly what `Crossfade`
does, then seeks the base layer's player to `startSeconds` (clamped by the
player: non-looping clips clamp to their duration, looping clips wrap) and
recomputes the final palette. The outgoing pose of the crossfade is
untouched; entry events between zero and `startSeconds` are not fired.
A NEGATIVE start holds the entry pose (clip and blend) for |start| of update
time before running: the case where the mark was noticed inside the current
frame, whose animation step would otherwise put the clip ahead of the clock.
Non-finite starts enter at zero, so `CrossfadeAt(s, b, 0)` is
`Crossfade(s, b)`.

Legacy Baseball's rig enters every one-shot action state at
`(playhead − markMs − frameDt)` — negative when the mark fell inside this
frame — so after the frame's animation step the state time equals the
choreography clock on every frame regardless of frame phase.

## Consequences

- Registry: +1 function, +1 method (inventory pins re-reviewed in the game's
  `verify_generated_inventory.sh`).
- A caller that relies on time-zero entry events must keep using
  `Crossfade`, or accept that events inside the skipped window are not fired.
- Test: `test_rt_animcontroller3d` — `CrossfadeAt` reports the requested
  state time, poses accordingly after a step, rejects unknown states, and
  treats a negative start as zero.
