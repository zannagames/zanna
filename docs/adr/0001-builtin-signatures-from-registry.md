---
status: active
audience: contributors
last-verified: 2026-06-27
---

# ADR 0001: Derive BASIC Builtin Signatures from Registry

Date: 2025-11-15

## Context

The BASIC frontend maintains builtin metadata in multiple places:

- builtin_registry.inc (descriptors: name, arity, result mask; lowering/scan rules)
- SemanticAnalyzer_Builtins.cpp (legacy static array of BuiltinSignature with per-arg type specs)

This duplication led to drift: new builtins (ARGC/ARG$/COMMAND$) had correct descriptors but stale semantic signatures,
causing bogus arity diagnostics and crashes. We already fixed arity by deriving it from the registry and added a
fixed-result mapping to reduce drift for result types.

## Decision

Unify builtin semantic signatures behind the registry. The registry is the preferred source for:

- argument arity (min/max),
- result kind (fixed when unambiguous),
- per-argument type allowances and optionality when a registry-backed semantic view exists.

The semantic analyzer calls a registry accessor to retrieve a semantic signature view for builtins that have one and
falls back to its legacy static table for builtins not yet covered by that view. The current implementation includes:

- Arity derived from registry everywhere,
- Fixed result kind mapping from registry descriptors when unambiguous,
- Registry-backed semantic signatures and safety overrides for ARGC/ARG$/COMMAND$ primary signatures,
- Unit tests to guard semantics and lowering.

## Consequences

Pros:

- Removes a common class of drift bugs (signature mismatches and crashes),
- Centralizes metadata maintenance in one place.

Cons:

- Requires enriching more entries in builtin_registry.inc with per-argument type metadata for full parity,
- The legacy `kBuiltinSignatures` table in `src/frontends/basic/SemanticAnalyzer_Builtins.cpp` still exists as a
  fallback for builtins without a registry-backed semantic view.

## Alternatives

- Keep the legacy static table and patch individual bugs as they appear (high risk of regressions and ongoing
  maintenance burden).

Spec Impact

No change to user-visible language semantics. This is an internal refactor that aligns arity and result checking with
the already-declared registry descriptors.

Implementation Status

Verified on 2026-06-27:

- `src/frontends/basic/BuiltinRegistry.cpp` exposes `getBuiltinSemanticSignature(...)`.
- The registry-backed semantic view currently covers ARGC, ARG$, and COMMAND$.
- `SemanticAnalyzer::validateBuiltinArgs(...)` derives builtin argument-count validation from
  `getBuiltinArity(...)`.
- `SemanticAnalyzer::builtinSignature(...)` consumes that view when available and keeps safety overrides for ARGC,
  ARG$, COMMAND$, and ERR.
- The legacy static table remains as the fallback for other builtins, so the registry is not yet the sole source for
  every per-argument type check.
