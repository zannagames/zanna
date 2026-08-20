---
status: active
audience: contributors
last-verified: 2026-07-22
---

# ADR 0154: Give Split-Editor Documents One Live Buffer Owner

Date: 2026-07-22

Status: Accepted

> The distinct-document safety contract remains active. ADR 0279 supersedes
> the same-document limitation by moving one canonical mutable buffer between
> views instead of attaching two independent buffers.

## Context

`Zanna.GUI.EditorBuffer` is a single-owner editor-state object. Zanna Studio's
initial split implementation copied the focused document text into a second
`CodeEditor` and associated both pane records with the same `Document`. The two
buffers then diverged. Pane focus also did not update `DocumentManager`'s
global active index, so a later save preflight could copy one pane's text into
the other pane's document model and silently overwrite edits.

A true same-document multi-view requires a runtime buffer that supports
multiple attached views. Studio cannot safely emulate that behavior with text
copies.

## Decision

Every visible split pane owns a distinct `Document`, and every `Document` owns
at most one live `CodeEditor` or detached `EditorBuffer`. Opening a split
requires a second open document. A request for a document already visible in
the other pane changes focus to that owner instead of attaching or copying a
new buffer.

Pane focus is authoritative for the global active document and tab. Focus
transitions first capture the departing pane, including its dirty state, then
the application synchronizes `DocumentManager`, the tab strip, document
surface, breadcrumb, status, minimap, find bar, and completion controller.
Command-level editor-state preflight repeats the active-document repair so a
native focus event in the same frame cannot route a save to a stale index.

`SaveToDocument` rejects a document that does not own the focused widget as a
final overwrite guard. Closing either visible document collapses the split,
and closing the split detaches the right buffer back into its document before
hiding the widget. Save-all flows clear modified flags on the owning focused,
non-focused, or detached buffer.

Session restore retains the split-active preference, but only recreates a
split when at least two distinct documents were restored. Same-document
multi-view remains unavailable until the GUI runtime provides a shared
multi-view buffer contract.

## Consequences

- One pane can no longer silently overwrite another pane's document text.
- Tab selection, save, auto-save, recovery, language tools, and file watching
  follow the physically focused document.
- Reopening a document visible in the other pane is a focus operation and
  preserves its undo-capable buffer.
- Users must open at least two documents before invoking Split Editor Right.
- Closing a visible split document intentionally returns to one pane rather
  than leaving a pane bound to a closed model object.
- A regression probe covers duplicate rejection, focus ownership, mismatched
  save rejection, modified-flag clearing, close detachment, and re-splitting.
