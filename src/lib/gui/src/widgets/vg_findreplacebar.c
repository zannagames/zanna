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
/// Literal search is UTF-8 column-aware and supports ASCII-insensitive and
/// whole-word modes. POSIX builds use `regcomp()` and `regexec()` for regular
/// expressions; Windows uses the local deterministic fallback supporting
/// literals, dot, classes, anchors, and `*`, `+`, or `?` quantifiers.
/// Replace-all processes matches in reverse document order so earlier columns
/// remain valid.
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
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <regex.h>
#endif

//=============================================================================
// Constants
//=============================================================================

#define FINDREPLACEBAR_HEIGHT 36
#define FINDREPLACEBAR_HEIGHT_REPLACE 72
#define INPUT_WIDTH 200
#define BUTTON_WIDTH 24
#define PADDING 4
#define INITIAL_MATCH_CAPACITY 64

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
static void add_match(vg_findreplacebar_t *bar, uint32_t line, uint32_t start, uint32_t end);
static const char *find_in_line(const char *text,
                                const char *query,
                                vg_search_options_t *options,
                                size_t *match_len);
#ifndef _WIN32
static const char *find_regex_in_line(const char *text,
                                      const char *start,
                                      regex_t *regex,
                                      vg_search_options_t *options,
                                      size_t *match_len);
#else
static const char *find_regex_in_line(const char *text,
                                      const char *start,
                                      const char *pattern,
                                      vg_search_options_t *options,
                                      size_t *match_len);
#endif
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

