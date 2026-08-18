---
status: active
audience: contributors
last-verified: 2026-08-17
---

# ADR 0161: Add Stable SceneNode Sibling Reordering

## Status

Accepted (2026-07-23)

## Context

`SceneNode.AddChild` and `TryAddChild` append a child, and `RemoveChild` detaches
one, but the public runtime has no way to change the order of existing siblings
without a detach/attach sequence. That workaround changes retained ownership,
temporarily breaks the parent relationship, moves the child to the end only,
and makes an editor operation harder to validate and roll back.

Sibling order is observable in `GetChild`, recursive traversal, canonical VSCN
serialization, rendering order for otherwise equivalent nodes, and Studio's
hierarchy. A substantial scene editor needs explicit, undoable ordering without
patching private runtime arrays or rebuilding whole subtrees.

This change adds a public runtime C ABI entry point, so ADR 0006 requires an
explicit decision.

## Decision

`Zanna.Graphics3D.SceneNode` gains this additive instance method:

```text
TryMoveChild(child: SceneNode, index: Integer) -> Boolean
```

Its C ABI entry point is:

```c
int8_t rt_scene_node3d_try_move_child(
    void *parent, void *child, int64_t index);
```

The method accepts only an existing direct child and a strict target index in
`0..ChildCount-1`. It moves that retained child slot to the target and shifts
the intervening slots, preserving the relative order of every other sibling.
Moving to the already-satisfied index succeeds.

The implementation validates the complete bounded direct-child table before
mutation. Null or wrong-class handles, a non-child relationship, an invalid
index, duplicate slots for the requested child, invalid child slots, or
inconsistent parent back-links return false without changing any slot. The
operation does not detach or retain the child, change its `Parent`, propagate
scene ownership, or allocate memory. A successful changed move dirties the
parent and its owning scene's traversal/spatial state.

Graphics-disabled builds export the same symbol and return false.

Zanna Studio uses the primitive for **Earlier** and **Later** hierarchy
commands. A command operates on one contiguous selection of direct siblings
under one parent, preserves the selected block's internal order, serializes
once, and creates one history entry. Mixed-parent or non-contiguous selections
and boundary moves are disabled and remain document/history no-ops. Runtime or
serialization failure restores the complete prior canonical scene and
selection.

## Consequences

- Runtime and tools can reorder children without an ownership-affecting
  detach/attach sequence.
- Child order remains deterministic through traversal and VSCN round trips.
- The API is additive, allocation-free, and O(number of direct children).
- Studio gains explicit multi-selection sibling ordering with exact undo/redo.
- Native runtime tests, the reviewed Graphics3D ABI manifest, generated runtime
  docs, the Graphics3D guide, and Studio probes must cover the new surface.
- This does not add arbitrary insertion during initial attachment, cross-parent
  sorting, drag-and-drop hierarchy rows, or world-transform-preserving
  reparenting.

## Alternatives Considered

- **Remove and re-add the child.** Rejected because it changes ownership state,
  exposes an intermediate detach, and only appends.
- **Expose the native child array.** Rejected because callers could violate
  retention, parent, ownership, and corruption-hardening invariants.
- **Add `InsertChild` only.** Deferred because Studio is moving an existing
  retained child; combining reparenting and insertion would make failure and
  ownership semantics larger.
- **Reorder only inside Studio's serialized JSON.** Rejected because Studio
  must use the canonical runtime graph and serializer rather than becoming a
  second VSCN implementation.
