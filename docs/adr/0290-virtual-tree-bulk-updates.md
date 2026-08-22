---
status: active
audience: contributors
last-verified: 2026-08-21
---

# ADR 0290: Add Nestable VirtualTree Bulk Updates

## Status

Accepted

## Context

Every `VirtualTree` mutation immediately rebuilt or invalidated its bound
TreeView projection. Variable trees and scene hierarchies update many labels,
icons, expansion states, and nodes as one logical refresh, making projection
work proportional to the number of mutations rather than the resulting model.

Adding methods changes the runtime C ABI and registry surface.

## Decision

Add nestable `VirtualTree.BeginUpdate()` and `EndUpdate()` methods. Mutations
continue to update the hash-indexed model immediately, while bound-control
projection is marked pending. The outermost `EndUpdate` synchronizes flattened
rows, selection, row count, and paint once. An unmatched `EndUpdate` traps as a
programming error.

Studio batches debugger variable-tree construction and 3D hierarchy label
refreshes through this surface.

## Consequences

- Large logical refreshes perform one bound-control synchronization.
- Queries can still observe the updated data model inside a batch.
- Callers must pair scopes on every control-flow path; nesting lets helper
  layers participate safely.

## Alternatives Considered

Automatically debounce projection until the next frame would make mutation
visibility timing implicit. Replacing the whole model in one call would require
a second schema and duplicate validation rules already enforced by `AddNode`.
