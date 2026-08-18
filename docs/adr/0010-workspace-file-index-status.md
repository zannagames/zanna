---
status: active
audience: contributors
last-verified: 2026-08-17
---

# ADR 0010: Workspace File Index Status Runtime API

Date: 2026-06-25

Status: Accepted

## Context

Zanna Studio uses `Zanna.Workspace.FileIndex.Enumerate` to populate project files and
semantic workspace indexes. `Enumerate` returns one map per matching entry and
silently caps output at the runtime's file-index limit. That is fine for normal
project traversal, but editor tooling also needs a cheap way to explain large or
invalid workspaces to users without materializing every entry or guessing when a
result was truncated.

Changing the runtime surface is covered by the spec-currency gate in ADR 0006,
so this addition is recorded explicitly.

## Decision

Add `Zanna.Workspace.FileIndex.Status(root, extensionsCsv, excludesCsv,
includeDirs)`.

The method performs the same root validation, extension filtering, hard excludes,
`.gitignore` handling, and explicit exclude handling as `Enumerate`, but returns
a summary map:

- `valid`: whether the root/traversal completed without a hard failure.
- `root`: normalized absolute root path when valid.
- `entryCount`: number of matching entries counted, capped at `maxEntries`.
- `maxEntries`: runtime file-index cap.
- `truncated`: true when the cap was reached.
- `diagnostics`: structured diagnostic maps for invalid roots, traversal errors,
  or truncation.

## Consequences

IDE surfaces can show meaningful workspace-index status and warnings before or
alongside full enumeration. Existing callers of `Enumerate` and `ShouldIgnore`
are unaffected. The new API is additive and does not change IL semantics,
compiler behavior, or the existing file-index record shape.
