//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: lib/gui/src/widgets/vg_textinput.c
// Purpose: Text input widget implementation — single/multi-line editable field
//          with UTF-8 text, cursor, selection, undo/redo, scrolling, placeholder,
//          max_length, read-only mode, and password masking.
// Key invariants:
//   - input->text is always a valid null-terminated UTF-8 string.
//   - All character positions (cursor_pos, selection_start/end) are in codepoints,
//     not bytes; textinput_byte_offset converts to bytes when needed.
//   - textinput_sanitize_utf8_copy ensures only well-formed UTF-8 enters the buffer.
//   - Undo history is reset on vg_textinput_set_text (programmatic replacement).
// Ownership/Lifetime:
//   - input->text and input->placeholder are heap-allocated; freed in destroy.
//   - The undo history ring buffer is owned by the widget.
// Links: lib/gui/include/vg_widgets.h,
//        lib/gui/include/vg_theme.h,
//        lib/gui/include/vg_event.h
//
//===----------------------------------------------------------------------===//
/// @file
/// @brief Implements UTF-8 text editing, grapheme-safe navigation, selection,
///        undo/redo, scrolling, password display, and IME composition.
/// @details Committed positions are codepoint indices and are snapped through
///          grapheme helpers for user-visible navigation. Composition remains
///          separate from committed text and undo history until explicit commit.
#include "../../../graphics/include/vgfx.h"
#include "../../include/vg_draw.h"
#include "../../include/vg_event.h"
#include "../../include/vg_grapheme.h"
#include "../../include/vg_theme.h"
#include "../../include/vg_widgets.h"
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

//=============================================================================
// Constants
//=============================================================================

#define TEXTINPUT_INITIAL_CAPACITY 64
#define TEXTINPUT_GROWTH_FACTOR 2
#define CURSOR_BLINK_RATE 0.5f // Seconds
#define TEXTINPUT_UNDO_CAPACITY 32
#define TEXTINPUT_UNDO_MAX_BYTES (64u * 1024u)
#define TEXTINPUT_STACK_TEXT_CAPACITY 512u

//=============================================================================
// Forward Declarations
//=============================================================================

static void textinput_destroy(vg_widget_t *widget);
static void textinput_measure(vg_widget_t *widget, float available_width, float available_height);
static void textinput_paint(vg_widget_t *widget, void *canvas);
static bool textinput_handle_event(vg_widget_t *widget, vg_event_t *event);
static bool textinput_can_focus(vg_widget_t *widget);
static void textinput_on_focus(vg_widget_t *widget, bool gained);
static void textinput_ensure_cursor_visible(vg_textinput_t *input);
static void textinput_delete_selection_internal(vg_textinput_t *input, bool notify);
static void textinput_clear_composition(vg_textinput_t *input);

// Forward declaration for clipboard support
char *vg_textinput_get_selection(vg_textinput_t *input);

//=============================================================================
// TextInput VTable
//=============================================================================

static vg_widget_vtable_t g_textinput_vtable = {.destroy = textinput_destroy,
                                                .measure = textinput_measure,
                                                .arrange = NULL,
                                                .paint = textinput_paint,
                                                .handle_event = textinput_handle_event,
                                                .can_focus = textinput_can_focus,
                                                .on_focus = textinput_on_focus};

//=============================================================================
// Helper Functions
//=============================================================================

/// @brief Grows input->text to hold at least @p needed bytes, doubling from
/// TEXTINPUT_INITIAL_CAPACITY.
/// @param input Text input owning the mutable buffer.
/// @param needed Minimum byte capacity including any terminator required by the caller.
/// @return `true` when capacity is sufficient; `false` with the old buffer intact
///         on overflow or allocation failure.
static bool ensure_capacity(vg_textinput_t *input, size_t needed) {
    if (needed <= input->text_capacity)
        return true;

    size_t new_capacity = input->text_capacity ? input->text_capacity : TEXTINPUT_INITIAL_CAPACITY;
    while (new_capacity < needed) {
        if (new_capacity > SIZE_MAX / TEXTINPUT_GROWTH_FACTOR)
            return false;
        new_capacity *= TEXTINPUT_GROWTH_FACTOR;
    }

    char *new_text = realloc(input->text, new_capacity);
    if (!new_text)
        return false;

    input->text = new_text;
    input->text_capacity = new_capacity;
    return true;
}

/// @brief True if a key event should produce literal text input — i.e. it
///        carries no command modifier (Super/Ctrl) that would make it a
///        shortcut rather than a character.
/// @param event Character event whose modifier state is inspected.
/// @return `true` when modifier policy permits insertion.
static bool textinput_key_char_allows_text(const vg_event_t *event) {
    if (!event)
        return false;

    uint32_t mods = event->modifiers;
    bool has_super = (mods & VG_MOD_SUPER) != 0;
    bool has_ctrl = (mods & VG_MOD_CTRL) != 0;
    bool has_alt = (mods & VG_MOD_ALT) != 0;

    if (has_super)
        return false;
    if (has_ctrl && !has_alt)
        return false;
    if (has_alt && !has_ctrl)
        return false;
    return true;
}

/// @brief Validate a Unicode scalar value for direct text insertion.
/// @details Rejects C0/C1 controls, surrogates, private-use ranges, and values
///          beyond the Unicode scalar limit.
/// @param cp Candidate Unicode codepoint.
/// @return `true` when the value is accepted as ordinary text.
static bool textinput_codepoint_is_text(uint32_t cp) {
    if (cp < 0x20 || cp == 0x7F || cp > 0x10FFFF)
        return false;
    if (cp >= 0x80 && cp <= 0x9F)
        return false;
    if (cp >= 0xD800 && cp <= 0xDFFF)
        return false;
    if ((cp >= 0xE000 && cp <= 0xF8FF) || (cp >= 0xF0000 && cp <= 0xFFFFD) ||
        (cp >= 0x100000 && cp <= 0x10FFFD))
        return false;
    return true;
}

/// @brief Returns the number of UTF-8 codepoints in the input's text buffer.
/// @param input Text input to inspect.
/// @return Cached committed codepoint count, or zero for `NULL`.
static size_t textinput_char_count(const vg_textinput_t *input) {
    return input ? input->text_char_count : 0;
}

/// @brief Increment an edge revision without allowing unsigned wraparound.
/// @param revision Counter to advance; NULL is ignored.
static void textinput_increment_revision(uint64_t *revision) {
    if (revision && *revision < UINT64_MAX)
        (*revision)++;
}

/// @brief Record one committed-text mutation before invoking the optional callback.
/// @details Text changes advance the input-specific edge and the common widget
///          revision independently of submit/composition state. The callback
///          observes the fully updated text and cursor state.
/// @param input Text input whose committed text changed.
/// @param notify true to invoke the configured change callback.
static void textinput_note_change(vg_textinput_t *input, bool notify) {
    if (!input)
        return;
    textinput_increment_revision(&input->change_revision);
    vg_widget_note_revision(&input->base);
    if (notify && input->on_change)
        input->on_change(&input->base, input->text, input->on_change_data);
}

/// @brief Record one independent single-line submission and invoke its callback.
/// @param input Text input that received an Enter/Return submission.
static void textinput_note_submit(vg_textinput_t *input) {
    if (!input)
        return;
    textinput_increment_revision(&input->submit_revision);
    vg_widget_note_revision(&input->base);
    if (input->on_commit)
        input->on_commit(&input->base, input->text, input->on_commit_data);
}

/// @brief Refresh cached codepoint and line counts after text mutation.
/// @details Text input code performs many cursor, selection, scroll, and paint
///          operations between edits.  Keeping these counts beside text_len
///          avoids repeatedly walking the whole UTF-8 buffer in hot paths while
///          preserving byte/codepoint separation for editing operations.
/// @param input Text input whose `text` and `text_len` fields are current.
static void textinput_refresh_text_metrics(vg_textinput_t *input) {
    if (!input || !input->text) {
        if (input) {
            input->text_char_count = 0;
            input->text_line_count = 1;
        }
        return;
    }

    int chars = vg_utf8_strlen(input->text);
    input->text_char_count = chars > 0 ? (size_t)chars : 0;

    size_t lines = 1;
    for (size_t i = 0; i < input->text_len; i++) {
        if (input->text[i] == '\n')
            lines++;
    }
    input->text_line_count = lines;
}

/// @brief Clamps @p pos to [0, char_count] so it is always a valid codepoint index.
/// @param input TextInput whose cached codepoint count bounds the position.
/// @param pos Requested codepoint index.
/// @return Valid index no greater than the current text length.
static size_t textinput_clamp_char_pos(const vg_textinput_t *input, size_t pos) {
    size_t chars = textinput_char_count(input);
    return pos > chars ? chars : pos;
}

/// @brief Returns the byte offset of codepoint @p char_pos within input->text.
/// @param input TextInput whose UTF-8 text is indexed.
/// @param char_pos Requested codepoint index, clamped to the text.
/// @return Byte offset of the clamped codepoint boundary.
static size_t textinput_byte_offset(const vg_textinput_t *input, size_t char_pos) {
    return (size_t)vg_utf8_offset(input->text, (int)textinput_clamp_char_pos(input, char_pos));
}

/// @brief Convert a public grapheme index to legacy codepoint storage units.
/// @param input Text input whose committed buffer is used for conversion.
/// @param grapheme_index Requested extended-grapheme boundary.
/// @return Clamped codepoint offset, or zero for invalid input.
static size_t textinput_char_pos_from_grapheme(const vg_textinput_t *input, size_t grapheme_index) {
    if (!input || !input->text)
        return 0;
    return vg_grapheme_codepoint_offset(input->text, input->text_len, grapheme_index);
}

/// @brief Convert one legacy codepoint position to a public grapheme index.
/// @param input Text input whose committed buffer is used for conversion.
/// @param char_pos Requested codepoint boundary, clamped to the buffer.
/// @return Grapheme index containing or beginning at @p char_pos.
static size_t textinput_grapheme_from_char_pos(const vg_textinput_t *input, size_t char_pos) {
    if (!input || !input->text)
        return 0;
    return vg_grapheme_index_from_codepoint(
        input->text, input->text_len, textinput_clamp_char_pos(input, char_pos));
}

/// @brief Snap a legacy codepoint position down to an extended-grapheme boundary.
/// @param input Text input whose committed buffer defines cluster boundaries.
/// @param char_pos Candidate legacy codepoint offset.
/// @return Codepoint offset at the containing cluster's start or end-of-text.
static size_t textinput_snap_char_pos(const vg_textinput_t *input, size_t char_pos) {
    size_t grapheme = textinput_grapheme_from_char_pos(input, char_pos);
    return textinput_char_pos_from_grapheme(input, grapheme);
}

/// @brief Snap a legacy codepoint position up to an extended-grapheme boundary.
/// @param input Text input whose committed buffer defines cluster boundaries.
/// @param char_pos Candidate legacy codepoint offset.
/// @return Exact boundary at or following the candidate position.
static size_t textinput_snap_char_pos_forward(const vg_textinput_t *input, size_t char_pos) {
    if (!input || !input->text)
        return 0;
    size_t clamped = textinput_clamp_char_pos(input, char_pos);
    size_t floor = textinput_snap_char_pos(input, clamped);
    if (floor == clamped)
        return floor;
    return vg_grapheme_next_codepoint_boundary(input->text, input->text_len, clamped);
}

/// @brief Advances @p *cursor past one well-formed UTF-8 sequence within [cursor, limit); returns
/// false on error.
/// @param[in,out] cursor Address of the current byte pointer, advanced only on success.
/// @param limit One-past-the-end bound for the readable byte range.
/// @return True when one valid sequence was consumed.
static bool textinput_utf8_advance_bounded(const char **cursor, const char *limit) {
    if (!cursor || !*cursor || *cursor >= limit || **cursor == '\0')
        return false;

    const unsigned char *s = (const unsigned char *)*cursor;
    size_t remaining = (size_t)(limit - *cursor);
    uint32_t cp = 0;

    if ((s[0] & 0x80u) == 0) {
        *cursor += 1;
        return true;
    }
    if ((s[0] & 0xE0u) == 0xC0u) {
        if (remaining < 2 || (s[1] & 0xC0u) != 0x80u)
            return false;
        cp = ((uint32_t)(s[0] & 0x1Fu) << 6) | (uint32_t)(s[1] & 0x3Fu);
        if (cp < 0x80u)
            return false;
        *cursor += 2;
        return true;
    }
    if ((s[0] & 0xF0u) == 0xE0u) {
        if (remaining < 3 || (s[1] & 0xC0u) != 0x80u || (s[2] & 0xC0u) != 0x80u)
            return false;
        cp = ((uint32_t)(s[0] & 0x0Fu) << 12) | ((uint32_t)(s[1] & 0x3Fu) << 6) |
             (uint32_t)(s[2] & 0x3Fu);
        if (cp < 0x800u || (cp >= 0xD800u && cp <= 0xDFFFu))
            return false;
        *cursor += 3;
        return true;
    }
    if ((s[0] & 0xF8u) == 0xF0u) {
        if (remaining < 4 || (s[1] & 0xC0u) != 0x80u || (s[2] & 0xC0u) != 0x80u ||
            (s[3] & 0xC0u) != 0x80u)
            return false;
        cp = ((uint32_t)(s[0] & 0x07u) << 18) | ((uint32_t)(s[1] & 0x3Fu) << 12) |
             ((uint32_t)(s[2] & 0x3Fu) << 6) | (uint32_t)(s[3] & 0x3Fu);
        if (cp < 0x10000u || cp > 0x10FFFFu)
            return false;
        *cursor += 4;
        return true;
    }
    return false;
}

/// @brief Returns the codepoint index corresponding to @p byte_offset bytes into @p text.
/// @param text NUL-terminated UTF-8 text to scan.
/// @param byte_offset Maximum prefix length in bytes.
/// @return Number of well-formed codepoints preceding the bounded byte offset.
static size_t textinput_char_index_from_byte_offset(const char *text, size_t byte_offset) {
    if (!text)
        return 0;

    const char *cursor = text;
    const char *target = text + byte_offset;
    size_t chars = 0;
    while (*cursor && cursor < target) {
        vg_utf8_decode(&cursor);
        chars++;
    }
    return chars;
}

