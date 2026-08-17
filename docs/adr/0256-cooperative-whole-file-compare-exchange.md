---
status: active
audience: contributors
last-verified: 2026-08-16
---

# ADR 0256: Cooperatively Compare and Exchange Whole-File Text

## Status

Accepted (2026-08-16)

## Consulted

- ADR 0006 — Spec Currency and ADR Triggers
- ADR 0119 — Trap-Safe Managed I/O Ownership
- ADR 0255 — Bound and Reserve Whole-File Text I/O in the Runtime
- `src/runtime/io/rt_file_ext.c` — durable adjacent staging
- `src/zannastudio/src/services/safe_io.zia` — Studio persistence adapters

## Context

Zanna Studio stores settings, breakpoints, recent paths, and session recovery in
one INI file. Each owner preserves unknown sections by reading the complete file,
changing its own keys, and atomically replacing the file. Atomic replacement
prevents torn contents, but two Studio processes can read the same revision and
silently overwrite one another's later update.

The same check-then-write race exists when project-wide replacement previews a
closed file, validates its contents, and later commits a replacement. Pathname
metadata and a second read cannot provide a linearization point around the
commit.

Exposing a public lock handle would add lifetime, exception-cleanup, and leaked
handle hazards to every language frontend. An INI-specific transaction in the C
runtime would invert the dependency between the generic runtime and Studio.

## Decision

Add `Zanna.IO.File.CompareExchangeAllText(path, expected, desired)` with registry
signature `i1(str,str,str)` and C symbol
`rt_io_file_compare_exchange_all_text`.

The primitive uses one deterministic, fixed-length lock filename per destination
directory, opens it without following links, and acquires an exclusive OS file
lock. On POSIX the basename includes the effective user ID so an owner-only lock
does not create a cross-user denial in shared temporary directories. While
holding that lock it compares the complete current
regular-file bytes with `expected`. A missing destination compares equal to an
empty expected string. A mismatch releases the lock and returns false without
writing. A match commits `desired` with the existing durable adjacent staging
path, then releases the lock.

The protocol is cooperative: processes using the compare/exchange API for a
given normalized path serialize correctly. Ordinary `WriteAllText` retains its
existing unconditional-replacement contract and does not acquire the lock.
Stable directory lock files remain beside destinations so unlink/recreate races
cannot split waiters across different lock objects. Independent compare/exchange
operations in the same directory are conservatively serialized.

Invalid inputs, unsafe lock entries, and I/O, flush, commit, unlock, or close
failures trap. Only an expected-content mismatch returns false.

## Consequences

- Shared state can reject stale read/modify/write commits instead of silently
  losing a newer process's update.
- Closed-file replacement can bind its commit to the exact previewed bytes.
- The API adds one runtime C ABI symbol and one static `Zanna.IO.File` member.
- Persistent hidden-style lock entries may remain in writable directories.
- Callers that need automatic conflict merging still implement that policy
  above this byte-oriented primitive.
- Noncooperating writers can still race the protocol; this is explicit rather
  than implying impossible universal locking around atomic rename.

## Alternatives Considered

- **Expose acquire/release lock handles.** Rejected because every frontend would
  need exception-safe ownership and could hold locks across arbitrary work.
- **Put INI section updates in the runtime.** Rejected because the runtime must
  not depend on Studio's persistence format.
- **Split every Studio section into a file.** Rejected as a migration-heavy
  change that still would not prevent two instances from overwriting the same
  recent-path collection.
- **Compare hashes without locking.** Rejected because another cooperating
  writer could commit between comparison and replacement.