/// @brief True if @p cp is a word boundary (NUL, or an ASCII non-alphanumeric
///        that is not '_'); non-ASCII is treated as a word character so
///        whole-word search keeps accented words intact.
/// @param cp Unicode code point to classify.
/// @return `true` when the code point forms a supported word boundary.
static bool is_word_boundary_codepoint(uint32_t cp) {
    if (cp == 0)
        return true;
    if (cp < 0x80)
        return !isalnum((unsigned char)cp) && cp != '_';
    return false;
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

#ifndef _WIN32
/// @brief Find the next POSIX-regex match of @p regex in @p text at or after
///        @p start, honoring the whole-word flag in @p options.
/// @param text Full line used for whole-word boundary checks.
/// @param start First position eligible for matching.
/// @param regex Compiled POSIX regular expression.
/// @param options Search options controlling whole-word validation.
/// @param[out] match_len Receives the matched byte length on success.
/// @return Pointer into @p text at the match, or NULL if none. (POSIX-only;
///         Windows builds use a non-regex fallback.)
static const char *find_regex_in_line(const char *text,
                                      const char *start,
                                      regex_t *regex,
                                      vg_search_options_t *options,
                                      size_t *match_len) {
    if (!text || !start || !regex || !match_len)
        return NULL;

    const char *pos = start;
    while (*pos) {
        regmatch_t match = {0};
        if (regexec(regex, pos, 1, &match, 0) != 0)
            return NULL;
        if (match.rm_so < 0 || match.rm_eo < match.rm_so)
            return NULL;

        const char *found = pos + match.rm_so;
        size_t len = (size_t)(match.rm_eo - match.rm_so);
        if (len == 0) {
            pos = fr_utf8_next(found);
            continue;
        }

        if (options->whole_word && !check_whole_word(text, found, len)) {
            pos = fr_utf8_next(found);
            continue;
        }

        *match_len = len;
        return found;
    }

    return NULL;
}
#else
/// @brief Applies optional ASCII case folding for the Windows regex fallback.
///
/// @param c Byte to fold.
/// @param case_sensitive Whether comparison must preserve ASCII case.
/// @return Original byte or its lowercase representation.
static int fr_regex_fold(unsigned char c, bool case_sensitive) {
    return case_sensitive ? (int)c : tolower(c);
}

/// @brief Finds the byte after one fallback-regex atom.
///
/// @param pattern Pattern beginning at a literal, escape, dot, or character
///                class.
/// @return Pointer after the atom, or null for an incomplete class or escape.
static const char *fr_regex_atom_end(const char *pattern) {
    if (!pattern || !*pattern)
        return NULL;
    if (*pattern == '\\')
        return pattern[1] ? pattern + 2 : NULL;
    if (*pattern != '[')
        return pattern + 1;

    const char *p = pattern + 1;
    if (*p == '^' || *p == '!')
        p++;
    if (*p == ']')
        p++;
    while (*p && *p != ']') {
        if (*p == '\\' && p[1])
            p += 2;
        else
            p++;
    }
    return *p == ']' ? p + 1 : NULL;
}

/// @brief Reads one possibly escaped byte from a fallback character class.
///
/// @param[in,out] cursor Current class-pattern cursor, advanced on success.
/// @param[out] out Receives the decoded literal byte.
/// @return `true` when a byte was read; otherwise `false`.
static bool fr_regex_read_class_char(const char **cursor, unsigned char *out) {
    const char *p = *cursor;
    if (!p || !*p)
        return false;
    if (*p == '\\' && p[1]) {
        *out = (unsigned char)p[1];
        *cursor = p + 2;
        return true;
    }
    *out = (unsigned char)*p;
    *cursor = p + 1;
    return true;
}

/// @brief Tests one byte against a fallback-regex character class.
///
/// @details The class may be negated and may contain inclusive byte ranges.
///
/// @param pattern Pattern beginning with `[`.
/// @param ch Candidate text byte.
/// @param case_sensitive Whether ASCII comparisons preserve case.
/// @param[out] after Receives the byte after the closing bracket.
/// @return `true` when a valid class accepts @p ch; otherwise `false`.
static bool fr_regex_match_class(const char *pattern,
                                 unsigned char ch,
                                 bool case_sensitive,
                                 const char **after) {
    const char *p = pattern + 1;
    bool negate = false;
    bool matched = false;
    bool valid = false;
    if (*p == '^' || *p == '!') {
        negate = true;
        p++;
    }

    while (*p && *p != ']') {
        unsigned char start = 0;
        if (!fr_regex_read_class_char(&p, &start))
            return false;

        if (*p == '-' && p[1] && p[1] != ']') {
            p++;
            unsigned char end = 0;
            if (!fr_regex_read_class_char(&p, &end))
                return false;
            int folded_ch = fr_regex_fold(ch, case_sensitive);
            int folded_start = fr_regex_fold(start, case_sensitive);
            int folded_end = fr_regex_fold(end, case_sensitive);
            if (folded_start > folded_end) {
                int tmp = folded_start;
                folded_start = folded_end;
                folded_end = tmp;
            }
            matched = matched || (folded_ch >= folded_start && folded_ch <= folded_end);
        } else {
            matched = matched ||
                      fr_regex_fold(ch, case_sensitive) == fr_regex_fold(start, case_sensitive);
        }
        valid = true;
    }

    if (!valid || *p != ']')
        return false;
    *after = p + 1;
    return negate ? !matched : matched;
}

/// @brief Matches one fallback-regex atom at the current text position.
///
/// @param pattern Atom pattern to evaluate.
/// @param text Current non-empty text position.
/// @param case_sensitive Whether ASCII literals preserve case.
/// @param[out] after Receives the pattern position after the atom.
/// @param[out] match_len Receives consumed text bytes.
/// @return `true` when the atom matches; otherwise `false`.
static bool fr_regex_match_atom(const char *pattern,
                                const char *text,
                                bool case_sensitive,
                                const char **after,
                                size_t *match_len) {
    if (!pattern || !*pattern || !text || !*text)
        return false;

    if (*pattern == '.') {
        const char *next = fr_utf8_next(text);
        *after = pattern + 1;
        *match_len = (size_t)(next - text);
        return *match_len > 0;
    }

    if (*pattern == '[') {
        if (!fr_regex_match_class(pattern, (unsigned char)*text, case_sensitive, after))
            return false;
        *match_len = 1;
        return true;
    }

    if (*pattern == '\\') {
        if (!pattern[1])
            return false;
        *after = pattern + 2;
        *match_len = 1;
        return fr_regex_fold((unsigned char)*text, case_sensitive) ==
               fr_regex_fold((unsigned char)pattern[1], case_sensitive);
    }

    *after = pattern + 1;
    *match_len = 1;
    return fr_regex_fold((unsigned char)*text, case_sensitive) ==
           fr_regex_fold((unsigned char)*pattern, case_sensitive);
}

/// @brief Matches a fallback-regex suffix at the current text position.
///
/// @param pattern Pattern suffix to evaluate.
/// @param text Current text position.
/// @param case_sensitive Whether ASCII literals preserve case.
/// @param[out] matched_len Receives total matched byte length.
/// @return `true` when the complete suffix matches.
static bool fr_regex_match_here(const char *pattern,
                                const char *text,
                                bool case_sensitive,
                                size_t *matched_len);

/// @brief Matches a quantified atom with greedy recursive backtracking.
///
/// @param atom Atom to repeat.
/// @param rest Pattern following its quantifier.
/// @param text Current text position.
/// @param case_sensitive Whether ASCII literals preserve case.
/// @param min_count Minimum remaining repetitions.
/// @param[out] matched_len Receives total bytes matched by repetition and tail.
/// @return `true` when a permitted repetition count lets @p rest match.
static bool fr_regex_match_repeat(const char *atom,
                                  const char *rest,
                                  const char *text,
                                  bool case_sensitive,
                                  int min_count,
                                  size_t *matched_len) {
    const char *after_atom = NULL;
    size_t atom_len = 0;
    if (fr_regex_match_atom(atom, text, case_sensitive, &after_atom, &atom_len) && atom_len > 0) {
        size_t tail_len = 0;
        int next_min = min_count > 0 ? min_count - 1 : 0;
        if (fr_regex_match_repeat(
                atom, rest, text + atom_len, case_sensitive, next_min, &tail_len)) {
            *matched_len = atom_len + tail_len;
            return true;
        }
    }

    if (min_count <= 0) {
        size_t tail_len = 0;
        if (fr_regex_match_here(rest, text, case_sensitive, &tail_len)) {
            *matched_len = tail_len;
            return true;
        }
    }

    return false;
}

/// @brief Matches a complete fallback-regex suffix at one text position.
///
/// @details End anchors and the `*`, `+`, and `?` quantifiers are resolved
/// recursively; unquantified atoms consume exactly one atom match.
///
/// @param pattern Pattern suffix to evaluate.
/// @param text Current text position.
/// @param case_sensitive Whether ASCII literals preserve case.
/// @param[out] matched_len Receives the consumed text length.
/// @return `true` when the suffix matches; otherwise `false`.
static bool fr_regex_match_here(const char *pattern,
                                const char *text,
                                bool case_sensitive,
                                size_t *matched_len) {
    if (!pattern)
        return false;
    if (*pattern == '\0') {
        *matched_len = 0;
        return true;
    }
    if (pattern[0] == '$' && pattern[1] == '\0') {
        if (*text == '\0') {
            *matched_len = 0;
            return true;
        }
        return false;
    }

    const char *after_atom = fr_regex_atom_end(pattern);
    if (!after_atom)
        return false;

    char quantifier = *after_atom;
    if (quantifier == '*')
        return fr_regex_match_repeat(pattern, after_atom + 1, text, case_sensitive, 0, matched_len);
    if (quantifier == '+')
        return fr_regex_match_repeat(pattern, after_atom + 1, text, case_sensitive, 1, matched_len);
    if (quantifier == '?') {
        const char *unused_after = NULL;
        size_t atom_len = 0;
        if (fr_regex_match_atom(pattern, text, case_sensitive, &unused_after, &atom_len) &&
            atom_len > 0) {
            size_t tail_len = 0;
            if (fr_regex_match_here(after_atom + 1, text + atom_len, case_sensitive, &tail_len)) {
                *matched_len = atom_len + tail_len;
                return true;
            }
        }
        return fr_regex_match_here(after_atom + 1, text, case_sensitive, matched_len);
    }

    const char *unused_after = NULL;
    size_t atom_len = 0;
    if (!fr_regex_match_atom(pattern, text, case_sensitive, &unused_after, &atom_len) ||
        atom_len == 0)
        return false;
    size_t tail_len = 0;
    if (!fr_regex_match_here(after_atom, text + atom_len, case_sensitive, &tail_len))
        return false;
    *matched_len = atom_len + tail_len;
    return true;
}

/// @brief Finds the next Windows fallback-regex match in a line.
///
/// @param text Complete line used for boundary checks.
/// @param start First text position eligible for matching.
/// @param pattern Fallback-regex pattern.
/// @param options Search options controlling case and whole-word checks.
/// @param[out] match_len Receives matched byte length.
/// @return Pointer into @p text at the next non-empty match, or null.
static const char *find_regex_in_line(const char *text,
                                      const char *start,
                                      const char *pattern,
                                      vg_search_options_t *options,
                                      size_t *match_len) {
    if (!text || !start || !pattern || !*pattern || !match_len)
        return NULL;

    bool anchored = pattern[0] == '^';
    const char *search_pattern = anchored ? pattern + 1 : pattern;
    if (anchored && start != text)
        return NULL;

    const char *pos = anchored ? text : start;
    while (*pos) {
        size_t len = 0;
        if (fr_regex_match_here(search_pattern, pos, options->case_sensitive, &len) && len > 0) {
            if (options->whole_word && !check_whole_word(text, pos, len)) {
                if (anchored)
                    return NULL;
            } else {
                *match_len = len;
                return pos;
            }
        }
        if (anchored)
            return NULL;
        pos = fr_utf8_next(pos);
    }

    return NULL;
}
#endif

/// @brief Appends one match record, growing the owned array as needed.
///
/// @param bar Bar that owns the match array.
/// @param line Zero-based editor line.
/// @param start Starting UTF-8 code-point column.
/// @param end Exclusive ending UTF-8 code-point column.
static void add_match(vg_findreplacebar_t *bar, uint32_t line, uint32_t start, uint32_t end) {
    // Grow array if needed
    if (bar->match_count >= bar->match_capacity) {
        size_t new_cap = bar->match_capacity ? bar->match_capacity : INITIAL_MATCH_CAPACITY;
        if (new_cap < INITIAL_MATCH_CAPACITY)
            new_cap = INITIAL_MATCH_CAPACITY;
        while (new_cap <= bar->match_count) {
            if (new_cap > SIZE_MAX / 2)
                return;
            new_cap *= 2;
        }
        if (new_cap > SIZE_MAX / sizeof(vg_search_match_t))
            return;

        vg_search_match_t *new_matches = realloc(bar->matches, new_cap * sizeof(vg_search_match_t));
        if (!new_matches)
            return;

        bar->matches = new_matches;
        bar->match_capacity = new_cap;
    }

    bar->matches[bar->match_count].line = line;
    bar->matches[bar->match_count].start_col = start;
    bar->matches[bar->match_count].end_col = end;
    bar->match_count++;
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
    snprintf(bar->result_text, sizeof(bar->result_text), "");
}

/// @brief Rescans the linked editor for the current query and options.
///
/// @details Matches are rebuilt across all lines, converted from byte offsets
/// to editor code-point columns, and the first result is highlighted. POSIX
/// regular expressions are compiled once per scan. The optional find callback
/// runs after search state and result text are updated.
///
/// @param bar Bar whose search state is rebuilt.
static void perform_search(vg_findreplacebar_t *bar) {
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

#ifndef _WIN32
    regex_t regex;
    bool regex_ready = false;
    if (bar->options.use_regex) {
        int flags = REG_EXTENDED;
        if (!bar->options.case_sensitive)
            flags |= REG_ICASE;
        if (regcomp(&regex, query, flags) != 0) {
            snprintf(bar->result_text, sizeof(bar->result_text), "Invalid regex");
            return;
        }
        regex_ready = true;
    }
#else
    bool regex_ready = bar->options.use_regex;
#endif

    // Search through editor lines
    for (int line = 0; line < ed->line_count; line++) {
        const char *text = ed->lines[line].text;
        if (!text)
            continue;

        const char *pos = text;
        size_t match_len;

#ifndef _WIN32
        while ((pos = regex_ready ? find_regex_in_line(text, pos, &regex, &bar->options, &match_len)
                                  : find_in_line(pos, query, &bar->options, &match_len)) != NULL) {
#else
        while ((pos = regex_ready ? find_regex_in_line(text, pos, query, &bar->options, &match_len)
                                  : find_in_line(pos, query, &bar->options, &match_len)) != NULL) {
#endif
            size_t start_byte = (size_t)(pos - text);
            size_t end_byte = start_byte + match_len;
            uint32_t start_col = fr_utf8_col_from_byte_offset(text, start_byte);
            uint32_t end_col = fr_utf8_col_from_byte_offset(text, end_byte);
            add_match(bar, (uint32_t)line, start_col, end_col);
            // Advance past the entire match (non-overlapping). This also keeps
            // the cursor on a UTF-8 codepoint boundary; the previous pos++ could
            // land mid-character when the match contained multi-byte codepoints.
            pos += match_len > 0 ? match_len : 1;
        }
    }

#ifndef _WIN32
    if (regex_ready)
        regfree(&regex);
#endif

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
}

/// @brief Updates the human-readable match summary.
///
/// @param bar Bar whose result buffer is rewritten.
static void update_result_text(vg_findreplacebar_t *bar) {
    if (bar->match_count == 0) {
        vg_textinput_t *find_input = (vg_textinput_t *)bar->find_input;
        const char *query = find_input ? vg_textinput_get_text(find_input) : NULL;
        if (query && *query) {
            snprintf(bar->result_text, sizeof(bar->result_text), "No results");
        } else {
            bar->result_text[0] = '\0';
        }
    } else {
        snprintf(bar->result_text,
                 sizeof(bar->result_text),
                 "%zu of %zu",
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
    perform_search(bar);
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

    // Layout first row: Find input, prev/next buttons, options, close
    float row_y = y + PADDING;
    float cur_x = x + PADDING;
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
        float close_x = x + width - BUTTON_WIDTH - PADDING;
        vg_widget_arrange(w, close_x, row_y + 4, BUTTON_WIDTH, row_height - 8);
    }

    // Second row (replace mode): Replace input, replace/all buttons
    if (bar->show_replace) {
        row_y = y + FINDREPLACEBAR_HEIGHT;
        cur_x = x + PADDING;

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

    // Delete selection (current match) and insert replacement
    vg_codeeditor_delete_selection(ed);
    vg_codeeditor_insert_text(ed, replace_text);

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
    size_t replacement_count = bar->match_count;

    // Replace from end to start to preserve positions
    for (size_t i = bar->match_count; i > 0; i--) {
        vg_search_match_t *match = &bar->matches[i - 1];

        // Select match
        vg_codeeditor_set_selection(
            ed, (int)match->line, (int)match->start_col, (int)match->line, (int)match->end_col);

        // Replace
        vg_codeeditor_delete_selection(ed);
        vg_codeeditor_insert_text(ed, replace_text);
    }

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