/// @brief Counts the number of well-formed UTF-8 codepoints in the first @p byte_len bytes of @p
/// text.
/// @param text UTF-8 byte sequence to scan.
/// @param byte_len Maximum number of bytes available.
/// @return Number of valid codepoints wholly contained in the prefix.
static size_t textinput_codepoint_count_in_prefix(const char *text, size_t byte_len) {
    if (!text)
        return 0;

    const char *cursor = text;
    const char *end = text + byte_len;
    size_t chars = 0;
    while (*cursor && cursor < end) {
        const char *prev = cursor;
        if (!textinput_utf8_advance_bounded(&cursor, end))
            break;
        if (cursor == prev)
            break;
        chars++;
    }
    return chars;
}

/// @brief Returns a heap-allocated copy of @p text with invalid UTF-8 bytes silently skipped.
/// @param text Source byte sequence; NULL is treated as empty.
/// @param input_len Number of source bytes available.
/// @param[out] out_len Optional destination for the sanitized byte length.
/// @param[out] out_chars Optional destination for the sanitized codepoint count.
/// @return NUL-terminated heap copy owned by the caller, or NULL on overflow or allocation failure.
static char *textinput_sanitize_utf8_copy(const char *text,
                                          size_t input_len,
                                          size_t *out_len,
                                          size_t *out_chars) {
    if (out_len)
        *out_len = 0;
    if (out_chars)
        *out_chars = 0;
    if (!text)
        input_len = 0;
    if (input_len > SIZE_MAX - 1)
        return NULL;

    char *clean = (char *)malloc(input_len + 1);
    if (!clean)
        return NULL;

    const char *cursor = text ? text : "";
    const char *end = cursor + input_len;
    size_t write = 0;
    size_t chars = 0;
    while (cursor < end && *cursor) {
        const char *prev = cursor;
        if (textinput_utf8_advance_bounded(&cursor, end)) {
            size_t span = (size_t)(cursor - prev);
            memcpy(clean + write, prev, span);
            write += span;
            chars++;
        } else {
            cursor = prev + 1;
        }
    }

    clean[write] = '\0';
    if (out_len)
        *out_len = write;
    if (out_chars)
        *out_chars = chars;
    return clean;
}

/// @brief Returns true if @p ch is a whitespace or punctuation character used as a Ctrl+Arrow word
/// boundary.
/// @param ch Single-byte character to classify.
/// @return True when the byte separates word-navigation runs.
static bool textinput_is_word_separator(unsigned char ch) {
    if (ch <= ' ')
        return true;
    return strchr(".,;:!?()[]{}<>/\\|+-*=~`'\"@", (int)ch) != NULL;
}

/// @brief Returns true if the codepoint at @p char_pos in the input's text is a word boundary
/// character.
/// @param input TextInput whose text is inspected.
/// @param char_pos Codepoint index to classify.
/// @return True for an in-range ASCII separator; false otherwise.
static bool textinput_char_is_word_separator_at(const vg_textinput_t *input, size_t char_pos) {
    size_t char_count = textinput_char_count(input);
    if (!input || char_pos >= char_count)
        return true;
    size_t byte_pos = textinput_byte_offset(input, char_pos);
    return textinput_is_word_separator((unsigned char)input->text[byte_pos]);
}

/// @brief Resets the cursor blink timer to zero and makes the cursor immediately visible.
/// @param input TextInput whose caret animation is reset.
static void textinput_reset_cursor_blink(vg_textinput_t *input) {
    if (!input)
        return;
    input->cursor_blink_time = 0.0f;
    input->cursor_visible = true;
}

/// @brief Clears all undo/redo slots and seeds the initial state with the current text and cursor.
/// @param input TextInput whose snapshot history is rebuilt.
static void textinput_reset_undo_history(vg_textinput_t *input) {
    if (!input)
        return;
    for (int i = 0; i < TEXTINPUT_UNDO_CAPACITY; i++) {
        free(input->undo_stack[i]);
        input->undo_stack[i] = NULL;
        input->undo_cursors[i] = 0;
    }
    input->undo_stack[0] = vg_strdup(input->text ? input->text : "");
    input->undo_cursors[0] = input->cursor_pos;
    input->undo_count = input->undo_stack[0] ? 1 : 0;
    input->undo_pos = 0;
}

/// @brief Clamps and sets cursor_pos, selection_start, and selection_end all to @p pos (collapses
/// selection).
/// @param input TextInput whose cursor and selection are updated.
/// @param pos Requested codepoint position, snapped to a valid boundary.
static void textinput_set_cursor_internal(vg_textinput_t *input, size_t pos) {
    size_t clamped = textinput_snap_char_pos(input, pos);
    input->cursor_pos = clamped;
    input->selection_start = clamped;
    input->selection_end = clamped;
}

/// @brief Selects the word (or run of separators) surrounding @p char_pos, used on double-click.
/// @param input TextInput whose selection is replaced.
/// @param char_pos Codepoint position around which the run is found.
static void textinput_select_word_at(vg_textinput_t *input, size_t char_pos) {
    if (!input)
        return;
    size_t char_count = textinput_char_count(input);
    if (char_count == 0) {
        textinput_set_cursor_internal(input, 0);
        return;
    }
    if (char_pos >= char_count)
        char_pos = char_count - 1;

    bool separators = textinput_char_is_word_separator_at(input, char_pos);
    size_t start = char_pos;
    size_t end = char_pos + 1;
    while (start > 0 && textinput_char_is_word_separator_at(input, start - 1) == separators) {
        start--;
    }
    while (end < char_count && textinput_char_is_word_separator_at(input, end) == separators) {
        end++;
    }

    input->selection_start = textinput_snap_char_pos(input, start);
    input->selection_end = textinput_snap_char_pos_forward(input, end);
    input->cursor_pos = end;
    textinput_ensure_cursor_visible(input);
    textinput_reset_cursor_blink(input);
    input->base.needs_paint = true;
}

/// @brief Returns the line height from font metrics, or font_size+4 if metrics are unavailable.
/// @param input TextInput supplying font and size.
/// @return Positive line height in pixels using metrics or the fallback.
static float textinput_line_height(const vg_textinput_t *input) {
    vg_font_metrics_t metrics = {0};
    if (!input || !input->font)
        return input ? input->font_size : 14.0f;
    vg_font_get_metrics(input->font, input->font_size, &metrics);
    return metrics.line_height > 0 ? (float)metrics.line_height : (input->font_size + 4.0f);
}

/// @brief Counts the number of newline-separated logical lines in the input (always ≥ 1).
/// @param input TextInput whose cached line count is queried.
/// @return Cached logical line count, falling back to one.
static size_t textinput_line_count(const vg_textinput_t *input) {
    return input && input->text_line_count > 0 ? input->text_line_count : 1;
}

/// @brief Copy a byte range into stack storage, using heap storage only for long lines.
/// @details Font measurement APIs expect NUL-terminated text.  Multiline text
///          input often needs temporary per-line strings while painting and
///          hit-testing; using caller-owned stack storage for typical short
///          lines avoids per-frame malloc/free traffic.  When the range does
///          not fit, `*heap_out` receives the owned heap string to free.
/// @param text Source string.
/// @param start_byte Inclusive byte offset.
/// @param end_byte Exclusive byte offset.
/// @param stack_buf Caller-owned fallback buffer.
/// @param stack_cap Size of @p stack_buf in bytes.
/// @param heap_out Receives heap allocation when one is used; may be NULL.
/// @return NUL-terminated text range, or an empty string on invalid input/OOM.
static const char *textinput_range_to_buffer(const char *text,
                                             size_t start_byte,
                                             size_t end_byte,
                                             char *stack_buf,
                                             size_t stack_cap,
                                             char **heap_out) {
    if (heap_out)
        *heap_out = NULL;
    if (!stack_buf || stack_cap == 0)
        return "";
    stack_buf[0] = '\0';
    if (!text || end_byte < start_byte)
        return stack_buf;
    size_t len = end_byte - start_byte;
    if (len >= stack_cap) {
        if (len == SIZE_MAX)
            return stack_buf;
        char *copy = (char *)malloc(len + 1u);
        if (!copy)
            return stack_buf;
        memcpy(copy, text + start_byte, len);
        copy[len] = '\0';
        if (heap_out)
            *heap_out = copy;
        return copy;
    }
    memcpy(stack_buf, text + start_byte, len);
    stack_buf[len] = '\0';
    return stack_buf;
}

/// @brief Build a password-mask string using stack storage when possible.
/// @details Password-mode paint needs a temporary string with one mask glyph per
///          codepoint.  The function mirrors textinput_range_to_buffer's
///          ownership contract so callers can free only `*heap_out`.
/// @param char_count Number of codepoints to mask.
/// @param stack_buf Caller-owned fallback buffer.
/// @param stack_cap Size of @p stack_buf in bytes.
/// @param heap_out Receives heap allocation when one is used; may be NULL.
/// @return NUL-terminated mask string, or an empty string on OOM.
static const char *textinput_mask_to_buffer(size_t char_count,
                                            char *stack_buf,
                                            size_t stack_cap,
                                            char **heap_out) {
    if (heap_out)
        *heap_out = NULL;
    if (!stack_buf || stack_cap == 0)
        return "";
    stack_buf[0] = '\0';
    if (char_count >= stack_cap) {
        if (char_count == SIZE_MAX)
            return stack_buf;
        char *copy = (char *)malloc(char_count + 1u);
        if (!copy)
            return stack_buf;
        memset(copy, '*', char_count);
        copy[char_count] = '\0';
        if (heap_out)
            *heap_out = copy;
        return copy;
    }
    memset(stack_buf, '*', char_count);
    stack_buf[char_count] = '\0';
    return stack_buf;
}

/// @brief Finds the byte range and char offset of the @p target_line'th newline-delimited line.
/// @param input TextInput whose text is scanned.
/// @param target_line Zero-based logical line index; out-of-range values select the final line.
/// @param[out] out_start_byte Optional destination for the line's first byte offset.
/// @param[out] out_end_byte Optional destination for its exclusive final byte offset.
/// @param[out] out_start_char Optional destination for its first codepoint index.
static void textinput_get_line_at_index(const vg_textinput_t *input,
                                        size_t target_line,
                                        size_t *out_start_byte,
                                        size_t *out_end_byte,
                                        size_t *out_start_char) {
    size_t start_byte = 0;
    size_t start_char = 0;
    size_t line = 0;

    while (1) {
        size_t end_byte = start_byte;
        while (end_byte < input->text_len && input->text[end_byte] != '\n')
            end_byte++;

        if (line == target_line || end_byte == input->text_len) {
            if (out_start_byte)
                *out_start_byte = start_byte;
            if (out_end_byte)
                *out_end_byte = end_byte;
            if (out_start_char)
                *out_start_char = start_char;
            return;
        }

        size_t line_chars =
            textinput_codepoint_count_in_prefix(input->text + start_byte, end_byte - start_byte);
        start_byte = end_byte + 1;
        start_char += line_chars + 1;
        line++;
    }
}

/// @brief Returns the line index and byte/char range for the logical line that contains codepoint
/// @p char_pos.
/// @param input TextInput whose text is scanned.
/// @param char_pos Requested codepoint position, clamped to the text.
/// @param[out] out_line_index Optional destination for the zero-based line index.
/// @param[out] out_start_byte Optional destination for the line's first byte offset.
/// @param[out] out_end_byte Optional destination for its exclusive final byte offset.
/// @param[out] out_start_char Optional destination for its first codepoint index.
static void textinput_get_line_for_char_pos(const vg_textinput_t *input,
                                            size_t char_pos,
                                            size_t *out_line_index,
                                            size_t *out_start_byte,
                                            size_t *out_end_byte,
                                            size_t *out_start_char) {
    size_t clamped = textinput_clamp_char_pos(input, char_pos);
    size_t start_byte = 0;
    size_t start_char = 0;
    size_t line = 0;

    while (1) {
        size_t end_byte = start_byte;
        while (end_byte < input->text_len && input->text[end_byte] != '\n')
            end_byte++;

        size_t line_chars =
            textinput_codepoint_count_in_prefix(input->text + start_byte, end_byte - start_byte);
        if (clamped <= start_char + line_chars || end_byte == input->text_len) {
            if (out_line_index)
                *out_line_index = line;
            if (out_start_byte)
                *out_start_byte = start_byte;
            if (out_end_byte)
                *out_end_byte = end_byte;
            if (out_start_char)
                *out_start_char = start_char;
            return;
        }

        start_byte = end_byte + 1;
        start_char += line_chars + 1;
        line++;
    }
}

/// @brief Returns the codepoint index of the character nearest to @p local_x within @p line_index.
/// @param input TextInput supplying text and font metrics.
/// @param line_index Zero-based logical line to hit-test.
/// @param local_x Horizontal coordinate within the text viewport.
/// @return Grapheme-safe codepoint index nearest the coordinate.
static size_t textinput_hit_test_line_x(const vg_textinput_t *input,
                                        size_t line_index,
                                        float local_x) {
    size_t start_byte = 0;
    size_t end_byte = 0;
    size_t start_char = 0;
    textinput_get_line_at_index(input, line_index, &start_byte, &end_byte, &start_char);

    char line_stack[TEXTINPUT_STACK_TEXT_CAPACITY];
    char *line_heap = NULL;
    const char *line_text = textinput_range_to_buffer(
        input->text, start_byte, end_byte, line_stack, sizeof(line_stack), &line_heap);
    size_t line_chars =
        textinput_codepoint_count_in_prefix(input->text + start_byte, end_byte - start_byte);
    int hit = vg_font_hit_test(input->font, input->font_size, line_text, local_x);
    free(line_heap);
    if (hit < 0)
        hit = (int)line_chars;
    if ((size_t)hit > line_chars)
        hit = (int)line_chars;
    return textinput_snap_char_pos(input, start_char + (size_t)hit);
}

/// @brief Converts a local (x,y) click point to a codepoint index across all lines using
/// line_height.
/// @param input Multiline TextInput to hit-test.
/// @param local_x Horizontal coordinate within the text viewport.
/// @param local_y Vertical coordinate within the text viewport.
/// @return Grapheme-safe codepoint index nearest the point.
static size_t textinput_hit_test_multiline(const vg_textinput_t *input,
                                           float local_x,
                                           float local_y) {
    if (!input || !input->font)
        return textinput_char_count(input);

    float line_h = textinput_line_height(input);
    size_t line_count = textinput_line_count(input);
    size_t line_index = 0;
    if (line_h > 0.0f && local_y > 0.0f)
        line_index = (size_t)(local_y / line_h);
    if (line_index >= line_count)
        line_index = line_count - 1;

    return textinput_hit_test_line_x(input, line_index, local_x);
}

