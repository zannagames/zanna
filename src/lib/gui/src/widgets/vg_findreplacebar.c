//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
///
/// @file vg_findreplacebar.c
/// @brief Implements the code editor's interactive find-and-replace toolbar.
///
/// @details The bar owns text inputs, navigation and replacement buttons,
/// search-option checkboxes, and a dynamically grown match array. Every query or
/// option change rebuilds matches across the linked live code editor. The
/// current match is selected in the editor and scrolled into view.
///
/// Literal search is UTF-8 column-aware. Regex search uses the same bounded
/// in-tree engine as `Zanna.Text.CompiledPattern` on every platform. Interactive
/// edits are debounced and every scan has byte/result caps. Replace-all runs in
/// reverse document order and expands `$0`, `$1`... capture references.
///
/// Child widgets are owned by the widget hierarchy. The match array is owned
/// directly by the bar.
///
/// @see vg_ide_widgets.h
/// @see vg_widgets.h
/// @see vg_theme.h
/// @see vg_event.h
//
//===----------------------------------------------------------------------===//
#include "../../../graphics/include/vgfx.h"
#include "../../include/vg_event.h"
#include "../../include/vg_ide_widgets.h"
#include "../../include/vg_theme.h"
#include "../../include/vg_widgets.h"
#include "rt_regex_internal.h"
#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//=============================================================================
// Constants
//=============================================================================

#define FINDREPLACEBAR_HEIGHT 36
#define FINDREPLACEBAR_HEIGHT_REPLACE 72
#define INPUT_WIDTH 200
#define BUTTON_WIDTH 24
#define PADDING 4
#define INITIAL_MATCH_CAPACITY 64
#define FINDREPLACEBAR_MAX_MATCHES 10000u
#define FINDREPLACEBAR_MAX_SCAN_BYTES (16u * 1024u * 1024u)
#define FINDREPLACEBAR_DEBOUNCE_SECONDS 0.15f

//=============================================================================
// Forward Declarations
//=============================================================================

static void findreplacebar_destroy(vg_widget_t *widget);
static void findreplacebar_measure(vg_widget_t *widget,
                                   float available_width,
                                   float available_height);
static void findreplacebar_arrange(
    vg_widget_t *widget, float x, float y, float width, float height);
static void findreplacebar_paint(vg_widget_t *widget, void *canvas);
static bool findreplacebar_handle_event(vg_widget_t *widget, vg_event_t *event);
static void findreplacebar_set_font_widget(vg_widget_t *widget, void *font, float size);

static void perform_search(vg_findreplacebar_t *bar);
static void clear_matches(vg_findreplacebar_t *bar);
static bool add_match(vg_findreplacebar_t *bar, uint32_t line, uint32_t start, uint32_t end);
static const char *find_in_line(const char *text,
                                const char *query,
                                vg_search_options_t *options,
                                size_t *match_len);
static const char *find_regex_in_line(const char *text,
                                      const char *start,
                                      re_compiled_pattern *regex,
                                      vg_search_options_t *options,
                                      size_t *match_len);
static void highlight_current_match(vg_findreplacebar_t *bar);
static void update_result_text(vg_findreplacebar_t *bar);

// Button callbacks
static void on_find_prev_click(vg_widget_t *btn, void *user_data);
static void on_find_next_click(vg_widget_t *btn, void *user_data);
static void on_replace_click(vg_widget_t *btn, void *user_data);
static void on_replace_all_click(vg_widget_t *btn, void *user_data);
static void on_close_click(vg_widget_t *btn, void *user_data);
static void on_option_change(vg_widget_t *cb, bool checked, void *user_data);
static void on_find_text_change(vg_widget_t *input, const char *text, void *user_data);

//=============================================================================
// FindReplaceBar VTable
//=============================================================================

static vg_widget_vtable_t g_findreplacebar_vtable = {.destroy = findreplacebar_destroy,
                                                     .measure = findreplacebar_measure,
                                                     .arrange = findreplacebar_arrange,
                                                     .paint = findreplacebar_paint,
                                                     .handle_event = findreplacebar_handle_event,
                                                     .can_focus = NULL,
                                                     .on_focus = NULL,
                                                     .set_font = findreplacebar_set_font_widget};

//=============================================================================
// Helper Functions - Case Insensitive String Search
//=============================================================================

/// @brief Performs a portable ASCII case-insensitive substring search.
///
/// @param haystack Null-terminated text to search.
/// @param needle Null-terminated query; an empty query matches @p haystack.
/// @return Pointer to the first match in @p haystack, or null.
static const char *strcasestr_custom(const char *haystack, const char *needle) {
    if (!*needle)
        return haystack;

    for (; *haystack; haystack++) {
        const char *h = haystack;
        const char *n = needle;

        while (*n && tolower((unsigned char)*h) == tolower((unsigned char)*n)) {
            h++;
            n++;
        }

        if (!*n)
            return haystack;
    }
    return NULL;
}

/// @brief Adapts the widget font vtable call to the typed bar API.
///
/// @param widget Find/replace bar base widget.
/// @param font Borrowed font; null calls are ignored.
/// @param size Font size to propagate.
static void findreplacebar_set_font_widget(vg_widget_t *widget, void *font, float size) {
    if (!widget || !font)
        return;
    vg_findreplacebar_set_font((vg_findreplacebar_t *)widget, (vg_font_t *)font, size);
}

/// @brief Tests whether a byte is a UTF-8 continuation byte.
///
/// @param c Byte to classify.
/// @return `true` when the two high bits are `10`.
static bool fr_utf8_is_continuation(unsigned char c) {
    return (c & 0xC0u) == 0x80u;
}

/// @brief Advance @p p past one UTF-8 codepoint (stops at NUL); NULL/empty
///        input returns @p p unchanged.
/// @param p Current UTF-8 sequence start.
/// @return Pointer to the next sequence start or terminator.
static const char *fr_utf8_next(const char *p) {
    if (!p || !*p)
        return p;
    p++;
    while (*p && fr_utf8_is_continuation((unsigned char)*p))
        p++;
    return p;
}

/// @brief Counts complete UTF-8 sequences before a byte offset.
///
/// @param text Null-terminated UTF-8 text.
/// @param byte_offset Exclusive byte limit.
/// @return Number of complete code-point columns before the limit.
static uint32_t fr_utf8_col_from_byte_offset(const char *text, size_t byte_offset) {
    if (!text)
        return 0;
    const char *cursor = text;
    const char *limit = text + byte_offset;
    uint32_t col = 0;
    while (*cursor && cursor < limit) {
        const char *next = fr_utf8_next(cursor);
        if (next <= cursor)
            break;
        if (next > limit)
            break;
        cursor = next;
        col++;
    }
    return col;
}

