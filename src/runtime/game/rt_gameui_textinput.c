//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/game/rt_gameui_textinput.c
/// @file
/// @brief Implements the UTF-8-aware, single-line GameUI text field.
//
// Purpose: UITextInput widget for the immediate-mode GameUI — editable single-
//          line text field with UTF-8-aware cursor/selection handling. Split
//          out of rt_gameui.c; shares helpers + key codes via
//          rt_gameui_internal.h.
//
// Key invariants:
//   - Cursor and selection indices are byte offsets kept on UTF-8 codepoint
//     boundaries via the shared ui_*_codepoint_byte helpers.
//   - Immediate-mode: validates its canvas and draws against the current frame.
//
// Ownership/Lifetime:
//   - Borrows the caller's canvas for each draw.
//   - Owns its dynamic text and placeholder buffers, retains an assigned
//     BitmapFont, and releases all three through its finalizer.
//
// Links: src/runtime/game/rt_gameui.c (other widgets + shared helpers),
//        src/runtime/game/rt_gameui_internal.h (shared helpers + key codes)
//
//===----------------------------------------------------------------------===//

#include "rt_gameui.h"
#include "rt_gameui_internal.h"

#include "rt_bitmapfont.h"
#include "rt_graphics.h"
#include "rt_internal.h"
#include "rt_object.h"
#include "rt_pixels.h"
#include "rt_trap.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

//=============================================================================
// UITextInput
//=============================================================================

/// @brief Initial capacity of the owned text and placeholder buffers.
#define RT_UITEXTINPUT_DEFAULT_BYTES 512
/// @brief Default caret half-cycle duration in milliseconds.
#define RT_UITEXTINPUT_DEFAULT_CURSOR_BLINK_MS 530

/// @brief Private editable state stored in a runtime UITextInput object.
typedef struct {
    void *vptr;
    int64_t x, y, w, h;
    char *text;
    int64_t text_bytes;
    int64_t text_capacity;
    int64_t cursor_byte;
    int64_t selection_anchor;
    int64_t scroll_byte;
    int64_t text_color;
    int64_t bg_color;
    int64_t cursor_color;
    int64_t selection_color;
    int64_t border_color;
    int64_t border_color_focused;
    int64_t cursor_blink_ms;
    int64_t cursor_blink_elapsed;
    void *font;
    int8_t visible;
    int8_t enabled;
    int8_t focused;
    int8_t password_mode;
    int8_t multiline;
    int64_t max_codepoints;
    char *placeholder;
    int64_t placeholder_capacity;
} rt_uitextinput_impl;

/// @brief Validate and cast an opaque UITextInput handle.
/// @param ptr Candidate handle; `NULL` is accepted.
/// @param api Trap message used for a non-null class mismatch.
/// @return The UITextInput payload when valid; otherwise `NULL`.
/// @details A mismatched handle raises a runtime trap.
static rt_uitextinput_impl *checked_textinput(void *ptr, const char *api) {
    if (!ptr)
        return NULL;
    if (rt_obj_class_id(ptr) != RT_UITEXTINPUT_CLASS_ID) {
        rt_trap(api);
        return NULL;
    }
    return (rt_uitextinput_impl *)ptr;
}

/// @brief GC finalizer: free the text buffer and release the field's font.
/// @param obj UITextInput payload being finalized; `NULL` is accepted.
/// @details Frees text and placeholder storage, clears their lengths/capacities,
///          and releases the retained font.
static void uitextinput_finalizer(void *obj) {
    rt_uitextinput_impl *ti = (rt_uitextinput_impl *)obj;
    if (!ti)
        return;
    free(ti->text);
    ti->text = NULL;
    ti->text_bytes = 0;
    ti->text_capacity = 0;
    free(ti->placeholder);
    ti->placeholder = NULL;
    ti->placeholder_capacity = 0;
    ui_release_obj(ti->font);
    ti->font = NULL;
}

/// @brief Grow an owned character buffer geometrically to a required capacity.
/// @param buffer Address of the allocation pointer.
/// @param capacity Address of the current byte capacity.
/// @param needed Minimum required byte capacity.
/// @return `1` when existing/new storage is sufficient; `0` for invalid growth,
///         representability overflow, or allocation failure.
/// @details New capacity doubles from at least one and newly added bytes are
///          zero-initialized. Failure preserves the original allocation.
static int8_t ensure_text_storage(char **buffer, int64_t *capacity, int64_t needed) {
    if (!buffer || !capacity || needed <= *capacity)
        return 1;
    int64_t new_capacity = *capacity > 0 ? *capacity : 1;
    while (new_capacity < needed) {
        if (new_capacity > INT64_MAX / 2 || (uint64_t)new_capacity > SIZE_MAX / 2)
            return 0;
        new_capacity *= 2;
    }
    if ((uint64_t)new_capacity > SIZE_MAX)
        return 0;
    char *resized = (char *)realloc(*buffer, (size_t)new_capacity);
    if (!resized)
        return 0;
    memset(resized + *capacity, 0, (size_t)(new_capacity - *capacity));
    *buffer = resized;
    *capacity = new_capacity;
    return 1;
}

