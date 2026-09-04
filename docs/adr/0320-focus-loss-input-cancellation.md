---
status: accepted
audience: contributors
last-verified: 2026-09-03
---

# ADR 0320: Focus Loss Cancels Runtime Input Without Activation Edges

## Context

ZannaGFX backends clear their platform key and mouse-button levels when a
window loses focus, but the language runtime keeps a separate input snapshot.
The Canvas and Canvas3D event pumps previously ignored `VGFX_EVENT_FOCUS_LOST`,
leaving `Keyboard.IsDown` and `Mouse.IsDown` latched until an unrelated later
event. Synthesizing ordinary release/click edges during recovery would be
worse: UI code could activate a control merely because the user switched
applications.

## Decision

Both graphics event pumps call the internal C runtime hook
`rt_input_focus_lost()` when they receive `VGFX_EVENT_FOCUS_LOST`. The hook
atomically resets held keyboard/mouse levels, per-frame press/release edges,
text input, wheel motion, and click/double-click history. It deliberately
emits no release, click, or double-click edge.

The hook is implementation-only and is classified in
`RuntimeSurfacePolicy.inc`; it does not add a frontend API. Higher-level UI
gesture owners detect the discontinuity (`active press` plus `IsDown == false`
without a release edge) as cancellation.

## Consequences

- Alt-tab, window deactivation, and similar focus changes cannot leave movement
  keys, mouse buttons, drags, or presses stuck in either Canvas pump.
- Losing focus cannot confirm a button or complete a click.
- The next real press starts from a clean state and can click normally.
- `test_rt_mouse` covers cancellation and the first subsequent click; the
  Canvas and Canvas3D pumps remain responsible for forwarding the focus event.

## Links

- `src/runtime/graphics/input/rt_input.c`
- `src/runtime/graphics/2d/rt_canvas.c`
- `src/runtime/graphics/3d/render/rt_canvas3d.c`
- `src/tests/runtime/RTMouseTests.cpp`
