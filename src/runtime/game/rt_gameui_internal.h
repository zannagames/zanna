//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/game/rt_gameui_internal.h
/// @file
/// @brief Declares shared geometry, UTF-8, ownership, and key helpers for GameUI.
//
// Purpose: Shared helpers and key constants for the immediate-mode GameUI
//          widgets, split across rt_gameui.c (core widgets) and
//          rt_gameui_widgets.c (table/slider/dropdown/tooltip/modal).
//
// Key invariants:
//   - Engine-internal; included only by the game/ GameUI translation units.
//   - Geometry/text/clamp helpers are pure and saturate rather than overflow.
//   - UI_KEY_* values mirror the canvas key codes the poll path receives.
//
// Ownership/Lifetime:
//   - Helpers borrow caller buffers/handles; ui_release_obj drops a GC ref.
//
// Links: src/runtime/game/rt_gameui.c, src/runtime/game/rt_gameui_widgets.c
//
//===----------------------------------------------------------------------===//
#pragma once

#include "rt_gameui_draw.h"
#include "rt_string.h"

#include <stddef.h>
#include <stdint.h>

/// @brief Escape key code delivered by the canvas poll path.
#define UI_KEY_ESCAPE 256
/// @brief Enter key code delivered by the canvas poll path.
#define UI_KEY_ENTER 257
/// @brief Tab key code delivered by the canvas poll path.
#define UI_KEY_TAB 258
/// @brief Backspace key code delivered by the canvas poll path.
#define UI_KEY_BACKSPACE 259
/// @brief Forward-delete key code delivered by the canvas poll path.
#define UI_KEY_DELETE 261
/// @brief Right-arrow key code delivered by the canvas poll path.
#define UI_KEY_RIGHT 262
/// @brief Left-arrow key code delivered by the canvas poll path.
#define UI_KEY_LEFT 263
/// @brief Down-arrow key code delivered by the canvas poll path.
#define UI_KEY_DOWN 264
/// @brief Up-arrow key code delivered by the canvas poll path.
#define UI_KEY_UP 265
/// @brief Page-up key code delivered by the canvas poll path.
#define UI_KEY_PAGE_UP 266
/// @brief Page-down key code delivered by the canvas poll path.
#define UI_KEY_PAGE_DOWN 267
/// @brief Home key code delivered by the canvas poll path.
#define UI_KEY_HOME 268
/// @brief End key code delivered by the canvas poll path.
#define UI_KEY_END 269

// Shared geometry / text / lifetime helpers (defined in rt_gameui.c).

/// @brief Clamp a widget dimension to the supported non-negative range.
/// @param value Candidate dimension.
/// @return @p value clamped to `[1, 16384]`.
int64_t ui_clamp_dim(int64_t value);

/// @brief Saturating 64-bit addition (no UB on overflow).
/// @param a First addend.
/// @param b Second addend.
/// @return Exact sum or the nearest `int64_t` endpoint.
int64_t ui_add_sat_i64(int64_t a, int64_t b);

/// @brief Saturating 64-bit multiplication (no UB on overflow).
/// @param a First factor.
/// @param b Second factor.
/// @return Exact product or the appropriate `int64_t` endpoint.
int64_t ui_mul_sat_i64(int64_t a, int64_t b);

/// @brief Convert a long double to i64, saturating at the type bounds.
/// @param value Floating-point input.
/// @return Zero for non-finite input, an endpoint outside/near the range, or
///         the in-range value truncated toward zero.
int64_t ui_ld_to_i64_sat(long double value);

/// @brief True when @p point lies within [start, start+extent).
/// @param start Inclusive origin.
/// @param extent Span length; nonpositive values describe an empty span.
/// @param point Coordinate to test.
/// @return `1` inside the overflow-safe half-open span; otherwise `0`.
int8_t ui_coord_inside(int64_t start, int64_t extent, int64_t point);

/// @brief Offset of @p point from @p start, clamped into [0, extent].
/// @param start Coordinate origin.
/// @param extent Maximum offset; nonpositive values return zero.
/// @param point Coordinate to map.
/// @return Zero before the origin, @p extent at/beyond the far edge, or the
///         exact unsigned-safe offset.
int64_t ui_coord_offset_clamped(int64_t start, int64_t extent, int64_t point);

/// @brief Validate a 2D canvas handle; traps with @p api context on failure.
/// @param canvas Candidate Canvas handle.
/// @param api Trap message for a non-null mismatch.
/// @return `1` for a valid 2D Canvas; `0` for null or after a trap.
int8_t ui_validate_canvas(void *canvas, const char *api);

/// @brief Resolve a Draw-call canvas (2D Canvas or Canvas3D) into a draw-ops table;
///        traps @p api on unknown handles.
/// @param canvas Candidate 2D Canvas or registered Canvas3D.
/// @param api Trap message for a non-null unresolved handle.
/// @param ops Required output table.
/// @return `1` on success; `0` for null arguments or after a trap.
int8_t ui_resolve_draw_ops(void *canvas, const char *api, rt_gameui_draw_ops_t *ops);

/// @brief Copy @p text into a fixed buffer with truncation and NUL termination.
/// @param dst Destination buffer.
/// @param cap Capacity including the NUL terminator.
/// @param text Runtime string to copy; `NULL` produces empty output.
/// @details Copies only the visible prefix and does not split validated UTF-8.
void ui_copy_text(char *dst, size_t cap, rt_string text);