/// @brief Replace the field's buffer with @p len bytes of @p text, resetting
///        caret/selection (respects the max-codepoints cap).
/// @param ti UITextInput payload to mutate.
/// @param text Source bytes, or `NULL` for empty content.
/// @param len Available source byte count.
/// @details Copying stops at embedded NUL and at the configured codepoint cap.
///          Allocation failure traps and leaves the prior state intact. Success
///          moves the caret to the end and resets selection and scroll.
static void textinput_set_bytes(rt_uitextinput_impl *ti, const char *text, size_t len) {
    if (!ti)
        return;
    if (!text)
        len = 0;
    else
        len = ui_visible_len(text, len);
    if (text && len > 0 && ti->max_codepoints > 0)
        len = ui_utf8_trunc_codepoints(text, len, (size_t)ti->max_codepoints);
    if (!ensure_text_storage(&ti->text, &ti->text_capacity, (int64_t)len + 1)) {
        rt_trap("UITextInput.SetText: text allocation failed");
        return;
    }
    if (len > 0)
        memmove(ti->text, text, len);
    ti->text[len] = '\0';
    ti->text_bytes = (int64_t)len;
    ti->cursor_byte = ti->text_bytes;
    ti->selection_anchor = -1;
    ti->scroll_byte = 0;
}

/// @brief Get the normalized selection byte range (start <= end) via out
///        params.
/// @param ti UITextInput payload to inspect.
/// @param start Optional output for the inclusive lower byte offset.
/// @param end Optional output for the exclusive upper byte offset.
/// @return Nonzero for a nonempty clamped selection; otherwise zero.
static int8_t textinput_selection_range(rt_uitextinput_impl *ti, int64_t *start, int64_t *end) {
    if (!ti || ti->selection_anchor < 0 || ti->selection_anchor == ti->cursor_byte)
        return 0;
    int64_t a = ti->selection_anchor;
    int64_t b = ti->cursor_byte;
    if (a > b) {
        int64_t tmp = a;
        a = b;
        b = tmp;
    }
    if (a < 0)
        a = 0;
    if (b > ti->text_bytes)
        b = ti->text_bytes;
    if (a >= b)
        return 0;
    if (start)
        *start = a;
    if (end)
        *end = b;
    return 1;
}

/// @brief Delete bytes [start, end) from the field and fix up the caret.
/// @param ti UITextInput payload to mutate.
/// @param start Inclusive byte offset.
/// @param end Exclusive byte offset, clamped to the text length.
/// @return Nonzero when bytes were removed; otherwise zero.
/// @details Compacts the terminating NUL, moves the caret to @p start, clears
///          selection, and pulls scroll back when necessary.
static int8_t textinput_delete_range(rt_uitextinput_impl *ti, int64_t start, int64_t end) {
    if (!ti || start < 0 || end <= start || start >= ti->text_bytes)
        return 0;
    if (end > ti->text_bytes)
        end = ti->text_bytes;
    memmove(ti->text + start, ti->text + end, (size_t)(ti->text_bytes - end + 1));
    ti->text_bytes -= end - start;
    ti->cursor_byte = start;
    ti->selection_anchor = -1;
    if (ti->scroll_byte > ti->cursor_byte)
        ti->scroll_byte = ti->cursor_byte;
    return 1;
}

/// @brief Delete the active selection, if any.
/// @param ti UITextInput payload to mutate.
/// @return Nonzero when a normalized nonempty selection was removed.
static int8_t textinput_delete_selection(rt_uitextinput_impl *ti) {
    int64_t start = 0;
    int64_t end = 0;
    return textinput_selection_range(ti, &start, &end) ? textinput_delete_range(ti, start, end) : 0;
}

/// @brief Insert @p src_len bytes at the caret, replacing any selection first
///        and enforcing the max-codepoints cap.
/// @param ti UITextInput payload to mutate.
/// @param src Source byte sequence.
/// @param src_len Available source bytes.
/// @return Nonzero when the buffer changed, including selection deletion when
///         no replacement codepoint could be accepted.
/// @details Truncates to complete validated/malformed UTF-8 units, skips NUL,
///          LF, and CR for this single-line widget, grows storage per accepted
///          unit, and clears selection. Allocation failure traps after keeping
///          any already-applied changes.
static int8_t textinput_insert_bytes(rt_uitextinput_impl *ti, const char *src, size_t src_len) {
    if (!ti || !src || src_len == 0)
        return 0;
    src_len = ui_utf8_trunc_len(src, src_len, src_len);
    if (src_len == 0)
        return 0;
    int8_t changed = textinput_delete_selection(ti);
    int64_t current_cps = ui_codepoint_count_bytes(ti->text, ti->text_bytes);
    int64_t remaining_cps = ti->max_codepoints > 0 ? (ti->max_codepoints - current_cps) : INT64_MAX;
    if (remaining_cps <= 0)
        return changed;
    size_t accepted = 0;
    int64_t cps = 0;
    while (accepted < src_len && cps < remaining_cps) {
        size_t cp_len = ui_utf8_cp_len(src, src_len, accepted);
        if (accepted + cp_len > src_len)
            break;
        if (!ensure_text_storage(
                &ti->text, &ti->text_capacity, ti->text_bytes + (int64_t)cp_len + 1)) {
            rt_trap("UITextInput.HandleText: text allocation failed");
            break;
        }
        if (src[accepted] == '\0' ||
            (!ti->multiline && (src[accepted] == '\n' || src[accepted] == '\r'))) {
            accepted += cp_len;
            continue;
        }
        memmove(ti->text + ti->cursor_byte + (int64_t)cp_len,
                ti->text + ti->cursor_byte,
                (size_t)(ti->text_bytes - ti->cursor_byte + 1));
        memcpy(ti->text + ti->cursor_byte, src + accepted, cp_len);
        ti->cursor_byte += (int64_t)cp_len;
        ti->text_bytes += (int64_t)cp_len;
        accepted += cp_len;
        cps++;
        changed = 1;
    }
    ti->selection_anchor = -1;
    return changed;
}

