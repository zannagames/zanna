---
status: active
audience: contributors
last-verified: 2026-08-20
---

# ADR 0280: Prepare Workspace Edit Transactions Once

Date: 2026-08-20

Status: Accepted

## Context

The workspace-edit runtime previously exposed separate `Validate*` and
`Apply*` operations. `Apply*` performed validation internally and then decoded
and validated the same files a second time. Studio's existing-file save path
also sampled metadata, read and hashed the complete file, and passed those bytes
to a locked compare/exchange primitive that read them again. The duplicate I/O
was expensive for large sources and widened the interval in which an external
writer could invalidate the caller's observations.

Validation still cannot make commit unconditional: non-cooperating writers may
change a pathname after validation. The runtime must retain the validated file
identity and bytes and recheck them immediately before replacement. The missing
piece was an explicit, immutable, one-shot transaction token.

This adds public runtime C ABI and registry surface, so an ADR is required.

## Decision

`Zanna.Workspace.Edit` gains `Prepare`, `PrepareInRoot`, and `PrepareInRoots`.
Each returns an explicitly owned `Zanna.Workspace.PreparedEdit` handle,
including when validation fails. Preparation decodes the edit sequence, resolves
root containment, reads each target once under the existing byte/file limits,
validates versions/ranges/overlap, and retains canonical edit records, exact
original bytes, and stable file identities.

`PreparedEdit.Result` returns a clone of the immutable validation result.
`PreparedEdit.Apply` is one-shot: it derives replacement images from the
retained snapshot, stages them beside each target, then rechecks both exact
content and stable identity immediately before the backup/rename commit. A
second apply fails with `edit.prepared.consumed`. `PreparedEdit.Destroy` is
mandatory and accepts null.

Edit maps may specify `wholeFile=true`. Such a record replaces the entire file
without line splitting and cannot be combined with another edit for that file.
It may carry `expectedMtime`, `expectedSize`, `expectedHash`, and `maxBytes`.
Studio uses this form for existing-file saves, so the runtime performs the only
preparation read and validates the document's last known digest before staging.

The legacy `Apply*` methods remain source-compatible but now implement
prepare-then-apply internally, eliminating their former second validation pass.
Multi-file Studio refactors use the explicit prepared API whenever they need to
inspect diagnostics before committing.

## Consequences

- Existing-file saves no longer stat, read, hash, and split the file in Studio
  before asking the runtime to repeat that work.
- Validate-then-apply workflows can inspect one retained validation snapshot
  without reopening every target.
- Commit still detects non-cooperating external writers through an immediate
  content-and-identity recheck; preparation is not a lock held across UI time.
- Prepared handles retain bounded source images until apply/destroy, so callers
  must keep their lifetime short and always destroy them.
- Whole-file records are deliberately exclusive per target, avoiding ambiguous
  interactions with ranged edits.
- The older validation/application entry points remain available for callers
  that do not need an inspectable token.
