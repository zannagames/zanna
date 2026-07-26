//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/gui/rt_gui_codeeditor_internal.h
// Purpose: Shared CodeEditor helpers used across the editor's feature modules
//          (syntax highlighting, gutter, folding, cursors, completion) split
//          between rt_gui_codeeditor.c and rt_gui_codeeditor_syntax.c.
//
// Key invariants:
//   - Engine-internal; included only by the gui/ CodeEditor translation units.
//   - Column helpers translate between byte and character columns for UTF-8
//     lines; the handle cast validates the widget class id.
//
// Ownership/Lifetime:
//   - Helpers borrow the caller's editor/selection; no allocation.
//
// Links: src/runtime/graphics/gui/rt_gui_codeeditor.c (editor core + features),
//        src/runtime/graphics/gui/rt_gui_codeeditor_syntax.c (highlighting)
//
//===----------------------------------------------------------------------===//

/// @file rt_gui_codeeditor_internal.h
/// @brief Declares shared validation, UTF-8 column, and selection helpers for CodeEditor modules.
///
/// @details
/// These internal helpers operate on borrowed widget state and centralize the
/// conversions required by the runtime-facing editor feature implementations.
#pragma once

#include "rt_gui_internal.h"

// Shared CodeEditor helpers (defined in rt_gui_codeeditor_syntax.c).
/// @brief Validate an opaque runtime handle as a live CodeEditor widget.
/// @param editor Candidate runtime widget handle.
/// @return Validated CodeEditor pointer, or `NULL` when the handle is invalid or has another type.
vg_codeeditor_t *rt_codeeditor_handle_checked(void *editor);

/// @brief Validate a gutter slot and optionally convert it to the widget-layer type.
/// @param slot Runtime gutter slot in the supported range 0 through 4.
/// @param[out] out_type Optional destination for the validated integer slot.
/// @return `1` when @p slot is valid; otherwise `0`.
int rt_codeeditor_gutter_slot_checked(int64_t slot, int *out_type);

/// @brief Obtain a logical line's UTF-8 byte length as a bounded integer.
/// @param ce CodeEditor whose line storage is queried.
/// @param line Zero-based logical line index.
/// @return Byte length clamped to `INT_MAX`, or zero for invalid input.
int rt_codeeditor_line_length_i32(const vg_codeeditor_t *ce, int line);

/// @brief Convert a UTF-8 byte offset into a character column.
/// @param ce CodeEditor containing the line.
/// @param line Zero-based logical line index.
/// @param byte_col Byte offset to convert; values beyond the line are clamped.
/// @return Zero-based character column, or zero for invalid input.
int rt_codeeditor_byte_col_to_char_col(const vg_codeeditor_t *ce, int line, int byte_col);

/// @brief Convert a character column into a UTF-8 byte offset.
/// @param ce CodeEditor containing the line.
/// @param line Zero-based logical line index.
/// @param char_col Character column to convert; values beyond the line select its end.
/// @return Byte offset clamped to `INT_MAX`, or zero for invalid input.
int rt_codeeditor_char_col_to_byte_col(const vg_codeeditor_t *ce, int line, int char_col);

/// @brief Order a selection's endpoints from earliest to latest.
/// @param[in,out] selection Selection to normalize; `NULL` is accepted.
void rt_codeeditor_normalize_selection(vg_selection_t *selection);
