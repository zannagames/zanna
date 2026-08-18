---
status: active
audience: contributors
last-verified: 2026-08-17
---

# ADR 0004: Graphics3D Runtime Surface Expansion Uses Registry-Only Semantics

Date: 2026-05-31

Status: Accepted; verified against source, the live runtime registry, and
focused tests on 2026-06-27

## Context

The 3dnextlevel3 work added and extended many public `Zanna.Graphics3D` and
`Zanna.Game3D` runtime APIs. Public runtime APIs are registered through
`src/il/runtime/runtime.def` and classified by
`src/il/runtime/RuntimeSurfacePolicy.inc`, so broad 3D API work can touch the IL
runtime registry even when it does not change IL instructions or VM execution.

GATE-009 requires ADR coverage for IL/VM-touching changes. The audit for
3dnextlevel3 found registry and surface-policy edits, but no VM execution,
native codegen, IL opcode, verifier, linker, or language semantic changes.

## Decision

Treat Graphics3D/Game3D runtime-surface additions as registry-only IL touchpoints
when they meet all of these conditions:

- they add or classify extern names, aliases, classes, or runtime signatures for
  existing runtime implementation functions;
- they do not add or reinterpret IL opcodes, IL types, linkage rules, verifier
  rules, VM call/heap/exception behavior, numeric semantics, or native codegen
  lowering;
- they are covered by runtime completeness, ABI/surface, docs-snippet, and
  focused behavior tests for the new API.

Registry-only touchpoints do not need a feature-specific ADR beyond this policy
note. Changes that alter IL, VM, verifier, linker, language semantic, or native
codegen behavior still require their own ADR and proof note.

## Implementation Status

Verified on 2026-06-27:

- `build/install/bin/zanna --dump-runtime-api` exposes 55
  `Zanna.Graphics3D.*` classes, 37 `Zanna.Game3D.*` classes, and 1322 runtime
  functions in those namespaces.
- `src/il/runtime/runtime.def` and
  `src/il/runtime/RuntimeSurfacePolicy.inc` remain the registry and
  classification sources for the public surface.
- The implementation is backed by source under `src/runtime/graphics/3d/` and
  shared 3D graphics helpers under `src/runtime/graphics/common/`.
- Focused source/runtime checks pass:
  `test_runtime_surface_audit`, `test_graphics3d_abi_surface`,
  `test_runtime_class_qualified_surface`, `g3d_3dnext2_surface_probe`,
  `test_rt_game3d`, and `test_rt_graphics3d_robustness`.

## Consequences

The 3dnextlevel3 runtime-surface edits are covered by this ADR, the runtime
surface/ABI tests, and the Game3D/Graphics3D behavior tests listed above. The
public API remains visible to interpreted and native execution through the same
runtime registry, while the IL and VM semantics remain unchanged.

## Spec Impact

No IL language semantics changed. The impact is limited to additional runtime
catalog entries and policy classifications for public Graphics3D/Game3D APIs.
