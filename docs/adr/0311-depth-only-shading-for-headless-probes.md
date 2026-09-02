---
status: active
audience: contributors
last-verified: 2026-09-01
---

# ADR 0311: Depth-Only Shading for Headless Probes

## Status

Accepted (2026-09-01)

## Context

Headless verification probes (Legacy Baseball's `replay_theater_probe`,
`watch3d_live_probe`, every `W3_TIER_HEADLESS` stage probe) drive the real
`Game3D.World3D` pipeline on the software backend for thousands of frames and
assert on state — plays settled, entity population, draw counts, hitch counts,
choreography digests. None of those gates reads a pixel. A `sample` of
`replay_theater_probe` showed 92 % of its main-thread time under
`rt_canvas3d_end → canvas3d_render_main_pass → sw_shade_fragment →
sw_shade_lighting_pbr`: the software rasterizer's full per-fragment PBR path
for a 240×135 frame whose colour is discarded.

Two facts compounded it. The Debug tree compiled the C runtime object
libraries with no optimization flag at all (only C++ targets received `-Og`
through `zanna_common_opts`), and the in-house Mach-O linker rejected the
`SUBTRACTOR` relocations that optimized C objects carry in `__eh_frame`, so
"just build the runtime at -O2" had never been tried.

## Decision

1. **`Canvas3D.SetDepthOnlyShading(enabled)` / `Canvas3D.DepthOnlyShading`.**
   A retained per-canvas flag copied into `vgfx3d_camera_params_t` at every
   `begin_frame`. With it set, the software backend still runs the vertex
   stage, clipping, the depth test, and the opaque depth write (so occlusion,
   later draws, draw counters and hitch counters see the same scene), but it
   never calls the fragment shader and never writes colour. Blend and additive
   draws neither shade nor write depth, matching their full-shading depth
   semantics. GPU backends ignore the flag; it is a probe-side instrument, not
   a quality tier. Shadow passes are unchanged (they are already depth-only).
2. **Capture paths own the flag.** A stage arms depth-only when its tier is
   headless and clears it whenever a caller enables pixel readback
   (`setCaptureReadback(true)`, the inset-luma seam). Probes that read pixels
   already arm readback; probes that do not get the saving for free.
3. **Fast Debug optimizes the C runtime.** `src/runtime/CMakeLists.txt` adds
   `$<$<CONFIG:Debug>:-O2>` to the runtime object libraries when
   `ZANNA_FAST_DEBUG` is on. `-g`, assertions and backtraces stay.
4. **The Mach-O linker lowers symbol differences.** `ObjReloc` gained
   `subtract`/`subSymIndex`; `MachOReader` folds a `SUBTRACTOR` + `UNSIGNED`
   pair into one relocation; `RelocApplier` writes `S(target) + A −
   S(subtrahend)` as a signed 32- or 64-bit delta with no rebase or bind
   bookkeeping (the value is position-independent). The macOS import planner
   also learned `__sincos_stret`, which Clang emits at -O2 next to the
   existing `__sincosf_stret`.

## Consequences

- `replay_theater_probe`: 1063 s → 327 s from the runtime optimization alone;
  depth-only shading removes most of the remaining fragment work.
- `zanna build` native executables link optimized runtime objects on macOS
  without diagnostics; `test_linker_object_readers` and
  `test_linker_reloc_edge_cases` cover the pair, the dangling-record error,
  and an undefined subtrahend.
- Public 3D ABI grows by two functions, one property and one method;
  `test_graphics3d_runtime_manifest` was re-pinned. `test_rt_canvas3d` gains a
  software-backend test proving depth-only keeps draw counts and leaves the
  target at the clear colour.
- A probe that reads pixels without arming readback now sees the clear
  colour. That is the intended tripwire: readback is the declared contract.