/// @brief Returns the pixel X offset of the cursor at @p cursor_pos within its containing line.
/// @param input TextInput supplying text and font metrics.
/// @param cursor_pos Codepoint position to measure.
/// @return Horizontal pixel offset from the containing line's start.
static float textinput_multiline_cursor_x(const vg_textinput_t *input, size_t cursor_pos) {
    if (!input || !input->font)
        return 0.0f;

    size_t line_start_byte = 0;
    size_t line_end_byte = 0;
    size_t line_start_char = 0;
    textinput_get_line_for_char_pos(
        input, cursor_pos, NULL, &line_start_byte, &line_end_byte, &line_start_char);

    char line_stack[TEXTINPUT_STACK_TEXT_CAPACITY];
    char *line_heap = NULL;
    const char *line_text = textinput_range_to_buffer(
        input->text, line_start_byte, line_end_byte, line_stack, sizeof(line_stack), &line_heap);
    size_t column = cursor_pos >= line_start_char ? (cursor_pos - line_start_char) : 0;
    float cursor_x = vg_font_get_cursor_x(input->font, input->font_size, line_text, (int)column);
    free(line_heap);
    return cursor_x;
}

/// @brief Moves @p cursor_pos up (direction < 0) or down (direction > 0) one line, preserving the
/// column offset.
/// @param input Multiline TextInput whose logical lines are navigated.
/// @param cursor_pos Current codepoint position.
/// @param direction Negative to move up or positive to move down.
/// @return Grapheme-safe codepoint position on the adjacent or boundary line.
static size_t textinput_move_vertical_cursor(const vg_textinput_t *input,
                                             size_t cursor_pos,
                                             int direction) {
    size_t line_index = 0;
    size_t line_start_byte = 0;
    size_t line_end_byte = 0;
    size_t line_start_char = 0;
    textinput_get_line_for_char_pos(
        input, cursor_pos, &line_index, &line_start_byte, &line_end_byte, &line_start_char);
    size_t column = cursor_pos >= line_start_char ? (cursor_pos - line_start_char) : 0;
    size_t line_count = textinput_line_count(input);
    size_t target_line = line_index;

    if (direction < 0) {
        if (line_index == 0)
            return 0;
        target_line = line_index - 1;
    } else {
        if (line_index + 1 >= line_count)
            return textinput_char_count(input);
        target_line = line_index + 1;
    }

    size_t target_start_byte = 0;
    size_t target_end_byte = 0;
    size_t target_start_char = 0;
    textinput_get_line_at_index(
        input, target_line, &target_start_byte, &target_end_byte, &target_start_char);
    size_t target_chars = textinput_codepoint_count_in_prefix(input->text + target_start_byte,
                                                              target_end_byte - target_start_byte);
    if (column > target_chars)
        column = target_chars;
    return textinput_snap_char_pos(input, target_start_char + column);
}

/// @brief Returns the codepoint index of the start (@p to_end=false) or end (@p to_end=true) of the
/// current line.
/// @param input Multiline TextInput whose line is queried.
/// @param cursor_pos Codepoint position identifying the current line.
/// @param to_end True for the exclusive line end, or false for the line start.
/// @return Requested codepoint boundary.
static size_t textinput_line_boundary(const vg_textinput_t *input, size_t cursor_pos, bool to_end) {
    size_t line_start_byte = 0;
    size_t line_end_byte = 0;
    size_t line_start_char = 0;
    textinput_get_line_for_char_pos(
        input, cursor_pos, NULL, &line_start_byte, &line_end_byte, &line_start_char);
    size_t line_chars = textinput_codepoint_count_in_prefix(input->text + line_start_byte,
                                                            line_end_byte - line_start_byte);
    return to_end ? (line_start_char + line_chars) : line_start_char;
}

/// @brief Adjusts scroll_x and scroll_y so the cursor is within the visible text viewport.
/// @param input TextInput whose scroll offsets may be updated.
static void textinput_ensure_cursor_visible(vg_textinput_t *input) {
    if (!input || !input->font)
        return;

    float padding = vg_theme_get_current()->input.padding_h;
    if (input->multiline) {
        const float padding_y = 6.0f;
        float viewport_w = input->base.width - padding * 2.0f;
        float viewport_h = input->base.height - padding_y * 2.0f;
        if (viewport_w <= 0.0f || viewport_h <= 0.0f) {
            input->scroll_x = 0.0f;
            input->scroll_y = 0.0f;
            return;
        }

        size_t line_index = 0;
        textinput_get_line_for_char_pos(input, input->cursor_pos, &line_index, NULL, NULL, NULL);
        float line_h = textinput_line_height(input);
        float cursor_x = textinput_multiline_cursor_x(input, input->cursor_pos);
        float cursor_y = (float)line_index * line_h;

        if (cursor_x < input->scroll_x) {
            input->scroll_x = cursor_x;
        } else if (cursor_x > input->scroll_x + viewport_w) {
            input->scroll_x = cursor_x - viewport_w + 2.0f;
        }

        if (cursor_y < input->scroll_y) {
            input->scroll_y = cursor_y;
        } else if (cursor_y + line_h > input->scroll_y + viewport_h) {
            input->scroll_y = cursor_y + line_h - viewport_h;
        }

        float max_line_width = 0.0f;
        size_t line_count = textinput_line_count(input);
        for (size_t line = 0; line < line_count; line++) {
            size_t start_byte = 0, end_byte = 0;
            textinput_get_line_at_index(input, line, &start_byte, &end_byte, NULL);
            char line_stack[TEXTINPUT_STACK_TEXT_CAPACITY];
            char *line_heap = NULL;
            const char *line_text = textinput_range_to_buffer(
                input->text, start_byte, end_byte, line_stack, sizeof(line_stack), &line_heap);
            vg_text_metrics_t metrics = {0};
            vg_font_measure_text(input->font, input->font_size, line_text, &metrics);
            if (metrics.width > max_line_width)
                max_line_width = metrics.width;
            free(line_heap);
        }
        float max_scroll_x = max_line_width - viewport_w;
        float max_scroll_y = (float)line_count * line_h - viewport_h;
        if (max_scroll_x < 0.0f)
            max_scroll_x = 0.0f;
        if (max_scroll_y < 0.0f)
            max_scroll_y = 0.0f;
        if (input->scroll_x < 0.0f)
            input->scroll_x = 0.0f;
        if (input->scroll_y < 0.0f)
            input->scroll_y = 0.0f;
        if (input->scroll_x > max_scroll_x)
            input->scroll_x = max_scroll_x;
        if (input->scroll_y > max_scroll_y)
            input->scroll_y = max_scroll_y;
        return;
    }

    float viewport = input->base.width - padding * 2.0f;
    if (viewport <= 0.0f) {
        input->scroll_x = 0.0f;
        return;
    }

    float cursor_x =
        vg_font_get_cursor_x(input->font, input->font_size, input->text, (int)input->cursor_pos);
    if (cursor_x < input->scroll_x) {
        input->scroll_x = cursor_x;
    } else if (cursor_x > input->scroll_x + viewport) {
        input->scroll_x = cursor_x - viewport;
    }

    float text_width = 0.0f;
    if (input->text_len > 0) {
        vg_text_metrics_t metrics;
        vg_font_measure_text(input->font, input->font_size, input->text, &metrics);
        text_width = metrics.width;
    }
    float max_scroll = text_width - viewport;
    if (max_scroll < 0.0f)
        max_scroll = 0.0f;
    if (input->scroll_x < 0.0f)
        input->scroll_x = 0.0f;
    if (input->scroll_x > max_scroll)
        input->scroll_x = max_scroll;
}

//=============================================================================
// TextInput Implementation
//=============================================================================

/// @brief Create a text input widget.
///
/// @param parent Widget to attach to as a child (may be NULL).
/// @return Newly allocated vg_textinput_t, or NULL on allocation failure.
vg_textinput_t *vg_textinput_create(vg_widget_t *parent) {
    vg_textinput_t *input = calloc(1, sizeof(vg_textinput_t));
    if (!input)
        return NULL;

    // Initialize base widget
    vg_widget_init(&input->base, VG_WIDGET_TEXTINPUT, &g_textinput_vtable);

    // Get theme
    vg_theme_t *theme = vg_theme_get_current();

    // Allocate initial text buffer
    input->text = malloc(TEXTINPUT_INITIAL_CAPACITY);
    if (!input->text) {
        vg_widget_destroy(&input->base);
        return NULL;
    }
    input->text[0] = '\0';
    input->text_len = 0;
    input->text_capacity = TEXTINPUT_INITIAL_CAPACITY;
    textinput_refresh_text_metrics(input);

    // Initialize text input fields
    input->cursor_pos = 0;
    input->selection_start = 0;
    input->selection_end = 0;
    input->placeholder = NULL;
    input->font = theme->typography.font_regular;
    input->font_size = theme->typography.size_normal;
    input->max_length = 0;
    input->password_mode = false;
    input->read_only = false;
    input->multiline = false;
    input->composition_text = NULL;
    input->composition_text_len = 0;
    input->composing = false;

    // Appearance
    input->text_color = theme->colors.fg_primary;
    input->placeholder_color = theme->colors.fg_placeholder;
    input->selection_color = theme->colors.bg_selected;
    input->cursor_color = theme->colors.fg_primary;
    input->bg_color = theme->colors.bg_primary;
    input->border_color = theme->colors.border_primary;

    // Scrolling
    input->scroll_x = 0;
    input->scroll_y = 0;

    // Callbacks
    input->on_change = NULL;
    input->on_change_data = NULL;

    // Internal state
    input->cursor_blink_time = 0;
    input->cursor_visible = true;

    // Initialize undo history with the initial empty-string snapshot at position 0.
    // push_undo is called AFTER each edit, so undo_stack[0] is the "before any typing"
    // baseline that Ctrl+Z can restore to.
    input->undo_stack[0] = vg_strdup("");
    input->undo_cursors[0] = 0;
    input->undo_count = input->undo_stack[0] ? 1 : 0;
    input->undo_pos = 0;

    // Set minimum size
    input->base.constraints.min_height = theme->input.height;
    input->base.constraints.min_width = 100.0f;

    // Add to parent
    if (parent) {
        vg_widget_add_child(parent, &input->base);
    }

    return input;
}

/// @brief VTable destroy: frees the text buffer, placeholder string, and all undo ring-buffer
/// snapshots.
/// @details Also disables platform text input and releases active composition storage.
/// @param widget Text-input widget base being destroyed.
static void textinput_destroy(vg_widget_t *widget) {
    vg_textinput_t *input = (vg_textinput_t *)widget;

    if (input->platform_window)
        (void)vgfx_set_text_input_enabled((vgfx_window_t)input->platform_window, 0);

    if (input->text) {
        free(input->text);
        input->text = NULL;
    }
    textinput_clear_composition(input);
    if (input->placeholder) {
        free(input->placeholder);
        input->placeholder = NULL;
    }

    // Free undo ring-buffer snapshots
    for (int i = 0; i < TEXTINPUT_UNDO_CAPACITY; i++) {
        free(input->undo_stack[i]);
        input->undo_stack[i] = NULL;
    }
}

/// @brief VTable measure: sizes to theme height (single-line) or line_height×lines+12 (multiline).
/// @param widget Text-input widget base to measure.
/// @param available_width Width offered by the parent.
/// @param available_height Height offered by the parent; constraints and content
///                         determine the result.
static void textinput_measure(vg_widget_t *widget, float available_width, float available_height) {
    vg_textinput_t *input = (vg_textinput_t *)widget;
    vg_theme_t *theme = vg_theme_get_current();
    (void)available_height;

    // Default height from theme
    float height = theme->input.height;

    // Single-line inputs report a bounded natural width: parent rows sum
    // intrinsic child widths (HBox/Flex measure), so claiming the entire
    // available width here inflates every "label + input + button" row beyond
    // its own lane and ratchets scrollable panes wider than they are. The
    // final width still comes from flex/stretch at arrange. Multiline inputs
    // remain fill-available content areas.
    float width;
    if (input->multiline) {
        width = available_width > 0 ? available_width : widget->constraints.min_width;
    } else {
        width = 160.0f;
        if (available_width > 0 && width > available_width)
            width = available_width;
        if (widget->constraints.min_width > 0 && width < widget->constraints.min_width)
            width = widget->constraints.min_width;
    }

    if (widget->constraints.preferred_width > 0) {
        width = widget->constraints.preferred_width;
    }

    // Apply multiline height
    if (input->multiline && input->font) {
        float line_h = textinput_line_height(input);
        size_t line_count = textinput_line_count(input);
        if (line_count < 3)
            line_count = 3;
        height = line_h * (float)line_count + 12.0f;
    }

    widget->measured_width = width;
    widget->measured_height = height;

    vg_widget_apply_constraints(widget);
}

/// @brief Build committed prefix + IME preedit + committed suffix for painting.
/// @details The returned text never mutates the committed buffer. Output
///          codepoint offsets describe the preedit span and its selected caret
///          endpoint in the synthesized string, allowing paint to underline
///          preedit and place the cursor without exposing it to undo/history.
/// @param input Text input with active composition state.
/// @param preedit_start Receives synthesized preedit start in codepoints.
/// @param preedit_end Receives synthesized preedit end in codepoints.
/// @param caret Receives synthesized IME caret/selection endpoint in codepoints.
/// @return Owned NUL-terminated display text, or NULL on invalid state/OOM.
static char *textinput_build_composition_display(const vg_textinput_t *input,
                                                 size_t *preedit_start,
                                                 size_t *preedit_end,
                                                 size_t *caret) {
    if (preedit_start)
        *preedit_start = 0;
    if (preedit_end)
        *preedit_end = 0;
    if (caret)
        *caret = 0;
    if (!input || !input->composing || !input->text)
        return NULL;

    size_t start = textinput_snap_char_pos(input, input->composition_start);
    size_t end = textinput_snap_char_pos_forward(input, input->composition_end);
    size_t start_byte = textinput_byte_offset(input, start);
    size_t end_byte = textinput_byte_offset(input, end);
    const char *preedit = input->composition_text ? input->composition_text : "";
    size_t preedit_len = input->composition_text ? input->composition_text_len : 0;
    size_t suffix_len = input->text_len - end_byte;
    if (start_byte > SIZE_MAX - preedit_len ||
        start_byte + preedit_len > SIZE_MAX - suffix_len - 1u)
        return NULL;

    size_t display_len = start_byte + preedit_len + suffix_len;
    char *display = (char *)malloc(display_len + 1u);
    if (!display)
        return NULL;
    memcpy(display, input->text, start_byte);
    memcpy(display + start_byte, preedit, preedit_len);
    memcpy(display + start_byte + preedit_len, input->text + end_byte, suffix_len);
    display[display_len] = '\0';

    size_t preedit_chars = textinput_codepoint_count_in_prefix(preedit, preedit_len);
    size_t preedit_caret_grapheme = input->composition_sel_start;
    if (input->composition_sel_length > SIZE_MAX - preedit_caret_grapheme)
        preedit_caret_grapheme = SIZE_MAX;
    else
        preedit_caret_grapheme += input->composition_sel_length;
    size_t preedit_caret_chars =
        vg_grapheme_codepoint_offset(preedit, preedit_len, preedit_caret_grapheme);
    if (preedit_start)
        *preedit_start = start;
    if (preedit_end)
        *preedit_end = start + preedit_chars;
    if (caret)
        *caret = start + preedit_caret_chars;
    return display;
}

