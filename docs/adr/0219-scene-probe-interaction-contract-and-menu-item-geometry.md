---
status: active
audience: contributors
last-verified: 2026-07-28
---

# ADR 0219: Scene Probe Interaction Contract and Menu Item Geometry

## Status

Accepted (2026-07-28)

## Context

Zanna Studio scene probes verify editor behavior through the real input
harness: they compute a widget's on-screen center from its own live geometry
and dispatch genuine pointer events. That idiom is robust for widgets because
`Zanna.GUI.Widget` exposes `GetScreenX/Y/Width/Height`.

Context-menu rows expose no geometry. `Zanna.GUI.MenuItem` handles carry
state (text, enabled, checked, click edges) but not position, so six probes
click menu rows at hardcoded pixel offsets below the anchoring button —
arithmetic derived by hand from the menu's private visual constants (28-pixel
rows, 9-pixel separators, 4-pixel vertical padding). Any change to menu row
height, separator height, padding, item order, or the addition of menu icons
silently re-targets which row a probe clicks; the probe then fails on an
unrelated assertion, or worse, passes while exercising the wrong command.
The planned toolbar and iconography work changes exactly these kinds of
chrome metrics, so the hazard is imminent, not theoretical.

The same probes also pin pane geometry with per-probe literal ceilings
(toolbar height, minimum canvas height). The values are individually
reasonable but scattered, so a deliberate chrome change must hunt each
literal, and an accidental regression is indistinguishable from a stale
budget.

## Decision

1. `Zanna.GUI.MenuItem` gains four read-only geometry methods:
   `GetScreenX()`, `GetScreenY()`, `GetScreenWidth()`, and
   `GetScreenHeight()`, returning the row's current on-screen rectangle in
   logical window coordinates — the same space as `Widget.GetScreenX`. The
   implementation walks the owning context menu's rows exactly as its
   hit-test does, on top of the menu origin that painting has already
   clamped to the window. The methods return zero when the item does not
   belong to a visible context menu (menubar-owned items report zero;
   on-screen geometry for menubar rows can be added later if a probe needs
   it).
2. Probes interact with menus only through a shared handle-based helper:
   `probes/scene_probe_support.zia` owns `HarnessCoordinate`, `ClickWidget`,
   and `ClickMenuItem(shell, harness, item)`. Pixel-offset menu clicking is
   removed everywhere and must not reappear.
3. The probe/editor stability contract is recorded in
   `src/zannastudio/docs/testing.md`:
   - Probes locate widgets only through exposed editor fields (or widget
     names); editor field names are probe API, and a rename or reparent
     updates every consuming probe in the same change.
   - Menu interaction is handle-based clicking through the real harness.
   - Geometry assertions use the shared named budgets in
     `scene_probe_support.zia`; probes do not carry private geometry
     literals.

## Consequences

- The GUI runtime surface grows by four functions and four qualified methods;
  the GUI ABI manifest counts and hash are re-baselined once alongside them.
- Six probes (`scene_command_bar`, `scene2d_rulers_guides`,
  `scene_shaded_viewport`, `scene_terrain_authoring`,
  `scene_viewport_picking`, `scene_viewport_picking_routing`) drop their
  local offset helpers for the shared module, and their menu clicks survive
  menu restyling, row-height changes, and icon columns.
- Reordering menu rows no longer affects probes at all — the click follows
  the intended item's handle. A removed or hidden item reports zero geometry,
  so its probe fails loudly at the intended item's behavioral assertion
  instead of silently activating a neighbor.
- Chrome-geometry budgets have one home; relaxing a budget for a deliberate
  chrome change is a reviewed one-line edit.

## Alternatives Considered

- **Publish the menu metric constants to probes.** Removes the magic numbers
  but keeps the fragile order-times-height arithmetic in every probe; item
  reordering and separators still silently re-target clicks.
- **Realize menu rows as child widgets.** Would inherit widget geometry for
  free, but menus deliberately paint their rows without a widget subtree;
  converting them is a large change with no user-facing benefit.
- **Keep pixel offsets and freeze menu metrics.** Rejected: it would make the
  visual-polish program impossible by contract.
