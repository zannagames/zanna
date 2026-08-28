---
status: active
audience: contributors
last-verified: 2026-08-28
---

# ADR 0301: Display-Referred Render-Target Sampling on Every Backend

## Status

Accepted (2026-08-28)

## Context

ADR 0299 made a bound `RenderTarget3D` a native texture source on Metal and
added an end-of-frame display resolve (`metal_encode_render_target_display`)
that runs the canvas's post-FX chain at the target's own size into a per-target
BGRA8 display image, so a target shown as a texture carries the same display
encoding as any other texture. That resolve never ran. `Canvas3D` latches its
post-FX state once per frame through `canvas3d_backend_uses_gpu_postfx()`,
which requires `render_target == NULL`; on every frame rendered into a target
the canvas therefore published a NULL chain to the backend, Metal reset its
chain (`gpuPostfxChainValid = 0`), and the resolve early-returned. Native
sampling then fell back to the RGBA16Float scene-linear colour attachment, and
the overlay blit — an unlit passthrough into a non-sRGB BGRA8 layer — put
linear radiance on screen as display code values. Legacy Baseball's
base-runner picture-in-picture insets read at roughly half the main frame's
brightness with crushed shadows (owner report 2026-08-28).

The same latch had a second cost. `set_gpu_postfx_enabled` is the WINDOW
present-route toggle; on Metal each 1→0→1 flip commits pending work and
reallocates every main target (scene, overlay, post-FX, bloom, TAA), and the
NULL snapshot that followed cleared TAA history. A game that renders one
target and then the window every frame paid two target rebuilds per frame and
never accumulated TAA. OpenGL and Direct3D 11 dropped their temporal state the
same way.

Mirror-path backends (software, OpenGL, Direct3D 11, and Metal's fallback for a
target it has no native storage for) were never display-referred either: the
CPU readback range-compresses HDR targets with a bare Reinhard curve and copies
LDR targets clamped-linear, with no display gamma and none of the chain. The
`AsPixels` doc comment claimed otherwise.

## Decision

1. **Split "present through GPU post-FX" from "chain published".**
   `rt_canvas3d` gains `frame_postfx_chain_latched`. `canvas3d_latch_gpu_postfx_state`
   captures the chain whenever the backend accepts snapshots
   (`canvas3d_backend_accepts_gpu_postfx_chain`, no render-target term) and sets
   `frame_gpu_postfx_enabled` only for window frames. `canvas3d_apply_gpu_postfx_state`
   publishes the snapshot on every frame and calls `set_gpu_postfx_enabled` only
   when no render target is bound, so the window route is never toggled by an
   interleaved render-target frame (no target churn, TAA history survives).
   `canvas3d_frame_needs_motion_vectors` stays keyed on the present flag: a
   render-target frame strips the passes that read motion data.
2. **Metal resolves on the chain alone.** `metal_encode_render_target_display`
   no longer consults `gpuPostfxEnabled`; the decision is the pure
   `vgfx3d_metal_should_resolve_render_target_display(chain_valid,
   pipelines_ready, has_command_buffer)` in the CI-tested shared translation
   unit. Vignette is excluded from the target resolve: a sampled target is a
   composited element, and a vignette at its own size darkens its corners
   inside the host frame.
3. **Mirror-path parity.** When a frame into a target ends, the canvas records
   the chain it rendered under on the `RenderTarget3D` wrapper
   (`display_chain`, `display_chain_revision`). `rt_rendertarget3d_material_pixels`
   — the mirror materials and `DrawImage2D` sample — runs the new internal
   `rt_postfx3d_resolve_display_pixels` over the refreshed mirror exactly once
   per `content_revision`: tone curve + exposure + gamma once (mode-0 explicit
   entries encode a linear source, as on the GPU), colour grade, LUT, FXAA and
   sharpen, in chain order; bloom/SSAO/DOF/motion blur/TAA/SSR/auto-exposure/
   sun shafts/vignette are skipped (window-sized, temporal, or excluded by 2).
   A linear HDR CPU mirror is preferred as the source; otherwise the 8-bit
   clamped-linear colour is unpacked. The resolve runs sequentially with no
   worker pool, so the software backend stays deterministic.
   `AsPixels`, `CopyTo`, `TryReadRgba` and the target's `color_buf` stay
   scene-referred exactly as ADR 0299 §3 promised; `Flip()` with a target still
   bound (the documented CPU-chain route) invalidates the recorded chain so the
   mirror is never encoded twice.
4. No registry or C-ABI surface changes: internal structs grow, no new runtime
   methods. The 3D ABI manifest, generated runtime docs and the game's
   inventory pins do not move.

## Consequences

- A target sampled as a texture is display-referred on every backend whenever
  the canvas that rendered it carried a usable chain; without a chain it keeps
  its historical scene-referred bytes.
- A software offscreen canvas feeding a Metal-sampled target (a jumbotron)
  now records its chain too: bytes are unchanged unless that canvas installs
  a `PostFX3D` chain.
- Window frame history and TAA accumulation survive interleaved render-target
  frames; a lone render-target frame no longer costs two main-target rebuilds.
- Headless digests are unaffected: `AsPixels`/`color_buf` are unchanged and
  no existing probe samples a chain-rendered target as a material.

## Tests

- `test_rt_canvas3d`: a render-target frame publishes the chain and leaves the
  present hook untouched; a sampled HDR target rendered under a tonemap chain
  reads display-encoded from its material mirror (0.5 linear → 0xBA ± 2) while
  `AsPixels` stays 0x80; a canvas without a chain leaves the mirror equal to
  `AsPixels`; a repeated mirror read does not re-encode.
- `test_rt_postfx3d_cpu`: the display resolver applies only the resolve subset
  (bloom + SSAO + tonemap chain ≡ tonemap-only) and prefers the linear HDR
  mirror.
- `test_vgfx3d_backend_metal_shared`: the resolve predicate ignores the window
  present route.
- Legacy Baseball `watch3d_runner_insets_probe` gains a rendered luma gate on
  the software backend (fails on the pre-ADR tree).

## References

- [ADR 0299](0299-native-render-target-sampling.md) — the resolve this record
  makes reachable (§3 partly superseded: the mirror is now display-referred).
- `src/runtime/graphics/3d/render/rt_canvas3d_frame_postfx.inc`,
  `rt_rendertarget3d.c`, `rt_postfx3d.c`,
  `src/runtime/graphics/3d/backend/vgfx3d_backend_metal_draw.inc`.
