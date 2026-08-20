---
status: active
audience: contributors
last-verified: 2026-08-20
---

# ADR 0278: Explicit Workspace File-Index Cursors and Precise Fallback Identity

Date: 2026-08-20

Status: Accepted

## Context

ADR 0015 made `Zanna.Workspace.FileIndex.Page` caller-stateless and permitted a
private process-wide cursor cache. The cache retains only eight traversals.
Independent Studio subsystems can therefore evict one another and turn a later
page into an O(offset) rescan. Because the underlying recursive directory
iterator is live and its order is unspecified, a restarted traversal can also
mix two filesystem generations and skip or duplicate rows.

Studio's degraded watcher additionally compared path, identity, size, and
whole-second modification time. A same-size rewrite inside one timestamp tick
was invisible. File-size failures were encoded as zero, making an unreadable
file indistinguishable from an empty file.

These changes add public runtime methods and result-map fields, so they change
the runtime C ABI and the cross-layer runtime registry.

## Decision

Add the explicitly owned static surface `Zanna.Workspace.FileIndexCursor`:

- `New(root, extensionsCsv, excludesCsv, includeDirs) -> obj`
- `IsValid(handle) -> Boolean`
- `Generation(handle) -> Integer`
- `Next(handle, limit) -> Map`
- `Destroy(handle)`

Each handle owns exactly one recursive traversal until `Destroy`. It cannot be
evicted by unrelated callers. A monotonic immutable `generation` is returned by
the handle and every page, allowing consumers to reject results captured before
they reset discovery. Each cursor also remembers normalized relative paths and
emits a path at most once within its generation.

`FileIndex.Page` remains as a compatibility entry point but is now genuinely
stateless. It creates a temporary traversal, skips to the requested offset, and
destroys it before returning. Long-lived and interleaved callers must use an
explicit cursor.

File-index entry maps now use `size=-1` when regular-file size metadata fails.
They also include `modifiedNs`, an opaque nanosecond-resolution file-clock value,
and `sampleHash`. All-extension directory-inclusive cursor pages compute
`sampleHash` from at most 64 bytes at each of the beginning, middle, and end of
a regular file. Other traversal modes return `-1` rather than adding duplicate
I/O. Studio's fallback watcher folds the precise timestamp and sample into two
independent order-insensitive aggregates.

## Consequences

Studio discovery work has explicit lifetime and no hidden global eviction.
Resetting a project root or observing topology changes destroys the old cursor,
so pages from different generations cannot be combined accidentally. Duplicate
paths are suppressed even if a live iterator revisits an entry.

Callers must destroy every successfully created handle. Compatibility paging is
still useful for one-off requests, but repeated offset paging intentionally pays
for rescanning and should not be used by frame-driven Studio features.

Fallback pages perform at most 192 bytes of content sampling per regular file.
This adds bounded I/O in degraded watcher mode in exchange for detecting the
important same-size, same-tick rewrite class. Content outside the three samples
can still collide; native watcher events remain the primary source of truth.

