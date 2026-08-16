---
status: active
audience: contributors
last-verified: 2026-08-16
---

# ADR 0250: Deterministic, Seekable Playback Primitives

## Status

Accepted (2026-08-16)

## Consulted

- `src/runtime/game/rt_tween.c` — frame-counted interpolation
- `src/runtime/game/rt_smoothvalue.c` — per-frame exponential damping
- `src/runtime/game/rt_timer.c` — the existing dual frame/millisecond API
- `src/runtime/core/rt_math.c` — interpolation shaping
- `src/il/runtime/defs/api/vectors.def`, `.../classes/input_game.def` — registry

## Context

The runtime's animation helpers are frame-counted. `rt_tween_update` increments
`elapsed` by exactly one per call (`rt_tween.c`), so a tween's duration is a
frame count and its motion is frame-rate dependent. `rt_smoothvalue_update`
applies one exponential step per call with the same consequence — its own header
says the factor applies *"independent of elapsed wall time"*.

That is workable for a fixed-step game loop and unusable for anything that must
be reproducible under an arbitrary dt:

- a replay or headless capture stepping at a synthetic rate,
- a paced cutscene whose beats are authored in milliseconds,
- a scrubbable timeline that must land on the same pose however it got there,
- any golden-output test that renders on machines with different frame rates.

`Zanna.Game.Timer` already solved this: it carries **both** modes side by side
(`Start`/`Update` in frames, `StartMs`/`UpdateMs`/`get_ElapsedMs` in
milliseconds) and refuses a mismatched call rather than reinterpreting units.
`Tween` and `SmoothValue` were simply never brought to the same shape.

Two smaller holes compound it:

- **`Zanna.Math` has 28 easing curves and no smoothstep**, despite the runtime
  implementing smoothstep internally four separate times (rail camera, terrain
  builder, timeline easer, Perlin lattice). `InverseLerp` and `Remap` — the
  natural companions to the existing `Lerp` — are also absent.
- **There is no bit-exact integer interpolation.** `Zanna.Math.Lerp` is
  deliberately *not* `a + t*(b-a)`: it carries NaN guards, exact `t==0`/`t==1`
  short-circuits, and switches to `(1-t)*a + t*b` when the endpoints straddle
  zero. That is the right choice for a float lerp and makes it unusable where
  bit-exactness across backends is the requirement.

The Legacy Baseball audit found four independent re-implementations of
"interpolate from A to B over N ms, advanced by explicit dt" and nine
hand-damped camera accumulators written as `f = dt/(dt+tau)` precisely because
`SmoothValue` could not express it.

## Decision

### `Zanna.Game.Tween` — millisecond mode

| Member | Signature | Notes |
|---|---|---|
| `StartMs(from, to, durationMs, ease)` | `void(f64,f64,i64,i64)` | Sets ms mode. |
| `UpdateMs(dtMs)` | `i1(i64)` | Advances by wall time; saturates rather than overflowing. |
| `SeekMs(ms)` | `i1(i64)` | **Absolute, path-independent** position. Seeking backwards un-completes and resumes. |
| `ProgressPermille` | property `i64` | Eased progress in 0..1000, integer-exact. |
| `LerpIntPermille(from, to, t)` | static `i64(i64,i64,i64)` | Integer end to end; identical on every backend. |
| `IsMs` | property `i1` | Which mode the tween is in. |
| `ReduceMotion` | property `i1` | When set, a subsequent Start lands on the target and reports complete, so an accessibility preference needs no special-casing at any call site. Pairs with `GUI.ThemePalette.SetMotionEnabled`. |

**The mode is sticky.** `UpdateMs` is a no-op on a frame-mode tween and
`Update` is a no-op on a ms-mode tween — a mismatched call cannot silently
reinterpret the units. This mirrors `rt_timer_update_ms`'s existing guard.

`SeekMs` is the member that makes a timeline scrubbable. A pure `UpdateMs`
accumulator is not sufficient on its own: a probe that seeks to 1500 ms must
land on the same value whether it arrived by one jump, sixty steps, or a seek
past the end and back.

### `Zanna.Game.SmoothValue` — time-constant damping

| Member | Signature | Notes |
|---|---|---|
| `UpdateMs(dtMs)` | `void(i64)` | Uses `f = dt / (dt + tau)` — the standard frame-rate-independent form. |
| `TimeConstantMs` | property `i64` | The response as a millisecond time constant: the value covers ~63% of its remaining distance every `tau`. |
| `SnapToTarget()` | `void()` | Immediate convergence, clearing velocity. |

`TimeConstantMs` is stored as the equivalent 60 Hz smoothing factor, so
`Smoothing`, `Update()`, and `UpdateMs()` stay mutually consistent and a value
configured the old way still behaves sensibly under the new call.

### `Zanna.Math` — interpolation shaping

`SmoothStep(edge0, edge1, x)`, `SmoothStep01(t)`, `SmootherStep01(t)`,
`InverseLerp(a, b, v)`, `Remap(v, inLo, inHi, outLo, outHi)`,
`LerpInt(a, b, num, den)`.

`LerpInt` stays in integer arithmetic and is therefore bit-exact across
backends — the deterministic counterpart to `Lerp`. `InverseLerp` and `Remap`
deliberately do **not** clamp, so callers can detect out-of-band inputs;
compose with `Clamp` when a bounded parameter is wanted.

## Consequences

- A deterministic presentation layer can use the runtime's tween and damping
  helpers instead of hand-rolling dt-driven interpolation. `SeekMs` in
  particular removes the main reason projects write their own span evaluators.
- Existing frame-mode behaviour is untouched: the new state defaults to frame
  mode and reduce-motion off, and `Update()`'s code path is unchanged.
- `TimeConstantMs`'s 60 Hz anchor is a convention, not a physical constant. It
  is documented on the accessor because a caller reading `Smoothing` after
  setting `TimeConstantMs` needs to know how the two relate.
- **Not addressed here:** a general span-track evaluator
  (`(fromMs, toMs, payload)`, last-applicable-wins) is still missing, and
  `Game3D.FollowController` / `ThirdPersonController` / `RailCamera3D` still
  take a damping scalar with no documented time base. Both are follow-on work.
- No IL opcode, grammar, or verifier changes. Registry surface only: 8 new
  members on `Zanna.Game.Tween`, 4 on `Zanna.Game.SmoothValue`, 6 on
  `Zanna.Math`.

## Links

- ADR 0249 — replayable RNG state (companion determinism work)
- `baseball/plans/58-runtime-adoption.md` — the audit that surfaced the gap
