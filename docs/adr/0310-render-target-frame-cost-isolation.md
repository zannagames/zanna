---
status: active
audience: contributors
last-verified: 2026-09-01
---

# ADR 0310: Render-Target Frame Cost Isolation

## Status

Accepted (2026-09-01)

## Context

ADR 0309 let render-target frames inherit non-cascade shadow slots, but a game
interleaving a per-frame offscreen bracket with its window frame (Legacy
Baseball's runner inset) still paid several structural costs the throttle and
inheritance did not touch:

1. **Metal opened one empty full-resolution scene pass per shadow slot.**
   `metal_shadow_end` eagerly reopened the scene encoder (Load/Store on
   color + motion + depth) and the next `metal_shadow_begin` immediately
   closed it — N zero-draw passes per frame, each a full tile load + store of
   every attachment on a TBDR GPU (~118 MB at 1440p on the post-FX route).
2. **The opaque-depth snapshot thrashed.** A single size-keyed cache texture
   alternated between the RT and window extents, allocating and deferred-
   freeing a full-resolution D32F texture twice per displayed frame.
3. **CPU occlusion culling collapsed.** The occlusion history wiped whenever
   the bound render-target identity changed (every bracket), and covered
   streaks required consecutive `frame_serial` values — which every canvas
   `Begin` advances, RT brackets and view-model passes included. With an
   interleaved bracket the streaks never warmed and the window frame lost the
   culling that pays for large-scene geometry. The RT bracket also ran the
   Hi-Z occluder rasterizer (an 8192-triangle CPU budget) for a small
   offscreen target.
4. **Metal had no GPU frame timing** (`get_frame_gpu_time_us` unimplemented),
   so none of this was measurable on the platform where it mattered.
5. **RT frames re-rendered the full cascade rig.** By design ADR 0309 keeps
   cascades camera-fitted; a 4-cascade 4096² rig per inset refresh dwarfs the
   inset's own rendering.

Registry members, a shared-code behavior change, and a backend vtable
implementation are involved, so this ADR records the batch.

## Decision

1. **Metal lazy scene encoder.** The scene pass is *requested* (load actions +
   target kind recorded) and only *materialized* by the first draw that needs
   it. Requests merge (a clear is sticky over a load); a pending pure-load
   pass is dropped silently — an empty Load/Store pass is a no-op, and that
   drop is the optimization; a pending clear can never be lost: it lands at
   materialization, at the target-kind-change flush inside a shared command
   buffer, or as a one-shot empty clear pass at `metal_finish_encoding` — the
   single choke point every commit, post-FX, RT display resolve, present, and
   readback path routes through. `metal_shadow_end`/`metal_leave_shadow_pass`
   now request instead of materializing, so N shadow slots produce zero
   interstitial scene passes. `metal_begin_frame` keeps its failure semantics
   through an availability probe that still acquires the swapchain drawable
   eagerly. D3D11/GL shadow transitions only rebind targets and need nothing.
2. **Per-target-kind opaque-depth snapshot slots** (`opaqueDepthWindow` /
   `opaqueDepthRtt`); the active binding is unchanged. Steady-state memory
   rises by one RT-sized depth texture for apps using transparency in both
   bracket kinds.
3. **Occlusion isolation.** A new internal `occlusion_frame_serial` advances
   only on window-backed, non-view-model camera passes and replaces
   `frame_serial` in the streak/prune logic (`frame_serial` keeps its
   every-Begin semantics for streaming, motion history, overlay caches, lens
   flares, and instanced batching). Render-target and view-model frames
   neither wipe the history, nor advance the serial, nor run the occlusion
   grid / Hi-Z rasterizer at all (`use_occlusion_grid` gates on
   `!render_target && !frame_is_view_model`); frustum culling stays active
   everywhere. This also repairs the pre-existing streak breakage caused by
   view-model passes, and covers reflection-probe bakes automatically.
4. **Metal `get_frame_gpu_time_us`.** Every committed command buffer's
   `GPUEndTime − GPUStartTime` is summed into an epoch that advances at
   present, so an RT bracket, the window frame, and any overlay replay sum
   into one displayed-frame figure. The published value trails 1–2 displayed
   frames (it lands when the first buffer of the next epoch completes) and is
   0 until then — the GL timer-ring contract. Handlers run on Metal's
   callback queue under `@synchronized`; errored buffers report zeros and are
   ignored.
5. **`Canvas3D.SetRenderTargetShadowCascadeLimit(n)`** (0 = no cap, default):
   render-target frames clamp their cascade count to `n`. The split
   computation always extends the last cascade to the far depth, so one
   cascade stays correct for a small offscreen subject. Window frames are
   never affected and RT frames never write the inherit cache. With ADR 0309
   inheritance active, the unused cascade slots `[limit, cached cascades)`
   re-arm as **fillers** through the same `shadow_inherit` hook — backends
   compute their shadow count as the contiguous complete-slot prefix, and
   without fillers every inherited slot above the gap would be sanitized
   away. Fillers are classic CSM slots, so atlas re-arm legality is
   untouched. Bonus: with a cap, RT frames stop overwriting the window
   frame's cascade textures and signatures for slots ≥ limit.

## Amendment to ADR 0309

ADR 0309's night-game example claimed the stadium mast lights were shadowed
point lights ("six atlas cube faces each"). In Legacy Baseball the masts are
**spot** lights — one atlas tile each. Inheritance applies identically (spot
selection is world-space), but the per-frame savings the ADR projected for
night games were overstated, and a day game (sun-only) inherited nothing at
all — which is what motivated the cascade limit above.

## Consequences

- The interstitial-pass bandwidth disappears for every Metal frame with
  shadows, not just inset frames.
- RT frames lose CPU occlusion culling; for a hypothetical app rendering a
  huge scene only into render targets this removes an optimization
  (documented trade — frustum culling remains, and the history was already
  being destroyed by the very interleaving that makes RT frames common).
- `Canvas3D.FrameGpuTimeUs` becomes meaningful on macOS.
- Considered and deferred: trimming the fixed 16 KB identity-padded
  bone-palette upload to `bone_count` entries. The 256-slot layout is
  load-bearing in the MSL clamp (`min(paletteBase + boneIdx, 255u)`) and in
  the instanced `skinBase`/`instance_bone_stride` packing; the benefit is a
  few MB/frame of CPU memcpy. Revisit only with `FrameGpuTimeUs` /
  `PassCpuMs` evidence.
- Fixtures: `test_canvas3d_rt_cascade_limit.zia`,
  `test_canvas3d_rt_occlusion_isolation.zia`,
  `test_canvas3d_frame_gpu_time.zia` (software + Metal lanes);
  `test_canvas3d_rt_shadow_inherit.zia` unchanged-green is the ADR 0309
  compatibility gate.
