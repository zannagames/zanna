---
status: active
audience: contributors
last-verified: 2026-08-16
---

# ADR 0259: Expose Stable Identity for Every Filesystem Entry

## Status

Accepted (2026-08-16)

## Consulted

- ADR 0006 — Spec Currency and ADR Triggers
- ADR 0152 — Stable File Identity for Editor Documents
- ADR 0153 — Non-Following Path Link Inspection
- `src/runtime/io/rt_path.c` — cross-platform path adapter
- `src/zannastudio/src/core/project_file_ops.zia` — Explorer path policy

## Context

Studio receives path spellings from dialogs, compiler diagnostics, filesystem
watchers, project indexes, and persisted sessions. Lexical absolute
normalization removes `.` and `..`, but cannot determine whether case aliases,
symlinks, reparse points, or alternate directory spellings identify the same
entry. Lowercasing every path is incorrect because Linux and case-sensitive
macOS volumes can contain distinct case-varying names.

ADR 0152 exposed `File.SameFile`, but its contract intentionally covers only
regular files. Workspace roots and directory caches also need identity-aware
comparison. Reimplementing device/inode or Windows file-index handling in Studio
would violate the runtime adapter boundary and duplicate platform logic.

## Decision

Add `Zanna.IO.Path.SameEntry(left, right)` with registry signature
`i1(str,str)` and C symbol `rt_path_same_entry`.

The predicate follows normal filesystem resolution and compares stable object
identity. POSIX compares `stat` device/inode pairs. Windows opens each entry with
directory-compatible backup semantics and compares volume serial and file-index
fields. Regular files, directories, and other stat-able entries are supported.
Invalid strings, embedded NUL bytes, missing entries, and inaccessible entries
compare unequal without trapping.

`File.SameFile` remains the regular-file-specific API and now explicitly checks
that both operands are regular before comparing identity. Studio centralizes
lexical normalization, existing-entry equality, separator-aware containment,
linked-descendant rejection, and portable child-name policy in one path service.
It never substitutes operating-system-wide case folding for filesystem
identity.

## Consequences

- Existing directory aliases can be compared correctly on the mounted
  filesystem without conflating case-distinct entries.
- Missing paths retain normalized, case-preserving lexical identity because no
  stable filesystem object exists yet.
- Symlink and reparse targets compare equal through `SameEntry`; callers that
  must reject traversal still use non-following `Path.IsLink` on each component.
- The public runtime surface gains one method and one C ABI symbol.
- Windows and POSIX identity logic remains contained in the runtime I/O adapter.

## Alternatives Considered

- **Lowercase every path on Windows and macOS.** Rejected because case policy is
  volume-specific, not reliably operating-system-wide.
- **Resolve canonical target strings.** Rejected because canonical spelling has
  platform permission/race semantics and exposes more path information than an
  equality predicate needs.
- **Treat directories as files in `File.SameFile`.** Rejected because it would
  contradict that API's accepted regular-file contract.
