---
status: active
audience: contributors
last-verified: 2026-08-28
---

# ADR 0303: Let Owned Workspace Index Cursors Reach Completion

## Status

Accepted

## Context

ADR 0287 bounds the work performed by each workspace file-index cursor page.
The implementation also imposed the legacy stateless scanner's 100,000-entry
aggregate ceiling on explicitly owned cursors. A Studio fallback scan could
therefore stop before reaching `done`, even though it already paged with a
bounded per-frame or background-worker work budget. This silently omitted
files in sufficiently large workspaces.

The cursor result-map semantics are a runtime C ABI contract.

## Decision

Explicit `Zanna.Workspace.FileIndexCursor` instances have no fixed aggregate
entry ceiling. Each call to `Next` retains ADR 0287's result and filesystem-work
budgets, and consumers must continue until `done` or explicitly cancel and
destroy the cursor.

The legacy stateless file-index functions retain their 100,000-entry ceiling
and continue to report truncation. Cursor result maps report `maxEntries = 0`
to mean that no aggregate ceiling applies.

Studio performs degraded watcher scans on an Async worker, so reaching a large
cursor's natural completion never moves unbounded filesystem traversal onto the
GUI thread.

## Consequences

- Explicit cursor consumers can enumerate every matching file without silent
  aggregate truncation.
- Every page remains work-bounded and cancellable; completeness does not imply
  one-call or one-frame enumeration.
- Compatibility callers using stateless scans retain their existing memory and
  traversal ceiling.

## Alternatives Considered

Raising the shared ceiling would only postpone omission and would make legacy
calls more expensive. Keeping the ceiling and asking consumers to open another
cursor cannot resume safely because a new cursor has no stable continuation
position in a mutating directory tree.
