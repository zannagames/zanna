---
status: accepted
audience: contributors
last-verified: 2026-09-15
---

# ADR 0367: 2D Canvas Fullscreen Presentation (Centered Letterbox, Last Request Wins, IsFullscreen)

## Status

Accepted. Extends the 2D `Zanna.Graphics.Canvas` fullscreen contract and adds
`Canvas.IsFullscreen`. The Canvas3D loan ownership rule of
[ADR 0242](0242-canvas3d-window-adoption.md) is unchanged; the new presentation
state follows it.

## Context

A 2D `Canvas` keeps its designed logical extent in native fullscreen and scales it
uniformly by `min(framebuffer / design)` onto the monitor's framebuffer. Three
things were wrong with how that presentation behaved, all found while adding a
DISPLAY setting to Cat 'n' Mouse (2026-09-15):

- **Anchored top-left.** Nothing centered the scaled design, so a 1344x872 game on a
  1920x1080 monitor sat against the left edge with a 256 px black band on the
  right. `Screenshot()` and `vgfx_get_size()` reported the monitor-aspect extent
  (1550x872) while `Width`/`Height` reported the design.
- **Dropped requests on macOS.** `vgfx_platform_set_fullscreen` compared the request
  with the window's live `styleMask`. Cocoa's `toggleFullScreen:` is a toggle that
  animates asynchronously and ignores a second toggle mid-flight, so a `Windowed()`
  issued ~100 ms after `Fullscreen()` saw "not fullscreen yet" and did nothing: the
  window ended fullscreen while the game believed it was windowed, and the mirror
  case ended windowed with the preference saying fullscreen. Win32 is synchronous
  and X11/Wayland send absolute add/remove requests that queue in order, so only
  macOS raced.
- **No query.** Canvas3D has `IsFullscreen`; 2D `Canvas` only had `Fullscreen()` and
  `Windowed()`, so every game tracked the mode itself and could not read the truth.

## Decision

### Centered letterbox

In native fullscreen the 2D canvas is scaled uniformly by `min(fb_w/design_w,
fb_h/design_h)` and **centered**. The remainder on the other axis becomes equal
bars that are black and dead:

- Drawing is confined to the content rectangle. Primitives, blits, gradients,
  flood fills and clips are transformed by scale **and** offset; `Clear` paints the
  content with the colour and the bars black.
- The pointer maps through the same transform, so a cursor in a bar reads below
  zero or past the designed extent rather than inside it.
- The public extent (`vgfx_get_size`, `Screenshot()`, RESIZE logical fields) is the
  **designed extent** while the offset is active, consistent with `Width`/`Height`.
  The scaled design is carried explicitly as the content extent so an odd
  remainder cannot round the public size away from the design (1920 - 1665 = 255
  used to yield 1345).

Implementation: `vgfx_set_coord_transform(window, scale, offset_x, offset_y,
content_w, content_h)` beside `vgfx_set_coord_scale` (which is now the zero-offset
form). The runtime computes the offset in
`rt_canvas_effective_coord_transform` and pushes it from
`rt_canvas_resync_window_state`. Platform adapters still emit physical
coordinates; only the shared logical/physical helpers changed.

The offset is 2D-Canvas presentation state under the ADR 0242 loan ownership
rule: withheld while a Canvas3D borrows the window (the borrower's
`vgfx_set_coord_scale` also zeroes it) and re-pushed on return. Adopted-window
games such as Legacy Baseball, which letterbox themselves in the borrower's space,
are unaffected.

### Last request wins on macOS

The Cocoa adapter records every request as the target mode. While a transition is
animating (`windowWillEnter/ExitFullScreen` to `windowDidEnter/ExitFullScreen`) a
request is only recorded; when the transition lands, a target that differs from
the live mode is applied on the next run-loop turn (a toggle issued inside the
did-* notification is dropped by AppKit). `vgfx_platform_is_fullscreen` keeps
reporting the live mask because the presentation scale must track the real
framebuffer.

### `Canvas.IsFullscreen`

`Zanna.Graphics.Canvas.IsFullscreen() -> Boolean` (`rt_canvas_is_fullscreen`,
`i1(obj)`) reports the platform's live mode. After `Fullscreen()`/`Windowed()` the
value converges over the following frames on macOS and X11; a game that must act
on the landed state polls it rather than assuming its own request took effect.

## Consequences

- Fullscreen 2D games are centered with black bars on every platform; a game that
  cleared the whole canvas every frame no longer paints the bars.
- `Screenshot()` in fullscreen now returns the design-sized image, not the
  monitor aspect. The single-window fixture `test_canvas3d_adopted_coordinates`
  still observes the live extent while the loan is active and the design after
  return, as before.
- Tests: `test_input` T17c (transform, mouse inversion, public extent, explicit
  content extent), `test_drawing` T14 (bars stay black under clear/primitives/
  clip), `test_rt_canvas_state_contract` (offset pushed in fullscreen, withheld
  under loan), displayed `rt_test_canvas_fullscreen_transition` (rapid on/off and
  off/on land on the last request; extent stays the design).
- `graphics_stub_functions` baseline 1396 -> 1397.

## Links

- `src/lib/graphics/src/vgfx_internal.h` (`vgfx_internal_to_physical_*`,
  `vgfx_internal_content_rect`, `vgfx_internal_public_*_i32`)
- `src/lib/graphics/src/vgfx_platform_macos.m` (`macos_apply_fullscreen`,
  `macos_settle_fullscreen_transition`)
- `src/runtime/graphics/common/rt_graphics_internal.h`
  (`rt_canvas_effective_coord_transform`, `rt_canvas_resync_window_state`)
- `src/il/runtime/defs/api/graphics2d.def`, `defs/classes/localization.def`
- `docs/zannalib/graphics/canvas.md`
- [ADR 0242](0242-canvas3d-window-adoption.md)