/// @brief Move the caret to byte offset @p byte_pos; @p shift_held extends the
///        selection, otherwise the selection is collapsed.
/// @param ti UITextInput payload to mutate.
/// @param byte_pos Requested byte boundary, clamped to the text.
/// @param shift_held Nonzero creates/preserves an anchor at the old caret.
/// @details Restarts the cursor blink phase.
static void textinput_move_cursor(rt_uitextinput_impl *ti, int64_t byte_pos, int8_t shift_held) {
    if (!ti)
        return;
    if (byte_pos < 0)
        byte_pos = 0;
    if (byte_pos > ti->text_bytes)
        byte_pos = ti->text_bytes;
    if (shift_held) {
        if (ti->selection_anchor < 0)
            ti->selection_anchor = ti->cursor_byte;
    } else {
        ti->selection_anchor = -1;
    }
    ti->cursor_byte = byte_pos;
    ti->cursor_blink_elapsed = 0;
}

/// @brief Map a mouse x-coordinate to the nearest caret byte offset
///        using font metrics and codepoint boundaries.
/// @param ti UITextInput payload to measure.
/// @param mx Absolute mouse X coordinate.
/// @return Nearest caret byte boundary, choosing a unit's start before its
///         measured midpoint and the text end beyond all midpoints.
static int64_t textinput_byte_from_mouse(rt_uitextinput_impl *ti, int64_t mx) {
    if (!ti || ti->text_bytes <= 0)
        return 0;
    int64_t origin = ui_add_sat_i64(ti->x, 4);
    if (mx <= origin)
        return 0;
    uint64_t local_offset = (uint64_t)mx - (uint64_t)origin;
    int64_t local = local_offset > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)local_offset;
    int64_t pos = 0;
    int64_t best = ti->text_bytes;
    while (pos < ti->text_bytes) {
        int64_t next = ui_next_codepoint_byte(ti->text, ti->text_bytes, pos);
        int64_t mid = ui_text_prefix_width(ti->text, pos, ti->font, 1) +
                      (ui_text_prefix_width(ti->text + pos, next - pos, ti->font, 1) / 2);
        if (local < mid) {
            best = pos;
            break;
        }
        pos = next;
    }
    return best;
}

/// @brief Create an empty, visible, enabled single-line text input.
/// @param x Initial left coordinate.
/// @param y Initial top coordinate.
/// @param w Width clamped to `[1, 16384]`.
/// @param h Height clamped to `[1, 16384]`.
/// @return A new UITextInput reference, or `NULL` if object or initial-buffer
///         allocation fails.
/// @details Initializes text and placeholder buffers to 512 bytes, selection
///          anchor to `-1`, caret blink to 530 ms, and default dark styling.
void *rt_uitextinput_new(int64_t x, int64_t y, int64_t w, int64_t h) {
    rt_uitextinput_impl *ti = (rt_uitextinput_impl *)rt_obj_new_i64(
        RT_UITEXTINPUT_CLASS_ID, (int64_t)sizeof(rt_uitextinput_impl));
    if (!ti)
        return NULL;
    memset(ti, 0, sizeof(*ti));
    ti->x = x;
    ti->y = y;
    ti->w = ui_clamp_dim(w);
    ti->h = ui_clamp_dim(h);
    ti->selection_anchor = -1;
    ti->text_color = 0xFFFFFF;
    ti->bg_color = 0x202020;
    ti->cursor_color = 0xFFFFFF;
    ti->selection_color = 0x3355AA;
    ti->border_color = 0x606060;
    ti->border_color_focused = 0x88AAFF;
    ti->cursor_blink_ms = RT_UITEXTINPUT_DEFAULT_CURSOR_BLINK_MS;
    ti->visible = 1;
    ti->enabled = 1;
    rt_obj_set_finalizer(ti, uitextinput_finalizer);
    if (!ensure_text_storage(&ti->text, &ti->text_capacity, RT_UITEXTINPUT_DEFAULT_BYTES) ||
        !ensure_text_storage(
            &ti->placeholder, &ti->placeholder_capacity, RT_UITEXTINPUT_DEFAULT_BYTES)) {
        if (rt_obj_release_check0(ti))
            rt_obj_free(ti);
        return NULL;
    }
    return ti;
}