/// @brief VTable paint: draws background, border, placeholder/text with selection highlight, and
/// blinking cursor.
/// @param widget Arranged TextInput base widget to render.
/// @param canvas Backend canvas used for text and primitive drawing.
static void textinput_paint(vg_widget_t *widget, void *canvas) {
    vg_textinput_t *input = (vg_textinput_t *)widget;
    vg_theme_t *theme = vg_theme_get_current();
    vgfx_window_t win = (vgfx_window_t)canvas;

    // Determine colors based on state
    uint32_t text_color = input->text_color;
    uint32_t bg_color = input->bg_color;
    uint32_t border_color = input->border_color;

    if (widget->state & VG_STATE_DISABLED) {
        text_color = theme->colors.fg_disabled;
        bg_color = theme->colors.bg_disabled;
        border_color = theme->colors.border_secondary;
    } else {
        if (input->read_only)
            bg_color = vg_color_blend(bg_color, theme->colors.bg_secondary, 0.45f);
        if (widget->state & VG_STATE_HOVERED)
            bg_color = vg_color_blend(bg_color, theme->colors.bg_hover, 0.25f);
        if (widget->state & VG_STATE_FOCUSED) {
            bg_color = vg_color_blend(bg_color, theme->colors.bg_secondary, 0.2f);
            border_color = theme->colors.border_focus;
        } else if (widget->state & VG_STATE_HOVERED) {
            border_color = theme->colors.border_secondary;
        }
    }

    // Refined Depth: a recessed rounded field with a soft accent glow on focus
    // and an anti-aliased border, all via the shared vg_draw core.
    float ix = widget->x, iy = widget->y, iw = widget->width, ih = widget->height;
    float irad = theme->input.border_radius > 0.0f ? theme->input.border_radius : 4.0f;
    bool ti_focused = (widget->state & VG_STATE_FOCUSED) != 0;
    bool ti_disabled = (widget->state & VG_STATE_DISABLED) != 0;

    if (ti_focused && !ti_disabled) {
        vg_draw_round_rect_shadow(win,
                                  ix,
                                  iy,
                                  iw,
                                  ih,
                                  irad,
                                  theme->focus.glow_width * 2.5f,
                                  0,
                                  0,
                                  theme->focus.glow_alpha,
                                  theme->focus.glow_color);
    }
    vg_draw_round_rect_fill(win, ix, iy, iw, ih, irad, bg_color);
    if (iw > 2.0f && !ti_disabled)
        vg_draw_inner_highlight_top(
            win, ix, iy + 1.0f, iw, irad, vg_color_lighten(bg_color, 0.06f));
    float ti_bw = theme->input.border_width > 0.0f ? theme->input.border_width : 1.0f;
    vg_draw_round_rect_stroke(win, ix, iy, iw, ih, irad, ti_bw, border_color);

    // Calculate text area
    float padding = theme->input.padding_h;
    float text_x = widget->x + padding - input->scroll_x;
    float text_y = widget->y;
    int32_t clip_x = (int32_t)(widget->x + 1.0f);
    int32_t clip_y = (int32_t)(widget->y + 1.0f);
    int32_t clip_w = (int32_t)(widget->width - 2.0f);
    int32_t clip_h = (int32_t)(widget->height - 2.0f);

    // Draw text or placeholder
    if (!input->font)
        return;

    vg_font_metrics_t font_metrics;
    vg_font_get_metrics(input->font, input->font_size, &font_metrics);
    text_y += (widget->height + (float)font_metrics.ascent + (float)font_metrics.descent) / 2.0f;

    input->platform_window = win;
    if (ti_focused && !ti_disabled && !input->read_only) {
        size_t cursor_byte = textinput_byte_offset(input, input->cursor_pos);
        size_t anchor_char = input->cursor_pos == input->selection_start
                                 ? input->selection_end
                                 : input->selection_start;
        size_t anchor_byte = textinput_byte_offset(input, anchor_char);
        float caret_x = text_x +
                        vg_font_get_cursor_x(
                            input->font, input->font_size, input->text, (int)input->cursor_pos);
        float caret_y = widget->y + 2.0f;
        float caret_height = widget->height - 4.0f;
        if (input->multiline) {
            size_t line = 0, line_start_byte = 0, line_end_byte = 0, line_start_char = 0;
            textinput_get_line_for_char_pos(input,
                                            input->cursor_pos,
                                            &line,
                                            &line_start_byte,
                                            &line_end_byte,
                                            &line_start_char);
            (void)line_end_byte;
            const char *line_text = input->text + line_start_byte;
            size_t column = input->cursor_pos - line_start_char;
            caret_x = widget->x + theme->input.padding_h - input->scroll_x +
                      vg_font_get_cursor_x(
                          input->font, input->font_size, line_text, (int)column);
            caret_y = widget->y + 6.0f - input->scroll_y +
                      (float)line * textinput_line_height(input);
            caret_height = textinput_line_height(input);
        }
        vgfx_text_input_state_t native_state = {
            .surrounding_text = input->text,
            .cursor_byte = cursor_byte > INT32_MAX ? INT32_MAX : (int32_t)cursor_byte,
            .anchor_byte = anchor_byte > INT32_MAX ? INT32_MAX : (int32_t)anchor_byte,
            .cursor_x = (int32_t)caret_x,
            .cursor_y = (int32_t)caret_y,
            .cursor_width = 1,
            .cursor_height = caret_height > 0.0f ? (int32_t)caret_height : 0,
            .purpose = input->password_mode ? VGFX_TEXT_INPUT_PASSWORD : VGFX_TEXT_INPUT_NORMAL};
        (void)vgfx_set_text_input_enabled(win, 1);
        (void)vgfx_set_text_input_state(win, &native_state);
    }

    size_t visual_preedit_start = 0;
    size_t visual_preedit_end = 0;
    size_t visual_caret = input->cursor_pos;
    char *composition_display = textinput_build_composition_display(
        input, &visual_preedit_start, &visual_preedit_end, &visual_caret);
    vg_textinput_t visual_input;
    const vg_textinput_t *content = input;
    if (composition_display) {
        visual_input = *input;
        visual_input.text = composition_display;
        visual_input.text_len = strlen(composition_display);
        visual_input.cursor_pos = visual_caret;
        visual_input.selection_start = visual_caret;
        visual_input.selection_end = visual_caret;
        textinput_refresh_text_metrics(&visual_input);
        content = &visual_input;
    }

    const char *display_text = content->text;
    uint32_t display_color = text_color;

    if (!input->composing && content->text_len == 0 && input->placeholder) {
        display_text = input->placeholder;
        display_color = input->placeholder_color;
    }

    if (clip_w > 0 && clip_h > 0)
        vgfx_set_clip(win, clip_x, clip_y, clip_w, clip_h);

    if (input->multiline) {
        const float padding_y = 6.0f;
        float line_h = textinput_line_height(input);
        float base_x = widget->x + padding - input->scroll_x;
        float base_y = widget->y + padding_y - input->scroll_y;
        vg_font_metrics_t metrics = {0};
        vg_font_get_metrics(input->font, input->font_size, &metrics);

        if (!input->composing && content->text_len == 0 && input->placeholder) {
            vg_font_draw_text(canvas,
                              input->font,
                              input->font_size,
                              base_x,
                              base_y + (float)metrics.ascent,
                              input->placeholder,
                              input->placeholder_color);
            if ((widget->state & VG_STATE_FOCUSED) && input->cursor_visible && !input->read_only) {
                int32_t cursor_x = (int32_t)base_x;
                int32_t cursor_y0 = (int32_t)(base_y + 2.0f);
                int32_t cursor_y1 = (int32_t)(base_y + line_h - 2.0f);
                vgfx_line(win, cursor_x, cursor_y0, cursor_x, cursor_y1, input->cursor_color);
            }
            if (clip_w > 0 && clip_h > 0)
                vgfx_clear_clip(win);
            free(composition_display);
            return;
        }

        size_t sel_start = content->selection_start < content->selection_end
                               ? content->selection_start
                               : content->selection_end;
        size_t sel_end = content->selection_start < content->selection_end
                             ? content->selection_end
                             : content->selection_start;
        size_t line_count = textinput_line_count(content);
        size_t current_line = 0;
        size_t current_line_start_char = 0;
        textinput_get_line_for_char_pos(
            content, content->cursor_pos, &current_line, NULL, NULL, &current_line_start_char);

        for (size_t line = 0; line < line_count; line++) {
            size_t line_start_byte = 0, line_end_byte = 0, line_start_char = 0;
            textinput_get_line_at_index(
                content, line, &line_start_byte, &line_end_byte, &line_start_char);
            size_t line_char_len = textinput_codepoint_count_in_prefix(
                content->text + line_start_byte, line_end_byte - line_start_byte);
            float line_y = base_y + (float)line * line_h;
            if (line_y + line_h < widget->y || line_y > widget->y + widget->height)
                continue;

            char line_stack[TEXTINPUT_STACK_TEXT_CAPACITY];
            char *line_heap = NULL;
            const char *line_text = textinput_range_to_buffer(content->text,
                                                              line_start_byte,
                                                              line_end_byte,
                                                              line_stack,
                                                              sizeof(line_stack),
                                                              &line_heap);

            if ((widget->state & VG_STATE_FOCUSED) && !input->composing && sel_start != sel_end) {
                size_t draw_start = sel_start > line_start_char ? sel_start : line_start_char;
                size_t draw_end = sel_end < (line_start_char + line_char_len)
                                      ? sel_end
                                      : (line_start_char + line_char_len);
                if (draw_start < draw_end) {
                    size_t start_col = draw_start - line_start_char;
                    size_t end_col = draw_end - line_start_char;
                    float sel_x0 =
                        base_x + vg_font_get_cursor_x(
                                     input->font, input->font_size, line_text, (int)start_col);
                    float sel_x1 =
                        base_x + vg_font_get_cursor_x(
                                     input->font, input->font_size, line_text, (int)end_col);
                    vgfx_fill_rect(win,
                                   (int32_t)sel_x0,
                                   (int32_t)line_y,
                                   (int32_t)(sel_x1 - sel_x0),
                                   (int32_t)line_h,
                                   input->selection_color);
                }
            }

            const char *draw_text = line_text;
            char mask_stack[TEXTINPUT_STACK_TEXT_CAPACITY];
            char *mask_heap = NULL;
            if (input->password_mode && line_char_len > 0) {
                size_t line_graphemes = vg_grapheme_count(content->text + line_start_byte,
                                                          line_end_byte - line_start_byte);
                draw_text = textinput_mask_to_buffer(
                    line_graphemes, mask_stack, sizeof(mask_stack), &mask_heap);
            }

            if (draw_text && draw_text[0]) {
                vg_font_draw_text(canvas,
                                  input->font,
                                  input->font_size,
                                  base_x,
                                  line_y + (float)metrics.ascent,
                                  draw_text,
                                  text_color);
            }

            if (input->composing && visual_preedit_start < visual_preedit_end) {
                size_t line_end_char = line_start_char + line_char_len;
                size_t underline_start =
                    visual_preedit_start > line_start_char ? visual_preedit_start : line_start_char;
                size_t underline_end =
                    visual_preedit_end < line_end_char ? visual_preedit_end : line_end_char;
                if (underline_start < underline_end) {
                    size_t start_col = underline_start - line_start_char;
                    size_t end_col = underline_end - line_start_char;
                    float underline_x0 =
                        base_x + vg_font_get_cursor_x(
                                     input->font, input->font_size, line_text, (int)start_col);
                    float underline_x1 =
                        base_x + vg_font_get_cursor_x(
                                     input->font, input->font_size, line_text, (int)end_col);
                    vgfx_fill_rect(win,
                                   (int32_t)underline_x0,
                                   (int32_t)(line_y + line_h - 2.0f),
                                   (int32_t)(underline_x1 - underline_x0),
                                   2,
                                   input->cursor_color);
                }
            }

            bool cursor_on_line = (line == current_line);
            if ((widget->state & VG_STATE_FOCUSED) && input->cursor_visible && !input->read_only &&
                cursor_on_line) {
                size_t col = content->cursor_pos >= current_line_start_char
                                 ? (content->cursor_pos - current_line_start_char)
                                 : 0;
                float cursor_x = base_x + vg_font_get_cursor_x(
                                              input->font, input->font_size, line_text, (int)col);
                vgfx_line(win,
                          (int32_t)cursor_x,
                          (int32_t)(line_y + 2.0f),
                          (int32_t)cursor_x,
                          (int32_t)(line_y + line_h - 2.0f),
                          input->cursor_color);
            }

            free(mask_heap);
            free(line_heap);
        }

        if (clip_w > 0 && clip_h > 0)
            vgfx_clear_clip(win);
        free(composition_display);
        return;
    }

    char mask_stack[256];
    char *mask_heap = NULL;
    const char *render_text = display_text;
    bool masked_content = input->password_mode && content->text_len > 0;
    if (masked_content) {
        size_t grapheme_count = vg_grapheme_count(content->text, content->text_len);
        render_text =
            textinput_mask_to_buffer(grapheme_count, mask_stack, sizeof(mask_stack), &mask_heap);
    }

    // Draw selection highlight if focused. Composition renders its own underline instead.
    if ((widget->state & VG_STATE_FOCUSED) && !input->composing &&
        content->selection_start != content->selection_end) {
        size_t sel_start = content->selection_start < content->selection_end
                               ? content->selection_start
                               : content->selection_end;
        size_t sel_end = content->selection_start < content->selection_end
                             ? content->selection_end
                             : content->selection_start;
        size_t draw_start =
            masked_content
                ? vg_grapheme_index_from_codepoint(content->text, content->text_len, sel_start)
                : sel_start;
        size_t draw_end =
            masked_content
                ? vg_grapheme_index_from_codepoint(content->text, content->text_len, sel_end)
                : sel_end;
        float start_x =
            vg_font_get_cursor_x(input->font, input->font_size, render_text, (int)draw_start);
        float end_x =
            vg_font_get_cursor_x(input->font, input->font_size, render_text, (int)draw_end);
        float selection_padding = theme->input.padding_h;
        float sel_abs_x = widget->x + selection_padding + start_x - input->scroll_x;
        vgfx_fill_rect(win,
                       (int32_t)sel_abs_x,
                       (int32_t)widget->y,
                       (int32_t)(end_x - start_x),
                       (int32_t)widget->height,
                       input->selection_color);
    }

    vg_font_draw_text(
        canvas, input->font, input->font_size, text_x, text_y, render_text, display_color);

    if (input->composing && visual_preedit_start < visual_preedit_end) {
        size_t draw_start = masked_content ? vg_grapheme_index_from_codepoint(content->text,
                                                                              content->text_len,
                                                                              visual_preedit_start)
                                           : visual_preedit_start;
        size_t draw_end = masked_content ? vg_grapheme_index_from_codepoint(
                                               content->text, content->text_len, visual_preedit_end)
                                         : visual_preedit_end;
        float underline_x0 =
            text_x +
            vg_font_get_cursor_x(input->font, input->font_size, render_text, (int)draw_start);
        float underline_x1 =
            text_x +
            vg_font_get_cursor_x(input->font, input->font_size, render_text, (int)draw_end);
        vgfx_fill_rect(win,
                       (int32_t)underline_x0,
                       (int32_t)(widget->y + widget->height - 3.0f),
                       (int32_t)(underline_x1 - underline_x0),
                       2,
                       input->cursor_color);
    }

    // Draw cursor if focused and visible (blinking).
    if ((widget->state & VG_STATE_FOCUSED) && input->cursor_visible && !input->read_only) {
        size_t cursor_index = masked_content ? vg_grapheme_index_from_codepoint(content->text,
                                                                                content->text_len,
                                                                                content->cursor_pos)
                                             : content->cursor_pos;
        float cursor_x =
            text_x +
            vg_font_get_cursor_x(input->font, input->font_size, render_text, (int)cursor_index);
        vgfx_line(win,
                  (int32_t)cursor_x,
                  (int32_t)widget->y + 2,
                  (int32_t)cursor_x,
                  (int32_t)(widget->y + widget->height - 2),
                  input->cursor_color);
    }

    free(mask_heap);
    free(composition_display);

    if (clip_w > 0 && clip_h > 0)
        vgfx_clear_clip(win);
}

