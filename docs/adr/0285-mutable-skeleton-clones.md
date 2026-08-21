---
status: active
audience: contributors
last-verified: 2026-08-20
---

# ADR 0285: Mutable Skeleton Clones Preserve Importer Bind Space

## Status

Accepted (2026-08-20).

## Context

Imported skeletons freeze when a mesh, player, controller, or animation runtime
binds them. Role-specific augmentation therefore cannot append joints in place.
Rebuilding a skeleton from public local bind poses is also incorrect: GLTF can
supply inverse-bind matrices containing mesh/armature unit conversions that are
not reconstructible from the local hierarchy. Recomputing those matrices made
new finger-weighted vertices jump roughly two orders of magnitude at render.

## Decision

Add `Skeleton3D.CloneMutable()`. It copies exact bone names, parents, local bind
data, importer-authored inverse-bind matrices, and retarget aliases into a new
unfrozen skeleton. Appending a bone derives its initial inverse bind as
`inverse(localBind) * parent.inverseBind`; this propagates the imported parent
space without rewriting existing bones. `ComputeInverseBind()` remains the
explicit full-hierarchy recomputation for entirely authored skeletons.

## Consequences

- A role can add fingers, sockets, or cosmetic joints without mutating a shared
  imported asset or losing its unit conversion.
- Clones duplicate bounded skeleton metadata; mesh and controller ownership
  remain explicit.
- Callers must not call full `ComputeInverseBind()` after appending to a clone
  whose source carried custom importer matrices.

## Tests

`test_rt_skeleton3d` freezes a source, injects an importer-style inverse-bind
conversion, clones it, verifies exact inverse/alias preservation and mutability,
then checks an appended child inherits the converted parent space.
