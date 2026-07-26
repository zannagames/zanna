//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: lib/gui/src/widgets/vg_outputpane.c
// Purpose: Output pane widget — an append-only, ANSI-aware terminal-style log
//          view with styled segments, selection, and auto-scroll behaviour.
// Key invariants:
//   - lines[] is a circular buffer capped at max_lines; when full, the oldest
//     entry is evicted by advancing line_start and selection coordinates are adjusted.
//   - Each vg_output_line_t holds a dynamic array of vg_styled_segment_t, each
//     owning its text via strdup/malloc; freed in free_output_line.
//   - ANSI escape state (in_escape, escape_buf, current_fg/bg, ansi_bold) is
//     preserved across append calls so multi-chunk writes render correctly.
//   - scroll_locked is set when the user scrolls up; auto_scroll is suppressed
//     until scroll_locked is cleared by vg_outputpane_scroll_to_bottom.
//   - sel_start/end coordinates are absolute line indices into lines[]; they are
//     decremented by outputpane_note_evicted_first_line on each eviction.
// Ownership/Lifetime:
//   - pane->lines and every segment text pointer are heap-allocated and freed in
//     outputpane_destroy / vg_outputpane_clear.
//   - The widget does not own the linked vg_font_t.
// Links: lib/gui/include/vg_ide_widgets.h,
//        lib/gui/include/vg_theme.h,
//        lib/gui/include/vg_event.h
//
//===----------------------------------------------------------------------===//
/// @file
/// @brief Implements ANSI-aware output retention, text selection, and an
///        interactive terminal cursor/escape-sequence model.
/// @details Append-only and terminal modes share a circular line buffer of
///          owned styled segments. Terminal mode additionally materializes one
///          editable row as cells, translates input to xterm byte sequences,
///          and preserves parser state across chunk boundaries.
#include "../../../graphics/include/vgfx.h"
#include "../../include/vg_event.h"
#include "../../include/vg_ide_widgets.h"
#include "../../include/vg_theme.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//=============================================================================
// Forward Declarations
//=============================================================================

static void outputpane_destroy(vg_widget_t *widget);
static void outputpane_measure(vg_widget_t *widget, float available_width, float available_height);
static void outputpane_paint(vg_widget_t *widget, void *canvas);
static bool outputpane_handle_event(vg_widget_t *widget, vg_event_t *event);
static void outputpane_set_font_widget(vg_widget_t *widget, void *font, float size);
static bool outputpane_can_focus(vg_widget_t *widget);
static void outputpane_on_focus(vg_widget_t *widget, bool gained);

//=============================================================================
// OutputPane VTable
//=============================================================================

static vg_widget_vtable_t g_outputpane_vtable = {.destroy = outputpane_destroy,
                                                 .measure = outputpane_measure,
                                                 .arrange = NULL,
                                                 .paint = outputpane_paint,
                                                 .handle_event = outputpane_handle_event,
                                                 .can_focus = outputpane_can_focus,
                                                 .on_focus = outputpane_on_focus,
                                                 .set_font = outputpane_set_font_widget};

//=============================================================================
// ANSI Color Tables
//=============================================================================

static const uint32_t g_ansi_colors[] = {
    0xFF000000, // Black
    0xFFCC0000, // Red
    0xFF00CC00, // Green
    0xFFCCCC00, // Yellow
    0xFF0000CC, // Blue
    0xFFCC00CC, // Magenta
    0xFF00CCCC, // Cyan
    0xFFCCCCCC, // White
};

static const uint32_t g_ansi_bright_colors[] = {
    0xFF666666, // Bright Black
    0xFFFF6666, // Bright Red
    0xFF66FF66, // Bright Green
    0xFFFFFF66, // Bright Yellow
    0xFF6666FF, // Bright Blue
    0xFFFF66FF, // Bright Magenta
    0xFF66FFFF, // Bright Cyan
    0xFFFFFFFF, // Bright White
};

/// @brief Map an ANSI SGR color code to a packed AARRGGBB value.
/// @param code Standard or bright ANSI foreground SGR code.
/// @return Opaque packed color, or the pane's default light foreground for an
///         unsupported code.
static uint32_t ansi_code_to_color(int code) {
    if (code >= 30 && code <= 37) {
        return g_ansi_colors[code - 30];
    } else if (code >= 90 && code <= 97) {
        return g_ansi_bright_colors[code - 90];
    }
    return 0xFFCCCCCC; // Default
}

/// @brief Pack 8-bit red, green, and blue channels into the pane's AARRGGBB format.
/// @details OutputPane stores colors as opaque ARGB values.  Extended ANSI SGR
///          true-color sequences provide separate RGB channel values, so this
///          helper centralizes the byte clamping and channel packing used by
///          both append-only output and interactive terminal mode.
/// @param r Red channel, clamped to the inclusive range [0, 255].
/// @param g Green channel, clamped to the inclusive range [0, 255].
/// @param b Blue channel, clamped to the inclusive range [0, 255].
/// @return Opaque 0xAARRGGBB color value.
static uint32_t ansi_rgb_to_color(int r, int g, int b) {
    if (r < 0)
        r = 0;
    if (r > 255)
        r = 255;
    if (g < 0)
        g = 0;
    if (g > 255)
        g = 255;
    if (b < 0)
        b = 0;
    if (b > 255)
        b = 255;
    return 0xFF000000u | ((uint32_t)r << 16u) | ((uint32_t)g << 8u) | (uint32_t)b;
}

/// @brief Convert an xterm 256-color palette index into AARRGGBB.
/// @details SGR 38;5;N and 48;5;N use the xterm indexed palette: 0-15 are
///          standard/bright ANSI colors, 16-231 form a 6x6x6 RGB cube, and
///          232-255 are a grayscale ramp. Invalid indexes fall back to the
///          default foreground color used by the existing eight-color parser.
/// @param index Palette index from an ANSI extended-color SGR sequence.
/// @return Opaque 0xAARRGGBB color value.
static uint32_t ansi_256_to_color(int index) {
    static const int cube_levels[6] = {0, 95, 135, 175, 215, 255};

    if (index >= 0 && index < 8)
        return g_ansi_colors[index];
    if (index >= 8 && index < 16)
        return g_ansi_bright_colors[index - 8];
    if (index >= 16 && index <= 231) {
        int n = index - 16;
        int r = cube_levels[n / 36];
        int g = cube_levels[(n / 6) % 6];
        int b = cube_levels[n % 6];
        return ansi_rgb_to_color(r, g, b);
    }
    if (index >= 232 && index <= 255) {
        int v = 8 + (index - 232) * 10;
        return ansi_rgb_to_color(v, v, v);
    }
    return 0xFFCCCCCC;
}

/// @brief Parse CSI/SGR integer parameters from an escape buffer.
/// @details Accepts buffers with or without the leading '[' and records a leading
///          '?' DEC-private marker for mode-setting sequences such as CSI ?1049h.
///          Empty parameters are represented as 0, matching the terminal default
///          handling used throughout the OutputPane parser.
/// @param buffer Escape parameter buffer; may include the '[' introducer.
/// @param params Destination integer array.
/// @param max_params Maximum number of parameters that can be written.
/// @param private_mode Optional output flag set when a leading '?' is present.
/// @return Number of parsed parameters written to @p params.
static int ansi_parse_csi_params(const char *buffer, int *params, int max_params, bool *private_mode) {
    int count = 0;
    const char *p = buffer;

    if (private_mode)
        *private_mode = false;
    if (!buffer || !params || max_params <= 0)
        return 0;
    if (*p == '[')
        p++;
    if (*p == '?') {
        if (private_mode)
            *private_mode = true;
        p++;
    }
    while (*p && count < max_params) {
        if (*p >= '0' && *p <= '9') {
            char *end = NULL;
            params[count++] = (int)strtol(p, &end, 10);
            p = end ? end : p + 1;
            if (*p == ';' || *p == ':')
                p++;
        } else if (*p == ';' || *p == ':') {
            params[count++] = 0;
            p++;
        } else if (*p >= 0x40 && *p <= 0x7e) {
            break;
        } else {
            p++;
        }
    }
    return count;
}

/// @brief Apply parsed ANSI SGR parameters to the pane's current style.
/// @details Handles reset, bold, normal/bright 8-color codes, default foreground
///          and background resets, and xterm extended colors (38/48 with 256-color
///          or true-color operands). The parser advances over operands consumed by
///          extended-color sequences so malformed tails do not corrupt later codes.
/// @param pane Output pane whose current style should be changed.
/// @param params Parsed SGR parameters.
/// @param count Number of parsed SGR parameters.
static void outputpane_apply_sgr_params(vg_outputpane_t *pane, const int *params, int count) {
    if (!pane)
        return;
    if (!params || count == 0) {
        pane->current_fg = pane->default_fg;
        pane->current_bg = 0;
        pane->ansi_bold = false;
        return;
    }

    for (int i = 0; i < count; i++) {
        int code = params[i];
        if (code == 0) {
            pane->current_fg = pane->default_fg;
            pane->current_bg = 0;
            pane->ansi_bold = false;
            pane->ansi_reverse = false;
        } else if (code == 1) {
            pane->ansi_bold = true;
        } else if (code == 22) {
            pane->ansi_bold = false;
        } else if (code == 7) {
            pane->ansi_reverse = true;
        } else if (code == 27) {
            pane->ansi_reverse = false;
        } else if (code >= 30 && code <= 37) {
            pane->current_fg = ansi_code_to_color(code);
        } else if (code == 39) {
            pane->current_fg = pane->default_fg;
        } else if (code >= 40 && code <= 47) {
            pane->current_bg = ansi_code_to_color(code - 10);
        } else if (code == 49) {
            pane->current_bg = 0;
        } else if (code >= 90 && code <= 97) {
            pane->current_fg = ansi_code_to_color(code);
        } else if (code >= 100 && code <= 107) {
            pane->current_bg = ansi_code_to_color(code - 10);
        } else if ((code == 38 || code == 48) && i + 1 < count) {
            uint32_t color = pane->default_fg;
            bool valid = false;
            if (params[i + 1] == 5 && i + 2 < count) {
                color = ansi_256_to_color(params[i + 2]);
                i += 2;
                valid = true;
            } else if (params[i + 1] == 2 && i + 4 < count) {
                color = ansi_rgb_to_color(params[i + 2], params[i + 3], params[i + 4]);
                i += 4;
                valid = true;
            }
            if (valid) {
                if (code == 38)
                    pane->current_fg = color;
                else
                    pane->current_bg = color;
            }
        }
    }
}

/// @brief VTable set_font trampoline — forwards to vg_outputpane_set_font.
/// @param widget Output-pane base widget whose font is updated.
/// @param font Borrowed font handle forwarded to the typed setter.
/// @param size Requested font size in pixels.
static void outputpane_set_font_widget(vg_widget_t *widget, void *font, float size) {
    if (!widget || !font)
        return;
    vg_outputpane_set_font((vg_outputpane_t *)widget, (vg_font_t *)font, size);
}

/// @brief Return the total character count across all segments of a line.
/// @details Counts UTF-8 codepoint starts rather than raw bytes so selection
///          columns cannot split a multibyte sequence.
/// @param text UTF-8 text; may be NULL.
/// @return Number of codepoint columns in the string.
static size_t outputpane_utf8_column_count(const char *text) {
    size_t count = 0;
    if (!text)
        return 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if ((*p & 0xC0u) != 0x80u)
            count++;
    }
    return count;
}

/// @brief Return the byte length of the UTF-8 sequence starting at text.
/// @details Invalid or truncated sequences are treated as one byte so selection
///          extraction always makes progress and never reads past the NUL
///          terminator.
/// @param text Pointer to a NUL-terminated UTF-8 sequence.
/// @return Number of bytes in the next codepoint-like unit.
static size_t outputpane_utf8_next_len(const char *text) {
    const unsigned char *p = (const unsigned char *)text;
    if (!p || p[0] == '\0')
        return 0;
    if (p[0] < 0x80u)
        return 1;
    size_t expected = 1;
    if ((p[0] & 0xE0u) == 0xC0u)
        expected = 2;
    else if ((p[0] & 0xF0u) == 0xE0u)
        expected = 3;
    else if ((p[0] & 0xF8u) == 0xF0u)
        expected = 4;
    for (size_t i = 1; i < expected; i++) {
        if (p[i] == '\0')
            return 1;
        if ((p[i] & 0xC0u) != 0x80u)
            return 1;
    }
    return expected;
}

/// @brief Map a UTF-8 codepoint column to a byte offset.
/// @param text UTF-8 text to inspect.
/// @param target_col Codepoint column to locate.
/// @return Byte offset into @p text at the requested column or string end.
static size_t outputpane_utf8_byte_offset_for_column(const char *text, size_t target_col) {
    size_t col = 0;
    size_t offset = 0;
    if (!text)
        return 0;
    while (text[offset] && col < target_col) {
        size_t cp_len = outputpane_utf8_next_len(text + offset);
        if (cp_len == 0)
            break;
        offset += cp_len;
        col++;
    }
    return offset;
}

