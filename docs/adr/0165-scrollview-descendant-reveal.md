---
status: active
audience: contributors
last-verified: 2026-08-17
---

# ADR 0165: Expose ScrollView Descendant Reveal

## Status

Accepted (2026-07-23)

## Context

The native GUI toolkit already implements
`vg_scrollview_scroll_to_widget(scroll, child)`. It validates that `child` is
a descendant, computes the descendant's content-relative bounds through its
complete parent chain, scrolls only the axes needed to make those bounds
visible, clamps the result, and invalidates layout and paint.

That capability is not present in Zanna's runtime registry or
`Zanna.GUI.ScrollView` class. Zanna applications can set raw scroll offsets,
but a controller cannot reliably reveal a deeply nested focus target without
duplicating private layout and scrollbar calculations. Zanna Studio's scene
hierarchy Find command exposes this gap: the 2D hierarchy query is intentionally
located beside the object tree, below other inspector sections, and focusing it
must also make it visible.

Adding a public runtime entry point changes the C ABI and registered GUI
surface, so ADR 0006 requires an explicit decision.

## Decision

`Zanna.GUI.ScrollView` gains one additive instance method:

```text
ScrollTo(widget: Zanna.GUI.Widget)
```

Its C ABI entry point is:

```c
void rt_scrollview_scroll_to(void *scroll, void *widget);
```

The runtime validates both handles. The first must be a live ScrollView and the
second must be a live GUI widget. A null, stale, wrong-type ScrollView, invalid
widget, or widget outside the ScrollView's descendant tree is an inert no-op.
The method does not reparent, resize, focus, or otherwise mutate the target.
The toolkit retains one non-owning descendant request, guarded by the target's
immutable live-widget ID, through the next layout pass. This makes a request
issued while an ancestor is restoring from a collapsed, zero-sized state
resolve against settled descendant and viewport geometry. A destroyed,
detached, or allocator-address-reused target is discarded safely.

The underlying toolkit remains responsible for:

- resolving arbitrary descendant depth;
- accounting for current scroll offsets and scrollbar-reduced viewport size;
- changing only offsets required for complete visibility;
- clamping offsets to the current content extent; and
- invalidating layout and paint after a successful reveal; and
- re-resolving one live-ID-guarded request after the next layout pass.

Graphics-disabled builds provide the same inert C symbol so registry and link
surfaces remain identical across build configurations.

Zanna Studio uses `ScrollTo(hierarchySearchInput)` before focusing a 2D or 3D
hierarchy query. The standard Find command can therefore reveal the inspector,
scroll to the query, and transfer focus without changing scene content,
history, selection structure, or camera state.

## Consequences

- Any Zanna GUI can reveal a nested validation error, search field, or selected
  editor control without duplicating ScrollView geometry.
- Restoring a collapsed pane and revealing one of its descendants may happen
  in the same command without a hard-coded second-frame callback.
- Scene hierarchy Find remains usable even when the inspector was hidden,
  compact, or scrolled far from the hierarchy.
- Existing ScrollView programs remain source- and binary-compatible because
  the method and C symbol are additive.
- GUI runtime tests, the reviewed manifest fingerprint, generated runtime
  documentation, authored widget documentation, and Studio scene probes must
  cover the new contract.

## Alternatives Considered

- **Set a hard-coded inspector scroll offset.** Rejected because control sizes,
  fonts, UI scale, wrapping, and future inspector sections make offsets stale.
- **Move hierarchy search to the global toolbar.** Rejected because it obscures
  which hierarchy is searched and still does not solve descendant reveal for
  other Zanna applications.
- **Duplicate descendant geometry in Studio.** Rejected because scrollbar
  visibility, scaling, and clamping are native ScrollView responsibilities.
- **Rely on focus to scroll automatically.** Rejected because focus and
  container scrolling are separate public contracts, and non-native or future
  backends must not infer one from the other.
