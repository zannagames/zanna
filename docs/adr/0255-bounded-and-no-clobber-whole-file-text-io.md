---
status: active
audience: contributors
last-verified: 2026-08-16
---

# ADR 0255: Bound and Reserve Whole-File Text I/O in the Runtime

## Status

Accepted (2026-08-16)

## Consulted

- ADR 0006 — Spec Currency and ADR Triggers
- ADR 0119 — Trap-Safe Managed I/O Ownership
- `src/runtime/io/rt_file_ext.c` — durable adjacent staging and whole-file reads
- `src/zannastudio/src/services/safe_io.zia` — Studio failure-value adapters

## Context

Zanna Studio reads settings, session recovery, workspaces, and source files under
explicit byte ceilings. Its existing `ReadAllTextBounded` adapter first calls
`File.SizeBytes`, then reopens the path through `File.ReadAllText`, and finally
stats the path again. A file can be replaced or grow between those operations,
so the initial ceiling does not constrain the allocation performed by the
second open.

Studio also needs no-clobber creation for Explorer files and authored assets.
It currently chooses an unreserved sibling pathname, writes that pathname, and
moves it into place. The runtime already has the stronger primitive internally:
whole-file writes use an exclusively created adjacent sidecar, durable flush,
atomic commit, and parent-directory synchronization. The missing part is a
public commit mode that refuses an existing destination.

Both additions change the public runtime C ABI and registry, so ADR 0006
requires an explicit decision.

## Decision

Add two methods to the static `Zanna.IO.File` runtime class:

| Member | Registry signature | C symbol |
|---|---|---|
| `ReadAllTextBounded(path, maxBytes)` | `str(str,i64)` | `rt_io_file_read_all_text_bounded` |
| `WriteAllTextNew(path, contents)` | `void(str,str)` | `rt_io_file_write_all_text_new` |

`ReadAllTextBounded` opens the path exactly once, requires a regular file,
stats that open descriptor, rejects a negative limit or a size above
`maxBytes` before allocation, and reads exactly the statted byte count. Early
EOF and observed trailing data trap rather than returning an unstable partial
snapshot. An empty file succeeds when `maxBytes` is zero.

`WriteAllTextNew` uses the same exclusively created, no-follow adjacent
sidecar and durable flush path as `WriteAllText`, but its final commit is
no-clobber. If the destination exists by commit time, the sidecar is removed,
the existing file remains byte-for-byte intact, and the call traps.

The existing `WriteAllText` contract remains atomic and crash-durable. Studio
adapters should call it directly for replacement instead of adding a second
application-level staging layer.

## Consequences

- A caller-owned byte ceiling now constrains allocation on the same descriptor
  that supplies the data.
- New-file creation has one cross-platform linearization point and cannot
  overwrite a racing destination.
- Existing source and IL remain compatible; the registry change is additive.
- Expected access, oversize, collision, and I/O failures still trap at the
  runtime boundary. Studio continues converting those traps into ordinary
  result values.
- The API does not introduce a public file-handle or lock type. More general
  transactional multi-file work remains separate.

## Alternatives Considered

- **Keep stat/read/stat in Studio.** Rejected because no sequence of pathname
  metadata checks can bound the allocation made by a separate open.
- **Expose raw exclusive temporary handles.** Rejected as unnecessary surface
  and ownership complexity for the two whole-file contracts.
- **Make every write no-clobber.** Rejected because `WriteAllText` has an
  established replacement contract used throughout Zanna programs.