/// @brief Return the total character count across all segments of a line.
/// @param line Output line whose segment lengths are summed.
/// @return Total logical character count, or zero for NULL.
static size_t outputpane_line_length(const vg_output_line_t *line) {
    size_t len = 0;
    if (!line)
        return 0;
    for (size_t i = 0; i < line->segment_count; i++) {
        if (line->segments[i].text) {
            size_t seg_cols = outputpane_utf8_column_count(line->segments[i].text);
            if (seg_cols > SIZE_MAX - len)
                return SIZE_MAX;
            len += seg_cols;
        }
    }
    return len;
}

/// @brief Measure the pixel width of the text in a line up to target_col characters.
/// @param pane Output pane supplying font and size information.
/// @param line Line whose prefix is measured.
/// @param target_col Exclusive logical character column at which to stop.
/// @return Pixel width of the available prefix.
static float outputpane_prefix_width(vg_outputpane_t *pane,
                                     const vg_output_line_t *line,
                                     uint32_t target_col) {
    float width = 0.0f;
    size_t col = 0;

    if (!pane || !pane->font || !line)
        return 0.0f;

    for (size_t i = 0; i < line->segment_count; i++) {
        const char *text = line->segments[i].text;
        size_t seg_len = 0;
        vg_text_metrics_t metrics = {0};

        if (!text)
            continue;
        seg_len = outputpane_utf8_column_count(text);
        if ((size_t)target_col <= col)
            return width;
        size_t remaining_cols = (size_t)target_col - col;
        if (seg_len <= remaining_cols) {
            vg_font_measure_text(pane->font, pane->font_size, text, &metrics);
            width += metrics.width;
            col += seg_len;
            continue;
        }

        if (target_col > col) {
            size_t partial_len =
                outputpane_utf8_byte_offset_for_column(text, (size_t)(target_col - col));
            char *partial = malloc(partial_len + 1);
            if (partial) {
                memcpy(partial, text, partial_len);
                partial[partial_len] = '\0';
                vg_font_measure_text(pane->font, pane->font_size, partial, &metrics);
                width += metrics.width;
                free(partial);
            }
        }
        return width;
    }

    return width;
}

//=============================================================================
// Output Line Management
//=============================================================================

/// @brief Free all segment text buffers and the segments array owned by line.
/// @param line Line whose owned segment storage is released and reset.
static void free_output_line(vg_output_line_t *line) {
    if (!line)
        return;

    for (size_t i = 0; i < line->segment_count; i++) {
        free(line->segments[i].text);
    }
    free(line->segments);
}

/// @brief Free every logical line in an output-line ring without freeing the ring array.
/// @details The OutputPane can hold both an active line ring and a saved primary
///          ring while terminal alternate-screen mode is active. This helper
///          applies the same logical-to-physical indexing to either storage block
///          so destroy/clear paths do not depend on the pane's current active ring.
/// @param lines Ring storage to clear; may be NULL.
/// @param line_start Physical index of logical line zero.
/// @param line_count Number of live logical lines in the ring.
/// @param line_capacity Allocated ring capacity.
static void outputpane_free_line_storage(vg_output_line_t *lines,
                                         size_t line_start,
                                         size_t line_count,
                                         size_t line_capacity) {
    if (!lines || line_capacity == 0)
        return;
    for (size_t i = 0; i < line_count; i++) {
        size_t idx = (line_start + i) % line_capacity;
        free_output_line(&lines[idx]);
        memset(&lines[idx], 0, sizeof(vg_output_line_t));
    }
}

/// @brief Reset all selection state fields to the unselected state.
/// @param pane Output pane whose selection is cleared.
static void outputpane_clear_selection(vg_outputpane_t *pane) {
    if (!pane)
        return;
    pane->has_selection = false;
    pane->sel_start_line = 0;
    pane->sel_start_col = 0;
    pane->sel_end_line = 0;
    pane->sel_end_col = 0;
}

/// @brief Adjust cursor and selection coordinates after the first line is evicted.
/// @param pane Output pane whose logical line indices shift down by one.
static void outputpane_note_evicted_first_line(vg_outputpane_t *pane) {
    if (!pane)
        return;
    if (pane->terminal_mode) {
        if (pane->term_cursor_line > 0)
            pane->term_cursor_line--;
        if (pane->term_origin_line > 0)
            pane->term_origin_line--;
        if (pane->saved_cursor_line > 0)
            pane->saved_cursor_line--;
    }
    if (!pane->has_selection)
        return;
    if (pane->sel_start_line == 0 || pane->sel_end_line == 0) {
        outputpane_clear_selection(pane);
        return;
    }
    pane->sel_start_line--;
    pane->sel_end_line--;
}

/// @brief Physical array index for a logical output line.
/// @param pane Output pane owning the circular line buffer.
/// @param logical_index Zero-based logical line index.
/// @return Wrapped physical slot index, or zero when no buffer exists.
static size_t outputpane_physical_line_index(const vg_outputpane_t *pane, size_t logical_index) {
    if (!pane || pane->line_capacity == 0)
        return 0;
    return (pane->line_start + logical_index) % pane->line_capacity;
}

/// @brief Return a logical line pointer from the circular line buffer.
/// @param pane Output pane owning the circular line buffer.
/// @param logical_index Zero-based logical line index.
/// @return Mutable line pointer, or NULL when the index is out of range.
static vg_output_line_t *outputpane_line_at(vg_outputpane_t *pane, size_t logical_index) {
    if (!pane || logical_index >= pane->line_count || pane->line_capacity == 0)
        return NULL;
    return &pane->lines[outputpane_physical_line_index(pane, logical_index)];
}

/// @brief Return the newest logical line, or NULL when the pane is empty.
/// @param pane Output pane whose final line is requested.
/// @return Mutable newest-line pointer, or NULL when no line exists.
static vg_output_line_t *outputpane_last_line(vg_outputpane_t *pane) {
    if (!pane || pane->line_count == 0)
        return NULL;
    return outputpane_line_at(pane, pane->line_count - 1);
}

/// @brief Grow the line ring while preserving logical order.
/// @param pane Output pane whose owned line array is replaced.
/// @param new_cap Required nonzero line capacity.
/// @return True when growth succeeds; false for invalid size or allocation failure.
static bool outputpane_grow_lines(vg_outputpane_t *pane, size_t new_cap) {
    if (!pane || new_cap == 0 || new_cap > SIZE_MAX / sizeof(vg_output_line_t))
        return false;
    vg_output_line_t *new_lines = calloc(new_cap, sizeof(vg_output_line_t));
    if (!new_lines)
        return false;
    for (size_t i = 0; i < pane->line_count; i++) {
        size_t old_idx = outputpane_physical_line_index(pane, i);
        new_lines[i] = pane->lines[old_idx];
    }
    free(pane->lines);
    pane->lines = new_lines;
    pane->line_capacity = new_cap;
    pane->line_start = 0;
    return true;
}

/// @brief Evict the oldest logical line in O(1).
/// @param pane Output pane whose oldest line is freed and removed.
static void outputpane_evict_first_line(vg_outputpane_t *pane) {
    if (!pane || pane->line_count == 0 || pane->line_capacity == 0)
        return;
    vg_output_line_t *line = outputpane_line_at(pane, 0);
    free_output_line(line);
    memset(line, 0, sizeof(vg_output_line_t));
    pane->line_start = (pane->line_start + 1) % pane->line_capacity;
    pane->line_count--;
    outputpane_note_evicted_first_line(pane);
}

/// @brief Append and return a zeroed segment slot to line, growing the array if needed.
/// @param line Output line that owns the segment array.
/// @return Newly appended segment slot, or NULL on overflow or allocation failure.
static vg_styled_segment_t *add_segment(vg_output_line_t *line) {
    if (line->segment_count >= line->segment_capacity) {
        if (line->segment_capacity > SIZE_MAX / 2u)
            return NULL;
        size_t new_cap = line->segment_capacity * 2;
        if (new_cap < 4)
            new_cap = 4;
        if (new_cap > SIZE_MAX / sizeof(vg_styled_segment_t))
            return NULL;
        vg_styled_segment_t *new_segs =
            realloc(line->segments, new_cap * sizeof(vg_styled_segment_t));
        if (!new_segs)
            return NULL;
        line->segments = new_segs;
        line->segment_capacity = new_cap;
    }

    vg_styled_segment_t *seg = &line->segments[line->segment_count++];
    memset(seg, 0, sizeof(vg_styled_segment_t));
    return seg;
}

/// @brief Append a copied styled text segment atomically.
/// @details Allocates the segment text before reserving the segment slot.  If
///          either allocation fails, the line is left unchanged and no empty
///          segment is committed.
/// @param line Destination output line.
/// @param text Source text bytes to copy.
/// @param len Number of bytes to copy from @p text.
/// @param fg Foreground color for the new segment.
/// @param bg Background color for the new segment.
/// @param bold Whether the segment should be rendered bold.
/// @return true when the segment was appended; false on invalid input or OOM.
static bool outputpane_append_segment_copy(
    vg_output_line_t *line, const char *text, size_t len, uint32_t fg, uint32_t bg, bool bold) {
    if (!line || !text || len == 0 || len == SIZE_MAX)
        return false;
    char *copy = malloc(len + 1u);
    if (!copy)
        return false;
    memcpy(copy, text, len);
    copy[len] = '\0';

    vg_styled_segment_t *seg = add_segment(line);
    if (!seg) {
        free(copy);
        return false;
    }
    seg->text = copy;
    seg->fg_color = fg;
    seg->bg_color = bg;
    seg->bold = bold;
    return true;
}

/// @brief Append a new empty line to pane, evicting the oldest if the ring buffer is full.
/// @param pane Output pane whose circular buffer receives the line.
/// @return Mutable new line, or NULL when disabled or allocation fails.
static vg_output_line_t *add_line(vg_outputpane_t *pane) {
    if (!pane || pane->max_lines == 0)
        return NULL;

    if (pane->line_count >= pane->max_lines)
        outputpane_evict_first_line(pane);

    if (pane->line_count >= pane->line_capacity) {
        if (pane->line_capacity > SIZE_MAX / 2u)
            return NULL;
        size_t new_cap = pane->line_capacity * 2;
        if (new_cap < 64)
            new_cap = 64;
        if (new_cap > pane->max_lines)
            new_cap = pane->max_lines;
        if (new_cap == 0)
            return NULL;
        if (!outputpane_grow_lines(pane, new_cap))
            return NULL;
    }

    size_t idx = outputpane_physical_line_index(pane, pane->line_count);
    vg_output_line_t *line = &pane->lines[idx];
    pane->line_count++;
    memset(line, 0, sizeof(vg_output_line_t));
    return line;
}

//=============================================================================
// ANSI Parser
//=============================================================================

/// @brief Apply the buffered ANSI SGR escape sequence to pane's current color/bold state.
/// @param pane Output pane containing the buffered escape and mutable style state.
static void process_ansi_escape(vg_outputpane_t *pane) {
    int params[16];
    int param_count = 0;
    char *buf = pane->escape_buf;

    if (!pane || buf[0] != '[') {
        pane->escape_len = 0;
        pane->in_escape = false;
        return;
    }
    size_t len = strlen(buf);
    if (len == 0 || buf[len - 1] != 'm') {
        pane->escape_len = 0;
        pane->in_escape = false;
        return;
    }

    param_count = ansi_parse_csi_params(buf, params, 16, NULL);
    outputpane_apply_sgr_params(pane, params, param_count);

    pane->escape_len = 0;
    pane->in_escape = false;
}

//=============================================================================
// OutputPane Implementation
//=============================================================================

/// @brief Create an output pane widget with default colours and a 10 000-line ring buffer.
///
/// @details Default background is dark grey (0xFF1E1E1E) and foreground is light grey
///          (0xFFCCCCCC).  auto_scroll is enabled.  Attach a font with
///          vg_outputpane_set_font before appending any text.
///
/// @return Newly allocated vg_outputpane_t, or NULL on allocation failure.
vg_outputpane_t *vg_outputpane_create(void) {
    vg_outputpane_t *pane = calloc(1, sizeof(vg_outputpane_t));
    if (!pane)
        return NULL;

    vg_widget_init(&pane->base, VG_WIDGET_OUTPUTPANE, &g_outputpane_vtable);

    vg_theme_t *theme = vg_theme_get_current();

    // Defaults
    pane->max_lines = 10000;
    pane->auto_scroll = true;
    pane->line_height = 16;
    pane->font_size = theme->typography.size_normal;

    pane->bg_color = 0xFF1E1E1E;
    pane->default_fg = 0xFFCCCCCC;
    pane->current_fg = pane->default_fg;
    pane->caret_visible = true;
    pane->term_scroll_top = -1;
    pane->term_scroll_bottom = -1;
    pane->primary_term_scroll_top = -1;
    pane->primary_term_scroll_bottom = -1;

    return pane;
}

/// @brief Widget-vtable destroy hook: free every output line (and its
///        segment buffers) and the line array.
/// @param widget Output-pane base widget being destroyed.
static void outputpane_destroy(vg_widget_t *widget) {
    vg_outputpane_t *pane = (vg_outputpane_t *)widget;

    outputpane_free_line_storage(pane->lines, pane->line_start, pane->line_count, pane->line_capacity);
    outputpane_free_line_storage(pane->primary_lines,
                                 pane->primary_line_start,
                                 pane->primary_line_count,
                                 pane->primary_line_capacity);
    free(pane->lines);
    free(pane->primary_lines);
    free(pane->cells);
    free(pane->pending_input);
}

