---
status: active
audience: contributors
last-verified: 2026-08-28
---

# ADR 0299: Sample Bound Render Targets Natively in Material Slots

## Status

Accepted (2026-08-28)

## Context

`Material3D.SetAlbedoRenderTarget` / `SetEmissiveRenderTarget` and any
`Material3D.SetTexture` (including the transient material behind
`Canvas3D.DrawImage2D`) accept a `RenderTarget3D`. Until now every backend
served that binding through the target's CPU mirror:
`rt_rendertarget3d_material_pixels` re-reads the GPU colour texture into a
Pixels object whenever the target's `content_revision` advances, and the
backend then uploads that Pixels as an ordinary texture. On Metal the readback
is `[pendingCommandBuffer waitUntilCompleted]` followed by a blit copy — a full
CPU/GPU serialization every time the bound target changes.

Legacy Baseball's live jumbotron (redrawn every few frames) and the new
picture-in-picture runner insets (an offscreen scene pass composited into the
HUD every frame) both pay that stall on the render thread; the inset made the
broadcast visibly stutter. The retained Metal render-target cache already keeps
a `ShaderRead`-capable colour texture per target after the target is unbound,
so the round trip is avoidable.

The draw-command layout (`vgfx3d_draw_cmd_t`) and the backend capability mask
are part of the internal runtime C ABI, so ADR 0006 requires this record.

## Decision

1. `vgfx3d_draw_cmd_t` gains `texture_target` and `emissive_map_target`
   (`const vgfx3d_rendertarget_t *`). `canvas3d_fill_material_cmd` sets them
   — and leaves the matching Pixels fallback NULL — when the material slot is
   a `RenderTarget3D` **and** the canvas backend advertises the new capability
   `RT_CANVAS3D_BACKEND_CAP_RENDER_TARGET_SAMPLING` (0x2000000000). Backends
   without the bit keep resolving the slot to the Pixels mirror exactly as
   before. `vgfx3d_draw_cmd_has_base_texture` / `_has_emissive_map` are the
   shared "is this slot textured" tests so every backend agrees on
   `texture_required` skipping and shader flags.
2. The Metal backend advertises the capability and binds the cached target
   texture directly (`metal_render_target_sample_texture`). No wait is
   inserted: command buffers on one queue execute in commit order, so the
   frame that rendered into the target has completed before the sampling draw
   executes. A target that is the current pass's own attachment is never
   sampled. The entry's `lastUsedFrame` is stamped so the periodic prune keeps
   it resident. The draw command also carries the RenderTarget3D wrapper
   (`texture_target_owner`); a target with no native storage on this device —
   one rendered by another canvas or backend, such as a software offscreen
   canvas feeding a jumbotron — is served through its Pixels mirror exactly
   as before.
3. **Render-target frames are post-FX resolved.** The window path skipped the
   post-FX chain for RTT frames, so a target held raw scene-referred linear
   radiance and read dark wherever it was shown. At the end of every Metal
   frame rendered into a target, `metal_encode_render_target_display` runs the
   canvas's ordered chain at the target's own size into a per-entry BGRA8
   display image (tone curve, exposure, colour grade, LUT, vignette, FXAA,
   sharpen; the passes that read window-sized scene buffers — bloom, SSAO,
   DOF, motion blur, TAA, SSR — are skipped). Native sampling prefers that
   display image when it is valid, so an RT-as-texture carries the same
   display encoding as any Pixels texture (PBR materials decode it as sRGB
   albedo, unlit HUD blits pass it through). A canvas without a usable chain
   leaves the raw colour texture in place (the historical look). The CPU
   mirror (`AsPixels`, `CopyTo`, mirror-only backends) is unchanged and stays
   scene-referred. *Partly superseded by [ADR 0301](0301-display-referred-render-target-sampling.md):
   this resolve was unreachable because the canvas published a NULL chain on
   render-target frames; ADR 0301 makes it run and additionally resolves the
   MATERIAL mirror on mirror-path backends (`AsPixels`/`CopyTo` stay
   scene-referred).*
4. OpenGL and Direct3D 11 keep the mirror path. Both own a single bound RTT
   (`rtt_fbo` / `rtt_color_tex`) that is reassigned on rebind, so there is no
   retained per-target texture to sample; adding one is a separate decision.
   The software backend is the mirror path by construction and stays
   byte-identical (probes, bakes, goldens).
5. `Canvas3D.DrawImage2D` documents that a `RenderTarget3D` is an accepted
   source. No registry surface changes: the parameter was already typed `obj`
   and the material slot already accepted the class.

Two neighbouring fixes ride the same record because they change backend
behaviour that the game observed while diagnosing the stutter:

- Every render pass — the overlay included — now starts with a fresh
  per-frame texture-upload budget on Metal, OpenGL, and D3D11. Previously the
  overlay pass consumed whatever the scene pass had left, so a hard camera cut
  that streamed new scene textures could budget-pause a HUD image quad, which
  since the texture-required change draws as nothing for that frame.
- The Metal and OpenGL resident 2D texture caches grow from 256 to 512
  entries, matching the Canvas3D antialiased-text raster LRU (512). A backend
  cache smaller than the canvas cache thrashes and re-uploads every frame once
  the overlay working set exceeds it.
- `AnimController3D.SetAnimationLod` keeps its accumulated interval when it is
  re-programmed with the rate it already runs at; only a rate change restarts
  the accumulator. Re-applying an unchanged throttle (a camera cut re-gating
  an already-throttled actor) used to drop up to one interval of pose time and
  read as a one-frame freeze.

## Consequences

- RT-bound materials and HUD blits of a RenderTarget3D are stall-free and
  display-encoded on Metal; other backends behave exactly as before.
- A target meant to be shown through the chain should be allocated with
  `RenderTarget3D.NewHdr` so the tone curve sees unclamped radiance; a UNORM8
  target resolves from clamped linear.
- A material bound to a target that has never been rendered into samples
  nothing on Metal (the draw is skipped when `texture_required`, otherwise the
  base colour shows), matching the blank mirror those backends would show.
- Draw sorting keys mix the native target pointer so RT-textured draws keep
  stable batching.

## Tests

- `test_rt_canvas3d`: `DrawImage2D` accepts a RenderTarget3D on the software
  backend and blits its last completed frame through the mirror path.
- `test_rt_animcontroller3d`: re-applying the same LOD rate preserves the
  pending accumulator; a rate change resets it.
- Legacy Baseball `watch3d_runner_insets_probe` (game repository) pins the
  inset composite through this path.
