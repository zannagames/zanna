---
status: active
audience: contributors
last-verified: 2026-07-29
---

# ADR 0223: Scene Editor Chrome and Feedback

- Status: Accepted
- Date: 2026-07-29
- Deciders: Zanna Studio maintainers
- Tags: zannastudio, ui, scene-editors, feedback

## Context

The program plan called for a chrome-and-feedback pass over the scene
editors: a scene status zone, progress/completion feedback for long
operations, destructive-action confirmation, command-palette coverage,
and empty-state guidance. Several of these already existed in part; this
ADR records what was added, what was already sufficient, and one
deliberate rejection.

## Decision

- **Scene status zone.** The status bar's center segment (already scene-
  aware in `main.zia`) now reports live editing state: the 2D zone adds
  selection count and zoom to canvas/layer/object counts; the 3D zone
  adds selection count and the effective viewport mode to the hierarchy
  count. It refreshes with the visual-document cadence, not per frame.
- **Empty states.** The 3D hierarchy pane shows "No nodes yet — use
  Create… or Import Instance" guidance when a scene has zero nodes; the
  2D hierarchy status explains Add Object / component stamping when the
  scene has zero objects. Both disappear the moment content exists, and
  the bounded-view clipping notice keeps precedence in 2D.
- **No new delete confirmations (deliberate rejection).** Every scene
  edit — node/object deletion included — is one undoable transaction, so
  modal confirms would add friction without adding safety; engines in
  this class do not confirm undoable deletes. `MessageBox` confirms
  remain reserved for genuinely unrecoverable actions (file-level
  operations already guarded in the file command layer). The
  `lint_zannastudio_modal_input_policy` allowlist therefore gains no new
  scene-editor entries.
- **Command palette.** The catalog already covers the scene workflow
  (new 2D/3D scene, Run Scene, Toggle Scene Focus, Duplicate/Delete
  Scene Selection); no additions were needed. Future scene commands
  register through the same catalog.
- **Long-operation feedback.** Bake, import, and library flows already
  toast within `lint_zannastudio_notification_policy`; the typed Scene
  Settings forms (ADR 0222) report through the inspector detail label
  like every other transactional edit.
- **Rich tooltips.** Scene chrome carries descriptive tooltips from the
  ADR 0220/0221 passes; the runtime title+body tooltip rendering upgrade
  remains deferred (recorded in ADR 0220).

## Consequences

- Users see selection/tool/zoom state at a glance without watching the
  inspector; empty panes teach the next action instead of showing
  nothing.
- Deletion stays one keystroke and one undo, matching the transactional
  model the probes enforce.

## Links

- `docs/adr/0221-live-commit-inspector-contract.md`
- `docs/adr/0222-schema-v19-typed-scene-settings.md`
- `src/zannastudio/src/main.zia` (status zone)