/// @brief Destroy the output pane widget, freeing all lines and segment text buffers.
///
/// @param pane The output pane to destroy; may be NULL.
void vg_outputpane_destroy(vg_outputpane_t *pane) {
    if (!pane)
        return;
    vg_widget_destroy(&pane->base);
}

/// @brief Widget-vtable measure hook: the output pane fills the available
///        space (no intrinsic size).
/// @param widget Output-pane base widget whose measured size is updated.
/// @param available_width Parent-provided width.
/// @param available_height Parent-provided height.
static void outputpane_measure(vg_widget_t *widget, float available_width, float available_height) {
    (void)available_width;
    (void)available_height;

    // OutputPane typically fills available space
    widget->measured_width = available_width;
    widget->measured_height = available_height;
}

/// @brief Widget-vtable paint hook: draw the visible lines with per-segment
///        colors, the selection highlight, and the border.
/// @param widget Arranged output-pane base widget to render.
/// @param canvas Backend canvas used for clipping and drawing.
static void outputpane_paint(vg_widget_t *widget, void *canvas) {
    vg_outputpane_t *pane = (vg_outputpane_t *)widget;
    vg_theme_t *theme = vg_theme_get_current();
    vgfx_window_t win = (vgfx_window_t)canvas;
    uint32_t selection_color =
        theme ? vg_color_blend(theme->colors.bg_selected, theme->colors.accent_primary, 0.18f)
              : 0x334C73u;
    uint32_t border_color = theme ? theme->colors.border_primary : 0x333333u;
    vg_font_metrics_t font_metrics = {0};
    uint32_t start_line = 0;
    uint32_t end_line = 0;
    uint32_t start_col = 0;
    uint32_t end_col = 0;

    vgfx_fill_rect(win,
                   (int32_t)widget->x,
                   (int32_t)widget->y,
                   (int32_t)widget->width,
                   (int32_t)widget->height,
                   pane->bg_color);
    vgfx_rect(win,
              (int32_t)widget->x,
              (int32_t)widget->y,
              (int32_t)widget->width,
              (int32_t)widget->height,
              border_color);

    if (!pane->font)
        return;

    vg_font_get_metrics(pane->font, pane->font_size, &font_metrics);

    // Calculate visible lines
    int first_visible = (int)(pane->scroll_y / pane->line_height);
    int visible_count = (int)(widget->height / pane->line_height) + 1;

    float y = widget->y - fmodf(pane->scroll_y, pane->line_height);
    vgfx_set_clip(win,
                  (int32_t)widget->x,
                  (int32_t)widget->y,
                  (int32_t)widget->width,
                  (int32_t)widget->height);

    if (pane->has_selection) {
        start_line = pane->sel_start_line;
        start_col = pane->sel_start_col;
        end_line = pane->sel_end_line;
        end_col = pane->sel_end_col;
        if (start_line > end_line || (start_line == end_line && start_col > end_col)) {
            uint32_t tmp = start_line;
            start_line = end_line;
            end_line = tmp;
            tmp = start_col;
            start_col = end_col;
            end_col = tmp;
        }
    }

    for (int i = 0; i < visible_count && first_visible + i < (int)pane->line_count; i++) {
        int line_idx = first_visible + i;
        if (line_idx < 0)
            continue;

        vg_output_line_t *line = outputpane_line_at(pane, (size_t)line_idx);
        if (!line)
            continue;
        float x = widget->x + 4; // Left padding
        float line_top = y;

        if (pane->has_selection && (uint32_t)line_idx >= start_line &&
            (uint32_t)line_idx <= end_line) {
            uint32_t line_start_col = (uint32_t)line_idx == start_line ? start_col : 0;
            uint32_t line_end_col = (uint32_t)line_idx == end_line && end_col != UINT32_MAX
                                        ? end_col
                                        : (uint32_t)outputpane_line_length(line);
            float sel_x0 = widget->x + 4.0f + outputpane_prefix_width(pane, line, line_start_col);
            float sel_x1 = widget->x + 4.0f + outputpane_prefix_width(pane, line, line_end_col);
            if (sel_x1 < sel_x0) {
                float tmp = sel_x0;
                sel_x0 = sel_x1;
                sel_x1 = tmp;
            }
            if (sel_x1 > sel_x0) {
                vgfx_fill_rect(win,
                               (int32_t)sel_x0,
                               (int32_t)line_top,
                               (int32_t)(sel_x1 - sel_x0),
                               (int32_t)pane->line_height,
                               selection_color);
            }
        }

        for (size_t s = 0; s < line->segment_count; s++) {
            vg_styled_segment_t *seg = &line->segments[s];
            vg_text_metrics_t metrics = {0};
            if (!seg->text)
                continue;

            vg_font_measure_text(pane->font, pane->font_size, seg->text, &metrics);

            // Draw segment background if any
            if (seg->bg_color != 0) {
                vgfx_fill_rect(win,
                               (int32_t)x,
                               (int32_t)line_top,
                               (int32_t)metrics.width,
                               (int32_t)pane->line_height,
                               seg->bg_color);
            }

            // Draw text
            vg_font_draw_text(canvas,
                              pane->font,
                              pane->font_size,
                              x,
                              line_top + font_metrics.ascent,
                              seg->text,
                              seg->fg_color);

            // Advance X position
            x += metrics.width;
        }

        // Terminal caret: a blinking block at the cursor column on the current
        // line. The glyph under it is redrawn in the background color (inverse
        // video) so it stays readable when editing mid-line.
        if (pane->terminal_mode && pane->has_focus && pane->caret_visible &&
            !pane->term_cursor_hidden && line_idx == (int)pane->term_cursor_line) {
            float caret_x =
                widget->x + 4.0f + outputpane_prefix_width(pane, line, pane->cursor_col);
            vg_text_metrics_t cw = {0};
            vg_font_measure_text(pane->font, pane->font_size, "M", &cw);
            float caret_w = cw.width > 0.0f ? cw.width : pane->font_size * 0.6f;
            vgfx_fill_rect(win,
                           (int32_t)caret_x,
                           (int32_t)line_top,
                           (int32_t)caret_w,
                           (int32_t)pane->line_height,
                           pane->default_fg);
            if (pane->cursor_col < pane->cell_count) {
                const char *glyph = pane->cells[pane->cursor_col].utf8;
                if (glyph[0] != '\0')
                    vg_font_draw_text(canvas,
                                      pane->font,
                                      pane->font_size,
                                      caret_x,
                                      line_top + font_metrics.ascent,
                                      glyph,
                                      pane->bg_color);
            }
        }

        y += pane->line_height;
    }
    vgfx_clear_clip(win);
}

// Defined later in the Interactive Terminal Mode section; declared here for the
// event hook below.
static void term_queue_input(vg_outputpane_t *pane, const char *bytes, size_t len);
static int outputpane_encode_utf8(uint32_t cp, char *out);

/// @brief Convert GUI modifier flags into the xterm CSI modifier parameter.
/// @details Xterm encodes special-key modifiers as 1 plus bit weights for Shift,
///          Alt, and Control. The IDE treats Super like Control for shortcuts, so
///          terminal special keys do the same to match local keyboard conventions.
/// @param mods Bitwise OR of VG_MOD_* flags from a key event.
/// @return xterm modifier parameter in the range [1, 8].
static int term_modifier_parameter(uint32_t mods) {
    int value = 1;
    if ((mods & VG_MOD_SHIFT) != 0)
        value += 1;
    if ((mods & VG_MOD_ALT) != 0)
        value += 2;
    if ((mods & (VG_MOD_CTRL | VG_MOD_SUPER)) != 0)
        value += 4;
    return value;
}

/// @brief Queue a CSI final-byte key such as arrows, Home, or End.
/// @details Unmodified keys use the compact sequence expected by shells. Modified
///          keys use CSI 1;<modifier><final>, which is the common xterm encoding
///          used by readline, shells, and terminal applications.
/// @param pane Terminal pane receiving queued bytes.
/// @param final CSI final byte, for example 'A' for Up or 'H' for Home.
/// @param plain Unmodified byte sequence to send when no modifiers are present.
/// @param mods Active GUI modifier flags.
static void term_queue_csi_final_key(vg_outputpane_t *pane,
                                     char final,
                                     const char *plain,
                                     uint32_t mods) {
    int mod = term_modifier_parameter(mods);
    if (mod == 1) {
        // DECSET ?1 (application cursor keys): unmodified arrows/home/end use
        // SS3 finals so full-screen apps see the sequences they asked for.
        if (pane->app_cursor_keys &&
            (final == 'A' || final == 'B' || final == 'C' || final == 'D' || final == 'H' ||
             final == 'F')) {
            char seq[3] = {'\x1b', 'O', final};
            term_queue_input(pane, seq, sizeof(seq));
            return;
        }
        term_queue_input(pane, plain, strlen(plain));
        return;
    }
    char seq[16];
    int len = snprintf(seq, sizeof(seq), "\x1b[1;%d%c", mod, final);
    if (len > 0)
        term_queue_input(pane, seq, (size_t)len);
}

/// @brief Queue a tilde-terminated CSI key such as Insert, Delete, or PageUp.
/// @details Modified variants follow xterm's CSI <code>;<modifier>~ form while
///          unmodified variants use CSI <code>~.
/// @param pane Terminal pane receiving queued bytes.
/// @param code Numeric CSI key code.
/// @param mods Active GUI modifier flags.
static void term_queue_csi_tilde_key(vg_outputpane_t *pane, int code, uint32_t mods) {
    char seq[24];
    int mod = term_modifier_parameter(mods);
    int len = 0;
    if (mod == 1)
        len = snprintf(seq, sizeof(seq), "\x1b[%d~", code);
    else
        len = snprintf(seq, sizeof(seq), "\x1b[%d;%d~", code, mod);
    if (len > 0)
        term_queue_input(pane, seq, (size_t)len);
}

/// @brief Translate a Ctrl+key chord into its ASCII control byte.
/// @details Handles letters plus common terminal punctuation controls. Ctrl+Space
///          returns NUL, so callers must queue the returned byte by length rather
///          than treating it as a C string.
/// @param key GUI virtual key from the key event.
/// @param out_byte Receives the control byte when a mapping exists.
/// @return true when @p key has a terminal control-byte mapping.
static bool term_control_byte_for_key(vg_key_t key, char *out_byte) {
    if (!out_byte)
        return false;
    if (key >= VG_KEY_A && key <= VG_KEY_Z) {
        *out_byte = (char)(key - VG_KEY_A + 1);
        return true;
    }
    switch (key) {
        case VG_KEY_SPACE:
        case VG_KEY_2:
            *out_byte = '\0';
            return true;
        case VG_KEY_LEFT_BRACKET:
        case VG_KEY_ESCAPE:
            *out_byte = 0x1b;
            return true;
        case VG_KEY_BACKSLASH:
            *out_byte = 0x1c;
            return true;
        case VG_KEY_RIGHT_BRACKET:
            *out_byte = 0x1d;
            return true;
        case VG_KEY_6:
            *out_byte = 0x1e;
            return true;
        case VG_KEY_MINUS:
        case VG_KEY_SLASH:
            *out_byte = 0x1f;
            return true;
        default:
            return false;
    }
}

/// @brief Queue an xterm-compatible function-key sequence.
/// @details F1-F4 use SS3 OP/OQ/OR/OS when unmodified and CSI 1;<modifier>P-S
///          when modified. F5-F12 use the standard tilde-key numeric codes, with
///          the same modifier encoding as other CSI tilde keys.
/// @param pane Terminal pane receiving queued bytes.
/// @param key Function key in the inclusive range VG_KEY_F1..VG_KEY_F12.
/// @param mods Active GUI modifier flags.
/// @return true when @p key was a supported function key.
static bool term_queue_function_key(vg_outputpane_t *pane, vg_key_t key, uint32_t mods) {
    static const char ss3_finals[4] = {'P', 'Q', 'R', 'S'};
    static const int tilde_codes[8] = {15, 17, 18, 19, 20, 21, 23, 24};

    if (key < VG_KEY_F1 || key > VG_KEY_F12)
        return false;
    int index = key - VG_KEY_F1;
    int mod = term_modifier_parameter(mods);
    if (index < 4) {
        if (mod == 1) {
            char seq[3] = {'\x1b', 'O', ss3_finals[index]};
            term_queue_input(pane, seq, sizeof(seq));
        } else {
            char seq[16];
            int len = snprintf(seq, sizeof(seq), "\x1b[1;%d%c", mod, ss3_finals[index]);
            if (len > 0)
                term_queue_input(pane, seq, (size_t)len);
        }
        return true;
    }
    term_queue_csi_tilde_key(pane, tilde_codes[index - 4], mods);
    return true;
}

/// @brief Widget-vtable can_focus: focusable only in interactive terminal mode.
/// @param widget Output-pane base widget whose focus eligibility is queried.
/// @return True when terminal mode is enabled and the widget is enabled and visible.
static bool outputpane_can_focus(vg_widget_t *widget) {
    vg_outputpane_t *pane = (vg_outputpane_t *)widget;
    return pane->terminal_mode && widget->enabled && widget->visible;
}

