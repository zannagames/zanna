---
status: active
audience: contributors
last-verified: 2026-09-03
---

# ADR 0318: Add Non-Consuming Editor Deltas for Split Mirrors

## Status

Accepted

## Context

Zanna Studio supports two independently scrolled views of one document. Only
one view owns the mutable editor buffer and undo history. Refreshing the other
view with `SetText` copied the complete document after every edit and discarded
its incremental layout state. Reusing `CodeEditor.TakeDeltas` was unsafe because
that method consumes the journal used by language-service synchronization.

The CodeEditor runtime class and its C entry points are runtime ABI surfaces.

## Decision

Add `CodeEditor.PeekDeltas(sinceRevision)` and the corresponding
`rt_codeeditor_peek_deltas` C entry point. It returns the same byte-column JSON
records and `overflow` marker as `TakeDeltas` without consuming or resetting the
journal. `TakeDeltas` retains its existing single-consumer behavior.

Add `CodeEditor.ApplyMirrorEdit(startLine, startColumn, endLine, endColumn,
text)` and `rt_codeeditor_apply_mirror_edit`. The operation replays one delta,
updates layout and cursor positions, and deliberately clears mirror undo,
modified, and outgoing-delta state. Studio falls back to one complete snapshot
when the source journal reports overflow.

Studio tracks the source revision acknowledged by the mirror. Normal editing
therefore publishes revision identity and replays bounded deltas without
materializing full text. Focus transfer swaps the already synchronized mirror
buffer into the old pane instead of rebuilding it from a complete string.

## Consequences

- Split views and language services can independently consume the same edit
  history without starving one another.
- Ordinary single-cursor edits no longer copy the full document into the mirror.
- Cold mutations and a 256-record journal wrap still require a full refresh.
- `ApplyMirrorEdit` is specialized for read-only mirrors and is not a general
  user-edit or undo API.

## Alternatives Considered

Sharing one mutable native buffer between two widgets would require separating
all cursor, selection, scroll, fold, and paint caches from the current editor
object. Keeping full `SetText` refreshes preserved correctness but retained the
per-keystroke O(document-size) cost.
