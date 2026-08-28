---
status: active
audience: contributors
last-verified: 2026-08-28
---

# ADR 0302: Animation Transition Continuity

## Status

Accepted (2026-08-28)

## Context

Legacy Baseball's owner reported that players "flash into another position
instead of transitioning". The game requests 40–200 ms crossfades for every
state change, but three `AnimController3D` behaviours render hard cuts anyway:

1. **Blend-tree install and clear are instantaneous.** `SetBlendTree` swaps
   `controller->blend_tree` and recomputes the palette synchronously, and
   `controller_compute_final_palette` uses the tree's locals exclusively while a
   tree is attached. Entering a locomotion tree (crossfade, then attach) and
   leaving one (detach, then crossfade) both replace the whole pose in one
   frame; the requested fade is never rendered.
2. **A finished one-shot cannot be faded out of.** `controller_set_layer_state`
   only crossfades when `rt_anim_player3d_is_playing()`; a non-looping clip stops
   at its endpoint, so throw → idle, catch → idle and umpire-call → idle are
   frame-zero cuts regardless of the blend the caller asked for.
3. **A retriggered fade discards the in-flight blend.** `AnimPlayer3D` has one
   `crossfade_from` slot; a crossfade issued mid-fade departs from the previous
   clip at its time, not from the blended pose on screen.

Animation LOD compounds all three: a throttled controller skips whole updates
and only advances `transition_time` on accepted ticks, so a short fade on a far
actor collapses into one frame of A then one frame of B.

Adding registry methods is a runtime surface change (ADR 0006).

## Decision

Two opt-in controller methods; both defaults reproduce today's palettes
bit-exactly.

- **`AnimController3D.SetBlendTreeFade(seconds)`** — `SetBlendTree` becomes a
  weight ramp. The controller carries `blend_tree_weight` (ramped linearly from
  `blend_tree_fade_from` toward 0/1 over the fade, advanced by `Update` after
  the layers and before the tree update, the final step landing exactly on the
  target). `controller_compute_final_palette` seeds from the tree when the
  weight is 1 (the historical branch, byte for byte), from layer 0 otherwise,
  and blends layer 0 toward the tree locals with
  `controller_blend_local_toward` — the overlay layer's non-additive blend
  extracted into a helper — while the weight is strictly inside (0, 1). A
  cleared tree stays retained until its weight reaches zero
  (`blend_tree_detach_pending`) and is released before the palette is
  composed. Re-attaching during a fade-out reverses the ramp from the current
  weight. Tree-to-tree replacement keeps the current weight (that swap stays
  instantaneous — documented limitation). With `seconds == 0` (default) the
  weight is set to 1/0 directly and nothing changes.
- **`AnimController3D.SetTransitionContinuity(enabled)`** — pushed to every
  layer player:
  - `controller_set_layer_state` also crossfades when the player merely
    *holds* a pose (a finished one-shot on its last frame, a stopped player at
    bind), not only when it is playing.
  - `rt_anim_player3d_crossfade` freezes the CURRENT blended local pose as the
    fade source (`crossfade_from_pose`, per-bone TRS, lazily allocated) whenever
    a fade is issued while another is in flight or from a non-playing clip;
    `compute_bone_palette` reads the frozen slot instead of sampling a from-clip
    and the frozen source holds still for the fade. A retrigger from a
    still-playing clip with no fade in flight keeps the moving-clip source, so
    gait→gait fades are unchanged.
  - `rt_anim_controller3d_update` evaluates every tick while a layer transition
    or tree ramp is in flight (the LOD remainder is consumed into the step), so
    fades never collapse to cuts on throttled controllers.
- Registry: two `RT_FUNC`/`RT_METHOD` entries; the 3D ABI manifest hash and
  function/method counts move (2266 → 2268, 1222 → 1224); the generated
  runtime docs and the game's inventory pins (fn 8007 → 8009, method
  5761 → 5763) are re-pinned. The stubs translation unit gains the two new
  entry points (and the ADR 0300 `CrossfadeAt` stub it was missing).

## Consequences

- Game code that opts in gets rendered fades at every gait boundary, out of
  every finished action clip, and through retriggered marks; the outgoing
  motion freezes for the remainder of a retriggered fade (40–200 ms in the
  game), which is the standard frozen-pose transition. True inertialization can
  be layered on later without another surface change.
- Controllers that never call the new methods are bit-identical: the
  exclusive-tree branch and the `is_playing` gate are unchanged paths, the
  overlay blend is a pure helper move, and the frozen-pose buffer is never
  allocated.
- Legacy Baseball's `watch3d_anim_probe` digests state marks, not poses, so
  opting in moves no pinned digest; probes that capture a rigged actor
  mid-transition re-pin.

## Tests

`test_rt_animcontroller3d`: crossfade from a finished one-shot blends (with the
legacy hard cut pinned as the default); crossfade out of a stopped layer blends
from bind; blend-tree fade produces intermediate palettes, retains the tree
until weight 0, and reverses continuously on re-attach; fade 0 matches the
legacy controller bit-exactly through attach/update/detach; a retrigger
mid-crossfade departs from the blended pose (legacy control pops); continuity
keeps the moving source when the previous clip is still playing.
`test_graphics3d_runtime_manifest` re-pinned.

## References

- [ADR 0300](0300-animation-state-entry-at-offset.md) — entry-time offsets the
  same controller path honours.
- `src/runtime/graphics/3d/anim/rt_skeleton3d_player.inc`,
  `rt_animcontroller3d_{api,internals,sampling}.inc`,
  `src/il/runtime/defs/graphics3d/extras.def`.
