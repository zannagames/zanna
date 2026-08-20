---
status: active
audience: contributors
last-verified: 2026-08-20
---

# ADR 0151: Bound Transactional Workspace Edits to Explicit Multiple Roots

Date: 2026-07-22

Status: Accepted

## Context

Zanna Studio applies semantic rename results to open buffers and closed files.
The closed-file path previously derived one runtime safety root from the process
working directory. A workspace opened from Finder, a launcher, or an unrelated
terminal commonly lives outside that directory, so a valid rename could fail
only because Studio was launched elsewhere. The fallback also lowercased path
comparisons on every platform, which is incorrect on case-sensitive filesystems.

A multi-root workspace can legitimately produce one rename batch spanning
unrelated folders. Applying each root separately would weaken the existing
all-files validation and rollback contract, while using their common filesystem
ancestor would broaden the trust boundary beyond folders the user opened.

## Decision

Add `Zanna.Workspace.Edit.ValidateInRoots(edits, roots)` and
`Zanna.Workspace.Edit.ApplyInRoots(edits, roots)`, backed by the new runtime C
entry points `rt_workspace_edit_validate_in_roots` and
`rt_workspace_edit_apply_in_roots`.

The runtime canonicalizes every supplied root and every edit target. Each target
must be equal to or below at least one explicit root. Relative targets are
accepted only when exactly one root is supplied; multi-root batches require
absolute targets so resolution is unambiguous. The complete batch is still
validated, staged, committed, and rolled back as one transaction even when its
files belong to unrelated roots.

Zanna Studio passes the root snapshot already owned by its project index or
background BASIC request. Open-buffer-only edits need no disk trust boundary,
but any closed-file edit is rejected when no workspace root is available.

## Consequences

- Closed-file rename works regardless of Studio's launch directory.
- Multi-root semantic rename retains atomic validation and rollback semantics.
- Files outside every opened workspace folder are rejected after canonical
  path resolution, including path traversal and symlink spellings.
- Filesystem case behavior is left to canonical native paths rather than
  unconditional text lowercasing.
- The runtime registry, C header, generated API reference, Studio call sites,
  and runtime/Studio regression coverage must evolve together.

## 2026-08-20 Amendment: Anchor Rooted Transactions to Directory Handles

Canonical containment alone left a path-component race between validation and
the later staging/rename operations. Rooted transactions now traverse every
component without following links and keep the verified parent anchored for the
transaction. POSIX performs reads and mutations with descriptor-relative
`openat`, `renameat`, and `unlinkat`; Windows retains non-reparse ancestor
handles without delete sharing and renames already-open file handles relative
to the retained parent. A component swapped to a symlink after preparation is
rejected before staging or commit.

Replacement metadata is also fail-closed and complete for writable platform
state. POSIX copies owner/group, mode, all bounded xattrs (including ACL and
security namespaces), and filesystem flags. Windows stages with `CopyFileW`,
rewrites only the unnamed data stream, and explicitly restores owner/group,
DACL, and attributes, retaining alternate streams, resource attributes,
compression, encryption, object metadata, and timestamps. The regression suite
pins a post-prepare symlink swap, ownership/mode/xattr/flag retention, and the
Windows alternate-stream and attribute cases.