/// @brief Widget-vtable on_focus: track focus so the terminal caret can render.
/// @param widget Output-pane base widget receiving the focus transition.
/// @param gained True when focus is acquired, or false when it is lost.
static void outputpane_on_focus(vg_widget_t *widget, bool gained) {
    vg_outputpane_t *pane = (vg_outputpane_t *)widget;
    pane->has_focus = gained;
    if (gained) {
        pane->caret_visible = true;
        pane->caret_blink_time = 0.0f;
    }
    widget->needs_paint = true;
}

/// @brief Widget-vtable event hook: terminal keyboard input + focus (terminal mode),
///        scrolling, and click/drag text selection. Returns true if consumed.
/// @param widget Output-pane base widget receiving the event.
/// @param event Mutable GUI event to interpret.
/// @return True when the pane consumes the event.
static bool outputpane_handle_event(vg_widget_t *widget, vg_event_t *event) {
    vg_outputpane_t *pane = (vg_outputpane_t *)widget;

    if (pane->terminal_mode) {
        if (event->type == VG_EVENT_MOUSE_DOWN) {
            vg_widget_set_focus(widget);
            return true;
        }
        if (event->type == VG_EVENT_KEY_DOWN) {
            vg_key_t k = event->key.key;
            uint32_t mods = event->modifiers;
            // Clipboard chords come before control-byte mapping so Cmd+V /
            // Ctrl+Shift+V paste (bracketed when DECSET 2004 is on) and Cmd+C /
            // Ctrl+Shift+C copy a selection. Plain Ctrl+V / Ctrl+C still reach
            // the child as 0x16 / 0x03 (vim blockwise / SIGINT).
            const bool clip_chord = (mods & VG_MOD_SUPER) != 0 ||
                                    ((mods & VG_MOD_CTRL) != 0 && (mods & VG_MOD_SHIFT) != 0);
            if (clip_chord && k == VG_KEY_V) {
                char *text = vgfx_clipboard_get_text();
                if (text) {
                    if (pane->bracketed_paste)
                        term_queue_input(pane, "\x1b[200~", 6);
                    term_queue_input(pane, text, strlen(text));
                    if (pane->bracketed_paste)
                        term_queue_input(pane, "\x1b[201~", 6);
                    free(text);
                }
                return true;
            }
            if (clip_chord && k == VG_KEY_C && pane->has_selection) {
                char *sel = vg_outputpane_get_selection(pane);
                if (sel) {
                    vgfx_clipboard_set_text(sel);
                    free(sel);
                }
                return true;
            }
            // Ctrl + letter -> control byte (Ctrl-C = 0x03, Ctrl-D = 0x04, ...).
            if ((mods & (VG_MOD_CTRL | VG_MOD_SUPER)) != 0) {
                char ctl = 0;
                if (term_control_byte_for_key(k, &ctl)) {
                    term_queue_input(pane, &ctl, 1);
                    return true;
                }
            }
            if (term_queue_function_key(pane, k, mods))
                return true;
            switch (k) {
                case VG_KEY_ENTER:
                    term_queue_input(pane, "\r", 1);
                    return true;
                case VG_KEY_BACKSPACE:
                    if ((mods & VG_MOD_ALT) != 0) {
                        term_queue_input(pane, "\x1b\x7f", 2);
                    } else {
                        term_queue_input(pane, "\x7f", 1);
                    }
                    return true;
                case VG_KEY_TAB:
                    if ((mods & VG_MOD_SHIFT) != 0)
                        term_queue_input(pane, "\x1b[Z", 3);
                    else
                        term_queue_input(pane, "\t", 1);
                    return true;
                case VG_KEY_ESCAPE:
                    term_queue_input(pane, "\x1b", 1);
                    return true;
                case VG_KEY_UP:
                    term_queue_csi_final_key(pane, 'A', "\x1b[A", mods);
                    return true;
                case VG_KEY_DOWN:
                    term_queue_csi_final_key(pane, 'B', "\x1b[B", mods);
                    return true;
                case VG_KEY_RIGHT:
                    term_queue_csi_final_key(pane, 'C', "\x1b[C", mods);
                    return true;
                case VG_KEY_LEFT:
                    term_queue_csi_final_key(pane, 'D', "\x1b[D", mods);
                    return true;
                case VG_KEY_HOME:
                    term_queue_csi_final_key(pane, 'H', "\x1b[H", mods);
                    return true;
                case VG_KEY_END:
                    term_queue_csi_final_key(pane, 'F', "\x1b[F", mods);
                    return true;
                case VG_KEY_INSERT:
                    term_queue_csi_tilde_key(pane, 2, mods);
                    return true;
                case VG_KEY_DELETE:
                    term_queue_csi_tilde_key(pane, 3, mods);
                    return true;
                case VG_KEY_PAGE_UP:
                    term_queue_csi_tilde_key(pane, 5, mods);
                    return true;
                case VG_KEY_PAGE_DOWN:
                    term_queue_csi_tilde_key(pane, 6, mods);
                    return true;
                default:
                    break;
            }
            return false; // printable input arrives via VG_EVENT_KEY_CHAR
        }
        if (event->type == VG_EVENT_KEY_CHAR) {
            uint32_t cp = event->key.codepoint;
            uint32_t mods = event->modifiers;
            if (cp >= 0x20 && cp != 0x7f) {
                char u[4];
                int l = outputpane_encode_utf8(cp, u);
                if ((mods & VG_MOD_ALT) != 0)
                    term_queue_input(pane, "\x1b", 1);
                term_queue_input(pane, u, (size_t)l);
            }
            return true;
        }
    }

    if (event->type == VG_EVENT_MOUSE_WHEEL) {
        float delta = event->wheel.delta_y * 30 * vg_get_wheel_speed(); // 30px per unit * sensitivity
        pane->scroll_y -= delta;

        // Clamp scroll
        float max_scroll = pane->line_count * pane->line_height - widget->height;
        if (max_scroll < 0)
            max_scroll = 0;
        if (pane->scroll_y < 0)
            pane->scroll_y = 0;
        if (pane->scroll_y > max_scroll)
            pane->scroll_y = max_scroll;

        // Lock auto-scroll if user scrolled up
        pane->scroll_locked = pane->scroll_y < max_scroll - pane->line_height;

        pane->base.needs_paint = true;
        return true;
    }

    return false;
}

//=============================================================================
// Interactive Terminal Mode
//=============================================================================
// A cursor-position overwrite model layered over the same line/segment storage.
// Only active when pane->terminal_mode is set (the integrated terminal); the
// build-output pane keeps the simple append-only path below. The cursor line is
// held as a cell array (cells[]) so \r/\b/ESC[K/cursor-moves render correctly;
// the cells are coalesced back into styled segments after each chunk.

/// @brief UTF-8 sequence length implied by a lead byte (1..4; 1 for invalid/continuation).
/// @param c Candidate UTF-8 lead byte.
/// @return Expected encoded length from one through four bytes.
static int outputpane_utf8_len(unsigned char c) {
    if (c < 0x80)
        return 1;
    if ((c & 0xE0) == 0xC0)
        return 2;
    if ((c & 0xF0) == 0xE0)
        return 3;
    if ((c & 0xF8) == 0xF0)
        return 4;
    return 1;
}

/// @brief Encode a Unicode codepoint to UTF-8; returns the byte count (1..4).
/// @param cp Unicode code point to encode.
/// @param[out] out Destination with capacity for at least four bytes.
/// @return Number of bytes written.
static int outputpane_encode_utf8(uint32_t cp, char *out) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/// @brief Ensure the terminal cell buffer can hold at least @p need cells.
/// @param pane Output pane that owns the cell buffer.
/// @param need Minimum required cell capacity.
/// @return True when sufficient storage is available; false on overflow or allocation failure.
static bool term_ensure_cells(vg_outputpane_t *pane, size_t need) {
    if (pane->cell_capacity >= need)
        return true;
    size_t new_cap = pane->cell_capacity ? pane->cell_capacity * 2 : 128;
    if (new_cap < need)
        new_cap = need;
    if (new_cap > SIZE_MAX / sizeof(vg_term_cell_t))
        return false;
    vg_term_cell_t *nc = realloc(pane->cells, new_cap * sizeof(vg_term_cell_t));
    if (!nc)
        return false;
    pane->cells = nc;
    pane->cell_capacity = new_cap;
    return true;
}

/// @brief Reset a cell to a blank space with default colors.
/// @param pane Output pane supplying default foreground and background colors.
/// @param[out] cell Terminal cell reset in place.
static void term_blank_cell(vg_outputpane_t *pane, vg_term_cell_t *cell) {
    cell->utf8[0] = '\0';
    cell->fg = pane->default_fg;
    cell->bg = 0;
    cell->bold = false;
}

/// @brief Free and reset all styled segments in a logical output line.
/// @details Terminal mode rewrites existing rows frequently. Resetting the line
///          through this helper keeps ownership of segment text centralized and
///          leaves the row ready for a fresh coalesced cell flush.
/// @param line Output line whose segment storage should be cleared; may be NULL.
static void outputpane_clear_line_segments(vg_output_line_t *line) {
    if (!line)
        return;
    free_output_line(line);
    line->segments = NULL;
    line->segment_count = 0;
    line->segment_capacity = 0;
}

/// @brief Clamp a requested logical terminal line to the retained line buffer.
/// @details OutputPane is not a full scrollback terminal; cursor-addressed rows
///          live inside the retained ring. Extremely large CSI row values land
///          on the last retained row instead of growing unbounded memory.
/// @param pane Output pane whose retention limit defines the maximum row.
/// @param logical_line Requested zero-based logical terminal row.
/// @return A row index that can fit in the retained line buffer.
static size_t term_clamp_line_index(const vg_outputpane_t *pane, size_t logical_line) {
    if (!pane || pane->max_lines == 0)
        return 0;
    if (logical_line >= pane->max_lines)
        return pane->max_lines - 1;
    return logical_line;
}

/// @brief Ensure that @p logical_line exists in the output line ring.
/// @details Missing rows are appended as empty lines. Callers should pass a
///          clamped line index; this helper still clamps defensively so malformed
///          escape streams cannot force unbounded line growth.
/// @param pane Terminal-mode output pane to grow.
/// @param logical_line Requested zero-based logical terminal row.
/// @return true when the row exists after the call; false on invalid input or OOM.
static bool term_ensure_line(vg_outputpane_t *pane, size_t logical_line) {
    if (!pane || pane->max_lines == 0)
        return false;
    logical_line = term_clamp_line_index(pane, logical_line);
    while (pane->line_count <= logical_line) {
        if (!add_line(pane))
            return false;
    }
    return true;
}

/// @brief Load a retained output row into the terminal cell buffer.
/// @details Cursor-addressing can jump between rows. The active cell buffer must
///          mirror the destination row before subsequent glyph writes overwrite
///          the correct columns and styles.
/// @param pane Terminal-mode output pane whose cells should be replaced.
/// @param logical_line Zero-based retained row to load into cells.
static void term_load_cells_from_line(vg_outputpane_t *pane, size_t logical_line) {
    if (!pane) {
        return;
    }
    logical_line = term_clamp_line_index(pane, logical_line);
    if (!term_ensure_line(pane, logical_line)) {
        pane->cell_count = 0;
        return;
    }

    vg_output_line_t *line = outputpane_line_at(pane, logical_line);
    pane->cell_count = 0;
    if (!line)
        return;

    for (size_t s = 0; s < line->segment_count; s++) {
        vg_styled_segment_t *seg = &line->segments[s];
        if (!seg->text)
            continue;
        const unsigned char *p = (const unsigned char *)seg->text;
        while (*p) {
            int len = outputpane_utf8_len(*p);
            int avail = 0;
            while (avail < len && p[avail])
                avail++;
            if (avail < len)
                len = avail > 0 ? avail : 1;
            if (!term_ensure_cells(pane, pane->cell_count + 1))
                return;
            vg_term_cell_t *cell = &pane->cells[pane->cell_count++];
            memcpy(cell->utf8, p, (size_t)len);
            cell->utf8[len] = '\0';
            cell->fg = seg->fg_color;
            cell->bg = seg->bg_color;
            cell->bold = seg->bold;
            p += len;
        }
    }
}

/// @brief Write a glyph at the cursor column (overwriting), extending with blanks as needed.
/// @param pane Output pane whose cursor line and column are updated.
/// @param bytes UTF-8 glyph bytes to copy.
/// @param len Number of bytes in @p bytes, clamped to the cell representation.
static void term_put_glyph(vg_outputpane_t *pane, const char *bytes, int len) {
    if (len < 1)
        len = 1;
    if (len > 4)
        len = 4;
    size_t col = pane->cursor_col;
    if (!term_ensure_cells(pane, col + 1))
        return;
    while (pane->cell_count <= col) {
        term_blank_cell(pane, &pane->cells[pane->cell_count]);
        pane->cell_count++;
    }
    vg_term_cell_t *cell = &pane->cells[col];
    memcpy(cell->utf8, bytes, (size_t)len);
    cell->utf8[len] = '\0';
    if (pane->ansi_reverse) {
        // SGR 7 swaps roles; a transparent background substitutes the pane's
        // own background color so reversed text stays a readable block.
        cell->fg = pane->current_bg ? pane->current_bg : pane->bg_color;
        cell->bg = pane->current_fg;
    } else {
        cell->fg = pane->current_fg;
        cell->bg = pane->current_bg;
    }
    cell->bold = pane->ansi_bold;
    pane->cursor_col = (uint32_t)(col + 1);
}

