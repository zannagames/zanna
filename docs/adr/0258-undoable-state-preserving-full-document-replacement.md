---
status: active
audience: contributors
last-verified: 2026-08-16
---

# ADR 0258: Make Full-Document Editor Replacement Undoable and State-Preserving

## Status

Accepted (2026-08-16)

## Consulted

- ADR 0006 — Spec Currency and ADR Triggers
- ADR 0011 — CodeEditor Editing Runtime API
- ADR 0154 — Single-Owner Split Editor Documents
- `src/lib/gui/src/widgets/vg_codeeditor_api.inc` — ordinary editor edit path
- `src/zannastudio/src/commands/file_save_support.zia` — save normalization

## Context

`CodeEditor.SetText` is a cold-load operation. It intentionally replaces the
document baseline and resets edit history, cursors, selections, folds, and
scroll state. Zanna Studio previously used it after trimming trailing
whitespace or adding a final newline during save. Consequently a formatting
preference could discard undo history and disturb the user's editing context.

Studio may normalize a document in the focused editor, the other side of a
split, or a detached `EditorBuffer`. Reconstructing the state in Studio would
duplicate lower-layer clamping rules and still could not append one coherent
undo record to a detached buffer's native history.

## Decision

Add two registry methods:

- `Zanna.GUI.CodeEditor.ReplaceAllText(text)` with signature `i1(str)` and C
  symbol `rt_codeeditor_replace_all_text`.
- `Zanna.GUI.EditorBuffer.ReplaceAllText(text)` with signature `i1(str)` and C
  symbol `rt_editorbuffer_replace_all_text`.

The lower GUI exposes `vg_codeeditor_replace_all_text` and
`vg_editor_buffer_replace_all_text`. Replacement travels through the ordinary
edit-history path as one full-range edit. It retains prior undo history,
primary and extra cursors, selections, folds, and scroll positions, clamping
positions and dropping collapsed fold ranges when the new document is shorter.
An equal replacement succeeds as a no-op. A read-only attached editor rejects
the operation. Allocation or invalid-handle failures return false without
claiming success.

`SetText` retains its cold-load contract. Callers deliberately establishing a
new document baseline continue to use it; transformations of an existing
document use `ReplaceAllText`.

The managed-string bridge rejects embedded NUL bytes because the current GUI
editing core accepts NUL-terminated UTF-8 text. Supporting binary text would
require a separate length-aware editor contract rather than silently changing
this API.

## Consequences

- Save-time normalization becomes a single undoable edit without destroying
  editing context.
- Focused, split, and detached Studio documents share the same behavior.
- The public runtime surface gains two methods and two C ABI symbols.
- A detached replacement temporarily moves the buffer state through a headless
  editor shell; ownership stays single and the buffer handle remains stable.
- State coordinates are preserved numerically, not semantically remapped across
  arbitrary rewrites. Callers needing semantic cursor mapping require a
  transformation-specific API.

## Alternatives Considered

- **Snapshot and rebuild state in Studio.** Rejected because Studio cannot
  safely reproduce native history, fold, multi-cursor, and clamping behavior.
- **Change `SetText` to preserve state.** Rejected because loading a different
  document needs a clean baseline and existing callers rely on that contract.
- **Apply whitespace edits one line at a time.** Rejected because it creates a
  large undo chain and does not cover other full-document transformations.
