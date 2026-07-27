---
status: active
audience: contributors
last-verified: 2026-07-27
---

# ADR 0205: Add Application-Directed ListBox Reordering

## Status

Accepted (2026-07-27)

## Context

`Zanna.GUI.ListBox` supports retained rows, stable row data, selection,
activation, and keyboard navigation, but it cannot express a direct
drag-to-reorder gesture. Applications must approximate ordering with separate
buttons or replace the control with ad hoc row widgets. Zanna Studio's 2D
layer navigator consequently exposes only Up and Down even though layer order
is a primary visual-authoring operation.

Using generic `Widget` drag/drop does not provide a sound row contract. A
widget cannot drop onto itself, the target has no retained-row insertion
position, and the application receives no drag source index or insertion
feedback. Letting the ListBox mutate an application-owned model would be
equally unsafe: Studio must validate the operation, update workspace identities,
serialize the scene, and create exactly one undo transaction.

The required polling surface changes the public runtime C ABI and registry, so
ADR 0006 requires an explicit decision.

## Decision

`Zanna.GUI.ListBox` gains these additive instance methods:

```text
SetReorderable(enabled: Boolean)
WasReorderRequested() -> Boolean
GetReorderSourceIndex() -> Integer
GetReorderTargetIndex() -> Integer
```

Their C ABI entry points are:

```c
void rt_listbox_set_reorderable(void *listbox, int64_t enabled);
int64_t rt_listbox_was_reorder_requested(void *listbox);
int64_t rt_listbox_get_reorder_source_index(void *listbox);
int64_t rt_listbox_get_reorder_target_index(void *listbox);
```

### Application-directed request

Reordering is disabled by default and applies only to retained-item ListBoxes.
Enabling it does not move rows. A primary-button press selects and arms the
pressed row; moving at least six scaled logical pixels begins a captured drag.
The control paints an insertion marker and scrolls near its top or bottom edge.
Releasing over a different valid position latches one source and final-target
index request. The target index is the row's index after removing the source
and inserting it at the indicated position.

`WasReorderRequested` consumes only the pending edge. The source and target
getters return the most recently latched indices, or `-1` before a request,
after disabling reordering, or after the retained rows are cleared. A later
request may replace those values. The ListBox never changes item linkage,
selection order, row data, or revision in response to a request; the
application owns model mutation and subsequent presentation refresh.

A press/release below the threshold remains an ordinary selection gesture.
Drops back at the source position are no-ops. Escape, disabling reordering,
clearing rows, widget destruction, or loss of a valid captured source cancels
the in-progress gesture and releases input capture without latching a request.
Virtual ListBoxes never arm or publish reorder requests.

### Keyboard parity

With a retained row selected and reordering enabled, Alt+Up and Alt+Down latch
an adjacent reorder request through the same polling surface. Requests at the
first or final boundary are no-ops. Existing unmodified and Shift-modified
navigation behavior is unchanged.

### Studio transaction

The 2D layer navigator enables reordering. Each pointer or keyboard request
calls the existing `SceneDocument.MoveLayer`, remaps Lock/Solo workspace state
to the same live layer, keeps that layer active, and commits exactly one
canonical scene history entry. Invalid, boundary, or identity no-ops leave
scene bytes, revision, dirty state, and history untouched. Up and Down remain
accessible alternatives.

## Consequences

- Retained ListBoxes can offer direct ordering without taking ownership of an
  application model.
- Studio layer drag order, keyboard order, and explicit buttons share one
  canonical transaction path.
- Existing ListBox programs remain compatible because reordering is opt-in.
- Virtualized models remain responsible for their own ordering protocol.
- Native widget tests, graphics-disabled runtime checks, runtime ABI tests,
  registry/generated documentation, authored widget documentation, and Studio
  probes must cover the new surface.

## Alternatives Considered

- **Use generic Widget drag/drop.** Rejected because self-drop is excluded and
  the generic contract has no row source, insertion target, or row feedback.
- **Reorder retained items inside ListBox.** Rejected because application-owned
  models require validation, transactional serialization, and rollback before
  presentation may change.
- **Expose only the hovered row.** Rejected because callers would still need to
  reconstruct threshold, capture, insertion, auto-scroll, and cancellation
  semantics inconsistently.
- **Keep Up and Down as the only controls.** Rejected because direct ordering
  is a baseline scene-authoring interaction and becomes cumbersome with many
  layers.
- **Support virtual models in the same protocol.** Deferred because virtual
  item ownership and asynchronous model mutation need a separate stable-ID
  request contract rather than retained numeric indices.
