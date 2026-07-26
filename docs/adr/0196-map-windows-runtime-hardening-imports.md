---
status: active
audience: contributors
last-verified: 2026-07-26
---

# ADR 0196: Map Windows Runtime Hardening Imports

## Status

Accepted

## Context

Zanna's in-tree Windows native linker resolves a deliberately fixed set of
dynamic symbols to their owning Windows system DLLs. The Windows runtime
hardening pass replaces mutable `_wfopen` snapshots with `_wsopen_s`, selects
the explicit Unicode `CreateEventW` API for WASAPI synchronization, and removes
the final `GetWindowLongA` and `SetWindowLongA` calls from the graphics window
adapter.

MSVC emits references to `_wsopen_s`, `CreateEventW`, `GetWindowLongW`, and
`SetWindowLongW` for those implementations. The system DLLs already export the
functions, but the fixed dynamic-symbol policy did not recognize them. Native
Zanna Studio therefore compiled successfully and then failed during its
Zanna-owned link. These mappings are cross-layer dependencies between the
Windows runtime adapters, MSVC object code, and the native linker, so they must
be recorded explicitly.

## Decision

The Windows dynamic-symbol policy recognizes these exact imports and maps them
as follows:

- `_wsopen_s` maps to `ucrtbase.dll`, or `ucrtbased.dll` when the existing
  debug-runtime policy is active.
- `CreateEventW` maps to `kernel32.dll`.
- `GetWindowLongW` and `SetWindowLongW` map to `user32.dll`.

All four imports remain Windows-only. Linux and macOS planners must not
recognize them. Platform-import regressions pin the owning DLL and the
Windows-only scope for each symbol.

## Consequences

- Native Zanna Studio, tools, and applications can use stable-sharing CRT file
  opens and explicit Unicode Win32 synchronization/window APIs.
- The supported Windows system DLLs remain the sole implementations; no
  product dependency or compatibility shim is introduced.
- IL, verifier rules, runtime C ABI, serialized formats, and public APIs are
  unchanged.
- Future Windows adapter API changes that introduce native imports still
  require an explicit planner mapping and regression coverage.

## Alternatives Considered

- Reverting to `_wfopen`, `CreateEventA`, `GetWindowLongA`, and
  `SetWindowLongA` was rejected because it would discard the stable-snapshot
  and explicit-Unicode guarantees that motivated the hardening.
- Resolving these ordinary baseline APIs dynamically through `LoadLibrary`
  and `GetProcAddress` was rejected because the native linker already owns a
  fixed, testable import policy.
- Adding in-tree forwarding shims was rejected because UCRT, Kernel32, and
  User32 already define and ship the required ABIs on supported Windows
  systems.
