---
status: draft
audience: contributors
last-verified: 2026-09-17
---

# ADR 0371: Canvas3D Render Thread (Deferred Frame Execution Off the Sim Thread)

## Status

Proposed. Design recorded; implementation is its own program (three phases
below) and is not part of the plan-127 landing. Nothing in this ADR changes a
public API until phase 1 lands.

## Context

`Canvas3D` already records every draw into a deferred queue during the frame
(`rt_canvas3d_deferred.inc`) and executes it only in `rt_canvas3d_end`
(`rt_canvas3d_render_pass.inc`): shadow passes, the main pass, post-FX and the
backend `end_frame`, followed by `present` in `rt_canvas3d_flip`. That execution
is the largest single block of CPU on the game thread. Measured on the M4 Max
after the plan-127 CPU work (native Metal, night, Balanced, 1920x1080, p95 per
camera):

| Phase (main thread) | Wide view | Pitch view |
|---|---|---|
| queue (`Scene3D.Draw`, `sceneUs`) | 5.9 ms | 4.1 ms |
| shadow + main + encode (`sceneEndUs`) | 10.9 ms | 5.7 ms |
| simulation (`stepUs`) | 5.0 ms | 4.3 ms |
| playback (`advanceUs`) | 3.0 ms | 2.9 ms |

The GPU finishes every one of those frames with time to spare; the frame is
CPU-bound and serial. Moving the execution half of the frame to a second thread
overlaps it with the next frame's simulation, queueing and playback, which is
the only remaining structural win once per-draw costs are trimmed.

What makes it non-trivial today:

- The frame arena (`canvas3d_frame_arena_*`) is single-buffered and reset at
  the start of the next frame, and the deferred command array is reused in
  place. Commands borrow payloads that live in that arena or are retained
  through the transient-object manager for exactly one frame.
- Mesh and texture payloads are borrowed from live runtime objects (`rt_mesh3d`,
  `rt_pixels`, `rt_textureasset3d`); a script may mutate or release them between
  queueing and execution. Backends key their caches by object pointer plus
  `identity_serial` and a geometry epoch, which is what makes the borrow safe
  within one frame on one thread.
- Backend thread rules differ: Metal command buffers may be encoded from any
  thread but the direct-swapchain route blocks on `nextDrawable` inside
  `begin_frame`; the D3D11 immediate context must be used from one thread only;
  an OpenGL context is current on one thread at a time and `gl_make_current`
  runs at frame start.
- Synchronous readbacks (`RenderTarget3D.Pixels`, `AsDisplayPixels`,
  `CopyTo`, snapshot capture, the software-backend `out_pixels` path) read the
  result of the current frame immediately after `rt_canvas3d_end`.
- Some engine state is written during execution and read by scripts the same
  frame: performance telemetry (`BackendDraws`, upload bytes, GPU time),
  occlusion history, shadow cache validity, streaming budgets.

## Decision

Introduce a one-frame render pipeline owned by the canvas, opt-in per canvas:

- `Canvas3D.SetRenderThread(enabled)` and the `RenderThreaded` property. Default
  off in phase 1, on for GPU backends in phase 3 once every gate below is green.
  The software backend and headless canvases never use it (their execution is
  the readback).
- `rt_canvas3d_end` splits into **close** (game thread: sort, flatten
  materials, resolve lights, compute the shadow plan, finish the frame arena)
  and **execute** (render thread: shadow passes, main pass, post-FX,
  `end_frame`). `rt_canvas3d_flip` enqueues `present` after execute.
- Two frame slots: the arena, the deferred command array, the flattened light
  block and the per-frame scratch (auto-instance matrices, sorted index arrays,
  Hi-Z buffers) are doubled and indexed by frame parity. Close on frame N+1 may
  not start until execute of frame N-1 has finished (the slot it reuses);
  execute of frame N may not start until close of frame N has finished. This is
  a plain two-stage pipeline with one frame of latency, matched to the ADR 0369
  pacer so the extra latency is one refresh period at most.
- **Payload pinning.** A queued command holds a pin on every object it borrows
  (mesh, textures, morph sets, bone palettes, render targets): a retain plus
  the object's `identity_serial` and geometry epoch. Execute skips a draw
  whose object changed epoch or serial (logged once per object per second as
  `Canvas3D: draw dropped, <class> mutated during execution`) instead of
  reading mutated memory. Pins are released when the slot is recycled. Script
  mutation of a mesh during the one-frame window is therefore visible one frame
  late, never torn.
- **Backend ownership moves to the render thread.** Every backend vtable call
  (uploads, cache maintenance, `begin_frame`, `end_frame`, `present`, render
  target creation and resize) runs on the render thread; the game thread only
  records. The OpenGL context is made current on the render thread once at
  enable time; the D3D11 immediate context is touched only there; the Metal
  route acquires its drawable in `present` only (the item-3 change already
  planned), so `begin_frame` never blocks the pipeline on the display.
- **Sync points.** Any call that needs the executed result (`RenderTarget3D`
  pixel readbacks, `AsDisplayPixels`, `CopyTo`, snapshots, `Canvas3D` telemetry
  getters that report execute-side counters, backend capability changes,
  canvas resize, backend switch and destroy) drains the pipeline first
  (`canvas3d_render_sync`). The runner-inset composite in Legacy Baseball
  already samples the render target natively on Metal (ADR 0299) and does not
  drain; the D3D11 and OpenGL native sampling in plan-127 item 2 is a
  prerequisite for enabling the thread there without losing the win.
- **Telemetry** is published per executed frame into a small double-buffered
  struct the getters read; `FrameGpuTimeUs`, `BackendDraws` and upload bytes
  therefore describe the previous frame, which the docs state.
- Trap and error paths: a trap raised on the render thread (backend loss,
  device removed) is stored and re-raised on the game thread at the next sync
  point or `begin`, so scripts see traps where they already handle them.

## Phases

1. Slot doubling and the close/execute split on one thread (no new thread
   yet), behind `SetRenderThread`. Gates: byte-identical Metal and software
   snapshots for the existing GPU-gated suites; `resource_ledger_probe` RSS
   growth unchanged (pins must release); every `rt_canvas3d` unit test.
2. The render thread itself (`rt_thread` worker, condition-variable handshake,
   drain on every sync point), Metal first. Gates: the liveperf ledger shows
   `sceneEndUs` leaving the game thread and cadence p95 falling by at least the
   wide-view execute cost; a new stress test that mutates meshes and releases
   textures every frame under the thread and asserts no invalid reads under
   the address sanitizer.
3. D3D11 and OpenGL (after plan-127 item 2's native render-target sampling),
   default on for GPU backends, Legacy Baseball ships with it.

## Consequences

- One frame of added latency between input and pixels when enabled; the ADR
  0369 pacer keeps it to a single refresh period. Games that need the lowest
  latency (none shipped today) leave it off.
- Memory: a second frame arena and command array (a few MB at Baseball's
  4,700-draw peak), plus pins that hold objects alive for one extra frame.
- Every backend gains a hard rule that it is only entered from the render
  thread when the pipeline is enabled; the shared source-shape tests pin the
  absence of direct backend calls from the recording path.
- Two runtime functions, one method and one property; manifest, inventory and
  stub pins are re-pinned when phase 1 lands. No IL, verifier or serialization
  change.