/// @brief Replace text and reset caret, selection, and scroll state.
/// @param ptr UITextInput to mutate; `NULL` is a no-op.
/// @param text Runtime string to copy, or `NULL` for empty content.
/// @details Copying stops at embedded NUL and the configured codepoint limit;
///          no input reference is retained. Allocation and wrong-class errors
///          trap.
void rt_uitextinput_set_text(void *ptr, rt_string text) {
    rt_uitextinput_impl *ti =
        checked_textinput(ptr, "UITextInput.SetText: expected Zanna.Game.UI.HudTextInput");
    if (!ti)
        return;
    const char *s = text ? rt_string_cstr(text) : "";
    size_t len = text ? (size_t)rt_str_len(text) : 0;
    textinput_set_bytes(ti, s, len);
}

/// @brief Copy the field's current text into a runtime string.
/// @param ptr UITextInput to query.
/// @return A caller-owned string copy, or the immortal empty singleton for a
///         null/invalid handle.
/// @details A non-null wrong-class handle raises a runtime trap.
rt_string rt_uitextinput_get_text(void *ptr) {
    rt_uitextinput_impl *ti =
        checked_textinput(ptr, "UITextInput.GetText: expected Zanna.Game.UI.HudTextInput");
    return ti ? rt_const_cstr(ti->text) : rt_str_empty();
}

/// @brief Count the field's UTF-8 units.
/// @param ptr UITextInput to query.
/// @return Validated codepoints plus malformed single-byte units, or zero for
///         null/invalid.
int64_t rt_uitextinput_text_length(void *ptr) {
    rt_uitextinput_impl *ti =
        checked_textinput(ptr, "UITextInput.TextLength: expected Zanna.Game.UI.HudTextInput");
    return ti ? ui_codepoint_count_bytes(ti->text, ti->text_bytes) : 0;
}

/// @brief Return the caret as a codepoint index.
/// @param ptr UITextInput to query.
/// @return Complete UTF-8 units before the internal byte caret, or zero for
///         null/invalid.
int64_t rt_uitextinput_get_cursor(void *ptr) {
    rt_uitextinput_impl *ti =
        checked_textinput(ptr, "UITextInput.GetCursor: expected Zanna.Game.UI.HudTextInput");
    return ti ? ui_codepoint_for_byte(ti->text, ti->text_bytes, ti->cursor_byte) : 0;
}

/// @brief Move the caret to a clamped codepoint boundary.
/// @param ptr UITextInput to mutate; `NULL` is a no-op.
/// @param pos Requested codepoint index.
/// @details Clears selection and restarts blink timing. Invalid handles trap.
void rt_uitextinput_set_cursor(void *ptr, int64_t pos) {
    rt_uitextinput_impl *ti =
        checked_textinput(ptr, "UITextInput.SetCursor: expected Zanna.Game.UI.HudTextInput");
    if (ti)
        textinput_move_cursor(ti, ui_byte_for_codepoint(ti->text, ti->text_bytes, pos), 0);
}

/// @brief Select the complete byte range.
/// @param ptr UITextInput to mutate; `NULL` is a no-op.
/// @details Anchors at zero and moves the caret to the text end. Invalid
///          handles trap.
void rt_uitextinput_select_all(void *ptr) {
    rt_uitextinput_impl *ti =
        checked_textinput(ptr, "UITextInput.SelectAll: expected Zanna.Game.UI.HudTextInput");
    if (!ti)
        return;
    ti->selection_anchor = 0;
    ti->cursor_byte = ti->text_bytes;
}

/// @brief Clear selection without moving the caret.
/// @param ptr UITextInput to mutate; `NULL` is a no-op.
/// @details A non-null wrong-class handle raises a runtime trap.
void rt_uitextinput_clear_selection(void *ptr) {
    rt_uitextinput_impl *ti =
        checked_textinput(ptr, "UITextInput.ClearSelection: expected Zanna.Game.UI.HudTextInput");
    if (ti)
        ti->selection_anchor = -1;
}

/// @brief Test for a nonempty normalized selection.
/// @param ptr UITextInput to query.
/// @return `1` when anchor and caret enclose bytes; otherwise `0`.
int8_t rt_uitextinput_has_selection(void *ptr) {
    rt_uitextinput_impl *ti =
        checked_textinput(ptr, "UITextInput.HasSelection: expected Zanna.Game.UI.HudTextInput");
    return textinput_selection_range(ti, NULL, NULL);
}

