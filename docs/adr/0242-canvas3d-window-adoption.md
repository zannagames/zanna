---
status: draft
audience: contributors
last-verified: 2026-08-07
---

# ADR 0242: Canvas3D Window Adoption (Single-Window 2D→3D Handoff)

## Status

Proposed (decision recorded)

## Context

A program that opens a 2D `Zanna.Graphics.Canvas` for menus and then starts a
`Game3D.World3D` gets **two OS windows**: every World3D constructor creates its
own window via `vgfx_create_window`. Games want one window like any commercial
title — menu, gameplay, and fullscreen all in the same surface.

The backends already support this shape: all 3D backends receive their window
as a `create_ctx(vgfx_window_t win, ...)` parameter and never create one; the
Metal backend attaches a `CAMetalLayer` sublayer to whatever NSView the window
has and removes it in `destroy_ctx`; `vgfx_set_gpu_present(win, enabled)` muxes
between GPU-layer and software presentation. Only window *creation* and
*teardown* were fused into Canvas3D.

## Decision

**Let a Canvas3D borrow the window of a live 2D canvas.** New runtime ABI
surface (frontend-visible, hence this ADR):

- `Zanna.Graphics3D.Canvas3D.NewOnCanvas(canvas)` →
  `void *rt_canvas3d_new_on_canvas(void *canvas2d)`
- `Zanna.Game3D.World3D.WithCanvasCamera(canvas, fov, near, far)` →
  `void *rt_game3d_world_new_with_canvas_camera(void *canvas2d, double, double, double)`

Supporting internal accessors in `rt_canvas.c` (C-level, not frontend
surface): `rt_canvas_borrow_window(canvas2d)` exclusively claims the 2D
canvas's `vgfx_window_t` and retains its owner; `rt_canvas_return_window(canvas2d)`
invalidates cached window state, clears the claim, and releases that retain.

Ownership model:

- `struct rt_canvas3d` gains `owns_window` and `lender_canvas`; `struct rt_canvas`
  tracks an atomic `window_loan_active` state (available, loaned, or closing/closed). The adoption
  path in `canvas3d_new_impl` skips `vgfx_create_window`, reads dimensions
  from the borrowed window, and records the lender.
- Teardown (finalizer and `close_window`) branches on ownership: an owned
  window is destroyed as before; a borrowed one is **returned** —
  `hide_gpu_layer` backend hook (Metal removes its sublayer),
  `vgfx_set_gpu_present(win, 0)`, resize callback cleared, and the lender's
  window-state cache invalidated so the 2D canvas repaints correctly.
- The Canvas3D loan retains the 2D canvas automatically, so releasing the caller's
  Canvas reference cannot destroy the shared window early. A second simultaneous
  adoption and an explicit `Canvas.Close()` while adopted fail closed; the 2D canvas
  remains the window's owner throughout. Adoption, return, and close claim the state with
  compare-and-exchange so concurrent callers cannot both win a check-then-act race.

Fullscreen: the adopted window keeps whatever mode the 2D canvas set
(`Canvas.Fullscreen()` / `Windowed()` with automatic logical-size upscaling
already work). The Canvas3D adoption branch forces its own `fullscreen=0`
bookkeeping so it never fights the 2D canvas over mode ownership.

## Consequences

- Single-window games become possible with zero platform-specific code: the
  change is confined to `rt_canvas3d.c`, `rt_canvas.c`, defs, and stubs.
- Graphics-disabled builds trap through the existing stateful-constructor
  stub policy (`rt_canvas3d_stubs.c`).
- All three platforms use the same adoption path; the Metal sublayer
  attach/detach was the only per-platform risk and is already exercised by
  `vgfx_set_gpu_present`.
- Existing constructors are unchanged; adoption is opt-in.

## Links

- `src/runtime/graphics/3d/render/rt_canvas3d.c` (adoption + return path)
- `src/runtime/graphics/2d/rt_canvas.c` (borrow/dirty accessors)
- `src/il/runtime/defs/graphics3d/rendering.def`, `defs/game3d/world.def`
