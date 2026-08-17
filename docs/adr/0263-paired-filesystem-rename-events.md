---
status: active
audience: contributors
last-verified: 2026-08-16
---

# ADR 0263: Expose Paired Filesystem Rename Endpoints

## Status

Accepted (2026-08-16)

## Consulted

- ADR 0006 — Spec Currency and ADR Triggers
- ADR 0010 — Workspace File Index Status Runtime API
- ADR 0015 — Workspace File Index Paging
- `src/runtime/io/rt_watcher.c` — native watcher normalization
- `src/zannastudio/src/app/workspace_watcher.zia` — Studio invalidation policy

## Context

The normalized Watcher ABI previously exposed one path for every event. Linux
inotify and Windows `ReadDirectoryChangesW` report a rename as separate source
and destination records, so callers observed two ambiguous `RENAMED` events.
They could neither remove the old index entry reliably nor add the new one
without rescanning the workspace. macOS kqueue can report that the watched
vnode was renamed but cannot identify its destination.

## Decision

Add `Watcher.EventOldPath()` and `Watcher.EventNewPath()` as public runtime
accessors. They return owned empty strings when the current event is not a
rename or when that endpoint is unavailable. They follow the existing
poll-then-access epoch and thread-affinity rules.

For compatibility, `EventPath()` remains the primary-path accessor. It returns
the destination for a complete or destination-only rename, the source for a
source-only rename, and the ordinary event path otherwise.

Pair Linux `IN_MOVED_FROM`/`IN_MOVED_TO` records by the inotify cookie while
they remain in the bounded event ring. Pair Windows consecutive
`FILE_ACTION_RENAMED_OLD_NAME`/`NEW_NAME` records before publication. A paired
file rename may resolve the new sibling path after the source endpoint has
already matched a file watcher. macOS publishes its known source endpoint.

Workspace event maps always include `path`, `oldPath`, and `newPath`. An
overflow or rename missing either endpoint sets `requiresRescan`; complete
renames permit path-scoped source-freshness, location, and index invalidation.
All endpoint strings participate in queue overflow cleanup, dequeue ownership,
Stop, and finalization.

## Consequences

- Linux and Windows directory renames normally become one actionable event.
- Existing callers of `EventPath()` remain source compatible.
- Studio removes the old path and indexes the new path without clearing
  unrelated search locations.
- Backend limitations are visible as conservative rescan requests rather than
  fabricated destinations.
- The Watcher payload and runtime registry grow additively; no event enum value
  or existing signature changes.

## Alternatives Considered

- **Keep publishing two single-path rename events.** Rejected because callers
  cannot associate concurrent renames safely.
- **Infer the destination by rescanning and comparing metadata.** Rejected
  because metadata is not unique and can remain unchanged across a rename.
- **Suppress incomplete renames.** Rejected because losing the source event can
  leave stale state indefinitely.
- **Fabricate a macOS destination from the watched path.** Rejected because the
  vnode API does not provide that information.
