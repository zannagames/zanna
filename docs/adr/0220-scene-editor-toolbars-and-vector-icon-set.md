---
status: active
audience: contributors
last-verified: 2026-07-29
---

# ADR 0220: Scene Editor Toolbars and the Extended Vector Icon Set

## Status

Accepted (2026-07-29)

## Context

The scene editors' command bars were wrapping rows of plain text buttons —
about 180 `GUI.Button` instances and two icons across the whole scene
surface — while the GUI toolkit already shipped a real `Toolbar` widget with
named vector icons, checked toggle items, separators, an overflow menu, and
icon-size presets. Editor state rode inside button labels ("Grid: on",
"Persp", "Local"), so widgets resized on every state flip, and the deterministic
vector icon library covered only the IDE's file/debug vocabulary (49 names),
none of the scene-authoring verbs.

Three toolkit gaps blocked adoption beyond the icon vocabulary itself.
Toolbar items had no per-item visibility, which the editors need for
width-based progressive disclosure, and no queryable on-screen geometry,
which the editors need to anchor their persistent command menus (and probes
need for handle-based clicking under ADR 0219). List rows and tabs could not
carry icons at all, while tree rows already could — the C structs carried
`vg_icon_t` fields and `TreeView.Node.SetIcon` already accepted a
`"vector:<name>"` specification.

## Decision

1. **The icon library grows by appending to the static registry; there is no
   registration API.** Forty-three scene-authoring icons join the table
   (`tool-select/paint/erase/fill/rect/line/ellipse/stamp/pick/object`,
   `tool-move/rotate/scale`, `space-local/world`, `snap`,
   `node-group/mesh/box/sphere/plane/cylinder/terrain/light/camera/water`,
   `grid`, `rulers`, `layers`, `eye`, `eye-off`, `lock`, `lock-open`,
   `game-view`, `fit-view`, `zoom-actual`, `prefab`, `material`, `bake`,
   `scene-2d`, `scene-3d`, `stop`, `open-folder`). Icon names are
   append-only stable. A runtime registration API is rejected: the set is a
   curated product asset, static const data preserves ADR 0137's bit-identical
   determinism trivially, and the only prospective external consumer — project
   schemas — is declarative-only and may reference built-in names later
   without executing project code.
2. **`ToolbarItem` gains visibility and geometry.** `SetVisible`/`IsVisible`
   hide an item without removing it: hidden items report zero extent, so
   layout, painting, hit-testing, focus traversal, the overflow popup, and
   inter-item spacing all skip them. `GetScreenX/Y/Width/Height` report the
   arranged on-screen rectangle (zeros when hidden or overflowed), mirroring
   ADR 0219's MenuItem geometry.
3. **List rows and tabs gain leading vector icons** via
   `ListBox.ItemSetNamedIcon(item, name)` and `Tab.SetNamedIcon(name)`; an
   empty or unknown name clears the icon. Tree rows keep the existing
   `SetIcon("vector:<name>")`/`("vector:<base>|<expanded>")` protocol.
4. **The scene editors adopt `Toolbar` while keeping their own persistent
   `ContextMenu`s.** `Toolbar.AddDropdown` is deliberately not used for the
   Scene/Tools/View menus: it pops a *cloned* menu tree, which would break the
   editors' retained `MenuItem` handle polling and ADR 0219 handle-based
   probe clicks (the visible clone's originals report zero geometry).
   Menu-anchor toolbar items show the editor-owned menu at their own reported
   rectangle instead.
5. **Tool modes become checked toggles** driven by the existing
   mode-reconciliation pumps; state stops living in labels. Menu-anchor
   accent styling (`SetStyle(1)` flips) and per-item accessible descriptions
   are retired with the Button anchors — the checked toggles and checkable
   menu items carry the visual state, and anchor tooltips carry the textual
   state.

## Consequences

- The GUI ABI grows by eight functions/methods (ToolbarItem visibility ×2 and
  geometry ×4, `Tab.SetNamedIcon`, `ListBox.ItemSetNamedIcon`); the manifest
  counts and hash are re-baselined alongside them, and the generated runtime
  reference regenerates.
- Probes click toolbar items by handle through
  `scene_probe_support.ClickToolbarItem`; hidden or overflowed items report
  zero geometry, so a stale probe fails loudly at the intended item's
  behavioral assertion instead of clicking a neighbor.
- The main IDE toolbar's icon names all resolve again ("stop" and
  "open-folder" now exist; the debug items use their registered
  `debug-*` names).
- Editors and probes must not assume toolbar items are widgets: items have no
  `Focus()`, and canvas-key contracts anchor on the focusable canvas image,
  which the 2D nudge probe now exercises directly.

## Alternatives Considered

- **A dynamic icon-registration API.** Rejected for determinism, lifetime,
  and ABI cost; revisit only if a concrete project-driven need appears.
- **`Toolbar.AddDropdown` for the command menus.** Rejected: the cloned popup
  breaks retained-handle polling and handle-based probe interaction.
- **Extending checked-state painting to button-type items** to keep the old
  anchor accents. Rejected as redundant once tool toggles carry the state.