/// @brief Copy the active selection into a runtime string.
/// @param ptr UITextInput to query.
/// @return A caller-owned selected substring, the immortal empty singleton
///         when unselected/null/invalid, or `NULL` on allocation failure.
rt_string rt_uitextinput_get_selected_text(void *ptr) {
    rt_uitextinput_impl *ti =
        checked_textinput(ptr, "UITextInput.GetSelectedText: expected Zanna.Game.UI.HudTextInput");
    int64_t start = 0;
    int64_t end = 0;
    if (!textinput_selection_range(ti, &start, &end))
        return rt_str_empty();
    return rt_string_from_bytes(ti->text + start, (size_t)(end - start));
}

/// @brief Delete the active selection.
/// @param ptr UITextInput to mutate; `NULL` or no selection is a no-op.
/// @details Moves the caret to the removed range's start and clears the anchor.
///          A non-null invalid handle raises a runtime trap.
void rt_uitextinput_delete_selection(void *ptr) {
    rt_uitextinput_impl *ti =
        checked_textinput(ptr, "UITextInput.DeleteSelection: expected Zanna.Game.UI.HudTextInput");
    textinput_delete_selection(ti);
}

/// @brief Handle editing and caret-navigation keys for a focused field.
/// @param ptr UITextInput to update.
/// @param key_code Backspace, Delete, arrows, Home, End, or value 1 for
///        select-all.
/// @param shift_held Nonzero extends arrow/Home/End selection.
/// @return `1` only when Backspace/Delete changes text; all other paths return
///         zero.
/// @details Disabled or unfocused fields ignore keys. Unshifted horizontal
///          arrows collapse selections to their lower/upper edge. Invalid
///          handles trap.
int64_t rt_uitextinput_handle_key(void *ptr, int64_t key_code, int8_t shift_held) {
    rt_uitextinput_impl *ti =
        checked_textinput(ptr, "UITextInput.HandleKey: expected Zanna.Game.UI.HudTextInput");
    if (!ti || !ti->enabled || !ti->focused)
        return 0;
    if (key_code == UI_KEY_BACKSPACE) {
        if (textinput_delete_selection(ti))
            return 1;
        if (ti->cursor_byte <= 0)
            return 0;
        return textinput_delete_range(
            ti, ui_prev_codepoint_byte(ti->text, ti->text_bytes, ti->cursor_byte), ti->cursor_byte);
    }
    if (key_code == UI_KEY_DELETE) {
        if (textinput_delete_selection(ti))
            return 1;
        if (ti->cursor_byte >= ti->text_bytes)
            return 0;
        return textinput_delete_range(
            ti, ti->cursor_byte, ui_next_codepoint_byte(ti->text, ti->text_bytes, ti->cursor_byte));
    }
    if (key_code == UI_KEY_LEFT) {
        if (!shift_held && rt_uitextinput_has_selection(ptr)) {
            int64_t start = 0;
            int64_t end = 0;
            textinput_selection_range(ti, &start, &end);
            (void)end;
            textinput_move_cursor(ti, start, 0);
        } else {
            textinput_move_cursor(
                ti, ui_prev_codepoint_byte(ti->text, ti->text_bytes, ti->cursor_byte), shift_held);
        }
        return 0;
    }
    if (key_code == UI_KEY_RIGHT) {
        if (!shift_held && rt_uitextinput_has_selection(ptr)) {
            int64_t start = 0;
            int64_t end = 0;
            (void)start;
            textinput_selection_range(ti, &start, &end);
            textinput_move_cursor(ti, end, 0);
        } else {
            textinput_move_cursor(
                ti, ui_next_codepoint_byte(ti->text, ti->text_bytes, ti->cursor_byte), shift_held);
        }
        return 0;
    }
    if (key_code == UI_KEY_HOME) {
        textinput_move_cursor(ti, 0, shift_held);
        return 0;
    }
    if (key_code == UI_KEY_END) {
        textinput_move_cursor(ti, ti->text_bytes, shift_held);
        return 0;
    }
    if (key_code == 1) {
        rt_uitextinput_select_all(ptr);
        return 0;
    }
    return 0;
}

/// @brief Insert typed runtime-string bytes at the caret.
/// @param ptr UITextInput to update.
/// @param typed_text Source text; no reference is retained.
/// @return Nonzero when insertion or replaced-selection deletion changes text.
/// @details Requires enabled and focused state, enforces the codepoint limit,
///          and filters NUL/CR/LF. Allocation and wrong-class errors trap.
int64_t rt_uitextinput_handle_text(void *ptr, rt_string typed_text) {
    rt_uitextinput_impl *ti =
        checked_textinput(ptr, "UITextInput.HandleText: expected Zanna.Game.UI.HudTextInput");
    if (!ti || !typed_text || !ti->enabled || !ti->focused)
        return 0;
    const char *s = rt_string_cstr(typed_text);
    return textinput_insert_bytes(ti, s, (size_t)rt_str_len(typed_text));
}

