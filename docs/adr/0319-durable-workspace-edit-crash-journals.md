---
status: active
audience: contributors
last-verified: 2026-09-03
---

# ADR 0319: Journal Workspace Edit Commit Decisions

## Status

Accepted

## Context

Prepared workspace edits stage complete replacement files and retain originals
in same-directory backups while committing a batch. In-process failures roll
back those backups, but abrupt process or machine termination could leave
opaque nonce-named temp and backup files without a durable record connecting
them to the transaction or indicating whether commit had completed.

Workspace edit commit behavior is part of the runtime C ABI contract.

## Decision

Before the first target rename, write and flush a versioned transaction journal
beside the first target. The journal records a keyed unpredictable name, a phase
(`prepared`, `committing`, or `committed`), the applied-file count, and
hex-encoded complete target, temp, and backup paths. Hex encoding preserves
otherwise valid path whitespace and control bytes without delimiter ambiguity.

Advance and flush the journal after each replacement and publish `committed`
before deleting any backup. Journal publication failure aborts and rolls back
the batch. Successful cleanup removes the journal; incomplete rollback or
backup cleanup deliberately retains it as a durable recovery manifest.

Journal creation is exclusive, writes reject POSIX symlinks, Windows opens the
reparse point itself, and file plus containing-directory state is flushed where
the host supports it.

## Consequences

- An interrupted edit leaves enough durable information to identify every
  original backup and staged/replaced target.
- No target mutation begins unless its complete recovery manifest is durable.
- Successful transactions leave no journal or backup artifact.
- A retained `committed` journal distinguishes cleanup debris from an
  uncommitted batch that should be rolled back by recovery tooling.

## Alternatives Considered

Inferring ownership from nonce filenames loses target mapping and commit intent.
One journal per target cannot atomically communicate the decision for a
multi-directory batch. Deleting backups immediately minimizes debris but makes
cross-file rollback impossible after later commit failures.
