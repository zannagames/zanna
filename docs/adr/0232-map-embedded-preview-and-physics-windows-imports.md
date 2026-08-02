---
status: active
audience: contributors
last-verified: 2026-08-01
---

# ADR 0232: Map Embedded Preview and Physics Windows Imports

## Status

Accepted (2026-08-01)

## Context

Zanna's native Windows linker admits a fixed, reviewed set of dynamic symbols
and maps each symbol to its owning system DLL. The embedded Studio play channel
uses named file mappings on Windows, while the hardened 2D physics arithmetic
uses additional standard C math functions to preserve numerical range and
small-value accuracy.

MSVC object compilation succeeds without planner entries because the native
APIs and CRT declarations are available. The complete standalone Zanna Studio
link then fails closed when it reaches `CreateFileMappingA`,
`OpenFileMappingA`, `MapViewOfFile`, `UnmapViewOfFile`, `expm1`, `fma`,
`frexp`, `log1p`, or `scalbn`. Focused library/test links use MSVC's normal
linker and therefore do not expose this integration boundary.

These mappings are cross-layer dependencies between the Windows runtime
adapters, MSVC object code, and Zanna's native linker, so they require an
explicit decision and regression coverage.

## Decision

The Windows import planner recognizes these exact symbols:

- `CreateFileMappingA`, `OpenFileMappingA`, `MapViewOfFile`, and
  `UnmapViewOfFile` map to `kernel32.dll`.
- `expm1`, `fma`, `frexp`, `log1p`, and `scalbn` map to `ucrtbase.dll`, or
  `ucrtbased.dll` when the existing debug-runtime policy is active.

The four file-mapping names remain Windows-only in the dynamic-symbol policy.
The five standard C math names retain their existing portable libm/libSystem
classification on Linux and macOS. The planner changes only DLL ownership for
Windows native output.

## Consequences

- Standalone native Studio and application links can include the embedded play
  channel and hardened 2D physics runtime without an unresolved-symbol failure.
- No external product dependency is added: Kernel32 and the Universal CRT are
  already baseline Windows runtime dependencies.
- IL opcodes, grammar, verifier rules, serialized formats, and the runtime C ABI
  are unchanged.
- New Windows adapter or CRT calls must continue to fail closed until their DLL
  ownership is explicitly mapped and tested.

## Alternatives Considered

- **Exclude embedded preview or physics objects from native Studio.** Rejected
  because it would make the standalone application behavior differ from the
  tested runtime libraries.
- **Resolve the APIs manually with `GetProcAddress`.** Rejected because all nine
  functions are baseline exports on supported Windows systems and the fixed
  import planner is the project-wide auditable mechanism.
- **Replace the math functions with lower-accuracy formulas.** Rejected because
  it would reintroduce overflow and cancellation defects that the runtime
  implementations intentionally avoid.

## Validation

The platform-import planner regression requires the four mapping functions to
resolve only on Windows and to `kernel32.dll`. It requires the five math
functions to resolve to release/debug UCRT variants. The clean canonical
Windows build must then link and smoke the standalone native Studio before the
full CTest, audit, and install gates can pass.