/// @brief Rebuild the cursor line's styled segments from the cell buffer.
/// @param pane Output pane whose current terminal line is regenerated.
static void term_flush_cells(vg_outputpane_t *pane) {
    if (!pane)
        return;
    pane->term_cursor_line = term_clamp_line_index(pane, pane->term_cursor_line);
    if (!term_ensure_line(pane, pane->term_cursor_line))
        return;
    vg_output_line_t *line = outputpane_line_at(pane, pane->term_cursor_line);
    if (!line)
        return;
    outputpane_clear_line_segments(line);

    if (pane->cell_count == 0)
        return;
    if (pane->cell_count > (SIZE_MAX - 1) / 4)
        return;
    char *run = malloc(pane->cell_count * 4 + 1);
    if (!run)
        return;
    size_t i = 0;
    while (i < pane->cell_count) {
        uint32_t fg = pane->cells[i].fg;
        uint32_t bg = pane->cells[i].bg;
        bool bold = pane->cells[i].bold;
        size_t rl = 0;
        size_t j = i;
        for (; j < pane->cell_count; j++) {
            vg_term_cell_t *c = &pane->cells[j];
            if (c->fg != fg || c->bg != bg || c->bold != bold)
                break;
            if (c->utf8[0] == '\0') {
                run[rl++] = ' ';
            } else {
                size_t l = strlen(c->utf8);
                memcpy(run + rl, c->utf8, l);
                rl += l;
            }
        }
        (void)outputpane_append_segment_copy(line, run, rl, fg, bg, bold);
        i = j;
    }
    free(run);
}

/// @brief Move the terminal cursor to a retained logical line and column.
/// @details The current row is flushed before switching rows, then the target
///          row is loaded into cells so later writes affect that row.
/// @param pane Terminal-mode output pane to update.
/// @param line Zero-based logical row target.
/// @param col Zero-based cursor column target.
static void term_set_cursor_line_col(vg_outputpane_t *pane, size_t line, uint32_t col) {
    if (!pane)
        return;
    term_flush_cells(pane);
    line = term_clamp_line_index(pane, line);
    if (!term_ensure_line(pane, line))
        return;
    pane->term_cursor_line = line;
    term_load_cells_from_line(pane, line);
    pane->cursor_col = col;
}

/// @brief Save the current terminal cursor position.
/// @param pane Terminal-mode output pane whose current cursor should be saved.
static void term_save_cursor(vg_outputpane_t *pane) {
    if (!pane)
        return;
    pane->saved_cursor_line = pane->term_cursor_line;
    pane->saved_cursor_col = pane->cursor_col;
}

/// @brief Restore the most recently saved terminal cursor position.
/// @param pane Terminal-mode output pane whose saved cursor should be restored.
static void term_restore_cursor(vg_outputpane_t *pane) {
    if (!pane)
        return;
    term_set_cursor_line_col(pane, pane->saved_cursor_line, pane->saved_cursor_col);
}

/// @brief Visible terminal rows for margin defaults (fallback before layout).
/// @param pane Output pane whose current geometry determines the row count.
/// @return Positive visible row count, falling back to 24 before layout.
static int term_screen_rows(const vg_outputpane_t *pane) {
    int rows = vg_outputpane_rows_for_height(pane);
    return rows > 0 ? rows : 24;
}

/// @brief True when a DECSTBM scroll region is set and well-formed.
/// @param pane Output pane whose terminal margins are inspected.
/// @return True when the configured bottom margin follows a non-negative top margin.
static bool term_region_active(const vg_outputpane_t *pane) {
    return pane->term_scroll_top >= 0 && pane->term_scroll_bottom > pane->term_scroll_top;
}

/// @brief Absolute logical row of the active region's top margin.
/// @param pane Output pane supplying origin and top-margin state.
/// @return Absolute zero-based logical top row.
static size_t term_region_top_abs(const vg_outputpane_t *pane) {
    return pane->term_origin_line + (size_t)(pane->term_scroll_top > 0 ? pane->term_scroll_top : 0);
}

/// @brief Absolute logical row of the active region's bottom margin (inclusive).
/// @param pane Output pane supplying origin and margin or screen-height state.
/// @return Absolute zero-based inclusive bottom row.
static size_t term_region_bottom_abs(const vg_outputpane_t *pane) {
    int bottom = term_region_active(pane) ? pane->term_scroll_bottom : term_screen_rows(pane) - 1;
    return pane->term_origin_line + (size_t)(bottom > 0 ? bottom : 0);
}

/// @brief Swap the full line structs at two logical rows (segment ownership
///        moves with the struct; both rows must exist).
/// @param pane Output pane owning both line records.
/// @param a First logical row index.
/// @param b Second logical row index.
static void term_swap_lines(vg_outputpane_t *pane, size_t a, size_t b) {
    vg_output_line_t *la = outputpane_line_at(pane, a);
    vg_output_line_t *lb = outputpane_line_at(pane, b);
    if (!la || !lb || la == lb)
        return;
    vg_output_line_t tmp = *la;
    *la = *lb;
    *lb = tmp;
}

/// @brief Scroll rows [top_abs, bottom_abs] by @p n lines, up or down, blanking
///        the vacated rows. Content moves by whole-line struct swaps so segment
///        allocations travel with their text; the cursor row's cell buffer is
///        flushed first and reloaded after so the mutation is visible in cells.
/// @param pane Terminal-mode pane to mutate.
/// @param top_abs Absolute logical top row of the range.
/// @param bottom_abs Absolute logical bottom row of the range (inclusive).
/// @param n Line count to scroll (clamped to the range height).
/// @param down False scrolls content up (text moves toward top_abs); true
///        scrolls content down (blank rows appear at the top).
static void term_scroll_range(
    vg_outputpane_t *pane, size_t top_abs, size_t bottom_abs, size_t n, bool down) {
    if (!pane || bottom_abs < top_abs || n == 0)
        return;
    term_flush_cells(pane);
    if (!term_ensure_line(pane, bottom_abs))
        return;
    const size_t height = bottom_abs - top_abs + 1;
    if (n > height)
        n = height;
    if (!down) {
        for (size_t i = top_abs; i + n <= bottom_abs; i++)
            term_swap_lines(pane, i, i + n);
        for (size_t i = bottom_abs + 1 - n; i <= bottom_abs; i++)
            outputpane_clear_line_segments(outputpane_line_at(pane, i));
    } else {
        for (size_t i = bottom_abs; i >= top_abs + n; i--)
            term_swap_lines(pane, i, i - n);
        for (size_t i = top_abs; i < top_abs + n; i++)
            outputpane_clear_line_segments(outputpane_line_at(pane, i));
    }
    pane->term_cursor_line = term_clamp_line_index(pane, pane->term_cursor_line);
    term_load_cells_from_line(pane, pane->term_cursor_line);
    pane->base.needs_paint = true;
    outputpane_clear_selection(pane);
}

/// @brief Reset the tab-stop bitset to the default every-8-columns ruler.
/// @param pane Output pane whose terminal tab stops are reset.
static void term_init_default_tabs(vg_outputpane_t *pane) {
    memset(pane->term_tab_stops, 0, sizeof(pane->term_tab_stops));
    for (size_t col = 8; col < sizeof(pane->term_tab_stops) * 8; col += 8)
        pane->term_tab_stops[col / 8] |= (uint8_t)(1u << (col % 8));
}

/// @brief Set or clear the tab stop at @p col in the bitset.
/// @param pane Output pane whose tab-stop bitset is updated.
/// @param col Zero-based terminal column.
/// @param set True to install the stop, or false to remove it.
static void term_set_tab_stop(vg_outputpane_t *pane, uint32_t col, bool set) {
    if (col >= sizeof(pane->term_tab_stops) * 8)
        return;
    if (set)
        pane->term_tab_stops[col / 8] |= (uint8_t)(1u << (col % 8));
    else
        pane->term_tab_stops[col / 8] &= (uint8_t)~(1u << (col % 8));
}

/// @brief Next tab stop strictly after @p col, or one column past the current
///        width when no stop remains (HT never writes glyphs, it only moves).
/// @param pane Output pane whose tab-stop bitset is searched.
/// @param col Current zero-based terminal column.
/// @return Next configured stop or the fallback column beyond the current width.
static uint32_t term_next_tab_stop(const vg_outputpane_t *pane, uint32_t col) {
    const uint32_t limit = (uint32_t)(sizeof(pane->term_tab_stops) * 8);
    for (uint32_t c = col + 1; c < limit; c++) {
        if (pane->term_tab_stops[c / 8] & (uint8_t)(1u << (c % 8)))
            return c;
    }
    int cols = vg_outputpane_columns_for_width(pane);
    uint32_t edge = cols > 0 ? (uint32_t)(cols - 1) : col + 8;
    return edge > col ? edge : col + 1;
}

/// @brief Finalize the current line and move to the next terminal row.
/// @details At the bottom margin of an active DECSTBM region the region scrolls
///          up in place instead of growing the buffer, which is how full-screen
///          programs (vim, htop) keep their status rows pinned.
/// @param pane Terminal-mode output pane to advance.
static void term_newline(vg_outputpane_t *pane) {
    if (!pane)
        return;
    if (term_region_active(pane) && pane->term_cursor_line == term_region_bottom_abs(pane)) {
        term_scroll_range(
            pane, term_region_top_abs(pane), term_region_bottom_abs(pane), 1, /*down=*/false);
        pane->cursor_col = 0;
        return;
    }
    term_flush_cells(pane);
    if (pane->max_lines > 0 && pane->line_count >= pane->max_lines &&
        pane->term_cursor_line + 1 >= pane->max_lines) {
        if (!add_line(pane))
            return;
        pane->term_cursor_line = pane->line_count > 0 ? pane->line_count - 1 : 0;
        pane->cell_count = 0;
        pane->cursor_col = 0;
        return;
    }

    size_t next_line = pane->term_cursor_line + 1;
    if (!term_ensure_line(pane, next_line))
        return;
    pane->term_cursor_line = next_line;
    term_load_cells_from_line(pane, pane->term_cursor_line);
    pane->cursor_col = 0;
}

/// @brief Clear the terminal display while preserving current ANSI styling.
/// @param pane Output pane whose terminal lines and cursor are reset.
static void term_clear_display(vg_outputpane_t *pane) {
    if (!pane)
        return;
    outputpane_free_line_storage(pane->lines, pane->line_start, pane->line_count, pane->line_capacity);
    pane->line_count = 0;
    pane->line_start = 0;
    pane->cell_count = 0;
    pane->term_cursor_line = 0;
    pane->term_origin_line = 0;
    pane->saved_cursor_line = 0;
    pane->saved_cursor_col = 0;
    pane->cursor_col = 0;
    pane->scroll_y = 0;
    pane->scroll_locked = false;
    outputpane_clear_selection(pane);
    (void)add_line(pane);
    pane->base.needs_paint = true;
}

/// @brief Switch the terminal pane into its alternate screen buffer.
/// @details Full-screen terminal applications use DEC private modes 47, 1047,
///          and 1049 to get a clean screen while preserving the user's shell
///          scrollback. This function flushes the active cursor row, moves the
///          primary line ring into the pane's saved-primary fields without
///          copying, and starts a fresh empty active ring for alternate-screen
///          output.
/// @param pane Terminal-mode pane to swap into alternate-screen storage.
static void term_enter_alternate_screen(vg_outputpane_t *pane) {
    if (!pane)
        return;
    if (pane->alternate_screen) {
        term_clear_display(pane);
        return;
    }

    term_flush_cells(pane);
    outputpane_free_line_storage(pane->primary_lines,
                                 pane->primary_line_start,
                                 pane->primary_line_count,
                                 pane->primary_line_capacity);
    free(pane->primary_lines);

    pane->primary_lines = pane->lines;
    pane->primary_line_start = pane->line_start;
    pane->primary_line_count = pane->line_count;
    pane->primary_line_capacity = pane->line_capacity;
    pane->primary_scroll_y = pane->scroll_y;
    pane->primary_scroll_locked = pane->scroll_locked;
    pane->primary_term_cursor_line = pane->term_cursor_line;
    pane->primary_term_origin_line = pane->term_origin_line;
    pane->primary_saved_cursor_line = pane->saved_cursor_line;
    pane->primary_saved_cursor_col = pane->saved_cursor_col;
    pane->primary_cursor_col = pane->cursor_col;
    pane->primary_term_scroll_top = pane->term_scroll_top;
    pane->primary_term_scroll_bottom = pane->term_scroll_bottom;
    pane->term_scroll_top = -1;
    pane->term_scroll_bottom = -1;

    pane->lines = NULL;
    pane->line_start = 0;
    pane->line_count = 0;
    pane->line_capacity = 0;
    pane->term_cursor_line = 0;
    pane->term_origin_line = 0;
    pane->saved_cursor_line = 0;
    pane->saved_cursor_col = 0;
    pane->cursor_col = 0;
    pane->cell_count = 0;
    pane->scroll_y = 0;
    pane->scroll_locked = false;
    pane->alternate_screen = true;
    outputpane_clear_selection(pane);
    (void)add_line(pane);
    pane->base.needs_paint = true;
}