/// @brief Update focus and caret from a mouse click.
/// @param ptr UITextInput to update.
/// @param mx Mouse X coordinate.
/// @param my Mouse Y coordinate.
/// @param shift_held Nonzero extends selection from the prior caret.
/// @details Enabled, visible fields focus only for an inside click; outside
///          clicks defocus. Text position uses nearest measured codepoint
///          midpoint. Invalid handles trap.
void rt_uitextinput_handle_mouse_click(void *ptr, int64_t mx, int64_t my, int8_t shift_held) {
    rt_uitextinput_impl *ti =
        checked_textinput(ptr, "UITextInput.HandleMouseClick: expected Zanna.Game.UI.HudTextInput");
    if (!ti || !ti->enabled || !ti->visible)
        return;
    ti->focused = ui_point_inside(ti->x, ti->y, ti->w, ti->h, mx, my);
    if (ti->focused)
        textinput_move_cursor(ti, textinput_byte_from_mouse(ti, mx), shift_held);
}

/// @brief Extend selection horizontally during a focused mouse drag.
/// @param ptr UITextInput to update.
/// @param mx Mouse X coordinate mapped to a caret boundary.
/// @param my Mouse Y coordinate, currently ignored.
/// @details Requires enabled and focused state. A missing anchor is created at
///          the old caret. Invalid handles trap.
void rt_uitextinput_handle_mouse_drag(void *ptr, int64_t mx, int64_t my) {
    (void)my;
    rt_uitextinput_impl *ti =
        checked_textinput(ptr, "UITextInput.HandleMouseDrag: expected Zanna.Game.UI.HudTextInput");
    if (!ti || !ti->enabled || !ti->focused)
        return;
    if (ti->selection_anchor < 0)
        ti->selection_anchor = ti->cursor_byte;
    ti->cursor_byte = textinput_byte_from_mouse(ti, mx);
}

/// @brief Advance caret-blink elapsed time.
/// @param ptr UITextInput to update.
/// @param delta_ms Positive elapsed milliseconds; nonpositive values are
///        ignored.
/// @details Overflow resets elapsed time to zero. Invalid handles trap.
void rt_uitextinput_update(void *ptr, int64_t delta_ms) {
    rt_uitextinput_impl *ti =
        checked_textinput(ptr, "UITextInput.Update: expected Zanna.Game.UI.HudTextInput");
    if (!ti || delta_ms <= 0)
        return;
    if (delta_ms > INT64_MAX - ti->cursor_blink_elapsed)
        ti->cursor_blink_elapsed = 0;
    else
        ti->cursor_blink_elapsed += delta_ms;
}

/// @brief Draw the text field through polymorphic GameUI canvas operations.
/// @param ptr UITextInput to render.
/// @param canvas 2D Canvas or registered Canvas3D target.
/// @details Hidden/null fields are no-ops. Draws background, focus border,
///          selection, placeholder or password-mask text, and a blinking caret.
///          Password allocation failure draws an empty mask for that frame.
///          Invalid non-null handles trap.
void rt_uitextinput_draw(void *ptr, void *canvas) {
    rt_gameui_draw_ops_t ops;
    rt_uitextinput_impl *ti =
        checked_textinput(ptr, "UITextInput.Draw: expected Zanna.Game.UI.HudTextInput");
    if (!ti || !canvas || !ui_resolve_draw_ops(canvas, "UITextInput.Draw: expected Canvas or Canvas3D", &ops))
        return;
    if (!ti->visible)
        return;
    ops.box(canvas, ti->x, ti->y, ti->w, ti->h, ti->bg_color);
    ops.frame(canvas,
                    ti->x,
                    ti->y,
                    ti->w,
                    ti->h,
                    ti->focused ? ti->border_color_focused : ti->border_color);

    const char *draw_text = ti->text ? ti->text : "";
    char *password = NULL;
    if (ti->password_mode && ti->text_bytes > 0) {
        draw_text = "";
        int64_t cps = ui_codepoint_count_bytes(ti->text, ti->text_bytes);
        if (cps > 0 && (uint64_t)cps < SIZE_MAX) {
            password = (char *)malloc((size_t)cps + 1);
            if (password) {
                memset(password, '*', (size_t)cps);
                password[cps] = '\0';
                draw_text = password;
            }
        }
    } else if (ti->text_bytes == 0 && !ti->focused && ti->placeholder &&
               ti->placeholder[0] != '\0') {
        draw_text = ti->placeholder;
    }

    int64_t start = 0;
    int64_t end = 0;
    if (textinput_selection_range(ti, &start, &end)) {
        int64_t x0 = ti->x + 4 + ui_text_prefix_width(ti->text, start, ti->font, 1);
        int64_t sel_w = ui_text_prefix_width(ti->text + start, end - start, ti->font, 1);
        ops.box_alpha(canvas, x0, ti->y + 2, sel_w, ti->h - 4, ti->selection_color, 160);
    }
    ui_draw_text_basic(&ops,
                       ti->x + 4,
                       ti->y + (ti->h - 8) / 2,
                       draw_text,
                       ti->font,
                       1,
                       ti->text_bytes == 0 && !ti->focused ? 0x909090 : ti->text_color);
    if (ti->focused && ti->enabled && ti->cursor_blink_ms > 0 &&
        ((ti->cursor_blink_elapsed / ti->cursor_blink_ms) % 2) == 0) {
        int64_t cx = ti->x + 4 + ui_text_prefix_width(ti->text, ti->cursor_byte, ti->font, 1);
        ops.line(canvas, cx, ti->y + 3, cx, ti->y + ti->h - 4, ti->cursor_color);
    }
    free(password);
}

