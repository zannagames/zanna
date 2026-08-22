---
status: active
audience: contributors
last-verified: 2026-08-21
---

# ADR 0287: Make Workspace Index Cursor Tokens Generation-Safe and Work-Bounded

## Status

Accepted

## Context

ADR 0278 introduced explicitly owned workspace file-index cursors as opaque
`obj` values backed directly by native pointers. Calling `IsValid`, `Generation`,
or `Next` after `Destroy` dereferenced freed storage, and concurrent destruction
could race a page operation. Page limits bounded returned matches but not the
number of filesystem candidates examined, so a page requesting one rare
extension could still traverse an entire workspace on the caller's thread.

The cursor result map and opaque-handle lifetime are runtime C ABI contracts.

## Decision

Keep the existing `Zanna.Workspace.FileIndexCursor` signatures, but interpret
the opaque value as a monotonic registry token rather than a native address.
Registry lookup leases the cursor for an operation; `Destroy` removes the token
immediately and defers native deletion until outstanding leases finish. Stale
queries fail closed, and repeated destruction is a no-op. Operations on the same
cursor are serialized while independent cursors remain independent.

`Next(handle, limit)` now bounds traversal work as well as result allocation. It
examines at most `clamp(limit * 8, 64, 32768)` directory entries per call and may
return an empty, valid, non-final page. Its map reports `work` for candidates
examined in that call and `scanned` for the cursor's cumulative candidates;
`nextOffset` remains the logical matching-entry offset.

Each cursor also caches the inherited ignore-rule vector once per visited
directory and no longer retains a duplicate set of every emitted path.

## Consequences

- Stale and double-destroyed cursor tokens cannot cause use-after-free.
- Frame-driven discovery has a deterministic filesystem-work budget.
- Consumers must treat an empty page with positive `work` as progress and keep
  paging until `done`.
- Cursor tokens are process-local capabilities and must never be persisted or
  interpreted as addresses.

## Alternatives Considered

Keeping tombstone wrapper allocations would make stale calls safe but leak one
allocation per cursor. Holding a global registry lock across filesystem I/O
would avoid leases but serialize unrelated traversals and make contenders spin
for the duration of slow storage operations.