/// @brief Restore the primary terminal screen after alternate-screen mode.
/// @details Leaves the alternate buffer by freeing its active line ring, moving
///          the saved primary ring back into the active fields, and reloading the
///          saved cursor row into the terminal cell buffer. The saved primary
///          storage fields are cleared so destroy/clear paths retain a single
///          owner for every line-ring allocation.
/// @param pane Terminal-mode pane to restore to the primary buffer.
static void term_leave_alternate_screen(vg_outputpane_t *pane) {
    if (!pane || !pane->alternate_screen)
        return;

    term_flush_cells(pane);
    outputpane_free_line_storage(pane->lines, pane->line_start, pane->line_count, pane->line_capacity);
    free(pane->lines);

    pane->lines = pane->primary_lines;
    pane->line_start = pane->primary_line_start;
    pane->line_count = pane->primary_line_count;
    pane->line_capacity = pane->primary_line_capacity;
    pane->scroll_y = pane->primary_scroll_y;
    pane->scroll_locked = pane->primary_scroll_locked;
    pane->term_cursor_line = pane->primary_term_cursor_line;
    pane->term_origin_line = pane->primary_term_origin_line;
    pane->saved_cursor_line = pane->primary_saved_cursor_line;
    pane->saved_cursor_col = pane->primary_saved_cursor_col;
    pane->cursor_col = pane->primary_cursor_col;

    pane->primary_lines = NULL;
    pane->primary_line_start = 0;
    pane->primary_line_count = 0;
    pane->primary_line_capacity = 0;
    pane->primary_scroll_y = 0;
    pane->primary_scroll_locked = false;
    pane->primary_term_cursor_line = 0;
    pane->primary_term_origin_line = 0;
    pane->primary_saved_cursor_line = 0;
    pane->primary_saved_cursor_col = 0;
    pane->primary_cursor_col = 0;
    pane->term_scroll_top = pane->primary_term_scroll_top;
    pane->term_scroll_bottom = pane->primary_term_scroll_bottom;
    pane->primary_term_scroll_top = -1;
    pane->primary_term_scroll_bottom = -1;
    pane->alternate_screen = false;
    pane->cell_count = 0;

    if (pane->line_count == 0)
        (void)add_line(pane);
    if (pane->line_count > 0 && pane->term_cursor_line >= pane->line_count)
        pane->term_cursor_line = pane->line_count - 1;
    term_load_cells_from_line(pane, pane->term_cursor_line);
    outputpane_clear_selection(pane);
    pane->base.needs_paint = true;
}

/// @brief Erase all retained terminal rows after the cursor line.
/// @details CSI J with mode 0 clears from the cursor to the end of display. The
///          active row tail is handled by the caller; this helper drops later
///          rows from the logical ring without disturbing earlier scrollback.
/// @param pane Terminal-mode output pane to mutate.
static void term_clear_lines_after_cursor(vg_outputpane_t *pane) {
    if (!pane || pane->term_cursor_line + 1 >= pane->line_count)
        return;
    for (size_t i = pane->term_cursor_line + 1; i < pane->line_count; i++) {
        vg_output_line_t *line = outputpane_line_at(pane, i);
        outputpane_clear_line_segments(line);
    }
    pane->line_count = pane->term_cursor_line + 1;
}

/// @brief Erase retained terminal rows before the cursor line.
/// @details CSI J with mode 1 clears from the start of display through the
///          cursor. Earlier rows are blanked in place so logical row addresses
///          remain stable for the current display.
/// @param pane Terminal-mode output pane to mutate.
static void term_clear_lines_before_cursor(vg_outputpane_t *pane) {
    if (!pane)
        return;
    for (size_t i = 0; i < pane->term_cursor_line && i < pane->line_count; i++) {
        vg_output_line_t *line = outputpane_line_at(pane, i);
        outputpane_clear_line_segments(line);
    }
}

/// @brief Dispatch a completed CSI control sequence.
/// @details The escape buffer contains only CSI parameter/intermediate bytes, not
///          the leading '[' or the final byte. Supported controls include SGR
///          styling, erase-in-line/display, cursor movement/positioning, cursor
///          save/restore, and DEC alternate-screen private modes.
/// @param pane Terminal-mode pane receiving the CSI action.
/// @param final Final CSI byte that determines the control family.
static void term_dispatch_csi(vg_outputpane_t *pane, char final) {
    int params[16];
    bool private_mode = false;
    int pc = ansi_parse_csi_params(pane->escape_buf, params, 16, &private_mode);
    int p0 = pc > 0 ? params[0] : 0;
    switch (final) {
        case 'm':
            outputpane_apply_sgr_params(pane, params, pc);
            break;
        case 'K': // erase in line
            if (p0 == 0) {
                if (pane->cursor_col < pane->cell_count)
                    pane->cell_count = pane->cursor_col;
            } else if (p0 == 1) {
                for (size_t i = 0; i <= pane->cursor_col && i < pane->cell_count; i++)
                    term_blank_cell(pane, &pane->cells[i]);
            } else if (p0 == 2) {
                pane->cell_count = 0;
            }
            break;
        case 'J': // erase in display
            if (p0 == 0) {
                if (pane->cursor_col < pane->cell_count)
                    pane->cell_count = pane->cursor_col;
                term_clear_lines_after_cursor(pane);
            } else if (p0 == 1) {
                term_clear_lines_before_cursor(pane);
                for (size_t i = 0; i <= pane->cursor_col && i < pane->cell_count; i++)
                    term_blank_cell(pane, &pane->cells[i]);
            } else if (p0 == 2 || p0 == 3) {
                term_clear_display(pane);
            }
            break;
        case 'A': { // cursor up
            int n = p0 > 0 ? p0 : 1;
            size_t target = pane->term_cursor_line;
            size_t floor = pane->term_origin_line;
            if (target <= floor)
                target = floor;
            else if ((size_t)n > target - floor)
                target = floor;
            else
                target -= (size_t)n;
            term_set_cursor_line_col(pane, target, pane->cursor_col);
            break;
        }
        case 'B': { // cursor down
            int n = p0 > 0 ? p0 : 1;
            term_set_cursor_line_col(pane, pane->term_cursor_line + (size_t)n, pane->cursor_col);
            break;
        }
        case 'C': { // cursor forward
            int n = p0 > 0 ? p0 : 1;
            pane->cursor_col += (uint32_t)n;
            break;
        }
        case 'D': { // cursor back
            int n = p0 > 0 ? p0 : 1;
            if ((uint32_t)n > pane->cursor_col)
                pane->cursor_col = 0;
            else
                pane->cursor_col -= (uint32_t)n;
            break;
        }
        case 'G': // cursor to column
            pane->cursor_col = p0 > 0 ? (uint32_t)(p0 - 1) : 0;
            break;
        case 'H':
        case 'f': { // cursor position
            int row = pc >= 1 && params[0] > 0 ? params[0] : 1;
            int col = pc >= 2 ? params[1] : 1;
            size_t target = pane->term_origin_line + (size_t)(row - 1);
            term_set_cursor_line_col(pane, target, col > 0 ? (uint32_t)(col - 1) : 0);
            break;
        }
        case 'd': { // vertical position absolute
            int row = p0 > 0 ? p0 : 1;
            size_t target = pane->term_origin_line + (size_t)(row - 1);
            term_set_cursor_line_col(pane, target, pane->cursor_col);
            break;
        }
        case 'E': { // cursor next line
            int n = p0 > 0 ? p0 : 1;
            term_set_cursor_line_col(pane, pane->term_cursor_line + (size_t)n, 0);
            break;
        }
        case 'F': { // cursor previous line
            int n = p0 > 0 ? p0 : 1;
            size_t target = pane->term_cursor_line;
            size_t floor = pane->term_origin_line;
            if (target <= floor)
                target = floor;
            else if ((size_t)n > target - floor)
                target = floor;
            else
                target -= (size_t)n;
            term_set_cursor_line_col(pane, target, 0);
            break;
        }
        case 's':
            term_save_cursor(pane);
            break;
        case 'u':
            term_restore_cursor(pane);
            break;
        case 'r': { // DECSTBM: set scroll region; cursor homes to the origin
            int top = pc >= 1 && params[0] > 0 ? params[0] - 1 : 0;
            int bottom = pc >= 2 && params[1] > 0 ? params[1] - 1 : term_screen_rows(pane) - 1;
            // Margins spanning the whole screen are equivalent to none; storing
            // them as unset keeps primary-buffer scrollback growing normally
            // after a full-screen program restores CSI r on exit.
            if (bottom > top && !(top == 0 && bottom >= term_screen_rows(pane) - 1)) {
                pane->term_scroll_top = top;
                pane->term_scroll_bottom = bottom;
            } else {
                pane->term_scroll_top = -1;
                pane->term_scroll_bottom = -1;
            }
            term_set_cursor_line_col(pane, pane->term_origin_line, 0);
            break;
        }
        case 'S': { // SU: scroll region (or screen) up n lines
            size_t n = p0 > 0 ? (size_t)p0 : 1;
            term_scroll_range(
                pane, term_region_top_abs(pane), term_region_bottom_abs(pane), n, /*down=*/false);
            break;
        }
        case 'T': { // SD: scroll region (or screen) down n lines
            size_t n = p0 > 0 ? (size_t)p0 : 1;
            term_scroll_range(
                pane, term_region_top_abs(pane), term_region_bottom_abs(pane), n, /*down=*/true);
            break;
        }
        case 'L': { // IL: insert n blank lines at the cursor row
            size_t bottom = term_region_bottom_abs(pane);
            if (pane->term_cursor_line <= bottom) {
                size_t n = p0 > 0 ? (size_t)p0 : 1;
                term_scroll_range(pane, pane->term_cursor_line, bottom, n, /*down=*/true);
            }
            break;
        }
        case 'M': { // DL: delete n lines at the cursor row
            size_t bottom = term_region_bottom_abs(pane);
            if (pane->term_cursor_line <= bottom) {
                size_t n = p0 > 0 ? (size_t)p0 : 1;
                term_scroll_range(pane, pane->term_cursor_line, bottom, n, /*down=*/false);
            }
            break;
        }
        case '@': { // ICH: insert n blank cells at the cursor, shifting right
            size_t n = p0 > 0 ? (size_t)p0 : 1;
            size_t col = pane->cursor_col;
            if (col > pane->cell_count)
                col = pane->cell_count;
            if (!term_ensure_cells(pane, pane->cell_count + n))
                break;
            memmove(&pane->cells[col + n],
                    &pane->cells[col],
                    (pane->cell_count - col) * sizeof(vg_term_cell_t));
            for (size_t i = col; i < col + n; i++)
                term_blank_cell(pane, &pane->cells[i]);
            pane->cell_count += n;
            break;
        }
        case 'P': { // DCH: delete n cells at the cursor, shifting left
            size_t n = p0 > 0 ? (size_t)p0 : 1;
            size_t col = pane->cursor_col;
            if (col >= pane->cell_count)
                break;
            if (n > pane->cell_count - col)
                n = pane->cell_count - col;
            memmove(&pane->cells[col],
                    &pane->cells[col + n],
                    (pane->cell_count - col - n) * sizeof(vg_term_cell_t));
            pane->cell_count -= n;
            break;
        }
        case 'X': { // ECH: blank n cells at the cursor in place
            size_t n = p0 > 0 ? (size_t)p0 : 1;
            for (size_t i = pane->cursor_col; i < pane->cursor_col + n && i < pane->cell_count;
                 i++)
                term_blank_cell(pane, &pane->cells[i]);
            break;
        }
        case 'g': // TBC: 0 = clear stop at cursor, 3 = clear all stops
            if (p0 == 0)
                term_set_tab_stop(pane, pane->cursor_col, false);
            else if (p0 == 3)
                memset(pane->term_tab_stops, 0, sizeof(pane->term_tab_stops));
            break;
        case 'n': // DSR: status (5) and cursor-position (6) reports
            if (p0 == 5) {
                term_queue_input(pane, "\x1b[0n", 4);
            } else if (p0 == 6) {
                char reply[32];
                size_t row = pane->term_cursor_line >= pane->term_origin_line
                                 ? pane->term_cursor_line - pane->term_origin_line + 1
                                 : 1;
                int len = snprintf(reply,
                                   sizeof(reply),
                                   "\x1b[%zu;%uR",
                                   row,
                                   (unsigned)(pane->cursor_col + 1));
                if (len > 0)
                    term_queue_input(pane, reply, (size_t)len);
            }
            break;
        case 'c': // DA: identify as a VT100-with-AVO (primary) / VT220-ish (secondary)
            if (pane->escape_buf[0] == '>')
                term_queue_input(pane, "\x1b[>0;95;0c", 10);
            else
                term_queue_input(pane, "\x1b[?1;2c", 7);
            break;
        case 'h':
        case 'l':
            if (private_mode) {
                const bool set = final == 'h';
                for (int i = 0; i < pc; i++) {
                    if (params[i] == 47 || params[i] == 1047 || params[i] == 1049) {
                        if (set)
                            term_enter_alternate_screen(pane);
                        else
                            term_leave_alternate_screen(pane);
                    } else if (params[i] == 25) {
                        pane->term_cursor_hidden = !set;
                        pane->base.needs_paint = true;
                    } else if (params[i] == 2004) {
                        pane->bracketed_paste = set;
                    } else if (params[i] == 1) {
                        pane->app_cursor_keys = set;
                    }
                }
            }
            break;
        default:
            break; // Unsupported controls are consumed without leaking bytes.
    }
}

