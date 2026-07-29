---
status: active
audience: contributors
last-verified: 2026-07-29
---

# ADR 0221: Live-Commit Inspector Contract for Scene Editors

- Status: Accepted
- Date: 2026-07-29
- Deciders: Zanna Studio maintainers
- Tags: zannastudio, ui, scene-editors, interaction

## Context

The 2D and 3D scene editors accumulated roughly thirty modal "Apply" buttons.
Most guarded *state-shaped* controls — a field that mirrors exactly one
canonical scene value (a camera bound, a light intensity, an object id).
Editing those through a draft-then-click ritual is the largest single source
of the "primitive" feel the editors carry: engines in this class commit such
fields the moment the gesture finishes, and reserve buttons for operations.

The editors already proved the live model works: node transforms
(`PumpTransformDraft`), 2D object X/Y (`PumpObjectTransformDraft`), and
gizmo drags all commit per completed gesture with one history entry each.
The Apply buttons survived only around the *component* inspectors.

## Decision

### The shape rule

- **State-shaped controls commit live.** A control whose value mirrors one
  canonical scene value commits when its gesture completes: spinners,
  checkboxes, dropdowns, and color pickers on their change edge; text inputs
  on Enter, or on focus leaving a draft that differs from the canonical
  snapshot. Each commit is one undoable transaction.
- **Operation-shaped controls keep verb buttons.** Anything that creates,
  forks, bakes, batches, imports, or otherwise does more than mirror a value
  keeps an explicit button, titled with a verb phrase — never bare "Apply".

### Mechanics

- Shared kit `ui/scene_field_rows.zia` provides the labeled `TextField`
  record: `SetCanonical` re-baselines without raising an edge; `TakeCommit`
  consumes exactly one Enter-or-blur commit edge per gesture.
- Component inspectors expose `PumpDraftIntent() -> Boolean`, aggregating
  the change edges of every state-shaped control. The editor pump layer
  applies the whole component through the existing single-transaction apply
  path when the selection is a single node owning the component.
- **Programmatic refresh must not look like intent.** Every refresh path
  that writes controls (`SetValue`/`SetChecked`/`SetSelected`/`SetText`)
  consumes the edges it raised before the next pump reads them. This is the
  load-bearing invariant; a missed consume turns selection changes into
  phantom edits.
- Creation stays a button (`Create Light`, `Create Camera`, …): a stray
  control edge on a component-less node must never materialize a component.
- Batch application over multi-select stays a button with a counted verb
  ("Set 3 Lights", "Apply deltas to 4"), because mixed-field resolution is
  a deliberate act.
- Forking stays a button ("Fork as Unique"): live edits on a shared 3D
  material must not silently clone it per keystroke.

### Converted sites (this program increment)

- 2D camera/lighting topic: twelve labeled `TextField`s, Apply buttons
  deleted, `TakeCameraEditCommitted`/`TakeLightingEditCommitted` edges.
- 2D object type/id: Enter or blur-with-diff commits; canonical snapshots
  tracked in editor state; the position row's button is hidden.
- 2D draw-stack layer/priority pair: live on either control's edge.
- 3D light, camera, collider, material inspectors: single-selection edits
  on an existing component apply live; the button remains only for
  creation, batch, and forking, retitled to verbs.
- 3D single-node name/visibility/transform: already live via drafts; the
  "Apply node properties" button now appears only for multi-select deltas.

### Deliberately not converted

- **Scene environment (`env.*`)**: its controls do not yet refresh from the
  loaded scene, so a live commit could write stale defaults (clearing an
  authored skybox on a checkbox toggle). It keeps an explicit verb button
  until the schema-driven Scene Settings forms supersede it.
- Probe-grid placement, bake, library, import, packaging: operations.

### Tokens

`ui/layout_metrics.zia` and `ui/style_tokens.zia` name the spacing and
button-style constants the scene chrome uses; scene code references tokens,
not bare numbers. Grep gates: no bare `"Apply"` button label, no
`SetStyle(<int>)`, no literal `SetSpacing`/`SetPadding` gaps in the scene
panels.

## Consequences

- One gesture produces exactly one history entry; Escape/undo semantics are
  unchanged because every live commit reuses the existing transactional
  apply paths.
- The `WasChanged` clear-on-read contract makes edge ownership explicit:
  a control edge is consumed either by a refresh (as noise) or by the pump
  (as intent), never both.
- `TreeView.Node.GetIcon` now round-trips `vector:` specs (the icon-spec
  string is preserved alongside the materialized icon), so probes can
  assert icon assignments through the public surface.
- Probes assert the one-gesture-one-entry contract per converted site.

## Links

- `docs/adr/0219-scene-probe-interaction-contract-and-menu-item-geometry.md`
- `docs/adr/0220-scene-editor-toolbars-and-vector-icon-set.md`
- `src/zannastudio/src/ui/scene_field_rows.zia`
- `src/zannastudio/src/ui/scene_camera_lighting_2d.zia`
