---
status: draft
audience: contributors
last-verified: 2026-08-03
---

# ADR 0240: Linux GPU Direction — OpenGL Parity over Vulkan

## Status

Proposed (decision recorded)

## Context

Linux's GPU story trails macOS/Windows: the OpenGL backend (12.3k LOC) has
one default-lane test, no point-shadow atlas (GAP-8), and thinner
capability coverage. The strategic question: invest OpenGL to parity, or
start a Vulkan backend.

## Decision

**Bring OpenGL to capability parity; do not start Vulkan.**

- The backend contract is the vtable capability surface (ADR era:
  `clustered_lighting`, `shadow_csm`, `BackendSupports`), and OpenGL 3.3+
  covers every capability the engine actually uses — clustered forward+,
  CSM/point shadows, HDR targets, TAA. Nothing Zanna renders needs
  Vulkan-only features.
- A Vulkan backend is a multi-quarter, ~30k-LOC undertaking (explicit
  sync, memory allocation, pipeline caches) whose payoff is performance
  headroom Zanna's scenes don't yet demand — while doubling the Linux test
  matrix and violating the "every feature complete on all platforms"
  principle for its entire buildout.
- Parity work is enumerable and small by comparison: the point-shadow
  atlas (GAP-8), capability-flag truthfulness, a default-lane test batch
  mirroring the Metal set, and perf baselines. All of it requires a Linux
  host and is queued for the next Linux session.

Revisit only if a concrete scene demonstrates an OpenGL-bound bottleneck
that clustered forward+ cannot mitigate, or if a target platform drops GL.

## Consequences

- GAP-8 and the GL test batch become ordinary backlog items, not a
  platform decision.
- Wayland/EGL work (already landed) remains the presentation layer for
  both current and any hypothetical future backend.

## Execution status (2026-08-13 Linux session)

The parity items above were queued for "the next Linux session". Landed:

- **Display-transform investigation.** The Linux session exposed a large
  cross-backend colour gap and promoted the post-FX conformance gate. Its
  initial display-referred interpretation was superseded during integration
  by ADR 0247's backend-wide scene-linear, single-encode contract and absolute
  ACES anchors; the OpenGL and D3D11 tone curves therefore retain linear input.
- **Presentation.** `ZANNA_OPENGL_PRESENT` now defaults to `auto`, so
  X11/GLX trusts the writability probe instead of always paying a
  full-screen `glReadPixels` plus CPU blit; a failed resolve demotes the
  context permanently via `gl_demote_to_offscreen_present`.
- **GPU frame timing.** GAP-9 narrowed to macOS-only.
- **Test batch.** `zia_graphics_conformance_cross_backend` — the only
  automated GPU colour gate — carried the `slow` label and so never ran
  under `ctest -LE slow`. That is why the display-transform regression
  shipped green. It is a default-lane test now, with `--postfx`.

Still outstanding, and each for a stated reason rather than by oversight:

- **GAP-8 point/omni shadow atlas.** `shadow_atlas_slots` is a static
  field on the backend vtable, so a GL implementation gated on a runtime
  `GL_MAX_TEXTURE_IMAGE_UNITS` probe cannot advertise itself without a
  per-context capability query — a backend-contract change that needs its
  own ADR. The unit budget itself is solvable within GL 3.3's 16-sampler
  minimum by collapsing `uShadowTex0..3` into one array/atlas sampler,
  which also frees a dedicated unit for the BRDF LUT and removes the
  splat-terrain aliasing.
- **Native compressed textures.** `gl_get_native_texture_caps` withholds
  BC/ASTC/ETC2 deliberately: some Mesa paths accept the upload and then
  sample garbage, so the caps need an upload-and-sample conformance gate,
  not just the extension probes that already exist.
- **Vsync.** The GLX `swap_control` fallbacks and Wayland frame-callback
  pacing are untouched; Zanna owns `wl_display` dispatch, so a blocking
  EGL swap would dispatch the caller's queue.
