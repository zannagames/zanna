---
status: active
audience: contributors
last-verified: 2026-08-17
---

# ADR 0266: Ratchet Zanna Studio Architecture Debt

## Status

Accepted (2026-08-16)

## Consulted

- ADR 0006 — Spec Currency and ADR Triggers
- ADR 0218 — Zanna Studio Source Module Boundaries
- `src/zannastudio/docs/architecture.md` — Studio ownership and dependency direction
- `src/zannastudio/scripts/check_architecture.sh` — architecture gate

## Context

The Studio architecture guard enforced one default 600-line ceiling and looked
only for a legacy `MODULE:` teaching block. The live tree already contained 87
files above their nominal budgets, while newer files used the repository's full
`File`/`Purpose`/`Key invariants` source header instead of the older block. The
guard consequently reported hundreds of failures on its own baseline and could
not distinguish existing debt from a new regression.

The guard also described itself as an architecture check while verifying no
module declaration, local bind target, source-root containment, or dependency
direction. A misspelled bind or a new leaf-to-UI dependency could pass while a
fully documented new source file failed.

## Decision

- Keep the target line budgets, but record every current over-budget file and
  its exact current line count in `src/zannastudio/scripts/architecture_baseline.tsv`.
- Treat that file as a ratchet, not an exemption list. Growth, new debt, stale
  entries, and improvements whose lower bound has not been recorded all fail.
  `--print-baseline` emits the deterministic baseline after an intentional
  improvement.
- Accept either the standard full source header (`File`, `Purpose`, and
  `Key invariants`) or the legacy `MODULE:` teaching block during migration.
- Require exactly one module declaration for ordinary modules. Probe entry
  points may omit the declaration, but may never contain more than one.
- Resolve every quoted local bind, reject missing or outside-root targets, and
  derive its source/target layer from the normalized path.
- Forbid new upward edges from `zia`, `basic`, `services`, `core`, `editor`,
  `build`, `commands`, and `ui` according to the dependency direction in the
  architecture guide. Existing upward edges are explicit baseline debt and
  become stale when removed.
- Register both the real guard and a fixture-based self-test as focused CTest
  tests on Unix hosts.

This decision changes the enforcement of Studio dependencies; it does not add
or remove a product dependency or change the runtime C ABI.

## Consequences

- The architecture command is green on its declared baseline and immediately
  blocks new or worsened size and layering debt.
- Refactors that reduce debt must lower the checked-in baseline in the same
  change, preventing later regrowth to an obsolete ceiling.
- Full repository source headers and legacy tutorial headers can coexist while
  older files migrate naturally.
- Broken local binds fail in a fast static check before a Studio compile.
- The baseline is intentionally visible review data. Broad regeneration
  without a corresponding architectural explanation is easy to spot in diff.

## Alternatives Considered

- **Raise the default line limit until the current tree passes.** Rejected
  because it would legalize future growth and erase the intended 600-line
  design target.
- **Maintain a path-only allowlist.** Rejected because allowed files could grow
  indefinitely and removed dependency debt could silently return.
- **Fail on every current violation.** Rejected because a permanently red gate
  supplies no protection to incremental work.
- **Depend on a third-party graph linter.** Rejected by the zero-dependency
  policy and because Zia's local bind grammar is small enough to validate here.
