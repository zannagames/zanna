---
status: active
audience: contributors
last-verified: 2026-06-30
---

# ADR 0019: GUI Text/Cell Metrics (Zanna.GUI.OutputPane measurement)

## Status

Accepted (runtime implemented; Zanna Studio's terminal and tool panels are the
intended first consumers). Driven by the GUI runtime-additions review,
recommendation **R2** (review document retired 2026-07-15; the recommendation ids are the surviving record).

## Context

A GUI app that lays out text in a pane needs to know the pane's font metrics:
how wide a character cell is, how tall a line is, how many columns/rows fit, and
how wide a given string renders. `Zanna.GUI.CodeEditor` already exposes pixel
geometry (`GetCursorPixelX/Y`, `GetLineAtPixel`, `GetColAtPixel`) and
`Zanna.Graphics.Canvas` has `TextWidth/TextHeight`, but the *other* text widgets —
`OutputPane` (which also backs the integrated terminal) and the tool panels —
expose nothing. So applications guess a fixed monospace cell:

- `zannastudio/src/terminal/terminal_controller.zia:127` — `var cols = width / 8;`
- `zannastudio/src/terminal/terminal_controller.zia:143` — `var rows = height / 18;`
- `zannastudio/src/ui/app_shell.zia:1602` — `col = app.GetWidth() / 8;` (output wrap)
- `zannastudio/src/ui/tool_panel_text.zia:41-59` — hardcoded column widths.

These `/ 8` and `/ 18` constants are wrong for any non-default font size or DPI
scale, and they duplicate a measurement the renderer already performs internally
(the OutputPane measures `"M"` to size its monospace grid). Exposing the metric
is missing runtime infrastructure, not application logic.

Adding runtime methods is a runtime C-ABI surface change, which requires an ADR.

## Decision

Add five measurement methods to `Zanna.GUI.OutputPane`:

- `GetCellWidth() -> i64` — pixel advance of one monospace character cell
  (the width of `"M"` in the pane's font), `0` when no font is set.
- `GetCellHeight() -> i64` — pixel line height, `0` when no font is set.
- `MeasureText(text: str) -> i64` — pixel width of `text` in the pane's font
  (sums glyph advances; works for proportional fonts too), `0` when no font/empty.
- `ColumnsForWidth() -> i64` — whole character columns that fit in the pane's
  current width: `floor(width / cellWidth)`, `0` when no font.
- `RowsForHeight() -> i64` — whole rows that fit in the pane's current height:
  `floor(height / cellHeight)`, `0` when no font.

`ColumnsForWidth` / `RowsForHeight` divide by the *integer* cell metrics, so the
identity `ColumnsForWidth() == floor(GetWidth() / GetCellWidth())` holds (a caller
can reproduce it), and `MeasureText("M") == GetCellWidth()`.

### Implementation / layering

The measurement lives in the GUI toolkit (`src/lib/gui`), where the font and the
pane's arranged size already are, as five `vg_outputpane_*` functions in
`vg_outputpane.c` (declared in `vg_ide_widgets_panels.h`). They reuse the
toolkit's `vg_font_measure_text` / the pane's `line_height` — the same source the
renderer uses — so a metric can never disagree with what is drawn. The Zanna
runtime binding (`rt_outputpane_*` in `rt_gui_widgets_complex.c`) is a thin
self-guarding wrapper, with graphics-disabled twins returning `0` in the same
file's `#else` block. No new class, no new class id, no new `rt_*.c/.h` file (so
no `source_health` runtime-contract surface change).

## Consequences

- **Adoption:** `width / 8` / `height / 18` become `pane.ColumnsForWidth()` /
  `pane.RowsForHeight()`, and the output wrap column / tool-panel widths derive
  from the real font. Correct under any font size and DPI scale.
- **Determinism / cross-platform:** pure measurement over the existing font
  rasterizer; no new OS surface, no platform `#ifdef`. The disabled-graphics
  build keeps the symbols as `0`-returning no-ops.
- **No behavior risk:** purely additive; existing OutputPane methods unchanged.

## Alternatives Considered

- **A generic `Widget.MeasureText(text)` on the widget base.** Rejected for this
  pass: most widgets have no single text font, and the concrete consumers
  (terminal, output, tool panels) are all `OutputPane`. The method can be lifted
  to a shared text-widget base later if a second consumer type appears.
- **`Zanna.GUI.Font.Measure(font, text)`.** Rejected as the primary surface: the
  caller would have to fetch the pane's font and size and re-derive the cell grid,
  re-introducing the coupling the pane already owns. (The pane methods delegate to
  exactly this internally.)
- **Keep the `/ 8` constants.** Rejected: wrong for every non-default font size
  and DPI scale, and every Zanna GUI app that renders monospaced text re-derives
  the same guess.