/// @brief Terminal-mode append: full escape state machine + cursor-position overwrite.
/// @param pane Terminal-mode output pane receiving the text.
/// @param text NUL-terminated byte stream containing UTF-8 and terminal escapes.
static void outputpane_append_terminal(vg_outputpane_t *pane, const char *text) {
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        unsigned char c = *p;
        switch (pane->esc_state) {
            case 0: // normal
                if (c == 0x1b) {
                    pane->esc_state = 1;
                    p++;
                } else if (c == '\n') {
                    term_newline(pane);
                    p++;
                } else if (c == '\r') {
                    pane->cursor_col = 0;
                    p++;
                } else if (c == '\b') {
                    if (pane->cursor_col > 0)
                        pane->cursor_col--;
                    p++;
                } else if (c == '\t') {
                    // HT only moves the cursor (never writes glyphs), so tabbing
                    // across existing cursor-addressed content leaves it intact.
                    pane->cursor_col = term_next_tab_stop(pane, pane->cursor_col);
                    p++;
                } else if (c < 0x20 || c == 0x7f) {
                    p++; // BEL and other C0 controls: ignore
                } else {
                    int l = outputpane_utf8_len(c);
                    int avail = 0;
                    while (avail < l && p[avail])
                        avail++;
                    if (avail < l)
                        l = avail > 0 ? avail : 1;
                    term_put_glyph(pane, (const char *)p, l);
                    p += l;
                }
                break;
            case 1: // after ESC
                if (c == '[') {
                    pane->esc_state = 2;
                    pane->escape_len = 0;
                    pane->escape_buf[0] = '\0';
                } else if (c == ']') {
                    pane->esc_state = 3; // OSC
                } else if (c == '(' || c == ')' || c == '*' || c == '+') {
                    pane->esc_state = 4; // charset designator (consume next byte)
                } else if (c == '7') {
                    term_save_cursor(pane);
                    pane->esc_state = 0;
                } else if (c == '8') {
                    term_restore_cursor(pane);
                    pane->esc_state = 0;
                } else if (c == 'D') {
                    // IND: at the bottom margin of an active region, scroll the
                    // region up (column preserved) instead of advancing rows.
                    if (term_region_active(pane) &&
                        pane->term_cursor_line == term_region_bottom_abs(pane)) {
                        term_scroll_range(pane,
                                          term_region_top_abs(pane),
                                          term_region_bottom_abs(pane),
                                          1,
                                          /*down=*/false);
                    } else {
                        term_set_cursor_line_col(
                            pane, pane->term_cursor_line + 1, pane->cursor_col);
                    }
                    pane->esc_state = 0;
                } else if (c == 'E') {
                    term_newline(pane);
                    pane->esc_state = 0;
                } else if (c == 'M') {
                    // RI: at the top margin of an active region, scroll the
                    // region down; otherwise move up within the screen.
                    if (term_region_active(pane) &&
                        pane->term_cursor_line == term_region_top_abs(pane)) {
                        term_scroll_range(pane,
                                          term_region_top_abs(pane),
                                          term_region_bottom_abs(pane),
                                          1,
                                          /*down=*/true);
                    } else if (pane->term_cursor_line > pane->term_origin_line) {
                        term_set_cursor_line_col(
                            pane, pane->term_cursor_line - 1, pane->cursor_col);
                    }
                    pane->esc_state = 0;
                } else if (c == 'H') {
                    term_set_tab_stop(pane, pane->cursor_col, true); // HTS
                    pane->esc_state = 0;
                } else if (c == 'c') {
                    pane->current_fg = pane->default_fg;
                    pane->current_bg = 0;
                    pane->ansi_bold = false;
                    pane->ansi_reverse = false;
                    pane->term_scroll_top = -1;
                    pane->term_scroll_bottom = -1;
                    pane->term_cursor_hidden = false;
                    pane->bracketed_paste = false;
                    pane->app_cursor_keys = false;
                    term_init_default_tabs(pane);
                    term_clear_display(pane);
                    pane->esc_state = 0;
                } else {
                    pane->esc_state = 0; // Unsupported ESC controls are swallowed.
                }
                p++;
                break;
            case 2: // CSI
                if (c >= 0x40 && c <= 0x7e) {
                    term_dispatch_csi(pane, (char)c);
                    pane->esc_state = 0;
                } else if (pane->escape_len < (int)sizeof(pane->escape_buf) - 1) {
                    pane->escape_buf[pane->escape_len++] = (char)c;
                    pane->escape_buf[pane->escape_len] = '\0';
                }
                p++;
                break;
            case 3: // OSC — consume until BEL or ST (ESC \)
                if (c == 0x07)
                    pane->esc_state = 0;
                else if (c == 0x1b)
                    pane->esc_state = 5;
                p++;
                break;
            case 5: // OSC saw ESC, expect '\' (ST)
                pane->esc_state = 0;
                p++;
                break;
            case 4: // charset designator — consume one byte
                pane->esc_state = 0;
                p++;
                break;
            default:
                pane->esc_state = 0;
                p++;
                break;
        }
    }

    term_flush_cells(pane);
    if (pane->auto_scroll && !pane->scroll_locked)
        vg_outputpane_scroll_to_bottom(pane);
    pane->base.needs_paint = true;
}

/// @brief Queue raw bytes for the controller to drain to the PTY (terminal keystrokes).
/// @param pane Output pane that owns the pending-input buffer.
/// @param bytes Raw input bytes to append.
/// @param len Number of bytes available at @p bytes.
static void term_queue_input(vg_outputpane_t *pane, const char *bytes, size_t len) {
    if (len == 0)
        return;
    // Keep the caret solid (not mid-blink) while the user is actively typing.
    pane->caret_visible = true;
    pane->caret_blink_time = 0.0f;
    if (pane->pending_len + len + 1 > pane->pending_capacity) {
        size_t nc = pane->pending_capacity ? pane->pending_capacity * 2 : 64;
        while (nc < pane->pending_len + len + 1)
            nc *= 2;
        char *nb = realloc(pane->pending_input, nc);
        if (!nb)
            return;
        pane->pending_input = nb;
        pane->pending_capacity = nc;
    }
    memcpy(pane->pending_input + pane->pending_len, bytes, len);
    pane->pending_len += len;
}

/// @brief Enable/disable interactive terminal mode (see header).
/// @copydetails vg_outputpane_set_terminal_mode
void vg_outputpane_set_terminal_mode(vg_outputpane_t *pane, bool enabled) {
    if (!pane)
        return;
    if (!enabled && pane->alternate_screen)
        term_leave_alternate_screen(pane);
    pane->terminal_mode = enabled;
    if (enabled) {
        if (pane->line_count == 0)
            (void)add_line(pane);
        pane->term_cursor_line = pane->line_count > 0 ? pane->line_count - 1 : 0;
        pane->term_origin_line = 0;
        pane->saved_cursor_line = pane->term_cursor_line;
        pane->saved_cursor_col = pane->cursor_col;
        pane->term_scroll_top = -1;
        pane->term_scroll_bottom = -1;
        pane->term_cursor_hidden = false;
        pane->bracketed_paste = false;
        pane->app_cursor_keys = false;
        pane->ansi_reverse = false;
        term_init_default_tabs(pane);
        term_load_cells_from_line(pane, pane->term_cursor_line);
    }
}

/// @brief Drain queued keystroke bytes with an explicit byte length.
///
/// @details Terminal input is binary at the PTY boundary even though most
///          keystrokes are printable UTF-8 or ASCII escape sequences. This helper
///          preserves embedded NUL bytes and returns the authoritative length
///          through @p len_out while still appending a trailing NUL byte for
///          diagnostics and compatibility callers.
///
/// @param pane Terminal-mode output pane whose queued bytes should be drained.
/// @param len_out Optional output receiving the number of bytes copied.
/// @return Heap buffer owned by the caller, or NULL when no bytes are queued.
char *vg_outputpane_take_input_bytes(vg_outputpane_t *pane, size_t *len_out) {
    if (len_out)
        *len_out = 0;
    if (!pane || pane->pending_len == 0)
        return NULL;
    char *out = malloc(pane->pending_len + 1);
    if (!out)
        return NULL;
    memcpy(out, pane->pending_input, pane->pending_len);
    out[pane->pending_len] = '\0';
    if (len_out)
        *len_out = pane->pending_len;
    pane->pending_len = 0;
    return out;
}

/// @brief Drain queued keystroke bytes; caller frees. NULL when empty.
/// @copydetails vg_outputpane_take_input
char *vg_outputpane_take_input(vg_outputpane_t *pane) {
    return vg_outputpane_take_input_bytes(pane, NULL);
}

/// @brief Advance the terminal caret blink (no-op unless terminal mode + focused).
/// @copydetails vg_outputpane_tick
void vg_outputpane_tick(vg_outputpane_t *pane, float dt) {
    if (!pane || !pane->terminal_mode || !pane->has_focus || dt <= 0.0f)
        return;
    pane->caret_blink_time += dt;
    if (pane->caret_blink_time >= 0.5f) {
        pane->caret_blink_time -= 0.5f;
        pane->caret_visible = !pane->caret_visible;
        pane->base.needs_paint = true;
    }
}

/// @brief Append a UTF-8 string to the output pane, interpreting ANSI SGR escape sequences.
///
/// @details Newlines split the current line and begin a new one.  Escape sequences update
///          the current foreground/background/bold state carried into subsequent segments.
///          ANSI state is preserved across calls, so multi-chunk appends render correctly.
///          Auto-scrolls to the bottom unless scroll_locked is set.
///
/// @param pane The output pane to append to.
/// @param text UTF-8 text to append; may be NULL (no-op).
void vg_outputpane_append(vg_outputpane_t *pane, const char *text) {
    if (!pane || !text)
        return;
    if (pane->terminal_mode) {
        outputpane_append_terminal(pane, text);
        return;
    }

    // Get or create current line
    vg_output_line_t *line = NULL;
    if (pane->line_count > 0) {
        line = outputpane_last_line(pane);
    } else {
        line = add_line(pane);
        if (!line)
            return;
    }

    const char *p = text;
    const char *segment_start = p;

    while (*p) {
        if (*p == '\033') {
            // Flush pending text
            if (p > segment_start) {
                (void)outputpane_append_segment_copy(line,
                                                     segment_start,
                                                     (size_t)(p - segment_start),
                                                     pane->current_fg,
                                                     pane->current_bg,
                                                     pane->ansi_bold);
            }

            // Start escape sequence
            pane->in_escape = true;
            pane->escape_len = 0;
            p++;
            segment_start = p;
        } else if (pane->in_escape) {
            // Accumulate escape sequence
            if (pane->escape_len < (int)sizeof(pane->escape_buf) - 1) {
                pane->escape_buf[pane->escape_len++] = *p;
                pane->escape_buf[pane->escape_len] = '\0';
            } else {
                pane->in_escape = false;
                pane->escape_len = 0;
                segment_start = p + 1;
            }

            if (pane->in_escape && *p >= '@' && *p <= '~' && *p != '[') {
                // End of a CSI/escape sequence on its final byte (m, h, l, A-K, ...).
                // CSI parameter/intermediate bytes are all < 0x40, so the first byte
                // in 0x40-0x7E (other than the '[' introducer) is the terminator.
                // Only SGR ("...m") changes color; other finals (e.g. the shell's
                // ESC[?1034h) are consumed and ignored rather than eating later text.
                process_ansi_escape(pane);
                segment_start = p + 1;
            }
            p++;
        } else if (*p == '\r') {
            // Carriage return: flush pending text and drop the CR. The line-append
            // model has no cursor column, so CRLF collapses to one newline and a
            // bare CR (line-rewrite) is dropped rather than rendered as a stray byte.
            if (p > segment_start) {
                (void)outputpane_append_segment_copy(line,
                                                     segment_start,
                                                     (size_t)(p - segment_start),
                                                     pane->current_fg,
                                                     pane->current_bg,
                                                     pane->ansi_bold);
            }
            p++;
            segment_start = p;
        } else if (*p == '\n') {
            // Flush pending text
            if (p > segment_start) {
                (void)outputpane_append_segment_copy(line,
                                                     segment_start,
                                                     (size_t)(p - segment_start),
                                                     pane->current_fg,
                                                     pane->current_bg,
                                                     pane->ansi_bold);
            }

            // Start new line
            line = add_line(pane);
            if (!line)
                return;

            p++;
            segment_start = p;
        } else {
            p++;
        }
    }

    // Flush remaining text
    if (p > segment_start && !pane->in_escape) {
        (void)outputpane_append_segment_copy(line,
                                             segment_start,
                                             (size_t)(p - segment_start),
                                             pane->current_fg,
                                             pane->current_bg,
                                             pane->ansi_bold);
    }

    // Auto-scroll
    if (pane->auto_scroll && !pane->scroll_locked) {
        vg_outputpane_scroll_to_bottom(pane);
    }

    pane->base.needs_paint = true;
}

