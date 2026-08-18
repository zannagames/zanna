---
status: active
audience: contributors
last-verified: 2026-06-30
---

# ADR 0022: GUI Layout Conveniences (panel centering + Zanna.GUI.Grid)

## Status

Accepted (runtime implemented; Zanna Studio's modal overlays and tool/data panels are
the intended first consumers). Driven by the GUI runtime-additions review,
recommendation **R5** (review document retired 2026-07-15; the recommendation ids are the surviving record).

## Context

Two common layout tasks have no runtime support, so apps hand-roll them:

1. **Centering a floating panel/modal.** `Zanna.GUI.FloatingPanel` exposes
   `SetPosition`/`SetSize` but no "center me in the window". Zanna Studio's
   `ui/ide_overlays.zia:690-715` (`layoutAboutPanel`) re-derives the window size,
   computes `(rootW - panelW)/2`, and clamps to the screen by hand.
2. **Tabular data with aligned columns.** There is no grid/table widget, so the
   tool panels in `ui/tool_panel_text.zia` hardcode column widths
   (`KindColumnWidth()=10`, …) and pad monospace strings to fake columns — which
   breaks for any proportional font or content that exceeds the guessed width.

Both are missing runtime infrastructure for dialogs and property/data panels.
Adding runtime methods/classes is a runtime C-ABI surface change, requiring an ADR.

## Decision

### Part 1 — `Zanna.GUI.FloatingPanel.CenterInParent()`

Add `CenterInParent()` to `FloatingPanel`: positions the panel centered within its
connected root's arranged bounds (the same coordinate space `SetPosition` uses),
clamped to the top-left when the panel is larger than the root. Replaces the
hand-rolled center-and-clamp.

### Part 2 — `Zanna.GUI.Grid`

Add a tabular **data grid** widget whose columns **auto-size to their widest cell**:

- `New(parent) -> Grid`
- `SetColumns(count)` — set the column count (clears headers and cells).
- `SetHeader(col, text)` — optional column header (drawn as a header row).
- `SetCell(row, col, text)` — set a cell, growing the row count as needed.
- `GetCell(row, col) -> str` — a cell's text (empty when out of range).
- `Clear()` — remove all rows (columns and headers are kept).
- `SetFont(font, size)` — header/cell font.
- `GetColumnWidth(col) -> i64` — the auto-sized pixel width of a column: its widest
  header/cell text plus padding (0 when no font is set).
- `RowCount` / `ColumnCount` properties.

Plus the common `Widget` methods (`Destroy`, `SetVisible`, `SetSize`, layout/state).

The grid is a **non-interactive display widget** — no selection, scrolling, or
in-place editing in this version (deferred; see below). Its value is correct
auto-sized columns for property and data panels, replacing hardcoded widths.

### Naming / implementation

- The user-facing class is `Zanna.GUI.Grid` (the leaf `Grid` is globally unique;
  `Table` already exists as `Zanna.Game.UI.Table`).
- The C internals are named `vg_datagrid` / `rt_datagrid` to avoid colliding with
  the pre-existing `vg_grid` **layout container** in `vg_layout.h` (a CSS-grid-style
  layout, a different concept).
- The widget lives in `src/lib/gui/src/widgets/vg_datagrid.c` (a new `VG_WIDGET_DATAGRID`
  type) with rt wrappers in `rt_gui_widgets_complex.c` (real + graphics-disabled
  twins) and a new `RTCLS_GuiGrid` runtime class id. Column auto-sizing reuses
  `vg_font_measure_text` — the same source the renderer uses — so a column width can
  never disagree with what is drawn, and it is unit-testable headlessly.

## Consequences

- **Adoption:** `layoutAboutPanel`'s center-and-clamp collapses to
  `panel.CenterInParent()`; the tool panels' hardcoded column widths and string
  padding are replaced by a `Grid` that sizes columns to content. Both generalize to
  any Zanna GUI app's dialogs and data panels.
- **Determinism / cross-platform:** pure layout bookkeeping over existing widgets
  and the existing font rasterizer; no new OS surface, no platform `#ifdef`.
- **No behavior risk:** purely additive; existing FloatingPanel methods and widgets
  are unchanged.

## Alternatives Considered

- **A full interactive grid (selection, sorting, scrolling, editing).** Deferred:
  the driving need is auto-sized columns for *display* panels; the interactive
  surface is large and can be layered on later (the widget already owns its cell
  model and column metrics). Shipping the display widget now delivers R5's value
  without gold-plating.
- **Reuse the existing `vg_grid` layout container.** Rejected: that is a positional
  *layout* (place children in a grid of cells), not a *data table* with text cells
  and content-driven column widths. Different model, hence a distinct widget.
- **A column-width helper instead of a widget.** Rejected: the report calls for a
  widget, and a helper would still leave every app rendering rows by hand. The
  widget owns the cells, the auto-sizing, and the aligned rendering together.
- **Center via `App.CenterPanel(panel)` instead of a panel method.** Rejected: the
  panel already knows its own size and root, so `panel.CenterInParent()` needs no
  extra arguments and reads at the call site.
