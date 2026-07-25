//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/game/rt_dialogue.h
/// @file
/// @brief Declares the queued, typewriter-revealed Dialogue runtime API.
//
// Purpose: Typewriter text reveal system for RPGs, visual novels, and tutorials.
//   Provides character-by-character reveal, word wrapping, speaker labels,
//   dialogue queue, and immediate-mode Canvas drawing.
//
// Key invariants:
//   - Queue holds up to 64 stored lines; adding a 65th shifts out the oldest.
//   - Each body buffer holds at most 511 bytes plus NUL, and each speaker
//     buffer holds at most 63 bytes plus NUL. Truncation avoids splitting a
//     nominal UTF-8 sequence.
//   - Update(dt_ms) advances reveal; Advance() skips or moves to next line.
//   - Non-positive update deltas are ignored; geometry and alpha setters clamp inputs.
//
// Ownership/Lifetime:
//   - Dialogue objects are runtime-reference-counted and expose no
//     module-specific destroy entry point.
//   - The Dialogue retains a configured font and releases it from its
//     finalizer. Speaker and body strings are copied and never retained.
//
// Links: src/runtime/game/rt_dialogue.c
//
//===----------------------------------------------------------------------===//
#pragma once

#include "rt_string.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Runtime class ID used to validate opaque Dialogue handles.
#define RT_DIALOGUE_CLASS_ID INT64_C(-0x510213)

// Construction
/// @brief Create a dialogue box widget at (x, y) sized (width × height) with default styling.
/// @param x Initial left coordinate, saturated to the `int32_t` range.
/// @param y Initial top coordinate, saturated to the `int32_t` range.
/// @param width Initial width; nonpositive values become one and oversized
///        values saturate at `INT32_MAX`.
/// @param height Initial height, normalized like @p width.
/// @return A new Dialogue reference, or `NULL` if allocation fails.
/// @details The object begins inactive with an empty queue, default canvas
///          font, and a 30-codepoint-per-second reveal speed.
void *rt_dialogue_new(int64_t x, int64_t y, int64_t width, int64_t height);

// Configuration
/// @brief Set the typewriter reveal speed (characters per second; <=0 reveals instantly).
/// @param dlg Dialogue to configure; `NULL` is a no-op.
/// @param chars_per_second Requested nominal UTF-8 units per second.
/// @details Positive values saturate at `INT32_MAX`; nonpositive values select
///          instant reveal on the next positive update. A non-null invalid
///          handle raises a runtime trap.
void rt_dialogue_set_speed(void *dlg, int64_t chars_per_second);

/// @brief Bind a custom BitmapFont (NULL falls back to built-in 8×8).
/// @param dlg Dialogue to configure; `NULL` is a no-op.
/// @param font Runtime object retained as the custom font, or `NULL`.
/// @details Retains the new pointer before releasing the old pointer. A
///          non-null invalid Dialogue handle raises a runtime trap.
void rt_dialogue_set_font(void *dlg, void *font);

/// @brief Set the body-text color.
/// @param dlg Dialogue to configure; `NULL` is a no-op.
/// @param color Packed color saturated to the `int32_t` range.
/// @details A non-null invalid handle raises a runtime trap.
void rt_dialogue_set_text_color(void *dlg, int64_t color);

/// @brief Set the speaker-name label color.
/// @param dlg Dialogue to configure; `NULL` is a no-op.
/// @param color Packed color saturated to the `int32_t` range.
/// @details A non-null invalid handle raises a runtime trap.
void rt_dialogue_set_speaker_color(void *dlg, int64_t color);

/// @brief Set background fill color and per-pixel alpha (0–255).
/// @param dlg Dialogue to configure; `NULL` is a no-op.
/// @param color Packed color saturated to the `int32_t` range.
/// @param alpha Opacity saturated to the inclusive range `[0, 255]`.
/// @details A non-null invalid handle raises a runtime trap.
void rt_dialogue_set_bg_color(void *dlg, int64_t color, int64_t alpha);

/// @brief Set the border color; any negative stored value disables the border.
/// @param dlg Dialogue to configure; `NULL` is a no-op.
/// @param color Packed color saturated to the `int32_t` range.
/// @details A non-null invalid handle raises a runtime trap.
void rt_dialogue_set_border_color(void *dlg, int64_t color);

/// @brief Set inner padding in pixels between the border and the text.
/// @param dlg Dialogue to configure; `NULL` is a no-op.
/// @param padding Requested inset; nonpositive values become zero and
///        oversized values saturate at `INT32_MAX`.
/// @details A non-null invalid handle raises a runtime trap.
void rt_dialogue_set_padding(void *dlg, int64_t padding);

/// @brief Set integer text scale (1 = native, 2 = double-size, etc.).
/// @param dlg Dialogue to configure; `NULL` is a no-op.
/// @param scale Multiplier clamped to `[1, INT32_MAX]`.
/// @details A non-null invalid handle raises a runtime trap.
void rt_dialogue_set_text_scale(void *dlg, int64_t scale);

/// @brief Move the dialogue box to (x, y).
/// @param dlg Dialogue to configure; `NULL` is a no-op.
/// @param x New left coordinate, saturated to the `int32_t` range.
/// @param y New top coordinate, saturated to the `int32_t` range.
/// @details A non-null invalid handle raises a runtime trap.
void rt_dialogue_set_pos(void *dlg, int64_t x, int64_t y);

