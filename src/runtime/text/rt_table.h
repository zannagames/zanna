//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/text/rt_table.h
/// @file
/// @brief Declares the GC-managed fixed-width text table builder.
///
// Purpose: Lay out a table of rows into aligned monospace columns. Zanna has
// `Zanna.Text.TextWrapper` for aligning a *single* string and interactive grid
// widgets in the GUI and HUD families, but nothing that emits an aligned table
// as text. Every report generator, CLI tool, and debug dump therefore
// hand-counts column widths in a header string literal and keeps them in sync
// with a chain of pad calls by eye.
//
// Key invariants:
//   - Column widths are byte widths, matching Zanna.String.PadLeft/PadRight.
//   - A cell wider than its column is never truncated silently; the row grows
//     and the caller sees the misalignment (truncation is opt-in per column).
//   - Header and body are generated from one width declaration, so they cannot
//     drift apart.
//   - Rows are emitted in insertion order; nothing here sorts.
//
// Ownership/Lifetime:
//   - Table objects are heap-allocated runtime objects managed through Zanna's
//     reference-counting/GC lifetime.
//   - Column headers and cell text are copied into the table's own storage, so
//     callers may free or mutate their sources afterwards.
//
// Links: src/runtime/text/rt_table.c (implementation),
//        src/runtime/text/rt_textwrap.c (the single-string aligners),
//        docs/adr/0252-text-table-layout.md
//
//===----------------------------------------------------------------------===//
#pragma once

#include <stdint.h>

#include "rt_string.h"

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Runtime class ID for Text.Table instances.
#define RT_TABLE_CLASS_ID INT64_C(-0x430A01)

/// @brief Left-aligned column (pad on the right).
#define RT_TABLE_ALIGN_LEFT INT64_C(0)
/// @brief Right-aligned column (pad on the left) — the numeric default.
#define RT_TABLE_ALIGN_RIGHT INT64_C(1)
/// @brief Centred column, extra space biased to the right.
#define RT_TABLE_ALIGN_CENTER INT64_C(2)

/// @brief Create an empty table.
/// @return New GC-managed Table handle, or NULL on allocation failure.
void *rt_table_new(void);

/// @brief Append a column with a fixed width.
/// @param obj Table receiver.
/// @param header Column heading; NULL becomes empty.
/// @param width Byte width; values below the header length are raised to it so
///        a heading is never clipped by its own declaration.
/// @param align One of the `RT_TABLE_ALIGN_*` constants; unknown values become
///        left.
/// @return Zero-based column index, or -1 on failure.
int64_t rt_table_add_column(void *obj, rt_string header, int64_t width, int64_t align);

/// @brief Append a column that sizes itself to its widest cell.
/// @details The width is resolved at render time, so cells added later still
///          count. This is what removes the hand-counted header literal.
/// @param obj Table receiver.
/// @param header Column heading; NULL becomes empty.
/// @param align One of the `RT_TABLE_ALIGN_*` constants.
/// @return Zero-based column index, or -1 on failure.
int64_t rt_table_add_column_auto(void *obj, rt_string header, int64_t align);

/// @brief Mark a column as truncating cells that exceed its width.
/// @details Off by default: silently clipping a value is usually worse than a
///          ragged row, because the ragged row is visible.
/// @param obj Table receiver.
/// @param column Zero-based column index.
/// @param on Non-zero to truncate.
void rt_table_set_truncate(void *obj, int64_t column, int8_t on);

/// @brief Number of declared columns.
/// @param obj Table receiver.
/// @return Column count, or zero for an invalid receiver.
int64_t rt_table_column_count(void *obj);

/// @brief Start a new empty row.
/// @param obj Table receiver.
/// @return Zero-based row index, or -1 on failure.
int64_t rt_table_add_row(void *obj);

/// @brief Set one cell's text.
/// @param obj Table receiver.
/// @param row Zero-based row index.
/// @param column Zero-based column index.
/// @param text Cell text; NULL becomes empty.
void rt_table_set_cell(void *obj, int64_t row, int64_t column, rt_string text);

/// @brief Read one cell's text.
/// @param obj Table receiver.
/// @param row Zero-based row index.
/// @param column Zero-based column index.
/// @return Owned copy of the cell text, or the empty string when out of range.
rt_string rt_table_get_cell(void *obj, int64_t row, int64_t column);

/// @brief Number of rows added.
/// @param obj Table receiver.
/// @return Row count, or zero for an invalid receiver.
int64_t rt_table_row_count(void *obj);

/// @brief Remove every row, keeping the column declarations.
/// @param obj Table receiver.
void rt_table_clear_rows(void *obj);

/// @brief Set the separator written between columns.
/// @param obj Table receiver.
/// @param gutter Separator text; NULL becomes a single space.
void rt_table_set_gutter(void *obj, rt_string gutter);

/// @brief Render the header row.
/// @param obj Table receiver.
/// @return Owned header line with no trailing newline; trailing padding on the
///         final column is trimmed so lines have no invisible whitespace.
rt_string rt_table_render_header(void *obj);

/// @brief Render a rule line matching the header width.
/// @param obj Table receiver.
/// @param fill Single-byte fill character; NULL or empty becomes `-`.
/// @return Owned rule line.
rt_string rt_table_render_rule(void *obj, rt_string fill);

/// @brief Render one body row.
/// @param obj Table receiver.
/// @param row Zero-based row index.
/// @return Owned line with no trailing newline, or empty when out of range.
rt_string rt_table_render_row(void *obj, int64_t row);

/// @brief Render the header followed by every row as one newline-joined block.
/// @param obj Table receiver.
/// @param with_header Non-zero to include the header line.
/// @param with_rule Non-zero to include a rule under the header.
/// @return Owned text; no trailing newline.
rt_string rt_table_render(void *obj, int8_t with_header, int8_t with_rule);

/// @brief Alignment constant for left-aligned columns.
/// @return `RT_TABLE_ALIGN_LEFT`.
int64_t rt_table_align_left(void);
/// @brief Alignment constant for right-aligned columns.
/// @return `RT_TABLE_ALIGN_RIGHT`.
int64_t rt_table_align_right(void);
/// @brief Alignment constant for centred columns.
/// @return `RT_TABLE_ALIGN_CENTER`.
int64_t rt_table_align_center(void);

#ifdef __cplusplus
}
#endif