//=============================================================================
// Undo / Redo — ring buffer of text snapshots
//=============================================================================

/// @brief Return total bytes currently owned by undo snapshot strings.
/// @param input Text input whose undo stack should be measured.
/// @return Sum of strlen(snapshot)+1 for every live undo snapshot.
static size_t textinput_undo_total_bytes(const vg_textinput_t *input) {
    if (!input)
        return 0;
    size_t total = 0;
    for (int i = 0; i < input->undo_count; i++) {
        if (!input->undo_stack[i])
            continue;
        size_t len = strlen(input->undo_stack[i]) + 1u;
        if (total > SIZE_MAX - len)
            return SIZE_MAX;
        total += len;
    }
    return total;
}

/// @brief Evict the oldest undo snapshot and compact the fixed stack arrays.
/// @details The current undo cursor is shifted to keep pointing at the same
///          logical snapshot after compaction.
/// @param input TextInput whose oldest owned snapshot is freed.
static void textinput_evict_oldest_undo(vg_textinput_t *input) {
    if (!input || input->undo_count <= 0)
        return;
    free(input->undo_stack[0]);
    if (input->undo_count > 1) {
        memmove(input->undo_stack,
                input->undo_stack + 1,
                (size_t)(input->undo_count - 1) * sizeof(char *));
        memmove(input->undo_cursors,
                input->undo_cursors + 1,
                (size_t)(input->undo_count - 1) * sizeof(size_t));
    }
    input->undo_count--;
    input->undo_stack[input->undo_count] = NULL;
    input->undo_cursors[input->undo_count] = 0;
    if (input->undo_pos > 0)
        input->undo_pos--;
}

/// @brief Records the current text and cursor as a new undo snapshot; truncates any redo tail
/// first.
/// @param input TextInput whose current state is appended to history.
static void textinput_push_undo(vg_textinput_t *input) {
    if (!input)
        return;

    char *snapshot = vg_strdup(input->text ? input->text : "");
    if (!snapshot)
        return;

    if (input->undo_count <= 0) {
        input->undo_stack[0] = snapshot;
        input->undo_cursors[0] = input->cursor_pos;
        input->undo_count = 1;
        input->undo_pos = 0;
        return;
    }

    // Truncate redo future: free entries above the current position
    while (input->undo_count > input->undo_pos + 1) {
        input->undo_count--;
        free(input->undo_stack[input->undo_count]);
        input->undo_stack[input->undo_count] = NULL;
    }

    // Deduplicate: skip if current text already matches the top snapshot
    if (input->undo_stack[input->undo_pos] &&
        strcmp(input->undo_stack[input->undo_pos], input->text) == 0) {
        free(snapshot);
        return;
    }

    // Advance the write position
    input->undo_pos++;

    // Ring overflow: evict the oldest entry by shifting everything down by one
    if (input->undo_pos >= TEXTINPUT_UNDO_CAPACITY) {
        free(input->undo_stack[0]);
        memmove(input->undo_stack,
                input->undo_stack + 1,
                (TEXTINPUT_UNDO_CAPACITY - 1) * sizeof(char *));
        memmove(input->undo_cursors,
                input->undo_cursors + 1,
                (TEXTINPUT_UNDO_CAPACITY - 1) * sizeof(size_t));
        input->undo_stack[TEXTINPUT_UNDO_CAPACITY - 1] = NULL;
        input->undo_pos = TEXTINPUT_UNDO_CAPACITY - 1;
        input->undo_count = TEXTINPUT_UNDO_CAPACITY;
    } else {
        input->undo_count = input->undo_pos + 1;
    }

    input->undo_stack[input->undo_pos] = snapshot;
    input->undo_cursors[input->undo_pos] = input->cursor_pos;

    while (input->undo_count > 1 && textinput_undo_total_bytes(input) > TEXTINPUT_UNDO_MAX_BYTES)
        textinput_evict_oldest_undo(input);
}

/// @brief Restore the previous committed snapshot and emit one text-change edge.
/// @copydetails vg_textinput_undo
bool vg_textinput_undo(vg_textinput_t *input) {
    if (!input || input->read_only || input->undo_pos <= 0)
        return false; // Already at the oldest snapshot
    vg_textinput_composition_cancel(input);

    int next_pos = input->undo_pos - 1;

    const char *snap = input->undo_stack[next_pos];
    if (!snap)
        return false;

    size_t len = strlen(snap);
    if (!ensure_capacity(input, len + 1))
        return false;
    input->undo_pos = next_pos;
    memcpy(input->text, snap, len + 1);
    input->text_len = len;
    textinput_refresh_text_metrics(input);

    size_t cur = input->undo_cursors[input->undo_pos];
    textinput_set_cursor_internal(input, cur);
    textinput_ensure_cursor_visible(input);
    textinput_note_change(input, true);
    input->base.needs_paint = true;
    return true;
}

/// @brief Reapply the next committed snapshot and emit one text-change edge.
/// @copydetails vg_textinput_redo
bool vg_textinput_redo(vg_textinput_t *input) {
    if (!input || input->read_only || input->undo_pos >= input->undo_count - 1)
        return false; // Already at the newest snapshot
    vg_textinput_composition_cancel(input);

    int next_pos = input->undo_pos + 1;

    const char *snap = input->undo_stack[next_pos];
    if (!snap)
        return false;

    size_t len = strlen(snap);
    if (!ensure_capacity(input, len + 1))
        return false;
    input->undo_pos = next_pos;
    memcpy(input->text, snap, len + 1);
    input->text_len = len;
    textinput_refresh_text_metrics(input);

    size_t cur = input->undo_cursors[input->undo_pos];
    textinput_set_cursor_internal(input, cur);
    textinput_ensure_cursor_visible(input);
    textinput_note_change(input, true);
    input->base.needs_paint = true;
    return true;
}

/// @brief Return whether an older committed snapshot is available.
/// @copydetails vg_textinput_can_undo
bool vg_textinput_can_undo(const vg_textinput_t *input) {
    return input && !input->read_only && input->undo_pos > 0;
}

/// @brief Return whether a newer committed snapshot is available.
/// @copydetails vg_textinput_can_redo
bool vg_textinput_can_redo(const vg_textinput_t *input) {
    return input && !input->read_only && input->undo_pos >= 0 &&
           input->undo_pos < input->undo_count - 1;
}

/// @brief VTable handle_event: dispatches mouse (click/drag/scroll) and keyboard events to editing
/// actions.
/// @brief Convert a codepoint boundary to the containing-or-next grapheme boundary.
/// @details Exact boundaries round-trip. A platform offset inside a multi-codepoint cluster rounds
///          upward so an IME replacement or preedit selection can never end by splitting that
///          cluster. Offsets beyond the decoded text clamp to its grapheme count.
/// @param text Borrowed bounded UTF-8 bytes.
/// @param text_length Number of readable bytes at @p text.
/// @param codepoint_offset Platform-provided Unicode-codepoint boundary.
/// @return Smallest grapheme boundary at or after the requested codepoint offset.
static size_t textinput_grapheme_ceil_from_codepoint(const char *text,
                                                     size_t text_length,
                                                     size_t codepoint_offset) {
    size_t index = vg_grapheme_index_from_codepoint(text, text_length, codepoint_offset);
    size_t boundary = vg_grapheme_codepoint_offset(text, text_length, index);
    size_t count = vg_grapheme_count(text, text_length);
    if (boundary < codepoint_offset && index < count)
        index++;
    return index;
}

/// @brief Resolve a platform composition replacement range into grapheme units.
/// @details A negative replacement start means the editor's current ordered selection. Explicit
///          platform offsets are clamped against committed text, with the start rounded down and
///          end rounded up to preserve complete Unicode extended grapheme clusters.
/// @param input TextInput whose committed text and selection define the range.
/// @param event Composition lifecycle event carrying codepoint replacement metadata.
/// @param out_start Receives the grapheme start boundary.
/// @param out_length Receives the non-negative grapheme count to replace.
static void textinput_composition_replacement_range(const vg_textinput_t *input,
                                                    const vg_event_t *event,
                                                    size_t *out_start,
                                                    size_t *out_length) {
    if (!input || !event || !out_start || !out_length)
        return;
    if (event->composition.replacement_start < 0) {
        size_t start = vg_textinput_get_selection_start_grapheme(input);
        size_t end = vg_textinput_get_selection_end_grapheme(input);
        *out_start = start;
        *out_length = end - start;
        return;
    }

    size_t start_codepoint = (size_t)event->composition.replacement_start;
    size_t replacement_codepoints = event->composition.replacement_length > 0
                                        ? (size_t)event->composition.replacement_length
                                        : 0;
    size_t end_codepoint = start_codepoint;
    if (replacement_codepoints <= SIZE_MAX - end_codepoint)
        end_codepoint += replacement_codepoints;
    else
        end_codepoint = SIZE_MAX;

    size_t start = vg_grapheme_index_from_codepoint(input->text, input->text_len, start_codepoint);
    size_t end =
        textinput_grapheme_ceil_from_codepoint(input->text, input->text_len, end_codepoint);
    if (end < start)
        end = start;
    *out_start = start;
    *out_length = end - start;
}