/// @brief Resize the dialogue box.
/// @param dlg Dialogue to configure; `NULL` is a no-op.
/// @param w New width; nonpositive values preserve the current width and
///        oversized values saturate at `INT32_MAX`.
/// @param h New height, normalized independently like @p w.
/// @details A non-null invalid handle raises a runtime trap.
void rt_dialogue_set_size(void *dlg, int64_t w, int64_t h);

// Dialogue queue
/// @brief Queue a line of text spoken by @p speaker (the speaker label appears above the text).
/// @param dlg Dialogue to mutate; `NULL` is a no-op.
/// @param speaker Optional label copied into a 64-byte buffer.
/// @param text Optional body copied into a 512-byte buffer.
/// @details Inputs are not retained. When capacity is full, the oldest stored
///          line is discarded. Appending while inactive restarts playback from
///          stored line zero and clears finished state. A non-null invalid
///          handle raises a runtime trap.
void rt_dialogue_say(void *dlg, rt_string speaker, rt_string text);

/// @brief Queue a line of text with no speaker label.
/// @param dlg Dialogue to mutate.
/// @param text Optional body text to copy.
/// @details Equivalent to rt_dialogue_say(@p dlg, `NULL`, @p text).
void rt_dialogue_say_text(void *dlg, rt_string text);

/// @brief Discard all queued lines and reset the typewriter.
/// @param dlg Dialogue to clear; `NULL` is a no-op.
/// @details Resets playback flags and timing but preserves styling, geometry,
///          and the retained font. A non-null invalid handle raises a trap.
void rt_dialogue_clear(void *dlg);

// Playback
/// @brief Advance the typewriter by @p dt_ms milliseconds (call once per frame).
/// @param dlg Dialogue to update.
/// @param dt_ms Positive elapsed milliseconds; nonpositive values are ignored.
/// @details Only an active, incomplete line advances. Completion marks the
///          Dialogue waiting for input. Accumulation saturates before converting
///          elapsed time into nominal UTF-8 units. A non-null invalid handle
///          raises a runtime trap.
void rt_dialogue_update(void *dlg, int64_t dt_ms);

/// @brief Move to the next queued line; if the current line isn't complete, complete it first.
/// @param dlg Dialogue to advance; `NULL` is a no-op.
/// @details Advancing a completed final line clears active and sets finished;
///          the waiting flag remains set on that transition. A non-null invalid
///          handle raises a runtime trap.
void rt_dialogue_advance(void *dlg);

/// @brief Instantly reveal all characters of the current line (skip typewriter animation).
/// @param dlg Dialogue to update; `NULL` is a no-op.
/// @details Marks an active, in-range line complete and waiting without moving
///          to the next line. A non-null invalid handle raises a runtime trap.
void rt_dialogue_skip(void *dlg);

// State queries
/// @brief Query whether playback is currently active.
/// @param dlg Dialogue to inspect.
/// @return The stored active flag, or zero for a null or invalid handle.
/// @details Stored lines may remain after this becomes false. A non-null
///          invalid handle raises a runtime trap.
int8_t rt_dialogue_is_active(void *dlg);

/// @brief True if the current line has finished its typewriter reveal.
/// @param dlg Dialogue to inspect.
/// @return The stored line-complete flag, or zero for a null or invalid handle.
/// @details A non-null invalid handle raises a runtime trap.
int8_t rt_dialogue_is_line_complete(void *dlg);

/// @brief Query whether Advance moved beyond the final stored line.
/// @param dlg Dialogue to inspect.
/// @return The stored finished flag, or zero for a null or invalid handle.
/// @details Revealing the final line alone does not set finished. A non-null
///          invalid handle raises a runtime trap.
int8_t rt_dialogue_is_finished(void *dlg);

/// @brief Query whether a completed line is waiting for Advance.
/// @param dlg Dialogue to inspect.
/// @return The stored waiting flag, or zero for a null or invalid handle.
/// @details The flag also remains set after advancing beyond the final line. A
///          non-null invalid handle raises a runtime trap.
int8_t rt_dialogue_is_waiting(void *dlg);

/// @brief Return the number of stored lines.
/// @param dlg Dialogue to inspect.
/// @return Stored-line count, including already played entries, or zero for a
///         null or invalid handle.
/// @details Playback does not consume entries. A non-null invalid handle raises
///          a runtime trap.
int64_t rt_dialogue_get_line_count(void *dlg);

/// @brief Index of the line currently being revealed.
/// @param dlg Dialogue to inspect.
/// @return Zero-based current index, or zero for a null or invalid handle.
/// @details The index equals the line count after final completion. A non-null
///          invalid handle raises a runtime trap.
int64_t rt_dialogue_get_current_line(void *dlg);

/// @brief Get the speaker name for the active line (empty string if none).
/// @param dlg Dialogue to inspect.
/// @return A caller-owned copy of a nonempty current speaker, the immortal
///         empty singleton when unavailable, or `NULL` if copying fails.
/// @details A non-null invalid handle raises a runtime trap before returning
///          the empty singleton.
rt_string rt_dialogue_get_speaker(void *dlg);

// Rendering
/// @brief Render the dialogue box and revealed text onto @p canvas.
/// @param dlg Dialogue to render.
/// @param canvas Candidate Canvas handle.
/// @details Null arguments, a non-Canvas handle, inactive playback, or an
///          out-of-range line produce no drawing. The panel includes an
///          optional border and speaker, wrapped revealed text, and a waiting
///          ellipsis. A non-null invalid Dialogue handle raises a runtime trap.
void rt_dialogue_draw(void *dlg, void *canvas);

#ifdef __cplusplus
}
#endif
