---
status: draft
audience: contributors
last-verified: 2026-09-03
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

**Fullscreen invariant (2026-09-03).** For whichever canvas pumps the window,
`Mouse.X/Y`, that canvas's `Width/Height`, and its 2D overlay draw space are
*one* space, and that space carries the framebuffer's aspect ratio. An adopted
Canvas3D therefore lives in the window's public space — physical pixels
divided by the window coordinate scale, which Canvas3D pins to the platform
backing scale at adoption — and **never** in the lender's designed logical
extent. The reason is structural: Canvas3D's overlay projection stretches its
public extent over the whole framebuffer (no letterbox), so an extent whose
aspect differs from the framebuffer would put the overlay, the mouse, and
`Width/Height` in three different spaces. The 2D Canvas's "keep the designed
size and scale the presentation" rule is a 2D-only contract; aspect-fit inside
the public extent is the application's job (a `ViewportTransform` in script).
`rt_canvas3d_coords.inc` holds the single physical↔public derivation, and
`test_rt_canvas3d_coords_contract` pins the invariant in every build
configuration; `g3d_test_canvas3d_adopted_coordinates` exercises the displayed
handoff in the order every single-window game uses (adopt, then the lender
goes fullscreen).

**Loan ownership rule (2026-09-04).** The invariant above was established once,
at adoption, and nothing kept it. The 2D lender's `rt_canvas_resync_window_state`
runs from every 2D entry point (`Fullscreen`, `Windowed`, `Resize`, `Width`,
`Height`, `Clear`, `Flip`, `Poll`, clips, every draw) and pushed the lender's own
scale onto the shared window — in native fullscreen the *presentation* scale
`min(framebuffer / designed extent)`, 1.5 on a 1080p display and 2.3625 on a
16:10 Retina — with no loan check. A coordinate-scale change emits no `RESIZE`,
so the adopted Canvas3D's cached `Width/Height` (and its overlay projection)
stayed at the backing-scale extent while `Mouse.X/Y` moved to the presentation
extent: `1920x1080` vs `1280x720`, a 1.5x skew anchored top-left. On macOS
AppKit resizes the window synchronously inside `toggleFullScreen:`, so the very
first `Canvas.Fullscreen()` after adoption poisoned the space (measured:
Canvas3D `1920x1080`, lender `Screenshot()` public extent `1280x720`). Windowed
1280x720 is the identity on every path, which hid it. Two rules now hold:

1. *The lender never touches window presentation state while loaned.*
   `rt_canvas_resync_window_state` returns early while `window_loan_active == 1`;
   mode and size requests still reach vgfx (mode ownership stays with the
   lender), only the coordinate-scale/clip push is withheld, and the loan return
   re-arms it (`window_state_synced = 0`).
2. *The borrower re-derives its extent every poll.* `rt_canvas3d_poll` compares
   the live `vgfx_get_size()` / physical extent against its cache before sampling
   the mouse (`canvas3d_coords_extent_drifted`) and applies any drift as a
   resize, so `Width/Height`, the overlay, and `Mouse.X/Y` are read from one
   scale in one frame no matter who wrote the window.

Two smaller rules ride along: a borrowed window is never detached from the
input subsystems on return (the lender stays bound), and the 2D `Poll` keeps
the designed logical size in native fullscreen (the presentation-scale contract
depends on it) while deriving a windowed size from physical pixels and the
backing scale rather than the event's logical fields. `test_rt_canvas_state_contract`
pins rule 1 and the 2D `RESIZE` contract; `test_rt_canvas3d_coords_contract` pins
the drift detection; the displayed fixture observes the window's public extent
through the lender's `Screenshot()` (never through `Mouse.SetPosition`, which
writes and reads through the same scale and cannot see a skew).

## Consequences

- Single-window games become possible with zero platform-specific code: the
  change is confined to `rt_canvas3d.c`, `rt_canvas.c`, defs, and stubs.
- Graphics-disabled builds trap through the existing stateful-constructor
  stub policy (`rt_canvas3d_stubs.c`).
- All three platforms use the same adoption path; the Metal sublayer
  attach/detach was the only per-platform risk and is already exercised by
  `vgfx_set_gpu_present`.
- Existing constructors are unchanged; adoption is opt-in.
- Fullscreen adoption reports the window's public extent (the monitor in
  backing-scale units); scripts that letterbox a fixed design space do so
  inside that extent, and mouse hit tests share it without any conversion.
- `test_rt_canvas3d_coords_contract` and `test_rt_canvas_state_contract`
  (headless) and `g3d_test_canvas3d_adopted_coordinates` (displayed) guard the
  invariant and the loan ownership rule.
- `VGFX_MAX_WIDTH/HEIGHT` is 8192: a refused fullscreen framebuffer resize
  leaves the framebuffer at its windowed size under a monitor-sized window and
  monitor-space mouse events — the same skew with no coordinate scale
  involved — so the cap must cover every shipping display.

## Links

- `src/runtime/graphics/3d/render/rt_canvas3d.c` (adoption + return path)
- `src/runtime/graphics/2d/rt_canvas.c` (borrow/dirty accessors)
- `src/il/runtime/defs/graphics3d/rendering.def`, `defs/game3d/world.def`
