---
status: active
audience: contributors
last-verified: 2026-08-20
---

# ADR 0282: Expose Opaque Regular-File Identity Keys

## Status

Accepted (2026-08-20)

## Context

ADR 0152 added equality testing for regular files and ADR 0259 generalized
entry equality to directories. Equality predicates are sufficient for a few
paths, but Zanna Studio workspace edits can contain up to 100,000 records.
Grouping every target by repeatedly calling `SameFile` would turn alias
canonicalization into an O(files²) preflight. Lexical absolute paths also fail
to group hard links, symlinks, reparse aliases, and case aliases on volumes that
support them.

The native workspace transaction already groups targets by stable filesystem
identity. Zia-side overlap detection needs the same equivalence relation before
it validates per-file ranges, without exposing platform checks in Studio.

## Decision

Add `Zanna.IO.File.IdentityKey(path)` with registry signature `str(str)` and C
ABI symbol `rt_file_identity_key`.

For an existing regular file, the result is one opaque, nonempty key derived
from the followed filesystem object identity: volume serial plus file index on
Windows, or device plus inode on POSIX. Aliases of the same live object return
equal keys. Missing, malformed, inaccessible, directory, and other non-regular
operands return the shared empty string without trapping.

The representation is deliberately unspecified. Callers must use it only for
short-lived equality, map keys, or grouping and must not parse or durably store
it. A key is stable only for the lifetime of the represented filesystem object;
identity reuse after deletion is permitted by the host filesystem.

Studio's `path_identity.FileKey` namespaces a nonempty runtime identity key and
falls back to a normalized lexical spelling for a missing target. Workspace
edit validation uses this key before building snapshots or checking overlaps.

## Consequences

- Large edit batches canonicalize existing files in expected O(files) time.
- Hard-link, symlink, reparse, case, separator, and relative aliases cannot
  bypass Zia-side same-file overlap checks.
- The public runtime gains one string-returning C ABI function and one static
  `Zanna.IO.File` method.
- Identity values remain platform-neutral to callers because their encoding is
  opaque and intentionally absent from the contract.

## Alternatives Considered

- **Compare each new path against every prior representative with `SameFile`.**
  Rejected because valid large batches become quadratic across distinct files.
- **Return a canonical path spelling.** Rejected because path canonicalization
  has permission, information-disclosure, and rename-race semantics and still
  does not give one spelling to hard links.
- **Expose numeric device/inode or Windows file-index fields.** Rejected because
  that leaks adapter details and invites callers to persist or interpret them.
