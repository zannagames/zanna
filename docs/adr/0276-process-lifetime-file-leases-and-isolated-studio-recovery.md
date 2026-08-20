---
status: active
audience: contributors
last-verified: 2026-08-20
---

# ADR 0276: Process-Lifetime File Leases and Isolated Studio Recovery

## Status

Accepted (2026-08-20)

## Consulted

- ADR 0006 — Spec Currency and ADR Triggers
- ADR 0119 — Trap-Safe Managed I/O Ownership
- ADR 0255 — Bounded and No-Clobber Whole-File Text I/O
- ADR 0256 — Cooperative Whole-File Compare/Exchange
- `src/zannastudio/src/core/recovery_store.zia` — crash-recovery owner

## Context

Studio historically used one shared `recovery/session.lock` marker and one flat
set of CRC32-named swap pairs. Claiming the marker was a check followed by an
ordinary replacement write. Two Studio processes could both claim it, use the
same swap names, and treat a clean exit as permission to delete every process's
recovery data. A plain marker also cannot distinguish an abandoned session from
another Studio process that is still running.

The runtime has atomic no-clobber writes and short-lived compare/exchange locks,
but neither holds ownership for a process lifetime. PID files and timestamp
heartbeats are not sufficient: PIDs can be reused and a paused or overloaded
process can miss a heartbeat without having abandoned its state.

## Decision

Add `Zanna.IO.FileLease`, a managed, nonblocking cross-process file lease:

- `TryAcquire(path)` creates or opens a persistent regular-file marker and
  returns a handle only when it acquires the OS lock immediately.
- `IsValid()` reports whether that handle still owns the lease.
- `Release()` relinquishes the lease but deliberately leaves the persistent
  marker for the owner to remove after its durable cleanup succeeds.
- Finalization releases an otherwise forgotten handle.

Windows uses a nonblocking whole-file `LockFileEx` lease on a non-reparse-point
handle. POSIX uses a nonblocking exclusive `flock` on a no-follow, close-on-exec,
single-link regular-file descriptor. Unlike process-owned record locks, these
leases also reject a second independent descriptor in the same process. Lock files are never
replaced while leased.

Studio assigns every process a cryptographically random UUID and stores its
recovery files under `recovery/session-<uuid>/`. Its `session.lock` remains
leased until clean shutdown. Startup tries to lease prior namespace markers:
a failed attempt means the owner is live and the namespace is ignored; a
successful attempt claims an abandoned namespace for recovery and cleanup.
Only claimed namespaces and the current instance namespace may be mutated.
Clean shutdown writes a `clean.exit` tombstone while each lease is still held,
then retires the namespace out of the `session-` discovery prefix. If best-effort
retirement fails, the tombstone remains and prevents a false crash report.

Each document also receives a random recovery nonce. Canonical swap basenames
use SHA-256 over instance ID, document nonce, and full document identity.
Validated metadata records all three inputs and the original path. The reader
recomputes the digest before accepting a payload. Legacy flat CRC32 `.path` /
`.swp` pairs remain readable once and are cleaned through the legacy namespace.

## Consequences

- Concurrent Studio processes cannot claim one logical recovery session, erase
  one another's swaps, or mistake a live namespace for a crashed session.
- A crashed process leaves both its durable namespace and marker; OS teardown
  releases the lease, making that namespace atomically claimable at next start.
- Hash collision and repeated untitled-name risks no longer determine swap
  ownership, while validated metadata retains the complete identity needed for
  audit and recovery.
- `FileLease` is an additive public runtime C ABI and registry surface. Callers
  must release handles deterministically when cleanup ordering matters.

## Alternatives Considered

- **One exclusive global Studio lock.** Rejected because it prevents legitimate
  concurrent workspaces instead of isolating them.
- **PID files.** Rejected because PID reuse and platform-specific liveness rules
  do not prove ownership of a particular Studio session.
- **Heartbeat timestamps.** Rejected because suspension, sleep, filesystem
  timestamp granularity, and slow I/O create false abandonment decisions.
- **Unique directories without an OS lease.** Rejected because a scanner still
  cannot safely distinguish a live namespace from an abandoned one.

## Tests

- `test_rt_file_ext` exercises exclusive acquisition, contention, release, and
  reacquisition of a persistent marker.
- `zia_zannastudio_recovery` exercises live-instance isolation, abandoned
  namespace recovery, collision-resistant metadata, scoped cleanup, legacy
  recovery compatibility, and asynchronous swap publication.