/// @brief Set the normal text color.
/// @param ptr UITextInput to mutate; `NULL` is a no-op.
/// @param color Packed value stored verbatim.
/// @details A non-null invalid handle raises a runtime trap.
void rt_uitextinput_set_text_color(void *ptr, int64_t color) {
    rt_uitextinput_impl *ti =
        checked_textinput(ptr, "UITextInput.SetTextColor: expected Zanna.Game.UI.HudTextInput");
    if (ti)
        ti->text_color = color;
}

/// @brief Return the normal text color.
/// @param ptr UITextInput to query.
/// @return Stored color, or zero for null/invalid.
int64_t rt_uitextinput_get_text_color(void *ptr) {
    rt_uitextinput_impl *ti =
        checked_textinput(ptr, "UITextInput.GetTextColor: expected Zanna.Game.UI.HudTextInput");
    return ti ? ti->text_color : 0;
}

/// @brief Set the field background color.
/// @param ptr UITextInput to mutate; `NULL` is a no-op.
/// @param color Packed value stored verbatim.
void rt_uitextinput_set_bg_color(void *ptr, int64_t color) {
    rt_uitextinput_impl *ti =
        checked_textinput(ptr, "UITextInput.SetBgColor: expected Zanna.Game.UI.HudTextInput");
    if (ti)
        ti->bg_color = color;
}

/// @brief Return the field background color.
/// @param ptr UITextInput to query.
/// @return Stored color, or zero for null/invalid.
int64_t rt_uitextinput_get_bg_color(void *ptr) {
    rt_uitextinput_impl *ti =
        checked_textinput(ptr, "UITextInput.GetBgColor: expected Zanna.Game.UI.HudTextInput");
    return ti ? ti->bg_color : 0;
}

/// @brief Set the caret line color.
/// @param ptr UITextInput to mutate; `NULL` is a no-op.
/// @param color Packed value stored verbatim.
void rt_uitextinput_set_cursor_color(void *ptr, int64_t color) {
    rt_uitextinput_impl *ti =
        checked_textinput(ptr, "UITextInput.SetCursorColor: expected Zanna.Game.UI.HudTextInput");
    if (ti)
        ti->cursor_color = color;
}

/// @brief Set the selection-highlight base color.
/// @param ptr UITextInput to mutate; `NULL` is a no-op.
/// @param color Packed value stored verbatim; draw uses alpha 160.
void rt_uitextinput_set_selection_color(void *ptr, int64_t color) {
    rt_uitextinput_impl *ti = checked_textinput(
        ptr, "UITextInput.SetSelectionColor: expected Zanna.Game.UI.HudTextInput");
    if (ti)
        ti->selection_color = color;
}

/// @brief Set the unfocused border color.
/// @param ptr UITextInput to mutate; `NULL` is a no-op.
/// @param color Packed value stored verbatim.
void rt_uitextinput_set_border_color(void *ptr, int64_t color) {
    rt_uitextinput_impl *ti =
        checked_textinput(ptr, "UITextInput.SetBorderColor: expected Zanna.Game.UI.HudTextInput");
    if (ti)
        ti->border_color = color;
}

/// @brief Set the focused border color.
/// @param ptr UITextInput to mutate; `NULL` is a no-op.
/// @param color Packed value stored verbatim.
void rt_uitextinput_set_border_color_focused(void *ptr, int64_t color) {
    rt_uitextinput_impl *ti = checked_textinput(
        ptr, "UITextInput.SetBorderColorFocused: expected Zanna.Game.UI.HudTextInput");
    if (ti)
        ti->border_color_focused = color;
}

/// @brief Assign an optional retained BitmapFont.
/// @param ptr UITextInput to mutate; `NULL` is a no-op.
/// @param font BitmapFont reference, or `NULL` for default canvas text.
/// @details Validates the font class, retains a replacement before releasing
///          the prior font, and traps on wrong non-null classes.
void rt_uitextinput_set_font(void *ptr, void *font) {
    rt_uitextinput_impl *ti =
        checked_textinput(ptr, "UITextInput.SetFont: expected Zanna.Game.UI.HudTextInput");
    if (!ti || !ui_validate_bitmapfont(font, "UITextInput.SetFont: expected BitmapFont"))
        return;
    ui_replace_ref(&ti->font, font);
}

/// @brief Set normalized visibility.
/// @param ptr UITextInput to mutate; `NULL` is a no-op.
/// @param visible Zero hides; any nonzero value shows.
void rt_uitextinput_set_visible(void *ptr, int8_t visible) {
    rt_uitextinput_impl *ti =
        checked_textinput(ptr, "UITextInput.SetVisible: expected Zanna.Game.UI.HudTextInput");
    if (ti)
        ti->visible = visible ? 1 : 0;
}