/// @brief Release one reference on a runtime object (NULL-safe).
/// @param obj Reference to release and free if its count reaches zero.
void ui_release_obj(void *obj);

/// @brief Pixel width of the first @p bytes of @p text in @p font at @p scale.
/// @param text Byte sequence to measure.
/// @param bytes Prefix size clamped to 511.
/// @param font Optional BitmapFont; null uses canvas-default metrics.
/// @param scale Integer multiplier clamped to `[1, 16]`.
/// @return Scaled measured width, or zero for null/nonpositive text input.
int64_t ui_text_prefix_width(const char *text, int64_t bytes, void *font, int64_t scale);

/// @brief Draw text through the resolved draw-ops table (default or bitmap font).
/// @param ops Resolved operations and canvas.
/// @param x Left coordinate.
/// @param y Top coordinate.
/// @param text Nonempty NUL-terminated text.
/// @param font Optional BitmapFont.
/// @param scale Integer multiplier clamped to `[1, 16]`.
/// @param color Packed text color.
void ui_draw_text_basic(const rt_gameui_draw_ops_t *ops,
                        int64_t x,
                        int64_t y,
                        const char *text,
                        void *font,
                        int64_t scale,
                        int64_t color);

/// @brief True when (@p px, @p py) lies inside the rectangle at (x, y, w, h).
/// @param x Rectangle left coordinate.
/// @param y Rectangle top coordinate.
/// @param w Rectangle width.
/// @param h Rectangle height.
/// @param px Point X coordinate.
/// @param py Point Y coordinate.
/// @return `1` inside both half-open spans; otherwise `0`.
int8_t ui_point_inside(int64_t x, int64_t y, int64_t w, int64_t h, int64_t px, int64_t py);

// Text/UTF-8 helpers shared with UITextInput (defined in rt_gameui.c).

/// @brief Validate a BitmapFont handle; traps with @p api context on failure.
/// @param font Candidate BitmapFont, or `NULL` for default-font behavior.
/// @param api Trap message for a non-null mismatch.
/// @return `1` for null/valid; `0` after a mismatch trap.
int8_t ui_validate_bitmapfont(void *font, const char *api);

/// @brief Length of @p s up to the first NUL, capped at @p max_len bytes.
/// @param s Byte sequence; `NULL` returns zero.
/// @param max_len Maximum readable bytes.
/// @return Visible prefix length.
size_t ui_visible_len(const char *s, size_t max_len);

/// @brief Byte length of the UTF-8 code point starting at @p pos (1 for invalid leads).
/// @param s Byte sequence.
/// @param len Readable byte count.
/// @param pos Candidate lead-byte offset.
/// @return Zero out of range/null; otherwise validated length 1–4, treating
///         malformed input as one byte.
size_t ui_utf8_cp_len(const char *s, size_t len, size_t pos);

/// @brief Longest prefix of @p s within @p max_bytes that ends on a code-point boundary.
/// @param s Non-null byte sequence when @p len is nonzero.
/// @param len Readable byte count.
/// @param max_bytes Maximum prefix size.
/// @return Largest complete validated/malformed-unit prefix within both limits.
size_t ui_utf8_trunc_len(const char *s, size_t len, size_t max_bytes);

/// @brief Byte length of the first @p max_codepoints code points of @p s.
/// @param s Byte sequence; `NULL` returns zero.
/// @param len Readable byte count.
/// @param max_codepoints Maximum units.
/// @return Byte length of the accepted prefix.
size_t ui_utf8_trunc_codepoints(const char *s, size_t len, size_t max_codepoints);

/// @brief Retain @p value, release the previous occupant, and store it in @p slot.
/// @param slot Address of an owned reference; `NULL` is a no-op.
/// @param value Replacement reference or `NULL`.
/// @details Retains the replacement before releasing the prior value and does
///          nothing for self-assignment.
void ui_replace_ref(void **slot, void *value);

/// @brief Number of code points in the first @p bytes of @p text.
/// @param text Byte sequence.
/// @param bytes Readable byte count.
/// @return Validated/malformed-unit count, or zero for null/nonpositive input.
int64_t ui_codepoint_count_bytes(const char *text, int64_t bytes);

/// @brief Byte boundary after at most @p cp_index code points.
/// @param text Byte sequence.
/// @param bytes Readable byte count.
/// @param cp_index Requested boundary count.
/// @return Boundary clamped to `[0, bytes]`.
int64_t ui_byte_for_codepoint(const char *text, int64_t bytes, int64_t cp_index);

/// @brief Code-point index containing the byte at @p byte_index.
/// @param text Byte sequence.
/// @param bytes Readable byte count.
/// @param byte_index Byte boundary clamped to @p bytes.
/// @return Count of complete units ending at/before the boundary.
int64_t ui_codepoint_for_byte(const char *text, int64_t bytes, int64_t byte_index);

/// @brief Byte offset of the code point preceding @p byte_index (0 at start).
/// @param text Byte sequence.
/// @param bytes Readable byte count.
/// @param byte_index Current byte position.
/// @return Prior/containing unit start, clamped to the text.
int64_t ui_prev_codepoint_byte(const char *text, int64_t bytes, int64_t byte_index);

/// @brief Byte offset of the code point following @p byte_index (len at end).
/// @param text Byte sequence.
/// @param bytes Readable byte count.
/// @param byte_index Current unit start; negative values become zero.
/// @return Next unit boundary clamped to @p bytes.
int64_t ui_next_codepoint_byte(const char *text, int64_t bytes, int64_t byte_index);
