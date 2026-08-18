---
status: active
audience: contributors
last-verified: 2026-06-30
---

# ADR 0023: Caret-Anchored Filtered Popup (Zanna.GUI.PopupList)

## Status

Accepted (runtime implemented; Zanna Studio's completion popup is the intended first
consumer). Driven by the GUI runtime-additions review, recommendation **R6**
(review document retired 2026-07-15; the recommendation ids are the surviving record).

## Context

Autocomplete is everywhere — code completion, search boxes, address bars, command
inputs — and it is always the same widget: a small list, anchored at the caret,
that filters as you type, navigates by arrow keys, accepts on Enter, and dismisses
on edit/escape. The runtime has `Dropdown`, `ContextMenu`, `CommandPalette`, and
`Tooltip`, but none is a *caret-anchored, filterable, keyboard-navigable list a host
can drive*.

So Zanna Studio hand-rolls it: `editor/completion.zia` (1,288 LOC) builds a
`FloatingPanel` + an embedded `ListBox`, then manages a state machine on top —
`isVisible`, `selectedIndex`, `lastLine`/`lastCol`, anchor-at-caret, filtering,
dismiss-on-edit, accept. The *positioning* is already supported (the caret-pixel
methods from the editor), but the popup mechanics are re-implemented by hand. Any
Zanna app with autocomplete re-derives the same machine. This is missing runtime
infrastructure, not application logic.

Adding a runtime class is a runtime C-ABI surface change, requiring an ADR.

## Decision

Add `Zanna.GUI.PopupList`, a caret-anchored filtered list rendered in the overlay
pass. The host supplies pre-ranked items and drives keyboard and visibility; the
widget owns the popup mechanics.

- `New(parent) -> PopupList` — created hidden; attached to the root so it paints in
  the overlay pass (floats above content).
- `AddItem(text)` / `Clear()` — populate (the host adds items in its own rank order;
  language-specific ranking stays in the host).
- `SetFilter(text)` — keep only items containing `text` (case-insensitive substring);
  resets the selection.
- `VisibleCount` — number of items currently matching the filter.
- `NavigateUp()` / `NavigateDown()` — move the selection within the visible items
  (clamped); `SetSelectedIndex(i)` / `GetSelectedIndex() -> i64` (`-1` when none).
- `GetSelected() -> str` — the selected visible item's text.
- `AcceptSelected()` / `WasAccepted() -> i1` — the host calls `AcceptSelected` on
  Enter and reads `WasAccepted` (consume-on-read) to perform the insertion.
- `AnchorAt(x, y)` — position the popup (e.g. at the caret pixel), `SetWidth`,
  `SetMaxRows`, `SetFont`.
- `SetVisible(on)` / `IsVisible() -> i1` — the host shows it when there are
  candidates and hides it on edit/escape (the "dismiss on edit" the host already
  detects via its document revision).

The filter/selection/accept logic is pure (no font/window), so it is unit-tested
headlessly; rendering draws the visible items at the anchor with a selection
highlight in the overlay pass.

### Naming / implementation

`Zanna.GUI.PopupList` — the leaf `PopupList` is globally unique. The widget is a new
`VG_WIDGET_POPUPLIST` (`vg_popuplist` in `src/lib/gui`) with rt wrappers
(`rt_popuplist_*` in `rt_gui_widgets_complex.c`, real + graphics-disabled twins) and
a new `RTCLS_GuiPopupList` class id. It reuses the existing overlay mechanism (a
`paint_overlay` vtable hook + attaching to root), so floating needs no new
framework support; it draws its own items (no child widgets), so it is simpler than
`FloatingPanel`.

## Consequences

- **Adoption:** Zanna Studio's completion popup collapses from "FloatingPanel + ListBox +
  hand-rolled state machine" to a single `PopupList` it populates, filters, and
  reads `WasAccepted` from. Any app gets autocomplete UI for free (search boxes,
  command inputs).
- **Determinism / cross-platform:** pure list/selection bookkeeping plus the existing
  overlay renderer and font rasterizer; no new OS surface, no platform `#ifdef`.
- **No behavior risk:** purely additive; existing widgets are unchanged.

## Alternatives Considered

- **Extend `CommandPalette`.** Rejected: the palette is a full-screen-ish modal
  keyed by command ids with its own show/hide and input capture, not a small list
  anchored at an arbitrary caret position that the host drives key-by-key while
  keeping editor focus. Different interaction model.
- **A `Dropdown` variant.** Rejected: a dropdown is bound to a control and toggles a
  list below it; it is not freely anchorable at a caret nor host-driven.
- **Let the widget own ranking/fuzzy-matching.** Rejected: ranking is
  language/domain specific (completion scoring, recency, kind weighting). The widget
  does only a neutral substring filter; the host adds items already ranked, keeping
  the policy where the knowledge is.
- **Build the overlay from scratch.** Rejected: the `paint_overlay` + attach-to-root
  mechanism already exists (used by `FloatingPanel`); reusing it keeps the widget to
  its list/selection logic.