/// @brief Return normalized visibility.
/// @param ptr UITextInput to query.
/// @return Stored flag, or zero for null/invalid.
int8_t rt_uitextinput_get_visible(void *ptr) {
    rt_uitextinput_impl *ti =
        checked_textinput(ptr, "UITextInput.GetVisible: expected Zanna.Game.UI.HudTextInput");
    return ti ? ti->visible : 0;
}

/// @brief Set normalized input-enabled state.
/// @param ptr UITextInput to mutate; `NULL` is a no-op.
/// @param enabled Zero disables; any nonzero value enables.
void rt_uitextinput_set_enabled(void *ptr, int8_t enabled) {
    rt_uitextinput_impl *ti =
        checked_textinput(ptr, "UITextInput.SetEnabled: expected Zanna.Game.UI.HudTextInput");
    if (ti)
        ti->enabled = enabled ? 1 : 0;
}

/// @brief Return normalized input-enabled state.
/// @param ptr UITextInput to query.
/// @return Stored flag, or zero for null/invalid.
int8_t rt_uitextinput_get_enabled(void *ptr) {
    rt_uitextinput_impl *ti =
        checked_textinput(ptr, "UITextInput.GetEnabled: expected Zanna.Game.UI.HudTextInput");
    return ti ? ti->enabled : 0;
}

/// @brief Set keyboard-focus state.
/// @param ptr UITextInput to mutate; `NULL` is a no-op.
/// @param focused Zero clears focus; any nonzero value sets it.
/// @details Always restarts blink timing and clears selection when focus is
///          removed.
void rt_uitextinput_set_focused(void *ptr, int8_t focused) {
    rt_uitextinput_impl *ti =
        checked_textinput(ptr, "UITextInput.SetFocused: expected Zanna.Game.UI.HudTextInput");
    if (!ti)
        return;
    ti->focused = focused ? 1 : 0;
    ti->cursor_blink_elapsed = 0;
    if (!ti->focused)
        ti->selection_anchor = -1;
}

/// @brief Return normalized focus state.
/// @param ptr UITextInput to query.
/// @return Stored flag, or zero for null/invalid.
int8_t rt_uitextinput_get_focused(void *ptr) {
    rt_uitextinput_impl *ti =
        checked_textinput(ptr, "UITextInput.GetFocused: expected Zanna.Game.UI.HudTextInput");
    return ti ? ti->focused : 0;
}

/// @brief Toggle password display masking.
/// @param ptr UITextInput to mutate; `NULL` is a no-op.
/// @param password Zero shows text; nonzero draws one asterisk per codepoint.
void rt_uitextinput_set_password_mode(void *ptr, int8_t password) {
    rt_uitextinput_impl *ti =
        checked_textinput(ptr, "UITextInput.SetPasswordMode: expected Zanna.Game.UI.HudTextInput");
    if (ti)
        ti->password_mode = password ? 1 : 0;
}

/// @brief Replace the owned placeholder text.
/// @param ptr UITextInput to mutate; `NULL` is a no-op.
/// @param placeholder Runtime string to copy, or `NULL` to clear.
/// @details Copying stops at embedded NUL and retains no input reference.
///          Storage growth and wrong-class failures trap.
void rt_uitextinput_set_placeholder(void *ptr, rt_string placeholder) {
    rt_uitextinput_impl *ti =
        checked_textinput(ptr, "UITextInput.SetPlaceholder: expected Zanna.Game.UI.HudTextInput");
    if (!ti)
        return;
    const char *text = placeholder ? rt_string_cstr(placeholder) : "";
    size_t len = placeholder ? (size_t)rt_str_len(placeholder) : 0;
    len = ui_visible_len(text, len);
    if (!ensure_text_storage(&ti->placeholder, &ti->placeholder_capacity, (int64_t)len + 1)) {
        rt_trap("UITextInput.SetPlaceholder: placeholder allocation failed");
        return;
    }
    if (len > 0)
        memmove(ti->placeholder, text, len);
    ti->placeholder[len] = '\0';
}

/// @brief Set or remove the accepted-codepoint limit.
/// @param ptr UITextInput to mutate; `NULL` is a no-op.
/// @param max_cps Positive maximum, or nonpositive for unlimited input.
/// @details Lowering the limit below current length immediately truncates text
///          through textinput_set_bytes(), moving the caret to the new end and
///          clearing selection/scroll. Every simple setter/getter above traps
///          on a non-null wrong UITextInput class.
void rt_uitextinput_set_max_codepoints(void *ptr, int64_t max_cps) {
    rt_uitextinput_impl *ti =
        checked_textinput(ptr, "UITextInput.SetMaxCodepoints: expected Zanna.Game.UI.HudTextInput");
    if (!ti)
        return;
    ti->max_codepoints = max_cps > 0 ? max_cps : 0;
    if (ti->max_codepoints > 0 &&
        ui_codepoint_count_bytes(ti->text, ti->text_bytes) > ti->max_codepoints)
        textinput_set_bytes(ti, ti->text, (size_t)ti->text_bytes);
}