/// @brief Handle mouse, keyboard, focus-routed composition, and text events for TextInput.
/// @details Native composition payloads are converted from codepoint offsets to grapheme-safe
///          ranges here because this is the first layer that owns both committed and preedit text.
/// @param widget TextInput base widget receiving the event.
/// @param event Mutable GUI event to interpret.
/// @return True when editing, selection, focus, or composition handling consumes the event.
static bool textinput_handle_event(vg_widget_t *widget, vg_event_t *event) {
    vg_textinput_t *input = (vg_textinput_t *)widget;

    if (widget->state & VG_STATE_DISABLED) {
        return false;
    }

    switch (event->type) {
        case VG_EVENT_MOUSE_DOWN:
            // Set focus and cursor position
            if (input->font) {
                size_t old_cursor = input->cursor_pos;
                float padding = vg_theme_get_current()->input.padding_h;
                if (input->multiline) {
                    float local_x = event->mouse.x - padding + input->scroll_x;
                    float local_y = event->mouse.y - 6.0f + input->scroll_y;
                    input->cursor_pos = textinput_hit_test_multiline(input, local_x, local_y);
                } else {
                    float local_x = event->mouse.x - padding + input->scroll_x;
                    int char_index =
                        vg_font_hit_test(input->font, input->font_size, input->text, local_x);
                    if (char_index >= 0) {
                        input->cursor_pos = textinput_snap_char_pos(input, (size_t)char_index);
                    } else {
                        input->cursor_pos = textinput_char_count(input);
                    }
                }
                if ((event->modifiers & VG_MOD_SHIFT) != 0) {
                    if (input->selection_start == input->selection_end)
                        input->selection_start = old_cursor;
                    input->selection_end = input->cursor_pos;
                } else {
                    input->selection_start = input->cursor_pos;
                    input->selection_end = input->cursor_pos;
                }
                textinput_ensure_cursor_visible(input);
                textinput_reset_cursor_blink(input);
            }
            vg_widget_set_input_capture(widget);
            return true;

        case VG_EVENT_DOUBLE_CLICK:
            if (input->font) {
                float padding = vg_theme_get_current()->input.padding_h;
                size_t hit = 0;
                if (input->multiline) {
                    float local_x = event->mouse.x - padding + input->scroll_x;
                    float local_y = event->mouse.y - 6.0f + input->scroll_y;
                    hit = textinput_hit_test_multiline(input, local_x, local_y);
                } else {
                    float local_x = event->mouse.x - padding + input->scroll_x;
                    int char_index =
                        vg_font_hit_test(input->font, input->font_size, input->text, local_x);
                    hit = char_index >= 0 ? textinput_snap_char_pos(input, (size_t)char_index)
                                          : textinput_char_count(input);
                }
                textinput_select_word_at(input, hit);
            } else {
                textinput_select_word_at(input, input->cursor_pos);
            }
            return true;

        case VG_EVENT_MOUSE_MOVE:
            if (vg_widget_get_input_capture() == widget && input->font) {
                float padding = vg_theme_get_current()->input.padding_h;
                if (input->multiline) {
                    float local_x = event->mouse.x - padding + input->scroll_x;
                    float local_y = event->mouse.y - 6.0f + input->scroll_y;
                    input->cursor_pos = textinput_hit_test_multiline(input, local_x, local_y);
                } else {
                    float local_x = event->mouse.x - padding + input->scroll_x;
                    int char_index =
                        vg_font_hit_test(input->font, input->font_size, input->text, local_x);
                    if (char_index >= 0)
                        input->cursor_pos = textinput_snap_char_pos(input, (size_t)char_index);
                    else
                        input->cursor_pos = textinput_char_count(input);
                }
                input->selection_end = input->cursor_pos;
                textinput_ensure_cursor_visible(input);
                textinput_reset_cursor_blink(input);
                widget->needs_paint = true;
                return true;
            }
            return false;

        case VG_EVENT_MOUSE_WHEEL:
            if (input->multiline) {
                float padding_y = 6.0f;
                float viewport_h = input->base.height - padding_y * 2.0f;
                float max_scroll_y =
                    (float)textinput_line_count(input) * textinput_line_height(input) - viewport_h;
                input->scroll_y -= event->wheel.delta_y * textinput_line_height(input) * 2.0f;
                if (input->scroll_y < 0.0f)
                    input->scroll_y = 0.0f;
                if (max_scroll_y < 0.0f)
                    max_scroll_y = 0.0f;
                if (input->scroll_y > max_scroll_y)
                    input->scroll_y = max_scroll_y;
                widget->needs_paint = true;
                return true;
            }
            return false;

        case VG_EVENT_MOUSE_UP:
            if (vg_widget_get_input_capture() == widget) {
                vg_widget_release_input_capture();
                return true;
            }
            return false;

        case VG_EVENT_KEY_DOWN: {
            // Check for modifier key shortcuts
            uint32_t mods = event->modifiers;
            bool has_ctrl = (mods & VG_MOD_CTRL) != 0 || (mods & VG_MOD_SUPER) != 0;
            textinput_reset_cursor_blink(input);

            // Clipboard shortcuts (work in read-only mode for copy)
            if (has_ctrl) {
                switch (event->key.key) {
                    case VG_KEY_C: // Copy
                        if (input->selection_start != input->selection_end) {
                            char *selection = vg_textinput_get_selection(input);
                            if (selection) {
                                vgfx_clipboard_set_text(selection);
                                free(selection);
                            }
                        }
                        widget->needs_paint = true;
                        return true;

                    case VG_KEY_X: // Cut
                        if (!input->read_only && input->selection_start != input->selection_end) {
                            char *selection = vg_textinput_get_selection(input);
                            if (selection) {
                                vgfx_clipboard_set_text(selection);
                                free(selection);
                                vg_textinput_delete_selection_checked(input);
                            }
                        }
                        widget->needs_paint = true;
                        return true;

                    case VG_KEY_V: // Paste
                        if (!input->read_only) {
                            char *text = vgfx_clipboard_get_text();
                            if (text) {
                                vg_textinput_insert_text(input, text);
                                free(text);
                            }
                        }
                        widget->needs_paint = true;
                        return true;

                    case VG_KEY_A: // Select all
                        input->selection_start = 0;
                        input->selection_end = textinput_char_count(input);
                        input->cursor_pos = input->selection_end;
                        widget->needs_paint = true;
                        return true;

                    case VG_KEY_Z: // Undo
                        if (!input->read_only) {
                            if ((mods & VG_MOD_SHIFT) != 0)
                                vg_textinput_redo(input);
                            else
                                vg_textinput_undo(input);
                        }
                        widget->needs_paint = true;
                        return true;

                    case VG_KEY_Y: // Redo
                        if (!input->read_only)
                            vg_textinput_redo(input);
                        widget->needs_paint = true;
                        return true;

                    default:
                        break;
                }
            }

            bool has_shift = (mods & VG_MOD_SHIFT) != 0;

            if (input->read_only) {
                size_t old_cursor = input->cursor_pos;
                size_t char_count = textinput_char_count(input);
                bool handled = true;
                // Only allow navigation in read-only mode
                switch (event->key.key) {
                    case VG_KEY_LEFT:
                        if (input->cursor_pos > 0)
                            input->cursor_pos = vg_grapheme_previous_codepoint_boundary(
                                input->text, input->text_len, input->cursor_pos);
                        break;
                    case VG_KEY_RIGHT:
                        if (input->cursor_pos < char_count)
                            input->cursor_pos = vg_grapheme_next_codepoint_boundary(
                                input->text, input->text_len, input->cursor_pos);
                        break;
                    case VG_KEY_UP:
                        if (input->multiline)
                            input->cursor_pos =
                                textinput_move_vertical_cursor(input, input->cursor_pos, -1);
                        break;
                    case VG_KEY_DOWN:
                        if (input->multiline)
                            input->cursor_pos =
                                textinput_move_vertical_cursor(input, input->cursor_pos, 1);
                        break;
                    case VG_KEY_HOME:
                        input->cursor_pos =
                            input->multiline && !has_ctrl
                                ? textinput_line_boundary(input, input->cursor_pos, false)
                                : 0;
                        break;
                    case VG_KEY_END:
                        input->cursor_pos =
                            input->multiline && !has_ctrl
                                ? textinput_line_boundary(input, input->cursor_pos, true)
                                : char_count;
                        break;
                    default:
                        handled = false;
                        break;
                }
                if (!handled)
                    return false;
                if (has_shift) {
                    if (input->selection_start == input->selection_end)
                        input->selection_start = old_cursor;
                    input->selection_end = input->cursor_pos;
                } else {
                    input->selection_start = input->selection_end = input->cursor_pos;
                }
                textinput_ensure_cursor_visible(input);
                widget->needs_paint = true;
                return true;
            }

            /* Ctrl+Left/Right: jump to next word boundary */
            if (has_ctrl) {
                switch (event->key.key) {
                    case VG_KEY_LEFT: {
                        size_t pos = input->cursor_pos;
                        size_t byte_pos = textinput_byte_offset(input, pos);
                        while (byte_pos > 0 && textinput_is_word_separator(
                                                   (unsigned char)input->text[byte_pos - 1]))
                            byte_pos--;
                        while (byte_pos > 0 && !textinput_is_word_separator(
                                                   (unsigned char)input->text[byte_pos - 1]))
                            byte_pos--;
                        pos = textinput_snap_char_pos(
                            input, textinput_char_index_from_byte_offset(input->text, byte_pos));
                        if (has_shift) {
                            /* Extend / shrink selection toward cursor */
                            input->selection_end = pos;
                        } else {
                            input->selection_start = input->selection_end = pos;
                        }
                        input->cursor_pos = pos;
                        textinput_ensure_cursor_visible(input);
                        widget->needs_paint = true;
                        return true;
                    }
                    case VG_KEY_RIGHT: {
                        size_t pos = input->cursor_pos;
                        size_t byte_pos = textinput_byte_offset(input, pos);
                        while (byte_pos < input->text_len &&
                               !textinput_is_word_separator((unsigned char)input->text[byte_pos]))
                            byte_pos++;
                        while (byte_pos < input->text_len &&
                               textinput_is_word_separator((unsigned char)input->text[byte_pos]))
                            byte_pos++;
                        pos = textinput_snap_char_pos_forward(
                            input, textinput_char_index_from_byte_offset(input->text, byte_pos));
                        if (has_shift) {
                            input->selection_end = pos;
                        } else {
                            input->selection_start = input->selection_end = pos;
                        }
                        input->cursor_pos = pos;
                        textinput_ensure_cursor_visible(input);
                        widget->needs_paint = true;
                        return true;
                    }
                    default:
                        break;
                }
            }

            // Handle editing keys
            bool handled = true;
            switch (event->key.key) {
                case VG_KEY_BACKSPACE:
                    if (input->selection_start != input->selection_end) {
                        vg_textinput_delete_selection_checked(input);
                    } else if (input->cursor_pos > 0) {
                        input->selection_start = vg_grapheme_previous_codepoint_boundary(
                            input->text, input->text_len, input->cursor_pos);
                        input->selection_end = input->cursor_pos;
                        vg_textinput_delete_selection_checked(input);
                    }
                    break;

                case VG_KEY_DELETE:
                    if (input->selection_start != input->selection_end) {
                        vg_textinput_delete_selection_checked(input);
                    } else if (input->cursor_pos < textinput_char_count(input)) {
                        input->selection_start = input->cursor_pos;
                        input->selection_end = vg_grapheme_next_codepoint_boundary(
                            input->text, input->text_len, input->cursor_pos);
                        vg_textinput_delete_selection_checked(input);
                    }
                    textinput_ensure_cursor_visible(input);
                    break;

                case VG_KEY_UP:
                    if (input->multiline) {
                        size_t old = input->cursor_pos;
                        input->cursor_pos =
                            textinput_move_vertical_cursor(input, input->cursor_pos, -1);
                        if (has_shift) {
                            if (input->selection_start == input->selection_end)
                                input->selection_start = old;
                            input->selection_end = input->cursor_pos;
                        } else {
                            input->selection_start = input->selection_end = input->cursor_pos;
                        }
                        textinput_ensure_cursor_visible(input);
                    }
                    break;

                case VG_KEY_DOWN:
                    if (input->multiline) {
                        size_t old = input->cursor_pos;
                        input->cursor_pos =
                            textinput_move_vertical_cursor(input, input->cursor_pos, 1);
                        if (has_shift) {
                            if (input->selection_start == input->selection_end)
                                input->selection_start = old;
                            input->selection_end = input->cursor_pos;
                        } else {
                            input->selection_start = input->selection_end = input->cursor_pos;
                        }
                        textinput_ensure_cursor_visible(input);
                    }
                    break;

                case VG_KEY_LEFT:
                    if (has_shift) {
                        /* Extend selection: anchor is kept, end moves */
                        if (input->cursor_pos > 0)
                            input->cursor_pos = vg_grapheme_previous_codepoint_boundary(
                                input->text, input->text_len, input->cursor_pos);
                        input->selection_end = input->cursor_pos;
                    } else {
                        /* Plain Left: collapse selection or move */
                        if (input->selection_start != input->selection_end)
                            input->cursor_pos = input->selection_start < input->selection_end
                                                    ? input->selection_start
                                                    : input->selection_end;
                        else if (input->cursor_pos > 0)
                            input->cursor_pos = vg_grapheme_previous_codepoint_boundary(
                                input->text, input->text_len, input->cursor_pos);
                        input->selection_start = input->selection_end = input->cursor_pos;
                    }
                    textinput_ensure_cursor_visible(input);
                    break;

                case VG_KEY_RIGHT:
                    if (has_shift) {
                        if (input->cursor_pos < textinput_char_count(input))
                            input->cursor_pos = vg_grapheme_next_codepoint_boundary(
                                input->text, input->text_len, input->cursor_pos);
                        input->selection_end = input->cursor_pos;
                    } else {
                        if (input->selection_start != input->selection_end)
                            input->cursor_pos = input->selection_start > input->selection_end
                                                    ? input->selection_start
                                                    : input->selection_end;
                        else if (input->cursor_pos < textinput_char_count(input))
                            input->cursor_pos = vg_grapheme_next_codepoint_boundary(
                                input->text, input->text_len, input->cursor_pos);
                        input->selection_start = input->selection_end = input->cursor_pos;
                    }
                    textinput_ensure_cursor_visible(input);
                    break;

                case VG_KEY_HOME:
                    if (has_shift) {
                        input->cursor_pos =
                            input->multiline && !has_ctrl
                                ? textinput_line_boundary(input, input->cursor_pos, false)
                                : 0;
                        input->selection_end = input->cursor_pos;
                    } else {
                        input->cursor_pos =
                            input->multiline && !has_ctrl
                                ? textinput_line_boundary(input, input->cursor_pos, false)
                                : 0;
                        input->selection_start = input->selection_end = input->cursor_pos;
                    }
                    textinput_ensure_cursor_visible(input);
                    break;

                case VG_KEY_END:
                    if (has_shift) {
                        input->cursor_pos =
                            input->multiline && !has_ctrl
                                ? textinput_line_boundary(input, input->cursor_pos, true)
                                : textinput_char_count(input);
                        input->selection_end = input->cursor_pos;
                    } else {
                        input->cursor_pos =
                            input->multiline && !has_ctrl
                                ? textinput_line_boundary(input, input->cursor_pos, true)
                                : textinput_char_count(input);
                        input->selection_start = input->selection_end = input->cursor_pos;
                    }
                    textinput_ensure_cursor_visible(input);
                    break;

                case VG_KEY_ENTER:
                    if (input->multiline) {
                        vg_textinput_insert_text(input, "\n");
                    } else {
                        textinput_note_submit(input);
                    }
                    break;

                default:
                    handled = false;
                    break;
            }
            if (!handled)
                return false;
            widget->needs_paint = true;
            return true;
        }

        case VG_EVENT_KEY_CHAR:
            if (!textinput_key_char_allows_text(event))
                return false;
            if (!input->read_only) {
                textinput_reset_cursor_blink(input);
                // Insert typed character
                char utf8[5] = {0};
                // Convert codepoint to UTF-8
                uint32_t cp = event->key.codepoint;
                if (cp == '\r' || cp == '\n')
                    return true;
                if (!textinput_codepoint_is_text(cp))
                    return true;
                if (cp < 0x80) {
                    utf8[0] = (char)cp;
                } else if (cp < 0x800) {
                    utf8[0] = (char)(0xC0 | (cp >> 6));
                    utf8[1] = (char)(0x80 | (cp & 0x3F));
                } else if (cp < 0x10000) {
                    utf8[0] = (char)(0xE0 | (cp >> 12));
                    utf8[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    utf8[2] = (char)(0x80 | (cp & 0x3F));
                } else {
                    utf8[0] = (char)(0xF0 | (cp >> 18));
                    utf8[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
                    utf8[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    utf8[3] = (char)(0x80 | (cp & 0x3F));
                }
                vg_textinput_insert_text(input, utf8);
            }
            return true;

        case VG_EVENT_COMPOSITION_START: {
            size_t replacement_start = 0;
            size_t replacement_length = 0;
            textinput_composition_replacement_range(
                input, event, &replacement_start, &replacement_length);
            textinput_reset_cursor_blink(input);
            if (!input->read_only)
                vg_textinput_composition_start(input, replacement_start, replacement_length);
            return true;
        }

        case VG_EVENT_COMPOSITION_UPDATE: {
            if (input->read_only)
                return true;
            if (!input->composing) {
                size_t replacement_start = 0;
                size_t replacement_length = 0;
                textinput_composition_replacement_range(
                    input, event, &replacement_start, &replacement_length);
                if (!vg_textinput_composition_start(input, replacement_start, replacement_length))
                    return true;
            }

            size_t selection_codepoint = event->composition.selection_start > 0
                                             ? (size_t)event->composition.selection_start
                                             : 0;
            size_t selection_codepoints = event->composition.selection_length > 0
                                              ? (size_t)event->composition.selection_length
                                              : 0;
            size_t selection_end_codepoint = selection_codepoint;
            if (selection_codepoints <= SIZE_MAX - selection_end_codepoint)
                selection_end_codepoint += selection_codepoints;
            else
                selection_end_codepoint = SIZE_MAX;
            size_t selection_start = vg_grapheme_index_from_codepoint(
                event->composition.text, event->composition.text_length, selection_codepoint);
            size_t selection_end = textinput_grapheme_ceil_from_codepoint(
                event->composition.text, event->composition.text_length, selection_end_codepoint);
            if (selection_end < selection_start)
                selection_end = selection_start;
            vg_textinput_composition_update(
                input, event->composition.text, selection_start, selection_end - selection_start);
            textinput_reset_cursor_blink(input);
            return true;
        }

        case VG_EVENT_COMPOSITION_COMMIT:
            if (input->read_only)
                return true;
            if (event->composition.truncated) {
                vg_textinput_composition_cancel(input);
                return true;
            }
            if (!input->composing) {
                size_t replacement_start = 0;
                size_t replacement_length = 0;
                textinput_composition_replacement_range(
                    input, event, &replacement_start, &replacement_length);
                vg_textinput_composition_start(input, replacement_start, replacement_length);
            }
            vg_textinput_composition_commit(input, event->composition.text);
            textinput_reset_cursor_blink(input);
            return true;

        case VG_EVENT_COMPOSITION_CANCEL:
            vg_textinput_composition_cancel(input);
            textinput_reset_cursor_blink(input);
            return true;

        default:
            break;
    }

    return false;
}

/// @brief VTable can_focus: returns true when the input is both enabled and visible.
/// @param widget TextInput base widget whose focus eligibility is queried.
/// @return True when enabled and visible.
static bool textinput_can_focus(vg_widget_t *widget) {
    return widget->enabled && widget->visible;
}

/// @brief VTable on_focus: resets the cursor blink state when focus is gained.
/// @param widget TextInput base widget receiving the focus transition.
/// @param gained True when focus was acquired, or false when it was lost.
static void textinput_on_focus(vg_widget_t *widget, bool gained) {
    vg_textinput_t *input = (vg_textinput_t *)widget;

    if (gained) {
        textinput_reset_cursor_blink(input);
    } else {
        if (input->platform_window)
            (void)vgfx_set_text_input_enabled((vgfx_window_t)input->platform_window, 0);
        vg_textinput_composition_cancel(input);
    }
}

//=============================================================================
// TextInput API
//=============================================================================

/// @brief Programmatically set the input's text content.
///
/// @details Sanitises the UTF-8 input, enforces max_length if set, resets scroll
///          and undo history, and fires on_change if the content changed.
///
/// @param input The text input to update.
/// @param text  New text (sanitised copy stored internally); NULL becomes empty.
void vg_textinput_set_text(vg_textinput_t *input, const char *text) {
    if (!input)
        return;

    size_t len = 0;
    size_t chars = 0;
    char *clean = textinput_sanitize_utf8_copy(text, text ? strlen(text) : 0, &len, &chars);
    if (!clean)
        return;

    if (input->max_length > 0 && vg_grapheme_count(clean, len) > input->max_length) {
        len = vg_grapheme_byte_offset(clean, len, input->max_length);
        clean[len] = '\0';
        chars = textinput_codepoint_count_in_prefix(clean, len);
    }

    bool changed = strcmp(input->text ? input->text : "", clean) != 0;

    if (len > SIZE_MAX - 1 || !ensure_capacity(input, len + 1)) {
        free(clean);
        return;
    }

    memcpy(input->text, clean, len + 1);
    free(clean);
    textinput_clear_composition(input);
    input->text_len = len;
    textinput_refresh_text_metrics(input);
    textinput_set_cursor_internal(input, chars);
    input->scroll_x = 0.0f;
    input->scroll_y = 0.0f;
    textinput_ensure_cursor_visible(input);
    textinput_reset_undo_history(input);

    if (input->multiline)
        input->base.needs_layout = true;
    input->base.needs_paint = true;
    if (changed)
        textinput_note_change(input, true);
}

/// @brief Return a pointer to the input's current text content.
///
/// @param input The text input to query.
/// @return Internal null-terminated UTF-8 string, or NULL if input is NULL.
const char *vg_textinput_get_text(vg_textinput_t *input) {
    return input ? input->text : NULL;
}

/// @brief Set the placeholder text shown when the input is empty and unfocused.
///
/// @param input       The text input to configure.
/// @param placeholder Placeholder string (copied); NULL removes the placeholder.
void vg_textinput_set_placeholder(vg_textinput_t *input, const char *placeholder) {
    if (!input)
        return;

    char *copy = placeholder ? vg_strdup(placeholder) : NULL;
    if (placeholder && !copy)
        return;

    free(input->placeholder);
    input->placeholder = copy;
    input->base.needs_paint = true;
}

/// @brief Configure the maximum committed length in extended grapheme clusters.
/// @details Lowering the limit truncates at an exact UAX #29 boundary through
///          the normal programmatic text setter, preserving valid UTF-8 and a
///          coherent undo baseline.
/// @copydetails vg_textinput_set_max_length
void vg_textinput_set_max_length(vg_textinput_t *input, size_t max_length) {
    if (!input || input->max_length == max_length)
        return;
    input->max_length = max_length;
    vg_widget_note_revision(&input->base);
    if (max_length > 0 && vg_grapheme_count(input->text, input->text_len) > max_length)
        vg_textinput_set_text(input, input->text);
}

/// @brief Return the configured maximum committed grapheme count.
/// @copydetails vg_textinput_get_max_length
size_t vg_textinput_get_max_length(const vg_textinput_t *input) {
    return input ? input->max_length : 0;
}

/// @brief Toggle presentation-only password masking.
/// @copydetails vg_textinput_set_password
void vg_textinput_set_password(vg_textinput_t *input, bool password) {
    if (!input || input->password_mode == password)
        return;
    input->password_mode = password;
    input->base.needs_paint = true;
    vg_widget_note_revision(&input->base);
}

/// @brief Return whether presentation-only password masking is active.
/// @copydetails vg_textinput_is_password
bool vg_textinput_is_password(const vg_textinput_t *input) {
    return input && input->password_mode;
}

/// @brief Toggle read-only editing while retaining selection and copy support.
/// @copydetails vg_textinput_set_read_only
void vg_textinput_set_read_only(vg_textinput_t *input, bool read_only) {
    if (!input || input->read_only == read_only)
        return;
    if (read_only)
        vg_textinput_composition_cancel(input);
    input->read_only = read_only;
    input->base.needs_paint = true;
    vg_widget_note_revision(&input->base);
}

/// @brief Return whether committed text mutation is disabled.
/// @copydetails vg_textinput_is_read_only
bool vg_textinput_is_read_only(const vg_textinput_t *input) {
    return input && input->read_only;
}

/// @brief Toggle multiline editing and normalize existing line breaks when disabled.
/// @copydetails vg_textinput_set_multiline
void vg_textinput_set_multiline(vg_textinput_t *input, bool multiline) {
    if (!input || input->multiline == multiline)
        return;

    char *single_line = NULL;
    if (!multiline && input->text && strpbrk(input->text, "\r\n")) {
        single_line = (char *)malloc(input->text_len + 1u);
        if (!single_line)
            return;
        size_t write = 0;
        for (size_t read = 0; read < input->text_len; read++) {
            if (input->text[read] != '\r' && input->text[read] != '\n')
                single_line[write++] = input->text[read];
        }
        single_line[write] = '\0';
    }

    input->multiline = multiline;
    input->base.needs_layout = true;
    input->base.needs_paint = true;
    vg_widget_note_revision(&input->base);
    if (single_line) {
        vg_textinput_set_text(input, single_line);
        free(single_line);
    }
}

/// @brief Return whether newline editing and multiline layout are enabled.
/// @copydetails vg_textinput_is_multiline
bool vg_textinput_is_multiline(const vg_textinput_t *input) {
    return input && input->multiline;
}

/// @brief Set the change callback invoked whenever the text content changes.
///
/// @param input     The text input to configure.
/// @param callback  Function called with (widget, new_text, user_data).
/// @param user_data Opaque pointer passed to @p callback.
void vg_textinput_set_on_change(vg_textinput_t *input,
                                vg_text_change_callback_t callback,
                                void *user_data) {
    if (!input)
        return;

    input->on_change = callback;
    input->on_change_data = user_data;
}

/// @brief Set the commit callback invoked when the user presses Enter.
///
/// @param input     The text input to configure.
/// @param callback  Function called with (widget, text, user_data) on Enter.
/// @param user_data Opaque pointer passed to @p callback.
void vg_textinput_set_on_commit(vg_textinput_t *input,
                                vg_text_change_callback_t callback,
                                void *user_data) {
    if (!input)
        return;

    input->on_commit = callback;
    input->on_commit_data = user_data;
}

/// @brief Move the cursor to a codepoint position and scroll to keep it visible.
///
/// @param input The text input to update.
/// @param pos   Zero-based codepoint index (clamped to valid range).
void vg_textinput_set_cursor(vg_textinput_t *input, size_t pos) {
    if (!input)
        return;

    size_t old_cursor = input->cursor_pos;
    size_t old_start = input->selection_start;
    size_t old_end = input->selection_end;
    textinput_set_cursor_internal(input, pos);
    textinput_ensure_cursor_visible(input);
    input->base.needs_paint = true;
    if (old_cursor != input->cursor_pos || old_start != input->selection_start ||
        old_end != input->selection_end)
        vg_widget_note_revision(&input->base);
}

/// @brief Move the cursor using a public extended-grapheme index.
/// @copydetails vg_textinput_set_cursor_grapheme
void vg_textinput_set_cursor_grapheme(vg_textinput_t *input, size_t grapheme_index) {
    if (!input)
        return;
    vg_textinput_set_cursor(input, textinput_char_pos_from_grapheme(input, grapheme_index));
}

/// @brief Return the current cursor as an extended-grapheme boundary index.
/// @copydetails vg_textinput_get_cursor_grapheme
size_t vg_textinput_get_cursor_grapheme(const vg_textinput_t *input) {
    return input ? textinput_grapheme_from_char_pos(input, input->cursor_pos) : 0;
}

/// @brief Set the selection range by codepoint indices.
///
/// @param input The text input to update.
/// @param start Zero-based start codepoint index (inclusive).
/// @param end   Zero-based end codepoint index (exclusive); cursor is placed here.
void vg_textinput_select(vg_textinput_t *input, size_t start, size_t end) {
    if (!input)
        return;

    size_t old_cursor = input->cursor_pos;
    size_t old_start = input->selection_start;
    size_t old_end = input->selection_end;
    input->selection_start = textinput_snap_char_pos(input, start);
    input->selection_end = textinput_snap_char_pos(input, end);
    input->cursor_pos = input->selection_end;
    textinput_ensure_cursor_visible(input);
    input->base.needs_paint = true;
    if (old_cursor != input->cursor_pos || old_start != input->selection_start ||
        old_end != input->selection_end)
        vg_widget_note_revision(&input->base);
}

/// @brief Select a range whose endpoints are expressed in extended grapheme units.
/// @copydetails vg_textinput_select_graphemes
void vg_textinput_select_graphemes(vg_textinput_t *input, size_t start, size_t end) {
    if (!input)
        return;
    vg_textinput_select(input,
                        textinput_char_pos_from_grapheme(input, start),
                        textinput_char_pos_from_grapheme(input, end));
}

/// @brief Collapse the current selection at its cursor endpoint.
/// @copydetails vg_textinput_clear_selection
void vg_textinput_clear_selection(vg_textinput_t *input) {
    if (!input ||
        (input->selection_start == input->cursor_pos && input->selection_end == input->cursor_pos))
        return;
    input->selection_start = input->cursor_pos;
    input->selection_end = input->cursor_pos;
    input->base.needs_paint = true;
    vg_widget_note_revision(&input->base);
}

/// @brief Return the ordered inclusive selection start in grapheme units.
/// @copydetails vg_textinput_get_selection_start_grapheme
size_t vg_textinput_get_selection_start_grapheme(const vg_textinput_t *input) {
    if (!input)
        return 0;
    size_t start = input->selection_start < input->selection_end ? input->selection_start
                                                                 : input->selection_end;
    return textinput_grapheme_from_char_pos(input, start);
}

/// @brief Return the ordered exclusive selection end in grapheme units.
/// @copydetails vg_textinput_get_selection_end_grapheme
size_t vg_textinput_get_selection_end_grapheme(const vg_textinput_t *input) {
    if (!input)
        return 0;
    size_t end = input->selection_start < input->selection_end ? input->selection_end
                                                               : input->selection_start;
    return textinput_grapheme_from_char_pos(input, end);
}

/// @brief Select the entire text content.
///
/// @param input The text input to update.
void vg_textinput_select_all(vg_textinput_t *input) {
    if (!input)
        return;

    input->selection_start = 0;
    input->selection_end = textinput_char_count(input);
    input->cursor_pos = input->selection_end;
    textinput_ensure_cursor_visible(input);
    input->base.needs_paint = true;
    vg_widget_note_revision(&input->base);
}

/// @brief Insert text at the cursor position, replacing any active selection.
///
/// @details Sanitises UTF-8, enforces max_length, shifts existing text, and
///          fires on_change. No-op if the input is read-only.
///
/// @param input The text input to modify.
/// @param text  UTF-8 string to insert (sanitised before use).
void vg_textinput_insert(vg_textinput_t *input, const char *text) {
    if (!input || !text || input->read_only)
        return;

    size_t insert_len = 0;
    size_t insert_chars = 0;
    char *clean = textinput_sanitize_utf8_copy(text, strlen(text), &insert_len, &insert_chars);
    if (!clean)
        return;
    if (insert_len == 0 || insert_chars == 0) {
        free(clean);
        return;
    }

    size_t selection_start = input->selection_start < input->selection_end ? input->selection_start
                                                                           : input->selection_end;
    size_t selection_end = input->selection_start < input->selection_end ? input->selection_end
                                                                         : input->selection_start;
    selection_start = textinput_snap_char_pos(input, selection_start);
    selection_end = textinput_snap_char_pos_forward(input, selection_end);
    if (selection_start == selection_end) {
        selection_start = textinput_snap_char_pos(input, input->cursor_pos);
        selection_end = selection_start;
    }
    size_t selection_start_byte = textinput_byte_offset(input, selection_start);
    size_t selection_end_byte = textinput_byte_offset(input, selection_end);
    size_t delete_bytes =
        selection_end_byte > selection_start_byte ? selection_end_byte - selection_start_byte : 0;
    size_t selection_start_grapheme = textinput_grapheme_from_char_pos(input, selection_start);
    size_t selection_end_grapheme = textinput_grapheme_from_char_pos(input, selection_end);
    size_t delete_graphemes = selection_end_grapheme > selection_start_grapheme
                                  ? selection_end_grapheme - selection_start_grapheme
                                  : 0;

    if (input->max_length > 0) {
        size_t current_graphemes = vg_grapheme_count(input->text, input->text_len);
        current_graphemes =
            current_graphemes > delete_graphemes ? current_graphemes - delete_graphemes : 0;
        if (current_graphemes >= input->max_length) {
            free(clean);
            return;
        }
        size_t insert_graphemes = vg_grapheme_count(clean, insert_len);
        if (current_graphemes + insert_graphemes > input->max_length) {
            size_t remaining_graphemes = input->max_length - current_graphemes;
            insert_len = vg_grapheme_byte_offset(clean, insert_len, remaining_graphemes);
            if (insert_len == 0) {
                free(clean);
                return;
            }
            clean[insert_len] = '\0';
            insert_chars = textinput_codepoint_count_in_prefix(clean, insert_len);
        }
    }

    size_t base_len = input->text_len >= delete_bytes ? input->text_len - delete_bytes : 0;
    if (base_len > SIZE_MAX - 1 || insert_len > SIZE_MAX - base_len - 1) {
        free(clean);
        return;
    }
    size_t new_len = base_len + insert_len;

    if (!ensure_capacity(input, new_len + 1)) {
        free(clean);
        return;
    }

    if (delete_bytes > 0) {
        memmove(input->text + selection_start_byte,
                input->text + selection_end_byte,
                input->text_len - selection_end_byte + 1);
        input->text_len = base_len;
    }
    input->cursor_pos = selection_start;
    input->selection_start = selection_start;
    input->selection_end = selection_start;

    // Make room for new text
    memmove(input->text + selection_start_byte + insert_len,
            input->text + selection_start_byte,
            input->text_len - selection_start_byte + 1);

    // Insert text
    memcpy(input->text + selection_start_byte, clean, insert_len);
    free(clean);
    input->text_len = new_len;
    textinput_refresh_text_metrics(input);
    input->cursor_pos = textinput_snap_char_pos_forward(input, input->cursor_pos + insert_chars);
    input->selection_start = input->selection_end = input->cursor_pos;
    textinput_ensure_cursor_visible(input);
    textinput_reset_cursor_blink(input);

    if (input->multiline)
        input->base.needs_layout = true;
    input->base.needs_paint = true;

    textinput_note_change(input, true);
}

/// @brief Insert one committed edit and append exactly one undo snapshot.
/// @copydetails vg_textinput_insert_text
bool vg_textinput_insert_text(vg_textinput_t *input, const char *text) {
    if (!input || !text || input->read_only)
        return false;
    uint64_t before = input->change_revision;
    vg_textinput_insert(input, text);
    if (input->change_revision == before)
        return false;
    textinput_push_undo(input);
    return true;
}

/// @brief Deletes the selected byte range, collapses the cursor to the start, and optionally fires
/// on_change.
/// @param input TextInput whose selected text is removed.
/// @param notify True to invoke the registered change callback after deletion.
static void textinput_delete_selection_internal(vg_textinput_t *input, bool notify) {
    if (!input || input->read_only)
        return;
    if (input->selection_start == input->selection_end)
        return;

    size_t start = input->selection_start < input->selection_end ? input->selection_start
                                                                 : input->selection_end;
    size_t end = input->selection_start < input->selection_end ? input->selection_end
                                                               : input->selection_start;
    start = textinput_snap_char_pos(input, start);
    end = textinput_snap_char_pos_forward(input, end);
    size_t start_byte = textinput_byte_offset(input, start);
    size_t end_byte = textinput_byte_offset(input, end);

    memmove(input->text + start_byte, input->text + end_byte, input->text_len - end_byte + 1);

    input->text_len -= (end_byte - start_byte);
    textinput_refresh_text_metrics(input);
    input->cursor_pos = start;
    input->selection_start = start;
    input->selection_end = start;
    textinput_ensure_cursor_visible(input);
    textinput_reset_cursor_blink(input);

    if (input->multiline)
        input->base.needs_layout = true;
    input->base.needs_paint = true;

    textinput_note_change(input, notify);
}

/// @brief Delete the current selection; fires on_change.
///
/// @param input The text input to modify.
void vg_textinput_delete_selection(vg_textinput_t *input) {
    textinput_delete_selection_internal(input, true);
}

/// @brief Delete one selected edit and append exactly one undo snapshot.
/// @copydetails vg_textinput_delete_selection_checked
bool vg_textinput_delete_selection_checked(vg_textinput_t *input) {
    if (!input || input->read_only || input->selection_start == input->selection_end)
        return false;
    uint64_t before = input->change_revision;
    textinput_delete_selection_internal(input, true);
    if (input->change_revision == before)
        return false;
    textinput_push_undo(input);
    return true;
}

/// @brief Return a heap-allocated copy of the currently selected text.
///
/// @param input The text input to query.
/// @return Null-terminated copy of the selection (caller must free), or NULL if
///         there is no selection or input is NULL.
char *vg_textinput_get_selection(vg_textinput_t *input) {
    if (!input)
        return NULL;
    if (input->selection_start == input->selection_end)
        return NULL;

    size_t start = input->selection_start < input->selection_end ? input->selection_start
                                                                 : input->selection_end;
    size_t end = input->selection_start < input->selection_end ? input->selection_end
                                                               : input->selection_start;
    size_t start_byte = textinput_byte_offset(input, start);
    size_t end_byte = textinput_byte_offset(input, end);

    size_t len = end_byte - start_byte;
    char *result = malloc(len + 1);
    if (!result)
        return NULL;

    memcpy(result, input->text + start_byte, len);
    result[len] = '\0';

    return result;
}

/// @brief Consume the independent committed-text change edge.
/// @copydetails vg_textinput_was_changed
bool vg_textinput_was_changed(vg_textinput_t *input) {
    if (!input || input->change_revision == input->reported_change_revision)
        return false;
    input->reported_change_revision = input->change_revision;
    return true;
}

/// @brief Consume the independent single-line submission edge.
/// @copydetails vg_textinput_was_submitted
bool vg_textinput_was_submitted(vg_textinput_t *input) {
    if (!input || input->submit_revision == input->reported_submit_revision)
        return false;
    input->reported_submit_revision = input->submit_revision;
    return true;
}

/// @brief Return the non-consuming common widget revision for this text input.
/// @copydetails vg_textinput_get_revision
uint64_t vg_textinput_get_revision(const vg_textinput_t *input) {
    return input ? vg_widget_get_revision(&input->base) : 0;
}

/// @brief Release preedit storage and reset every composition-only field.
/// @details This helper deliberately does not restore or alter committed cursor
///          state; callers choose between cancellation restoration and commit.
/// @param input Text input whose composition storage should be cleared.
static void textinput_clear_composition(vg_textinput_t *input) {
    if (!input)
        return;
    free(input->composition_text);
    input->composition_text = NULL;
    input->composition_text_len = 0;
    input->composition_start = 0;
    input->composition_end = 0;
    input->composition_sel_start = 0;
    input->composition_sel_length = 0;
    input->composition_saved_cursor = 0;
    input->composition_saved_start = 0;
    input->composition_saved_end = 0;
    input->composing = false;
}

/// @brief Begin an IME preedit session over a committed grapheme range.
/// @copydetails vg_textinput_composition_start
bool vg_textinput_composition_start(vg_textinput_t *input,
                                    size_t replacement_start,
                                    size_t replacement_length) {
    if (!input || input->read_only)
        return false;
    if (input->composing)
        vg_textinput_composition_cancel(input);

    size_t grapheme_count = vg_grapheme_count(input->text, input->text_len);
    if (replacement_start > grapheme_count)
        replacement_start = grapheme_count;
    if (replacement_length > grapheme_count - replacement_start)
        replacement_length = grapheme_count - replacement_start;

    input->composition_saved_cursor = input->cursor_pos;
    input->composition_saved_start = input->selection_start;
    input->composition_saved_end = input->selection_end;
    input->composition_start = textinput_char_pos_from_grapheme(input, replacement_start);
    input->composition_end =
        textinput_char_pos_from_grapheme(input, replacement_start + replacement_length);
    input->selection_start = input->composition_start;
    input->selection_end = input->composition_end;
    input->cursor_pos = input->composition_end;
    input->composing = true;
    textinput_reset_cursor_blink(input);
    input->base.needs_paint = true;
    return true;
}

/// @brief Replace visible IME preedit storage without mutating committed text.
/// @copydetails vg_textinput_composition_update
bool vg_textinput_composition_update(vg_textinput_t *input,
                                     const char *text,
                                     size_t selection_start,
                                     size_t selection_length) {
    if (!input || !input->composing || input->read_only)
        return false;

    size_t text_len = 0;
    char *clean = textinput_sanitize_utf8_copy(text, text ? strlen(text) : 0, &text_len, NULL);
    if (!clean)
        return false;
    size_t grapheme_count = vg_grapheme_count(clean, text_len);
    if (selection_start > grapheme_count)
        selection_start = grapheme_count;
    if (selection_length > grapheme_count - selection_start)
        selection_length = grapheme_count - selection_start;

    free(input->composition_text);
    input->composition_text = clean;
    input->composition_text_len = text_len;
    input->composition_sel_start = selection_start;
    input->composition_sel_length = selection_length;
    input->base.needs_paint = true;
    return true;
}

/// @brief Commit one IME result through the normal single-edit history path.
/// @copydetails vg_textinput_composition_commit
bool vg_textinput_composition_commit(vg_textinput_t *input, const char *text) {
    if (!input || !input->composing || input->read_only)
        return false;
    size_t replacement_start = input->composition_start;
    size_t replacement_end = input->composition_end;
    textinput_clear_composition(input);
    input->selection_start = replacement_start;
    input->selection_end = replacement_end;
    input->cursor_pos = replacement_end;

    bool changed = false;
    if (text && text[0] != '\0')
        changed = vg_textinput_insert_text(input, text);
    else
        changed = vg_textinput_delete_selection_checked(input);
    if (!changed)
        vg_textinput_clear_selection(input);
    input->base.needs_paint = true;
    return changed;
}

/// @brief Cancel preedit and restore cursor/selection saved at composition start.
/// @copydetails vg_textinput_composition_cancel
bool vg_textinput_composition_cancel(vg_textinput_t *input) {
    if (!input || !input->composing)
        return false;
    size_t saved_cursor = input->composition_saved_cursor;
    size_t saved_start = input->composition_saved_start;
    size_t saved_end = input->composition_saved_end;
    textinput_clear_composition(input);
    input->cursor_pos = textinput_snap_char_pos(input, saved_cursor);
    input->selection_start = textinput_snap_char_pos(input, saved_start);
    input->selection_end = textinput_snap_char_pos_forward(input, saved_end);
    textinput_ensure_cursor_visible(input);
    textinput_reset_cursor_blink(input);
    input->base.needs_paint = true;
    return true;
}

/// @brief Return whether this widget currently owns IME preedit state.
/// @copydetails vg_textinput_is_composing
bool vg_textinput_is_composing(const vg_textinput_t *input) {
    return input && input->composing;
}

/// @brief Return borrowed preedit UTF-8 or a stable empty string.
/// @copydetails vg_textinput_get_composition_text
const char *vg_textinput_get_composition_text(const vg_textinput_t *input) {
    return input && input->composition_text ? input->composition_text : "";
}

/// @brief Return the committed-text preedit insertion point in grapheme units.
/// @copydetails vg_textinput_get_composition_start
size_t vg_textinput_get_composition_start(const vg_textinput_t *input) {
    return input && input->composing
               ? textinput_grapheme_from_char_pos(input, input->composition_start)
               : 0;
}

/// @brief Return the visible preedit text length in extended grapheme clusters.
/// @copydetails vg_textinput_get_composition_length
size_t vg_textinput_get_composition_length(const vg_textinput_t *input) {
    return input && input->composing
               ? vg_grapheme_count(input->composition_text, input->composition_text_len)
               : 0;
}

/// @brief Override the input field's font and size.
///
/// @param input The text input to configure.
/// @param font  Font to use; NULL keeps the existing font.
/// @param size  Font size in pixels; <= 0 falls back to the theme normal size.
void vg_textinput_set_font(vg_textinput_t *input, vg_font_t *font, float size) {
    if (!input)
        return;

    if (font)
        input->font = font;
    input->font_size =
        (isfinite(size) && size > 0.0f) ? size : vg_theme_get_current()->typography.size_normal;
    textinput_ensure_cursor_visible(input);
    input->base.needs_layout = true;
    input->base.needs_paint = true;
}

/// @brief Advance the cursor blink animation; call once per frame for focused inputs.
///
/// @param input The text input to advance.
/// @param dt    Elapsed time in seconds since the last tick.
void vg_textinput_tick(vg_textinput_t *input, float dt) {
    if (!input || !(input->base.state & VG_STATE_FOCUSED))
        return;
    if (!isfinite(dt) || dt < 0.0f)
        return;
    if (dt > CURSOR_BLINK_RATE * 16.0f)
        dt = fmodf(dt, CURSOR_BLINK_RATE);

    input->cursor_blink_time += dt;
    if (input->cursor_blink_time >= CURSOR_BLINK_RATE) {
        input->cursor_blink_time -= CURSOR_BLINK_RATE;
        input->cursor_visible = !input->cursor_visible;
        input->base.needs_paint = true;
    }
}