/// @brief Convert a code-point column to a clamped UTF-8 byte offset.
static size_t fr_utf8_byte_offset_from_col(const char *text, uint32_t column) {
    if (!text)
        return 0;
    const char *cursor = text;
    uint32_t current = 0;
    while (*cursor && current < column) {
        const char *next = fr_utf8_next(cursor);
        if (!next || next <= cursor)
            break;
        cursor = next;
        current++;
    }
    return (size_t)(cursor - text);
}

/// @brief Walk back from @p p to the start of the previous UTF-8 codepoint
///        within @p text; returns NULL if @p p is already at the start.
/// @param text Beginning of the UTF-8 buffer.
/// @param p Current position in the same buffer.
/// @return Previous sequence start, or null when none exists.
static const char *fr_utf8_prev_start(const char *text, const char *p) {
    if (!text || !p || p <= text)
        return NULL;
    p--;
    while (p > text && fr_utf8_is_continuation((unsigned char)*p))
        p--;
    return p;
}

/// @brief Decode the UTF-8 codepoint at @p p, reading at most @p max_len bytes.
///        Returns 0 on empty input, U+FFFD on a malformed/truncated sequence.
/// @param p Current UTF-8 byte position.
/// @param max_len Maximum readable byte count.
/// @return Decoded code point, zero for empty input, or U+FFFD when malformed.
static uint32_t fr_utf8_decode_at(const char *p, size_t max_len) {
    const unsigned char *s = (const unsigned char *)p;
    if (!s || max_len == 0)
        return 0;
    if (s[0] < 0x80)
        return s[0];
    if (max_len >= 2 && (s[0] & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80)
        return ((uint32_t)(s[0] & 0x1F) << 6) | (uint32_t)(s[1] & 0x3F);
    if (max_len >= 3 && (s[0] & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80)
        return ((uint32_t)(s[0] & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6) |
               (uint32_t)(s[2] & 0x3F);
    if (max_len >= 4 && (s[0] & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 &&
        (s[3] & 0xC0) == 0x80)
        return ((uint32_t)(s[0] & 0x07) << 18) | ((uint32_t)(s[1] & 0x3F) << 12) |
               ((uint32_t)(s[2] & 0x3F) << 6) | (uint32_t)(s[3] & 0x3F);
    return 0xFFFD;
}

/// @brief True if @p cp is a Unicode whole-word boundary.
/// @param cp Unicode code point to classify.
/// @return `true` when the code point does not continue a Unicode word.
static bool is_word_boundary_codepoint(uint32_t cp) {
    return cp == 0 || !re_is_word_codepoint(cp);
}

/// @brief Tests whether a match is surrounded by supported word boundaries.
///
/// @param text Beginning of the full line.
/// @param match Pointer into @p text at the match.
/// @param match_len Match length in bytes.
/// @return `true` when both adjacent code points are boundaries or buffer edges.
static bool check_whole_word(const char *text, const char *match, size_t match_len) {
    // Check character before match
    const char *prev = fr_utf8_prev_start(text, match);
    if (prev) {
        uint32_t cp = fr_utf8_decode_at(prev, (size_t)(match - prev));
        if (!is_word_boundary_codepoint(cp))
            return false;
    }
    // Check character after match
    if (match[match_len]) {
        uint32_t cp = fr_utf8_decode_at(match + match_len, strlen(match + match_len));
        if (!is_word_boundary_codepoint(cp))
            return false;
    }
    return true;
}

//=============================================================================
// Search Implementation
//=============================================================================

/// @brief Find the first occurrence of @p query in @p text, honoring the
///        case-sensitive and whole-word flags in @p options.
/// @param text Line or line suffix to search.
/// @param query Non-empty literal query.
/// @param options Search options controlling case and whole-word matching.
/// @param[out] match_len Receives the matched byte length on success.
/// @return Pointer into @p text at the match, or NULL if not found.
static const char *find_in_line(const char *text,
                                const char *query,
                                vg_search_options_t *options,
                                size_t *match_len) {
    if (!text || !query || !*query)
        return NULL;

    *match_len = strlen(query);
    const char *pos = text;

    while (*pos) {
        const char *found;

        if (options->case_sensitive) {
            found = strstr(pos, query);
        } else {
            found = strcasestr_custom(pos, query);
        }

        if (!found)
            return NULL;

        // Check whole word if required
        if (options->whole_word) {
            if (!check_whole_word(text, found, *match_len)) {
                pos = fr_utf8_next(found);
                continue;
            }
        }

        return found;
    }

    return NULL;
}

/// @brief Find the next match with the shared bounded runtime regex engine.
/// @details Matches, anchors, case options, and malformed-pattern behavior are
///          identical on Windows, macOS, and Linux. Zero-width matches are
///          returned to the caller, which owns progress between occurrences.
static const char *find_regex_in_line(const char *text,
                                      const char *start,
                                      re_compiled_pattern *regex,
                                      vg_search_options_t *options,
                                      size_t *match_len) {
    if (!text || !start || !regex || !options || !match_len || start < text)
        return NULL;
    size_t text_size = strlen(text);
    size_t start_size = (size_t)(start - text);
    if (text_size > (size_t)INT32_MAX || start_size > text_size)
        return NULL;

    int search_offset = (int)start_size;
    for (;;) {
        int match_start = 0;
        int match_end = 0;
        if (!re_find_match(regex, text, (int)text_size, search_offset, &match_start, &match_end) ||
            match_start < search_offset || match_end < match_start || match_end > (int)text_size)
            return NULL;

        const char *found = text + match_start;
        size_t length = (size_t)(match_end - match_start);
        if (!options->whole_word || check_whole_word(text, found, length)) {
            *match_len = length;
            return found;
        }

        if (match_start >= (int)text_size)
            return NULL;
        const char *next = fr_utf8_next(found);
        if (!next || next <= found)
            return NULL;
        search_offset = (int)(next - text);
    }
}

/// @brief Appends one match record, growing the owned array as needed.
///
/// @param bar Bar that owns the match array.
/// @param line Zero-based editor line.
/// @param start Starting UTF-8 code-point column.
/// @param end Exclusive ending UTF-8 code-point column.
static bool add_match(vg_findreplacebar_t *bar, uint32_t line, uint32_t start, uint32_t end) {
    if (!bar || bar->match_count >= FINDREPLACEBAR_MAX_MATCHES) {
        if (bar)
            bar->search_truncated = true;
        return false;
    }
    // Grow array if needed
    if (bar->match_count >= bar->match_capacity) {
        size_t new_cap = bar->match_capacity ? bar->match_capacity : INITIAL_MATCH_CAPACITY;
        if (new_cap < INITIAL_MATCH_CAPACITY)
            new_cap = INITIAL_MATCH_CAPACITY;
        while (new_cap <= bar->match_count) {
            if (new_cap > SIZE_MAX / 2)
                return false;
            new_cap *= 2;
        }
        if (new_cap > SIZE_MAX / sizeof(vg_search_match_t))
            return false;

        vg_search_match_t *new_matches = realloc(bar->matches, new_cap * sizeof(vg_search_match_t));
        if (!new_matches)
            return false;

        bar->matches = new_matches;
        bar->match_capacity = new_cap;
    }

    bar->matches[bar->match_count].line = line;
    bar->matches[bar->match_count].start_col = start;
    bar->matches[bar->match_count].end_col = end;
    bar->match_count++;
    return true;
}

/// @brief Validates and returns the bar's linked code editor.
///
/// @details A stale or wrongly typed target is detached before returning null.
///
/// @param bar Bar whose target is validated.
/// @return Borrowed live code-editor pointer, or null.
static vg_codeeditor_t *findreplacebar_live_target(vg_findreplacebar_t *bar) {
    if (!bar || !bar->target_editor)
        return NULL;
    if (!vg_widget_is_live(&bar->target_editor->base) ||
        bar->target_editor->base.type != VG_WIDGET_CODEEDITOR) {
        bar->target_editor = NULL;
        return NULL;
    }
    return bar->target_editor;
}

/// @brief Clears logical matches and the displayed result summary.
///
/// @param bar Bar whose reusable match allocation is retained.
static void clear_matches(vg_findreplacebar_t *bar) {
    bar->match_count = 0;
    bar->current_match = 0;
    bar->search_truncated = false;
    bar->search_scanned_bytes = 0;
    bar->result_text[0] = '\0';
}

/// @brief Rescans the linked editor for the current query and options.
///
/// @details Matches are rebuilt across bounded source bytes, converted from
///          byte offsets to editor code-point columns, and the first result is
///          highlighted. Regexes compile once through the shared cross-platform
///          engine. Zero-width matches advance by one UTF-8 scalar.
///
/// @param bar Bar whose search state is rebuilt.
static void perform_search(vg_findreplacebar_t *bar) {
    if (!bar)
        return;
    bar->search_pending = false;
    bar->search_debounce_remaining = 0.0f;
    clear_matches(bar);

    vg_codeeditor_t *ed = findreplacebar_live_target(bar);
    if (!ed)
        return;

    vg_textinput_t *find_input = (vg_textinput_t *)bar->find_input;
    if (!find_input)
        return;

    const char *query = vg_textinput_get_text(find_input);
    if (!query || !*query) {
        update_result_text(bar);
        return;
    }

    re_compiled_pattern *regex = NULL;
    if (bar->options.use_regex) {
        unsigned int flags = RE_COMPILE_DEFAULT;
        if (!bar->options.case_sensitive)
            flags |= RE_COMPILE_CASE_INSENSITIVE;
        char error[256];
        regex = re_compile_diagnostic(query, flags, error, sizeof(error));
        if (!regex) {
            snprintf(bar->result_text,
                     sizeof(bar->result_text),
                     "Invalid regex: %.46s",
                     error[0] ? error : "invalid syntax");
            vg_widget_invalidate(&bar->base);
            return;
        }
    }

    bool stop = false;
    for (int line = 0; line < ed->line_count; line++) {
        const char *text = ed->lines[line].text;
        if (!text)
            continue;
        size_t line_size = strlen(text);
        if (line_size > FINDREPLACEBAR_MAX_SCAN_BYTES - bar->search_scanned_bytes) {
            bar->search_truncated = true;
            break;
        }
        bar->search_scanned_bytes += line_size;

        const char *pos = text;
        const char *line_end = text + line_size;
        size_t match_len = 0;
        while (pos <= line_end &&
               (pos = regex ? find_regex_in_line(text, pos, regex, &bar->options, &match_len)
                            : find_in_line(pos, query, &bar->options, &match_len)) != NULL) {
            size_t start_byte = (size_t)(pos - text);
            size_t end_byte = start_byte + match_len;
            uint32_t start_col = fr_utf8_col_from_byte_offset(text, start_byte);
            uint32_t end_col = fr_utf8_col_from_byte_offset(text, end_byte);
            if (!add_match(bar, (uint32_t)line, start_col, end_col)) {
                stop = true;
                break;
            }
            if (match_len > 0) {
                pos += match_len;
            } else if (pos < line_end) {
                const char *next = fr_utf8_next(pos);
                if (!next || next <= pos) {
                    stop = true;
                    break;
                }
                pos = next;
            } else {
                break;
            }
        }
        if (stop)
            break;
    }

    re_free(regex);

    // Update result text and highlight
    if (bar->match_count > 0) {
        bar->current_match = 0;
        highlight_current_match(bar);
    }
    update_result_text(bar);

    // Call callback if set
    if (bar->on_find) {
        bar->on_find(bar, query, &bar->options, bar->user_data);
    }
    vg_widget_invalidate(&bar->base);
}

/// @brief Updates the human-readable match summary.
///
/// @param bar Bar whose result buffer is rewritten.
static void update_result_text(vg_findreplacebar_t *bar) {
    if (bar->match_count == 0) {
        vg_textinput_t *find_input = (vg_textinput_t *)bar->find_input;
        const char *query = find_input ? vg_textinput_get_text(find_input) : NULL;
        if (query && *query) {
            snprintf(bar->result_text,
                     sizeof(bar->result_text),
                     bar->search_truncated ? "No results (scan capped)" : "No results");
        } else {
            bar->result_text[0] = '\0';
        }
    } else {
        snprintf(bar->result_text,
                 sizeof(bar->result_text),
                 bar->search_truncated ? "%zu of %zu+" : "%zu of %zu",
                 bar->current_match + 1,
                 bar->match_count);
    }
}

/// @brief Applies replace-row visibility to its child controls.
///
/// @param bar Bar whose child visibility is synchronized.
static void findreplacebar_apply_replace_visibility(vg_findreplacebar_t *bar) {
    if (!bar)
        return;
    if (bar->replace_input)
        vg_widget_set_visible((vg_widget_t *)bar->replace_input, bar->show_replace);
    if (bar->replace_btn)
        vg_widget_set_visible((vg_widget_t *)bar->replace_btn, bar->show_replace);
    if (bar->replace_all_btn)
        vg_widget_set_visible((vg_widget_t *)bar->replace_all_btn, bar->show_replace);
}

/// @brief Selects the current match in the linked editor and reveals its line.
///
/// @param bar Bar containing the current match and target editor.
static void highlight_current_match(vg_findreplacebar_t *bar) {
    vg_codeeditor_t *ed = findreplacebar_live_target(bar);
    if (bar->match_count == 0 || !ed)
        return;

    vg_search_match_t *match = &bar->matches[bar->current_match];

    // Set selection to current match
    vg_codeeditor_set_selection(
        ed, (int)match->line, (int)match->start_col, (int)match->line, (int)match->end_col);

    // Scroll to make match visible
    vg_codeeditor_scroll_to_line(ed, (int)match->line);
}

/// @brief Compile the bar's current regex once for a replacement operation.
static re_compiled_pattern *compile_bar_regex(vg_findreplacebar_t *bar) {
    if (!bar || !bar->options.use_regex)
        return NULL;
    vg_textinput_t *find_input = (vg_textinput_t *)bar->find_input;
    const char *query = find_input ? vg_textinput_get_text(find_input) : NULL;
    if (!query || !*query)
        return NULL;
    unsigned int flags =
        bar->options.case_sensitive ? RE_COMPILE_DEFAULT : RE_COMPILE_CASE_INSENSITIVE;
    char error[256];
    return re_compile_diagnostic(query, flags, error, sizeof(error));
}

/// @brief Build the literal or capture-expanded replacement for one stored match.
/// @details A stale span returns null instead of editing unrelated text.
static char *replacement_for_match(vg_findreplacebar_t *bar,
                                   vg_codeeditor_t *editor,
                                   const vg_search_match_t *match,
                                   re_compiled_pattern *regex,
                                   const char *replacement) {
    if (!bar || !editor || !match || match->line >= (uint32_t)editor->line_count)
        return NULL;
    if (!replacement)
        replacement = "";
    if (!bar->options.use_regex) {
        size_t length = strlen(replacement);
        if (length == SIZE_MAX)
            return NULL;
        char *copy = (char *)malloc(length + 1);
        if (!copy)
            return NULL;
        memcpy(copy, replacement, length + 1);
        return copy;
    }

    const char *line_text = editor->lines[match->line].text;
    if (!regex || !line_text)
        return NULL;
    size_t line_length = strlen(line_text);
    if (line_length > (size_t)INT32_MAX)
        return NULL;

    size_t start = fr_utf8_byte_offset_from_col(line_text, match->start_col);
    char *expanded = NULL;
    size_t expanded_length = 0;
    bool ok = start <= (size_t)INT32_MAX && re_expand_replacement(regex,
                                                                  line_text,
                                                                  (int)line_length,
                                                                  (int)start,
                                                                  replacement,
                                                                  strlen(replacement),
                                                                  &expanded,
                                                                  &expanded_length);
    (void)expanded_length;
    return ok ? expanded : NULL;
}

//=============================================================================
// Button Callbacks
//=============================================================================

/// @brief Handles activation of the previous-match button.
///
/// @param btn Button that emitted the callback; otherwise unused.
/// @param user_data Owning find/replace bar.
static void on_find_prev_click(vg_widget_t *btn, void *user_data) {
    (void)btn;
    vg_findreplacebar_t *bar = (vg_findreplacebar_t *)user_data;
    vg_findreplacebar_find_prev(bar);
}

/// @brief Handles activation of the next-match button.
///
/// @param btn Button that emitted the callback; otherwise unused.
/// @param user_data Owning find/replace bar.
static void on_find_next_click(vg_widget_t *btn, void *user_data) {
    (void)btn;
    vg_findreplacebar_t *bar = (vg_findreplacebar_t *)user_data;
    vg_findreplacebar_find_next(bar);
}

/// @brief Handles activation of the replace-current button.
///
/// @param btn Button that emitted the callback; otherwise unused.
/// @param user_data Owning find/replace bar.
static void on_replace_click(vg_widget_t *btn, void *user_data) {
    (void)btn;
    vg_findreplacebar_t *bar = (vg_findreplacebar_t *)user_data;
    (void)vg_findreplacebar_replace_current(bar);
}

/// @brief Handles activation of the replace-all button.
///
/// @param btn Button that emitted the callback; otherwise unused.
/// @param user_data Owning find/replace bar.
static void on_replace_all_click(vg_widget_t *btn, void *user_data) {
    (void)btn;
    vg_findreplacebar_t *bar = (vg_findreplacebar_t *)user_data;
    (void)vg_findreplacebar_replace_all(bar);
}

/// @brief Handles activation of the close button.
///
/// @param btn Button that emitted the callback; otherwise unused.
/// @param user_data Owning find/replace bar.
static void on_close_click(vg_widget_t *btn, void *user_data) {
    (void)btn;
    vg_findreplacebar_t *bar = (vg_findreplacebar_t *)user_data;
    bar->base.visible = false;

    if (bar->on_close) {
        bar->on_close(bar, bar->user_data);
    }
}

/// @brief Synchronizes a changed option checkbox and reruns search.
///
/// @param cb Checkbox that changed.
/// @param checked New checkbox state; the authoritative value is read from the
///                typed checkbox widget.
/// @param user_data Owning find/replace bar.
static void on_option_change(vg_widget_t *cb, bool checked, void *user_data) {
    (void)checked;
    vg_findreplacebar_t *bar = (vg_findreplacebar_t *)user_data;

    // Update options from checkboxes
    vg_checkbox_t *case_cb = (vg_checkbox_t *)bar->case_sensitive_cb;
    vg_checkbox_t *word_cb = (vg_checkbox_t *)bar->whole_word_cb;
    vg_checkbox_t *regex_cb = (vg_checkbox_t *)bar->regex_cb;

    if (case_cb && cb == (vg_widget_t *)case_cb) {
        bar->options.case_sensitive = vg_checkbox_is_checked(case_cb);
    }
    if (word_cb && cb == (vg_widget_t *)word_cb) {
        bar->options.whole_word = vg_checkbox_is_checked(word_cb);
    }
    if (regex_cb && cb == (vg_widget_t *)regex_cb) {
        bar->options.use_regex = vg_checkbox_is_checked(regex_cb);
    }

    // Re-run search with new options
    perform_search(bar);
}

/// @brief Reruns search when the find input changes.
///
/// @param input Text input that changed; otherwise unused.
/// @param text New text; the search reads the input's authoritative value.
/// @param user_data Owning find/replace bar.
static void on_find_text_change(vg_widget_t *input, const char *text, void *user_data) {
    (void)input;
    (void)text;
    vg_findreplacebar_t *bar = (vg_findreplacebar_t *)user_data;
    if (!bar)
        return;
    clear_matches(bar);
    bar->search_pending = true;
    bar->search_debounce_remaining = FINDREPLACEBAR_DEBOUNCE_SECONDS;
    snprintf(bar->result_text, sizeof(bar->result_text), "Searching...");
    vg_widget_invalidate(&bar->base);
}

//=============================================================================
// Widget Implementation
//=============================================================================

/// @brief Create a find/replace bar with all child widgets pre-initialised.
///
/// @details The bar is created hidden (visible=false); call vg_widget_set_visible
///          to show it.  The replace row is hidden by default; toggle with
///          vg_findreplacebar_set_show_replace.  No target editor is associated
///          until vg_findreplacebar_set_target is called.
///
/// @return Newly allocated vg_findreplacebar_t, or NULL on allocation failure.
vg_findreplacebar_t *vg_findreplacebar_create(void) {
    vg_findreplacebar_t *bar = calloc(1, sizeof(vg_findreplacebar_t));
    if (!bar)
        return NULL;

    // Initialize base widget
    vg_widget_init(&bar->base, VG_WIDGET_CUSTOM, &g_findreplacebar_vtable);

    // Get theme
    vg_theme_t *theme = vg_theme_get_current();

    // Set default colors
    bar->bg_color = theme->colors.bg_secondary;
    bar->border_color = theme->colors.border_primary;
    bar->match_highlight = 0x40FFFF00;   // Yellow, semi-transparent
    bar->current_highlight = 0x80FF9900; // Orange, semi-transparent
    bar->font_size = theme->typography.size_normal;

    // Default options
    bar->options.wrap_around = true;

    // Create child widgets
    bar->find_input = vg_textinput_create(&bar->base);
    if (bar->find_input) {
        vg_textinput_set_placeholder((vg_textinput_t *)bar->find_input, "Find");
        vg_textinput_set_on_change((vg_textinput_t *)bar->find_input, on_find_text_change, bar);
    }

    bar->replace_input = vg_textinput_create(&bar->base);
    if (bar->replace_input) {
        vg_textinput_set_placeholder((vg_textinput_t *)bar->replace_input, "Replace");
    }

    // Create buttons with simple text labels
    bar->find_prev_btn = vg_button_create(&bar->base, "<");
    if (bar->find_prev_btn) {
        vg_button_set_on_click((vg_button_t *)bar->find_prev_btn, on_find_prev_click, bar);
    }

    bar->find_next_btn = vg_button_create(&bar->base, ">");
    if (bar->find_next_btn) {
        vg_button_set_on_click((vg_button_t *)bar->find_next_btn, on_find_next_click, bar);
    }

    bar->replace_btn = vg_button_create(&bar->base, "Replace");
    if (bar->replace_btn) {
        vg_button_set_on_click((vg_button_t *)bar->replace_btn, on_replace_click, bar);
    }

    bar->replace_all_btn = vg_button_create(&bar->base, "All");
    if (bar->replace_all_btn) {
        vg_button_set_on_click((vg_button_t *)bar->replace_all_btn, on_replace_all_click, bar);
    }

    bar->close_btn = vg_button_create(&bar->base, "X");
    if (bar->close_btn) {
        vg_button_set_on_click((vg_button_t *)bar->close_btn, on_close_click, bar);
    }

    // Create option checkboxes
    bar->case_sensitive_cb = vg_checkbox_create(&bar->base, "Aa");
    if (bar->case_sensitive_cb) {
        vg_checkbox_set_on_change((vg_checkbox_t *)bar->case_sensitive_cb, on_option_change, bar);
    }

    bar->whole_word_cb = vg_checkbox_create(&bar->base, "W");
    if (bar->whole_word_cb) {
        vg_checkbox_set_on_change((vg_checkbox_t *)bar->whole_word_cb, on_option_change, bar);
    }

    bar->regex_cb = vg_checkbox_create(&bar->base, ".*");
    if (bar->regex_cb) {
        vg_checkbox_set_on_change((vg_checkbox_t *)bar->regex_cb, on_option_change, bar);
    }

    if (!bar->find_input || !bar->replace_input || !bar->find_prev_btn || !bar->find_next_btn ||
        !bar->replace_btn || !bar->replace_all_btn || !bar->close_btn || !bar->case_sensitive_cb ||
        !bar->whole_word_cb || !bar->regex_cb) {
        vg_widget_destroy(&bar->base);
        return NULL;
    }

    return bar;
}

/// @brief Releases bar-owned search matches during widget destruction.
///
/// @details Child widgets are released by the base widget hierarchy.
///
/// @param widget Find/replace bar base widget being destroyed.
static void findreplacebar_destroy(vg_widget_t *widget) {
    vg_findreplacebar_t *bar = (vg_findreplacebar_t *)widget;

    // Free matches array
    free(bar->matches);

    // Child widgets are destroyed by parent destruction
}

/// @brief Measures the bar as one or two fixed-height rows.
///
/// @param widget Find/replace bar base widget whose size is updated.
/// @param available_width Width claimed by the toolbar.
/// @param available_height Available height; currently unused.
static void findreplacebar_measure(vg_widget_t *widget,
                                   float available_width,
                                   float available_height) {
    vg_findreplacebar_t *bar = (vg_findreplacebar_t *)widget;
    (void)available_height;

    widget->measured_width = available_width;
    widget->measured_height =
        bar->show_replace ? FINDREPLACEBAR_HEIGHT_REPLACE : FINDREPLACEBAR_HEIGHT;
}

/// @brief Arranges inputs, buttons, and option checkboxes within the bar.
///
/// @param widget Find/replace bar base widget to arrange.
/// @param x Allocated left coordinate.
/// @param y Allocated top coordinate.
/// @param width Allocated width.
/// @param height Allocated height.
static void findreplacebar_arrange(
    vg_widget_t *widget, float x, float y, float width, float height) {
    vg_findreplacebar_t *bar = (vg_findreplacebar_t *)widget;

    widget->x = x;
    widget->y = y;
    widget->width = width;
    widget->height = height;

    // Children are stored parent-relative: painters and screen-bounds queries
    // add the bar's origin, so rows must not include (x, y) here.
    // Layout first row: Find input, prev/next buttons, options, close
    float row_y = PADDING;
    float cur_x = PADDING;
    float row_height = FINDREPLACEBAR_HEIGHT - PADDING * 2;

    // Find input
    if (bar->find_input) {
        vg_widget_t *w = (vg_widget_t *)bar->find_input;
        vg_widget_arrange(w, cur_x, row_y + 4, INPUT_WIDTH, row_height - 8);
        cur_x += INPUT_WIDTH + PADDING;
    }

    // Prev button
    if (bar->find_prev_btn) {
        vg_widget_t *w = (vg_widget_t *)bar->find_prev_btn;
        vg_widget_arrange(w, cur_x, row_y + 4, BUTTON_WIDTH, row_height - 8);
        cur_x += BUTTON_WIDTH + PADDING;
    }

    // Next button
    if (bar->find_next_btn) {
        vg_widget_t *w = (vg_widget_t *)bar->find_next_btn;
        vg_widget_arrange(w, cur_x, row_y + 4, BUTTON_WIDTH, row_height - 8);
        cur_x += BUTTON_WIDTH + PADDING * 2;
    }

    // Case sensitive checkbox
    if (bar->case_sensitive_cb) {
        vg_widget_t *w = (vg_widget_t *)bar->case_sensitive_cb;
        vg_widget_arrange(w, cur_x, row_y + 4, 40, row_height - 8);
        cur_x += 40 + PADDING;
    }

    // Whole word checkbox
    if (bar->whole_word_cb) {
        vg_widget_t *w = (vg_widget_t *)bar->whole_word_cb;
        vg_widget_arrange(w, cur_x, row_y + 4, 32, row_height - 8);
        cur_x += 32 + PADDING;
    }

    // Regex checkbox
    if (bar->regex_cb) {
        vg_widget_t *w = (vg_widget_t *)bar->regex_cb;
        vg_widget_arrange(w, cur_x, row_y + 4, 36, row_height - 8);
        cur_x += 36 + PADDING;
    }

    // Close button at right
    if (bar->close_btn) {
        vg_widget_t *w = (vg_widget_t *)bar->close_btn;
        float close_x = width - BUTTON_WIDTH - PADDING;
        vg_widget_arrange(w, close_x, row_y + 4, BUTTON_WIDTH, row_height - 8);
    }

    // Second row (replace mode): Replace input, replace/all buttons
    if (bar->show_replace) {
        row_y = FINDREPLACEBAR_HEIGHT;
        cur_x = PADDING;

        // Replace input
        if (bar->replace_input) {
            vg_widget_t *w = (vg_widget_t *)bar->replace_input;
            vg_widget_arrange(w, cur_x, row_y + 4, INPUT_WIDTH, row_height - 8);
            cur_x += INPUT_WIDTH + PADDING;
        }

        // Replace button
        if (bar->replace_btn) {
            vg_widget_t *w = (vg_widget_t *)bar->replace_btn;
            vg_widget_arrange(w, cur_x, row_y + 4, 60, row_height - 8);
            cur_x += 60 + PADDING;
        }

        // Replace all button
        if (bar->replace_all_btn) {
            vg_widget_t *w = (vg_widget_t *)bar->replace_all_btn;
            vg_widget_arrange(w, cur_x, row_y + 4, 40, row_height - 8);
        }
    }

    findreplacebar_apply_replace_visibility(bar);
}

/// @brief Paints the toolbar surface, separator, and result summary.
///
/// @details Child controls paint themselves through the widget hierarchy.
///
/// @param widget Find/replace bar base widget to render.
/// @param canvas Destination drawing context.
static void findreplacebar_paint(vg_widget_t *widget, void *canvas) {
    vg_findreplacebar_t *bar = (vg_findreplacebar_t *)widget;
    vg_theme_t *theme = vg_theme_get_current();
    vgfx_window_t win = (vgfx_window_t)canvas;

    vgfx_fill_rect(win,
                   (int32_t)widget->x,
                   (int32_t)widget->y,
                   (int32_t)widget->width,
                   (int32_t)widget->height,
                   bar->bg_color ? bar->bg_color : theme->colors.bg_secondary);
    vgfx_rect(win,
              (int32_t)widget->x,
              (int32_t)widget->y,
              (int32_t)widget->width,
              (int32_t)widget->height,
              bar->border_color ? bar->border_color : theme->colors.border_primary);
    vgfx_fill_rect(win,
                   (int32_t)widget->x,
                   (int32_t)(widget->y + FINDREPLACEBAR_HEIGHT - 1),
                   (int32_t)widget->width,
                   1,
                   theme->colors.border_secondary);

    if (bar->show_replace) {
        vgfx_fill_rect(win,
                       (int32_t)widget->x,
                       (int32_t)(widget->y + FINDREPLACEBAR_HEIGHT - 1),
                       (int32_t)widget->width,
                       1,
                       theme->colors.border_primary);
    }

    if (bar->result_text[0] && bar->font) {
        vg_text_metrics_t result_metrics = {0};
        vg_font_metrics_t font_metrics = {0};
        float close_left = widget->x + widget->width - BUTTON_WIDTH - PADDING;
        float text_x = 0.0f;
        float text_y = 0.0f;
        uint32_t text_color = bar->match_count > 0 ? 0xFF00FF00 : 0xFFFF6666;

        vg_font_measure_text(bar->font, bar->font_size, bar->result_text, &result_metrics);
        vg_font_get_metrics(bar->font, bar->font_size, &font_metrics);
        text_x = close_left - result_metrics.width - 10.0f;
        text_y = widget->y + ((float)FINDREPLACEBAR_HEIGHT - bar->font_size) * 0.5f +
                 font_metrics.ascent;
        if (text_x < widget->x + INPUT_WIDTH + BUTTON_WIDTH * 2 + PADDING * 6 + 100.0f)
            text_x = widget->x + INPUT_WIDTH + BUTTON_WIDTH * 2 + PADDING * 6 + 100.0f;

        vg_font_draw_text(
            canvas, bar->font, bar->font_size, text_x, text_y, bar->result_text, text_color);
    }
}

/// @brief Handles toolbar keyboard shortcuts and dispatches pointer input to children.
///
/// @param widget Find/replace bar base widget receiving the event.
/// @param event Event to inspect.
/// @return `true` when the bar or a child consumes the event; otherwise `false`.
static bool findreplacebar_handle_event(vg_widget_t *widget, vg_event_t *event) {
    vg_findreplacebar_t *bar = (vg_findreplacebar_t *)widget;

    // Handle keyboard shortcuts
    if (event->type == VG_EVENT_KEY_DOWN) {
        uint32_t mods = event->modifiers;

        // Escape: close
        if (event->key.key == VG_KEY_ESCAPE) {
            vg_widget_set_visible(&bar->base, false);
            if (bar->on_close) {
                bar->on_close(bar, bar->user_data);
            }
            return true;
        }

        // Enter: find next
        if (event->key.key == VG_KEY_ENTER) {
            if (bar->search_pending)
                perform_search(bar);
            if (mods & VG_MOD_SHIFT) {
                vg_findreplacebar_find_prev(bar);
            } else {
                vg_findreplacebar_find_next(bar);
            }
            return true;
        }

        // Ctrl+H: toggle replace mode
        bool has_ctrl = (mods & VG_MOD_CTRL) != 0 || (mods & VG_MOD_SUPER) != 0;
        if (has_ctrl && event->key.key == VG_KEY_H) {
            bar->show_replace = !bar->show_replace;
            findreplacebar_apply_replace_visibility(bar);
            vg_widget_invalidate(widget);
            return true;
        }
    }

    if (event->type == VG_EVENT_MOUSE_MOVE || event->type == VG_EVENT_MOUSE_DOWN ||
        event->type == VG_EVENT_MOUSE_UP || event->type == VG_EVENT_CLICK ||
        event->type == VG_EVENT_DOUBLE_CLICK || event->type == VG_EVENT_MOUSE_WHEEL) {
        float screen_x =
            event->type == VG_EVENT_MOUSE_WHEEL ? event->wheel.screen_x : event->mouse.screen_x;
        float screen_y =
            event->type == VG_EVENT_MOUSE_WHEEL ? event->wheel.screen_y : event->mouse.screen_y;
        vg_widget_t *target = vg_widget_hit_test(widget, screen_x, screen_y);
        if (target && target != widget) {
            return vg_event_send(target, event);
        }
    }

    return false;
}

//=============================================================================
// Public API
//=============================================================================

/// @brief Destroy the find/replace bar and all its child widgets.
///
/// @param bar The bar to destroy; may be NULL.
void vg_findreplacebar_destroy(vg_findreplacebar_t *bar) {
    if (!bar)
        return;
    vg_widget_destroy(&bar->base);
}

/// @brief Associate a code editor with this bar and immediately re-run the search.
///
/// @param bar    The find/replace bar to configure.
/// @param editor The code editor to search; may be NULL to detach.
void vg_findreplacebar_set_target(vg_findreplacebar_t *bar, struct vg_codeeditor *editor) {
    if (!bar)
        return;
    bar->target_editor =
        (editor && vg_widget_is_live(&editor->base) && editor->base.type == VG_WIDGET_CODEEDITOR)
            ? editor
            : NULL;

    // Re-run search if there's text
    perform_search(bar);
}

/// @brief Show or hide the replace row (replace input, Replace, and Replace All buttons).
///
/// @param bar  The find/replace bar to configure.
/// @param show true to show the replace row; false to hide it.
void vg_findreplacebar_set_show_replace(vg_findreplacebar_t *bar, bool show) {
    if (!bar)
        return;
    bar->show_replace = show;
    findreplacebar_apply_replace_visibility(bar);
    vg_widget_invalidate(&bar->base);
}

/// @brief Apply a complete set of search options and re-run the search.
///
/// @details Updates the option checkboxes to reflect the new state.
///
/// @param bar     The find/replace bar to configure.
/// @param options New option values; may not be NULL.
void vg_findreplacebar_set_options(vg_findreplacebar_t *bar, vg_search_options_t *options) {
    if (!bar || !options)
        return;
    bar->options = *options;

    // Update checkboxes
    if (bar->case_sensitive_cb) {
        vg_checkbox_set_checked((vg_checkbox_t *)bar->case_sensitive_cb, options->case_sensitive);
    }
    if (bar->whole_word_cb) {
        vg_checkbox_set_checked((vg_checkbox_t *)bar->whole_word_cb, options->whole_word);
    }
    if (bar->regex_cb) {
        vg_checkbox_set_checked((vg_checkbox_t *)bar->regex_cb, options->use_regex);
    }

    perform_search(bar);
}

/// @brief Set the search query text and immediately run a new search.
///
/// @param bar   The find/replace bar to update.
/// @param query Null-terminated search string; NULL is silently ignored.
void vg_findreplacebar_find(vg_findreplacebar_t *bar, const char *query) {
    if (!bar)
        return;

    if (bar->find_input && query) {
        vg_textinput_set_text((vg_textinput_t *)bar->find_input, query);
    }

    perform_search(bar);
}

/// @brief Advance to the next match and highlight it in the target editor.
///
/// @details Wraps to the first match when wrap_around is enabled and the last
///          match is already current.
///
/// @param bar The find/replace bar to advance.
void vg_findreplacebar_find_next(vg_findreplacebar_t *bar) {
    if (!bar || bar->match_count == 0)
        return;

    bar->current_match++;
    if (bar->current_match >= bar->match_count) {
        if (bar->options.wrap_around) {
            bar->current_match = 0;
        } else {
            bar->current_match = bar->match_count - 1;
        }
    }

    highlight_current_match(bar);
    update_result_text(bar);
    vg_widget_invalidate(&bar->base);
}

/// @brief Move to the previous match and highlight it in the target editor.
///
/// @details Wraps to the last match when wrap_around is enabled and the first
///          match is already current.
///
/// @param bar The find/replace bar to advance backwards.
void vg_findreplacebar_find_prev(vg_findreplacebar_t *bar) {
    if (!bar || bar->match_count == 0)
        return;

    if (bar->current_match == 0) {
        if (bar->options.wrap_around) {
            bar->current_match = bar->match_count - 1;
        }
    } else {
        bar->current_match--;
    }

    highlight_current_match(bar);
    update_result_text(bar);
    vg_widget_invalidate(&bar->base);
}

/// @brief Replace the currently highlighted match with the replace-input text.
///
/// @details After replacement the search is re-run to update the match list.
///
/// @param bar The find/replace bar to use.
/// @return `true` when a live target and current match were replaced.
bool vg_findreplacebar_replace_current(vg_findreplacebar_t *bar) {
    vg_codeeditor_t *ed = findreplacebar_live_target(bar);
    if (!bar || bar->match_count == 0 || !ed)
        return false;

    vg_textinput_t *replace_input = (vg_textinput_t *)bar->replace_input;
    vg_textinput_t *find_input = (vg_textinput_t *)bar->find_input;
    if (!replace_input || !find_input)
        return false;

    const char *replace_text = vg_textinput_get_text(replace_input);
    const char *find_text = vg_textinput_get_text(find_input);
    if (!replace_text)
        replace_text = "";

    re_compiled_pattern *regex = compile_bar_regex(bar);
    if (bar->options.use_regex && !regex) {
        perform_search(bar);
        return false;
    }
    vg_search_match_t *match = &bar->matches[bar->current_match];
    char *expanded = replacement_for_match(bar, ed, match, regex, replace_text);
    re_free(regex);
    if (!expanded) {
        perform_search(bar);
        return false;
    }

    vg_codeeditor_set_selection(
        ed, (int)match->line, (int)match->start_col, (int)match->line, (int)match->end_col);
    vg_codeeditor_delete_selection(ed);
    vg_codeeditor_insert_text(ed, expanded);
    free(expanded);

    // Callback
    if (bar->on_replace) {
        bar->on_replace(bar, find_text, replace_text, bar->user_data);
    }

    // Re-search
    perform_search(bar);
    return true;
}

/// @brief Replace every match with the replace-input text in a single pass.
///
/// @details Applies replacements from end to beginning to preserve earlier
///          column positions.  The search is re-run after all replacements.
///
/// @param bar The find/replace bar to use.
/// @return Number of replacements applied.
size_t vg_findreplacebar_replace_all(vg_findreplacebar_t *bar) {
    vg_codeeditor_t *ed = findreplacebar_live_target(bar);
    if (!bar || bar->match_count == 0 || !ed)
        return 0;

    vg_textinput_t *replace_input = (vg_textinput_t *)bar->replace_input;
    vg_textinput_t *find_input = (vg_textinput_t *)bar->find_input;
    if (!replace_input || !find_input)
        return 0;

    const char *replace_text = vg_textinput_get_text(replace_input);
    const char *find_text = vg_textinput_get_text(find_input);
    if (!replace_text)
        replace_text = "";
    if (bar->match_count > SIZE_MAX / sizeof(char *))
        return 0;
    char **expanded = (char **)calloc(bar->match_count, sizeof(char *));
    if (!expanded)
        return 0;
    re_compiled_pattern *regex = compile_bar_regex(bar);
    if (bar->options.use_regex && !regex) {
        free(expanded);
        perform_search(bar);
        return 0;
    }

    for (size_t i = 0; i < bar->match_count; i++) {
        expanded[i] = replacement_for_match(bar, ed, &bar->matches[i], regex, replace_text);
        if (!expanded[i]) {
            for (size_t j = 0; j < i; j++)
                free(expanded[j]);
            free(expanded);
            re_free(regex);
            perform_search(bar);
            return 0;
        }
    }
    re_free(regex);
    size_t replacement_count = 0;

    // Replace from end to start to preserve positions
    for (size_t i = bar->match_count; i > 0; i--) {
        vg_search_match_t *match = &bar->matches[i - 1];

        // Select match
        vg_codeeditor_set_selection(
            ed, (int)match->line, (int)match->start_col, (int)match->line, (int)match->end_col);

        // Replace
        vg_codeeditor_delete_selection(ed);
        vg_codeeditor_insert_text(ed, expanded[i - 1]);
        free(expanded[i - 1]);
        replacement_count++;
    }
    free(expanded);

    // Callback
    if (bar->on_replace_all) {
        bar->on_replace_all(bar, find_text, replace_text, bar->user_data);
    }

    // Re-search (should find nothing)
    perform_search(bar);
    return replacement_count;
}

/// @brief Return the number of matches found in the last search.
///
/// @param bar The find/replace bar to query.
/// @return Total match count, or 0 if bar is NULL.
size_t vg_findreplacebar_get_match_count(vg_findreplacebar_t *bar) {
    return bar ? bar->match_count : 0;
}

/// @brief Return the zero-based index of the currently highlighted match.
///
/// @param bar The find/replace bar to query.
/// @return Current match index, or 0 if bar is NULL.
size_t vg_findreplacebar_get_current_match(vg_findreplacebar_t *bar) {
    return bar ? bar->current_match : 0;
}

/// @brief Move keyboard focus into the find-text input field.
///
/// @param bar The find/replace bar to focus.
void vg_findreplacebar_focus(vg_findreplacebar_t *bar) {
    if (!bar || !bar->find_input)
        return;
    vg_widget_set_focus((vg_widget_t *)bar->find_input);
}

/// @brief Set the find-input text programmatically and re-run the search.
///
/// @param bar  The find/replace bar to update.
/// @param text Null-terminated search string to set in the find input.
void vg_findreplacebar_set_find_text(vg_findreplacebar_t *bar, const char *text) {
    if (!bar || !bar->find_input)
        return;
    vg_textinput_set_text((vg_textinput_t *)bar->find_input, text);
    perform_search(bar);
}

/// @copydoc vg_findreplacebar_tick
bool vg_findreplacebar_tick(vg_findreplacebar_t *bar, float dt) {
    if (!bar || !bar->search_pending)
        return false;
    if (!isfinite(dt) || dt < 0.0f)
        return true;
    if (dt >= bar->search_debounce_remaining) {
        perform_search(bar);
        return false;
    }
    bar->search_debounce_remaining -= dt;
    return true;
}

/// @copydoc vg_findreplacebar_tick_widget
bool vg_findreplacebar_tick_widget(vg_widget_t *widget, float dt) {
    if (!widget || !vg_widget_is_live(widget) || widget->vtable != &g_findreplacebar_vtable)
        return false;
    return vg_findreplacebar_tick((vg_findreplacebar_t *)widget, dt);
}

/// @brief Register a callback invoked when the bar's close button is clicked.
///
/// @param bar       The find/replace bar to configure.
/// @param callback  Function called with (bar, user_data) on close.  May be NULL.
/// @param user_data Opaque pointer forwarded unchanged to the callback.
void vg_findreplacebar_set_on_close(vg_findreplacebar_t *bar,
                                    void (*callback)(vg_findreplacebar_t *, void *),
                                    void *user_data) {
    if (!bar)
        return;
    bar->on_close = callback;
    bar->user_data = user_data;
}

/// @brief Set the font and size for result text and propagate it to child inputs.
///
/// @param bar  The find/replace bar to configure.
/// @param font Font to use; may be NULL.
/// @param size Font size in logical pixels.
void vg_findreplacebar_set_font(vg_findreplacebar_t *bar, vg_font_t *font, float size) {
    if (!bar)
        return;
    bar->font = font;
    bar->font_size = size;

    // Set font on child widgets
    if (bar->find_input) {
        vg_textinput_set_font((vg_textinput_t *)bar->find_input, font, size);
    }
    if (bar->replace_input) {
        vg_textinput_set_font((vg_textinput_t *)bar->replace_input, font, size);
    }
}
