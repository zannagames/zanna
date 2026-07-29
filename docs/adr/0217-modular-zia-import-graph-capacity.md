---
status: active
audience: contributors
last-verified: 2026-07-28
---

# ADR 0217: Raise Bounded Zia Import Graph Capacity

## Status

Accepted (2026-07-28)

## Context

The Zia import resolver limits both import depth and total imported source files
to keep malformed graphs from consuming unbounded compiler resources. The
depth limit is 50, imported source text is capped independently at 64 MiB, and
the total-file limit was 256.

Zanna Studio reached the 256-file ceiling while its largest controllers still
contained thousands of lines. Splitting those controllers into cohesive
modules made the application easier to navigate and test, but each valid split
then failed with `V1000`. Keeping unrelated behavior consolidated solely to
stay under the resolver ceiling would make the safety guard dictate application
architecture and block further modularization.

The resolver deduplicates canonical paths, skips supported cycle re-entry, and
retains the independent depth and total-source-byte guards. A bounded increase
in file count therefore permits a wider legitimate graph without making import
resolution unbounded.

## Decision

Raise the maximum number of imported files in one Zia compilation unit from
256 to 512.

Keep the maximum import depth at 50 and the aggregate imported-source limit at
64 MiB. Keep canonical-path deduplication, cycle handling, and the `V1000`
diagnostic unchanged. The diagnostic continues to print the live configured
limit.

Maintain a frontend regression that compiles a flat graph of 300 imported
files. This deliberately crosses the former ceiling while remaining well below
the new bound and avoiding any dependency on Zanna Studio's current module
count.

## Consequences

- Zanna applications can use focused modules without consolidating unrelated
  code to satisfy the former 256-file ceiling.
- Pathological graphs remain bounded by file count, source bytes, and depth.
- A worst-case valid graph may consume more parsing and AST memory than before;
  the unchanged 64 MiB source cap limits that growth.
- The change affects frontend resource policy only. It changes no Zia grammar,
  IL opcode or verifier rule, runtime C ABI, product dependency, or platform
  adapter.
- Zanna Studio can continue its source refactor while retaining one normal
  compiler invocation and its existing cross-platform build path.

## Alternatives Considered

- **Keep 256 and merge unrelated Studio modules.** Rejected because it preserves
  the safety number by recreating the oversized source units being removed.
- **Make the limit unbounded.** Rejected because hostile or accidental import
  graphs still need deterministic resource limits.
- **Add a Studio-only compiler flag.** Rejected because source validity should
  not depend on an application-specific invocation and other large Zia
  applications need the same bounded capacity.
- **Raise the depth limit too.** Rejected because Studio needs a wider graph,
  not deeper recursion; retaining 50 preserves the stack-safety policy.