/// @brief Append text as a complete new line, ensuring it lands on its own row.
///
/// @details If the last line already contains content a new blank line is inserted
///          first, then text is appended via vg_outputpane_append.  A trailing
///          blank line is also added after the content so subsequent appends begin
///          on a fresh line.
///
/// @param pane The output pane to append to.
/// @param text Text to append as a new line; NULL appends an empty line.
void vg_outputpane_append_line(vg_outputpane_t *pane, const char *text) {
    if (!pane)
        return;

    if (pane->line_count == 0) {
        if (!add_line(pane))
            return;
    } else if (outputpane_line_length(outputpane_last_line(pane)) > 0) {
        if (!add_line(pane))
            return;
    }

    if (text && *text)
        vg_outputpane_append(pane, text);

    if (pane->max_lines > 1)
        (void)add_line(pane);

    if (pane->auto_scroll && !pane->scroll_locked)
        vg_outputpane_scroll_to_bottom(pane);
    pane->base.needs_paint = true;
}

/// @brief Append a pre-styled text segment without ANSI parsing.
///
/// @details The entire text string is added as a single styled segment with the
///          given colours and bold flag.  No newline splitting or ANSI processing
///          is performed — use vg_outputpane_append for that.
///
/// @param pane The output pane to append to.
/// @param text Text to append; may be NULL (no-op).
/// @param fg   Foreground colour as AARRGGBB.
/// @param bg   Background colour as AARRGGBB (0 = transparent).
/// @param bold true to render the segment in bold.
void vg_outputpane_append_styled(
    vg_outputpane_t *pane, const char *text, uint32_t fg, uint32_t bg, bool bold) {
    if (!pane || !text)
        return;

    // Get or create current line
    vg_output_line_t *line = NULL;
    if (pane->line_count > 0) {
        line = outputpane_last_line(pane);
    } else {
        line = add_line(pane);
        if (!line)
            return;
    }

    (void)outputpane_append_segment_copy(line, text, strlen(text), fg, bg, bold);

    // Auto-scroll
    if (pane->auto_scroll && !pane->scroll_locked) {
        vg_outputpane_scroll_to_bottom(pane);
    }

    pane->base.needs_paint = true;
}

/// @brief Remove all lines, reset ANSI state, and scroll back to the top.
///
/// @param pane The output pane to clear; may be NULL.
void vg_outputpane_clear(vg_outputpane_t *pane) {
    if (!pane)
        return;

    outputpane_free_line_storage(pane->lines, pane->line_start, pane->line_count, pane->line_capacity);
    pane->line_count = 0;
    pane->line_start = 0;
    outputpane_free_line_storage(pane->primary_lines,
                                 pane->primary_line_start,
                                 pane->primary_line_count,
                                 pane->primary_line_capacity);
    free(pane->primary_lines);
    pane->primary_lines = NULL;
    pane->primary_line_start = 0;
    pane->primary_line_count = 0;
    pane->primary_line_capacity = 0;
    pane->primary_scroll_y = 0;
    pane->primary_scroll_locked = false;
    pane->primary_term_cursor_line = 0;
    pane->primary_term_origin_line = 0;
    pane->primary_saved_cursor_line = 0;
    pane->primary_saved_cursor_col = 0;
    pane->primary_cursor_col = 0;
    pane->alternate_screen = false;

    // Reset ANSI state
    pane->current_fg = pane->default_fg;
    pane->current_bg = 0;
    pane->ansi_bold = false;
    pane->in_escape = false;
    pane->escape_len = 0;

    // Reset terminal-mode cursor / escape / input state.
    pane->esc_state = 0;
    pane->term_cursor_line = 0;
    pane->term_origin_line = 0;
    pane->saved_cursor_line = 0;
    pane->saved_cursor_col = 0;
    pane->cursor_col = 0;
    pane->cell_count = 0;
    pane->pending_len = 0;
    if (pane->terminal_mode)
        (void)add_line(pane);

    // Reset scroll
    pane->scroll_y = 0;
    pane->scroll_locked = false;
    outputpane_clear_selection(pane);

    pane->base.needs_paint = true;
}

/// @brief Scroll the output pane to the last line and clear scroll_locked.
///
/// @param pane The output pane to scroll; may be NULL.
void vg_outputpane_scroll_to_bottom(vg_outputpane_t *pane) {
    if (!pane)
        return;

    float content_height = pane->line_count * pane->line_height;
    float max_scroll = content_height - pane->base.height;
    if (max_scroll < 0)
        max_scroll = 0;

    pane->scroll_y = max_scroll;
    pane->scroll_locked = false;
    pane->base.needs_paint = true;
}

/// @brief Scroll the output pane to the first line and set scroll_locked.
///
/// @param pane The output pane to scroll; may be NULL.
void vg_outputpane_scroll_to_top(vg_outputpane_t *pane) {
    if (!pane)
        return;

    pane->scroll_y = 0;
    pane->scroll_locked = true;
    pane->base.needs_paint = true;
}

/// @brief Enable or disable automatic scrolling to the bottom on new content.
///
/// @param pane        The output pane to configure; may be NULL.
/// @param auto_scroll true to auto-scroll on append; false to keep the current position.
void vg_outputpane_set_auto_scroll(vg_outputpane_t *pane, bool auto_scroll) {
    if (!pane)
        return;
    pane->auto_scroll = auto_scroll;
}

/// @brief Return a heap-allocated string containing the currently selected text.
///
/// @details Lines in the selection are joined with newline characters.  The
///          caller is responsible for freeing the returned string.
///
/// @param pane The output pane to query.
/// @return Heap-allocated UTF-8 string, or NULL if there is no selection or on
///         allocation failure.
char *vg_outputpane_get_selection(vg_outputpane_t *pane) {
    if (!pane || !pane->has_selection || pane->line_count == 0)
        return NULL;

    // Normalize selection bounds
    uint32_t start_line = pane->sel_start_line;
    uint32_t start_col = pane->sel_start_col;
    uint32_t end_line = pane->sel_end_line;
    uint32_t end_col = pane->sel_end_col;
    if (start_line > end_line || (start_line == end_line && start_col > end_col)) {
        uint32_t tmp = start_line;
        start_line = end_line;
        end_line = tmp;
        tmp = start_col;
        start_col = end_col;
        end_col = tmp;
    }

    // Clamp to actual line range
    if (start_line >= (uint32_t)pane->line_count)
        return NULL;
    if (end_line >= (uint32_t)pane->line_count)
        end_line = (uint32_t)(pane->line_count - 1);

    // First pass: calculate required buffer size
    size_t total = 0;
    for (uint32_t li = start_line; li <= end_line; li++) {
        vg_output_line_t *line = outputpane_line_at(pane, li);
        if (!line)
            continue;
        size_t col = 0;
        for (size_t si = 0; si < line->segment_count; si++) {
            const char *seg = line->segments[si].text;
            if (!seg)
                continue;
            for (size_t ci = 0; seg[ci] != '\0';) {
                size_t cp_len = outputpane_utf8_next_len(seg + ci);
                if (cp_len == 0)
                    break;
                if (li == start_line && col < start_col)
                    goto next_codepoint_count;
                if (li == end_line && col >= end_col && end_col != UINT32_MAX)
                    break;
                if (cp_len > SIZE_MAX - total)
                    return NULL;
                total += cp_len;
            next_codepoint_count:
                ci += cp_len;
                col++;
            }
        }
        if (li < end_line) {
            if (total == SIZE_MAX)
                return NULL;
            total++; // newline between lines
        }
    }

    if (total == 0)
        return NULL;

    char *buf = malloc(total + 1);
    if (!buf)
        return NULL;

    // Second pass: copy selected text
    size_t out = 0;
    for (uint32_t li = start_line; li <= end_line; li++) {
        vg_output_line_t *line = outputpane_line_at(pane, li);
        if (!line)
            continue;
        size_t col = 0;
        for (size_t si = 0; si < line->segment_count; si++) {
            const char *seg = line->segments[si].text;
            if (!seg)
                continue;
            for (size_t ci = 0; seg[ci] != '\0';) {
                size_t cp_len = outputpane_utf8_next_len(seg + ci);
                if (cp_len == 0)
                    break;
                if (li == start_line && col < start_col)
                    goto next_codepoint_copy;
                if (li == end_line && col >= end_col && end_col != UINT32_MAX)
                    break;
                memcpy(buf + out, seg + ci, cp_len);
                out += cp_len;
            next_codepoint_copy:
                ci += cp_len;
                col++;
            }
        }
        if (li < end_line)
            buf[out++] = '\n';
    }
    buf[out] = '\0';
    return buf;
}

/// @brief Select all text in the output pane from the first to the last line.
///
/// @param pane The output pane to update; may be NULL.
void vg_outputpane_select_all(vg_outputpane_t *pane) {
    if (!pane || pane->line_count == 0)
        return;

    pane->has_selection = true;
    pane->sel_start_line = 0;
    pane->sel_start_col = 0;
    pane->sel_end_line = (uint32_t)(pane->line_count - 1);
    pane->sel_end_col = UINT32_MAX; // End of line

    pane->base.needs_paint = true;
}

/// @brief Set the maximum number of lines retained in the ring buffer.
///
/// @details If the current line count exceeds max, the oldest lines are evicted
///          immediately.  Minimum effective value is 1.
///
/// @param pane The output pane to configure; may be NULL.
/// @param max  Maximum number of lines to retain; 0 is clamped to 1.
void vg_outputpane_set_max_lines(vg_outputpane_t *pane, size_t max) {
    if (!pane)
        return;
    if (max == 0)
        max = 1;
    pane->max_lines = max;
    while (pane->line_count > pane->max_lines) {
        outputpane_evict_first_line(pane);
    }
    pane->base.needs_paint = true;
}

/// @brief Set the font used to measure and render text in the output pane.
///
/// @param pane The output pane to configure; may be NULL.
/// @param font The font to use; must outlive the pane.
/// @param size Point size for text rendering.
void vg_outputpane_set_font(vg_outputpane_t *pane, vg_font_t *font, float size) {
    if (!pane)
        return;
    if (pane->font == font && pane->font_size == size)
        return;

    pane->font = font;
    pane->font_size = size;
    if (font && size > 0.0f) {
        vg_font_metrics_t metrics = {0};
        vg_font_get_metrics(font, size, &metrics);
        if (metrics.line_height > 0)
            pane->line_height = (float)metrics.line_height;
        else
            pane->line_height = size * 1.35f;
    }
    pane->base.needs_layout = true;
    pane->base.needs_paint = true;
}

/// @brief Measure the terminal's representative monospace cell width.
/// @param pane Output pane supplying font and size.
/// @return Rounded width of the `M` glyph in pixels, or zero when unavailable.
int vg_outputpane_cell_width(const vg_outputpane_t *pane) {
    if (!pane || !pane->font || pane->font_size <= 0.0f)
        return 0;
    vg_text_metrics_t metrics = {0};
    vg_font_measure_text(pane->font, pane->font_size, "M", &metrics);
    if (metrics.width <= 0.0f)
        return 0;
    return (int)(metrics.width + 0.5f);
}

/// @brief Query the pane's configured terminal cell height.
/// @param pane Output pane to inspect.
/// @return Rounded line height in pixels, or zero when unavailable.
int vg_outputpane_cell_height(const vg_outputpane_t *pane) {
    if (!pane || !pane->font || pane->line_height <= 0.0f)
        return 0;
    return (int)(pane->line_height + 0.5f);
}

/// @brief Measure a complete string using the output pane's active font.
/// @param pane Output pane supplying font and size.
/// @param text Null-terminated text to measure.
/// @return Rounded rendered width in pixels, or zero for invalid/empty input.
int vg_outputpane_measure_text(const vg_outputpane_t *pane, const char *text) {
    if (!pane || !pane->font || pane->font_size <= 0.0f || !text || text[0] == '\0')
        return 0;
    vg_text_metrics_t metrics = {0};
    vg_font_measure_text(pane->font, pane->font_size, text, &metrics);
    if (metrics.width <= 0.0f)
        return 0;
    return (int)(metrics.width + 0.5f);
}

/// @brief Convert the arranged pane width to whole terminal columns.
/// @param pane Arranged output pane to inspect.
/// @return Number of representative cells that fit across the width, or zero.
int vg_outputpane_columns_for_width(const vg_outputpane_t *pane) {
    int cell = vg_outputpane_cell_width(pane);
    if (cell <= 0 || pane->base.width <= 0.0f)
        return 0;
    return (int)(pane->base.width / (float)cell);
}

/// @brief Convert the arranged pane height to whole terminal rows.
/// @param pane Arranged output pane to inspect.
/// @return Number of configured line-height cells that fit vertically, or zero.
int vg_outputpane_rows_for_height(const vg_outputpane_t *pane) {
    int cell = vg_outputpane_cell_height(pane);
    if (cell <= 0 || pane->base.height <= 0.0f)
        return 0;
    return (int)(pane->base.height / (float)cell);
}
