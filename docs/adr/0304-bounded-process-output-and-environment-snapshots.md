---
status: active
audience: contributors
last-verified: 2026-08-28
---

# ADR 0304: Bound Process Output Publication and Own Environment Snapshots

## Status

Accepted

## Context

`ProcessHandle.ReadOutputResult` can materialize the full retained 16 MiB
ordered-output queue into managed strings and maps in one call. Studio invoked
it from GUI update handling, so a noisy build could monopolize a frame even
though native capture itself was bounded.

On POSIX, inherited and overlay process launches also borrowed `environ`
pointers after releasing any synchronization. A concurrent
`Environment.SetVariable` could replace that storage during executable lookup
or `posix_spawn`. The new method is a runtime C ABI addition, and the private
environment synchronization is a cross-runtime dependency.

## Decision

Add `ProcessHandle.ReadOutputResultBounded(maxBytes, maxChunks)`. It returns an
ordered prefix as `{ chunks, truncated, emittedBytes, remainingBytes, hasMore }`
and leaves unconsumed native bytes queued. Nonpositive arguments select 64 KiB
and 64 chunks; byte requests are capped at 16 MiB and chunk requests at 4,096.
The existing `ReadOutputResult` retains its consume-all compatibility behavior.

Studio build/run publication consumes at most 64 KiB and 64 chunks per update,
keeps activity polling enabled while `hasMore`, and delays final job completion
until the queue is empty.

POSIX process launch copies every inherited environment entry while holding a
private runtime environment lock shared with `GetVariable`, `HasVariable`, and
`SetVariable`. PATH fallback is copied under the same lock. Executable lookup
and process spawning use only owned snapshots after the lock is released.

## Consequences

- One Studio update has a deterministic managed allocation and parsing budget
  for process output.
- Output remains ordered and no retained suffix is discarded between bounded
  reads.
- Runtime-mediated environment mutation cannot invalidate a child launcher's
  environment or PATH pointers.
- Native embedders that mutate `environ` directly must still provide their own
  external coordination.

## Alternatives Considered

Reducing the native capture cap would discard useful diagnostics without
bounding the cost of materializing the retained prefix. Holding the environment
lock across `posix_spawn` would keep borrowed pointers valid but unnecessarily
serialize setters across filesystem lookup and process creation.
