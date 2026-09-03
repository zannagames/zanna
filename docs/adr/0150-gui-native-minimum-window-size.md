---
status: active
audience: contributors
last-verified: 2026-07-22
---

# ADR 0150: Enforce GUI Minimum Window Sizes Through Native Adapters

Date: 2026-07-22

Status: Accepted

## Context

`Zanna.GUI.App` could request an immediate window size but could not express a
persistent minimum. Desktop window managers therefore allowed a resizable app
to collapse to a nearly empty title bar. Layout-level clamping is insufficient:
it runs after the native resize, produces visible snapping, and does not inform
the operating system's resize affordances.

Zanna Studio needs enough room for its activity bar, project sidebar, editor,
toolbar, and status bar. Its startup sizing already treated 720 by 520 effective
UI-layout units as the smallest practical workbench on ordinary displays, but
that floor was not retained after launch. A fixed native 720-by-520 floor is
also insufficient when whole-UI zoom enlarges every control: at 200 percent it
leaves only a 360-by-260 effective viewport and reintroduces clipped chrome.

## Decision

Add `Zanna.GUI.App.SetMinimumSize(width, height)` and the corresponding
`rt_app_set_minimum_size` runtime C entry point. Dimensions use the same logical
client/content coordinate space as `SetWindowSize`; values below one normalize
to one.

ZannaGFX stores the logical floor on each window, clamps future programmatic
resize requests, and publishes the constraint through every desktop adapter:

- Cocoa uses `NSWindow.contentMinSize`.
- Win32 answers `WM_GETMINMAXINFO`, converting logical client dimensions to the
  DPI-aware outer-window tracking size.
- X11 updates `WM_NORMAL_HINTS` `PMinSize` values.
- Wayland sends the stable xdg-toplevel `set_min_size` request.
- The mock adapter relies on the shared core clamp for deterministic tests.

Zanna Studio applies a base 720 by 520 workbench minimum after constructing its
main window. The native floor multiplies that base by whole-UI zoom at 100 to
300 percent. Zoom below 100 percent does not reduce the usability floor. On
displays smaller than the calculated floor, each dimension contracts to the
monitor size minus a 96-pixel safety gap so the application remains reachable.
On macOS sessions where Cocoa exposes no `NSScreen` (for example login-less CI
or a hardened launch context), the graphics adapter reports a deterministic
1920×1080 backing-pixel desktop rather than `0×0`, so the same clamping logic
remains well-defined.
The monitor API reports the full display rather than the desktop work area, so
this reserve also accommodates common menu-bar, taskbar, and title-bar chrome.

The shell republishes this constraint whenever UI zoom changes. If an ordinary
window is already smaller than the new floor, it grows around its previous
center immediately. Growth on the primary monitor is clamped inside the
desktop-chrome margin so a formerly compact window cannot expand past a screen
edge. Maximized, minimized, and fullscreen windows retain their current desktop
state and inherit the native floor when restored.

## Consequences

- Interactive resizing stops at a usable Studio workbench size instead of
  allowing overlapping or inaccessible chrome.
- Raising whole-UI zoom preserves roughly the same usable workbench area until
  the physical display cap makes that impossible.
- Programmatic resizing follows the same contract, making probes and automation
  agree with native user interaction.
- Other GUI applications remain unconstrained until they explicitly call the
  new method; setting 1 by 1 restores the previous effective default.
- The runtime registry, C ABI, public ZannaGFX API, native adapters, and GUI
  regression coverage must evolve together.
