//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/gui/rt_gui_codeeditor_syntax.c
// Purpose: CodeEditor syntax-highlighting feature (language tokenizing, token
//          colors, custom keywords, highlight spans, inlay hints) plus the
//          shared column/handle/selection helpers used by all editor features.
//          Split out of rt_gui_codeeditor.c.
//
// Key invariants:
//   - Mirrors rt_gui_codeeditor.c's ZANNA_ENABLE_GRAPHICS guard: real syntax
//     functions when graphics is enabled, no-op stubs otherwise.
//   - The shared helpers (rt_codeeditor_handle_checked, column conversion,
//     normalize_selection) are exported via rt_gui_codeeditor_internal.h.
//
// Ownership/Lifetime:
//   - Borrows the caller's editor widget; highlight/keyword storage is owned by
//     the underlying vg_codeeditor_t.
//
// Links: src/runtime/graphics/gui/rt_gui_codeeditor.c (editor core + features),
//        src/runtime/graphics/gui/rt_gui_codeeditor_internal.h (shared helpers)
//
//===----------------------------------------------------------------------===//

/// @file rt_gui_codeeditor_syntax.c
/// @brief Implements CodeEditor lexical coloring, semantic overlays, and shared text helpers.
///
/// @details
/// The graphics-enabled implementation tokenizes Zia, BASIC, and Zanna IL,
/// manages user-defined highlighting and inlay state, and supplies UTF-8-aware
/// editor utilities. Graphics-disabled definitions preserve the runtime ABI
/// with deterministic no-op behavior.

#include "rt_error.h"
#include "rt_gui_codeeditor_internal.h"
#include "rt_gui_internal.h"
#include "rt_pixels.h"
#include "rt_platform.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef ZANNA_ENABLE_GRAPHICS

/// @brief Duplicate a syntax keyword token with malloc ownership.
/// @details Custom keyword lists are stored as `char *` entries and released
///          with `free`; this helper avoids platform-specific `strdup`
///          availability while keeping the parser's cleanup paths unchanged.
/// @param text Source token to copy; NULL returns NULL.
/// @return Newly allocated copy, or NULL on invalid input, overflow, or OOM.
static char *rt_codeeditor_syntax_strdup(const char *text) {
    if (!text)
        return NULL;
    size_t len = strlen(text);
    if (len > SIZE_MAX - 1u)
        return NULL;
    char *copy = (char *)malloc(len + 1u);
    if (!copy)
        return NULL;
    memcpy(copy, text, len + 1u);
    return copy;
}

//=============================================================================
// CodeEditor Enhancements - Syntax Highlighting (Phase 4)
//=============================================================================

// VS Code dark-theme inspired palette (ARGB 0xAARRGGBB)
#define SYN_COLOR_DEFAULT 0xFFD4D4D4u   // light grey
#define SYN_COLOR_KEYWORD 0xFF569CD6u   // blue
#define SYN_COLOR_TYPE 0xFF4EC9B0u      // teal
#define SYN_COLOR_STRING 0xFFCE9178u    // orange
#define SYN_COLOR_COMMENT 0xFF6A9955u   // green
#define SYN_COLOR_NUMBER 0xFFB5CEA8u    // light green
#define SYN_COLOR_FUNCTION 0xFFDCDCAAu  // yellow — function/method calls
#define SYN_COLOR_OPERATOR 0xFF56B6C2u  // soft cyan — operator punctuation
#define SYN_COLOR_BRACKET 0xFFE0C878u   // muted gold — brackets / delimiters
#define SYN_COLOR_PARAMETER 0xFF9CDCFEu // light blue — parameters (semantic)
#define SYN_COLOR_PROPERTY 0xFF9CDCFEu  // light blue — properties (semantic)
#define SYN_COLOR_CONSTANT 0xFF4FC1FFu  // bright blue — constants (semantic)
#define SYN_COLOR_DECORATOR 0xFFDCDCAAu // yellow — attributes / decorators

/// @brief Set `n` adjacent slots in the per-character `colors` array to `color`.
///
/// Used by every tokenizer to paint a span of characters (a keyword,
/// string literal, comment) the same color in one sweep.
/// @param[out] colors Per-byte color array to update.
/// @param pos First array index to paint.
/// @param n Number of consecutive entries to paint.
/// @param color Packed ARGB color assigned to the span.
static void syn_fill(uint32_t *colors, size_t pos, size_t n, uint32_t color) {
    for (size_t i = 0; i < n; i++)
        colors[pos + i] = color;
}

/// @brief Pick the color for a token kind, honoring per-editor overrides.
///
/// Each editor instance can override the default theme colors via
/// `SetTokenColor`. If no override is set, returns the supplied
/// `fallback` (which is the VS Code dark-theme default).
///
/// `token_type` is a `vg_syntax_token_type` value (0..VG_SYN_TOKEN_COUNT-1).
/// @param ce CodeEditor supplying optional token-color overrides.
/// @param token_type Syntax token category.
/// @param fallback Default packed ARGB color.
/// @return Editor-specific override when present; otherwise @p fallback.
static uint32_t syn_color(vg_codeeditor_t *ce, int token_type, uint32_t fallback) {
    if (ce && token_type >= 0 && token_type < VG_SYN_TOKEN_COUNT && ce->token_colors[token_type])
        return ce->token_colors[token_type];
    return fallback;
}

/// @brief Safe-cast an opaque handle to a live CodeEditor widget.
/// @param editor Candidate runtime widget handle.
/// @return The code editor, or NULL if @p editor is not a live one.
vg_codeeditor_t *rt_codeeditor_handle_checked(void *editor) {
    RT_ASSERT_MAIN_THREAD();
    return (vg_codeeditor_t *)rt_gui_widget_handle_checked_type(editor, VG_WIDGET_CODEEDITOR);
}

/// @brief Invalidate cached lexical state and request repainting of an editor.
/// @param ce CodeEditor whose generation and per-line cache markers are reset.
static void rt_codeeditor_invalidate_syntax_cache(vg_codeeditor_t *ce) {
    if (!ce)
        return;
    ce->highlight_generation =
        ce->highlight_generation == UINT64_MAX ? 1 : ce->highlight_generation + 1;
    for (int i = 0; i < ce->line_count; i++) {
        ce->lines[i].highlight_generation = 0;
        ce->lines[i].syntax_state_generation = 0;
    }
    ce->base.needs_paint = true;
}

/// @brief Validate a gutter-marker slot index (0–3) and report it via @p out_type.
/// @param slot Runtime slot index; slots 0 through 4 are accepted.
/// @param[out] out_type Optional destination for the validated slot.
/// @return 1 if the slot is in range; 0 otherwise (leaving `*out_type` untouched).
int rt_codeeditor_gutter_slot_checked(int64_t slot, int *out_type) {
    // Slots 0-3 are the disc/image icon slots (0=breakpoint, 1=warning,
    // 2=error, 3=info). Slot 4 is the SCM change-bar slot; it is only ever used
    // with SetGutterBar (which supplies an explicit color), so it never indexes
    // the 4-entry default-color table.
    if (slot < 0 || slot > 4)
        return 0;
    if (out_type)
        *out_type = (int)slot;
    return 1;
}

/// @brief Byte length of a line clamped to INT_MAX; 0 for an out-of-range line index.
/// @param ce CodeEditor containing the line.
/// @param line Zero-based logical line index.
/// @return Line length in bytes, clamped to `INT_MAX`, or zero for invalid input.
int rt_codeeditor_line_length_i32(const vg_codeeditor_t *ce, int line) {
    if (!ce || line < 0 || line >= ce->line_count)
        return 0;
    return ce->lines[line].length > (size_t)INT_MAX ? INT_MAX : (int)ce->lines[line].length;
}

/// @brief Byte length (1–4) of the UTF-8 sequence beginning at @p p.
/// @details Validates continuation bytes and respects @p remaining so a truncated
///          multibyte sequence at the buffer end cannot over-read. Any malformed
///          lead byte yields 1, guaranteeing forward progress in column-walk loops.
/// @param p Address of the candidate UTF-8 lead byte.
/// @param remaining Number of readable bytes starting at @p p.
/// @return Validated span length from one through four, or zero when no byte is available.
static size_t rt_codeeditor_utf8_span(const char *p, size_t remaining) {
    if (!p || remaining == 0)
        return 0;
    if (*p == '\0')
        return 1;
    const unsigned char *s = (const unsigned char *)p;
    if ((s[0] & 0x80u) == 0)
        return 1;
    if ((s[0] & 0xE0u) == 0xC0u && remaining >= 2 && (s[1] & 0xC0u) == 0x80u)
        return 2;
    if ((s[0] & 0xF0u) == 0xE0u && remaining >= 3 && (s[1] & 0xC0u) == 0x80u &&
        (s[2] & 0xC0u) == 0x80u)
        return 3;
    if ((s[0] & 0xF8u) == 0xF0u && remaining >= 4 && (s[1] & 0xC0u) == 0x80u &&
        (s[2] & 0xC0u) == 0x80u && (s[3] & 0xC0u) == 0x80u)
        return 4;
    return 1;
}

/// @brief Convert a byte offset within a line to a character (codepoint) column.
/// @details The editor stores text as UTF-8 bytes but the scripting API speaks in
///          characters; this walks the line by UTF-8 spans so multibyte glyphs count
///          as one column. @p byte_col is clamped to the line length and never splits
///          a multibyte sequence (it stops at the last whole character that fits).
/// @param ce CodeEditor containing the line.
/// @param line Zero-based logical line index.
/// @param byte_col Byte offset to translate.
/// @return Zero-based character column, or zero for invalid input.
int rt_codeeditor_byte_col_to_char_col(const vg_codeeditor_t *ce, int line, int byte_col) {
    if (!ce || line < 0 || line >= ce->line_count || byte_col <= 0)
        return 0;
    const vg_code_line_t *code_line = &ce->lines[line];
    if (byte_col > (int)code_line->length)
        byte_col = (int)code_line->length;
    int chars = 0;
    size_t pos = 0;
    while (pos < (size_t)byte_col) {
        size_t span = rt_codeeditor_utf8_span(code_line->text + pos, code_line->length - pos);
        if (pos + span > (size_t)byte_col)
            break;
        pos += span;
        chars++;
    }
    return chars;
}

/// @brief Convert a character (codepoint) column to a byte offset within a line.
/// @details Inverse of rt_codeeditor_byte_col_to_char_col: advances @p char_col
///          UTF-8 spans (or to end of line), then returns the byte position clamped
///          to INT_MAX.
/// @param ce CodeEditor containing the line.
/// @param line Zero-based logical line index.
/// @param char_col Character column to translate.
/// @return UTF-8 byte offset clamped to `INT_MAX`, or zero for invalid input.
int rt_codeeditor_char_col_to_byte_col(const vg_codeeditor_t *ce, int line, int char_col) {
    if (!ce || line < 0 || line >= ce->line_count || char_col <= 0)
        return 0;
    const vg_code_line_t *code_line = &ce->lines[line];
    int chars = 0;
    size_t pos = 0;
    while (pos < code_line->length && chars < char_col) {
        size_t span = rt_codeeditor_utf8_span(code_line->text + pos, code_line->length - pos);
        pos += span;
        chars++;
    }
    return pos > (size_t)INT_MAX ? INT_MAX : (int)pos;
}

/// @brief Lexicographic compare of two (line, col) caret positions.
/// @param lhs_line Left-hand logical line.
/// @param lhs_col Left-hand character column.
/// @param rhs_line Right-hand logical line.
/// @param rhs_col Right-hand character column.
/// @return -1 if lhs precedes rhs, 1 if it follows, 0 if equal.
static int rt_codeeditor_compare_positions(int lhs_line, int lhs_col, int rhs_line, int rhs_col) {
    if (lhs_line != rhs_line)
        return lhs_line < rhs_line ? -1 : 1;
    if (lhs_col != rhs_col)
        return lhs_col < rhs_col ? -1 : 1;
    return 0;
}

/// @brief Order a selection so `start <= end`, swapping the endpoints if the user
///        dragged backwards. Lets downstream range logic assume a forward span.
/// @param[in,out] selection Selection whose endpoints should be normalized.
void rt_codeeditor_normalize_selection(vg_selection_t *selection) {
    if (!selection)
        return;
    if (rt_codeeditor_compare_positions(
            selection->start_line, selection->start_col, selection->end_line, selection->end_col) <=
        0)
        return;
    int line = selection->start_line;
    int col = selection->start_col;
    selection->start_line = selection->end_line;
    selection->start_col = selection->end_col;
    selection->end_line = line;
    selection->end_col = col;
}

/// @brief Linear-scan the editor's user-supplied keyword list for an exact match.
///
/// Custom keywords let scripts add domain-specific syntax (e.g., your
/// game's scripting commands) without modifying the built-in tables.
/// Case-sensitive — matches `SetCustomKeywords`'s contract.
/// @param word Candidate token bytes.
/// @param wlen Candidate token length.
/// @param ce CodeEditor owning the custom keyword table.
/// @return `1` for an exact custom-keyword match; otherwise `0`.
static int syn_is_custom_keyword(const char *word, size_t wlen, vg_codeeditor_t *ce) {
    if (!ce || !ce->custom_keywords)
        return 0;
    for (int i = 0; i < ce->custom_keyword_count; i++) {
        if (!ce->custom_keywords[i])
            continue;
        size_t klen = strlen(ce->custom_keywords[i]);
        if (klen == wlen && memcmp(word, ce->custom_keywords[i], wlen) == 0)
            return 1;
    }
    return 0;
}

/// @brief True if `c` may begin an identifier (letter or underscore).
///
/// ASCII-only — Unicode identifiers are not yet supported by the
/// tokenizer (would require a full UTF-8 codepoint classifier).
/// @param c Character to classify.
/// @return Non-zero for an ASCII letter or underscore.
static int syn_is_id_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

/// @brief True if `c` may continue an identifier (id-start chars + digits).
/// @param c Character to classify.
/// @return Non-zero for an ASCII identifier-continuation character.
static int syn_is_id_cont(char c) {
    return syn_is_id_start(c) || (c >= '0' && c <= '9');
}

/// @brief True for a hexadecimal digit (0-9, a-f, A-F).
/// @param c Character to classify.
/// @return Non-zero when @p c is an ASCII hexadecimal digit.
static int syn_is_hex_digit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/// @brief Scan a numeric literal starting at `start` and return the index just
///        past it. Handles `0x`/`0X` hex, `0b`/`0B` binary, a decimal fractional
///        part, digit-group underscores, and an `e`/`E[+/-]` exponent. The caller
///        guarantees text[start] is a decimal digit.
/// @param text Source line being scanned.
/// @param len Readable byte length of @p text.
/// @param start Index of the first decimal digit.
/// @return Index immediately after the recognized numeric literal.
static size_t syn_scan_number(const char *text, size_t len, size_t start) {
    size_t i = start;
    if (text[i] == '0' && i + 1 < len && (text[i + 1] == 'x' || text[i + 1] == 'X')) {
        i += 2;
        while (i < len && (syn_is_hex_digit(text[i]) || text[i] == '_'))
            i++;
        return i;
    }
    if (text[i] == '0' && i + 1 < len && (text[i + 1] == 'b' || text[i + 1] == 'B')) {
        i += 2;
        while (i < len && (text[i] == '0' || text[i] == '1' || text[i] == '_'))
            i++;
        return i;
    }
    while (i < len && ((text[i] >= '0' && text[i] <= '9') || text[i] == '.' || text[i] == '_'))
        i++;
    if (i < len && (text[i] == 'e' || text[i] == 'E')) {
        size_t e = i + 1;
        if (e < len && (text[e] == '+' || text[e] == '-'))
            e++;
        if (e < len && text[e] >= '0' && text[e] <= '9') {
            i = e;
            while (i < len && ((text[i] >= '0' && text[i] <= '9') || text[i] == '_'))
                i++;
        }
    }
    return i;
}

/// @brief True for a bracket / delimiter character: ( ) [ ] { }.
/// @param c Character to classify.
/// @return Non-zero for a supported bracket or delimiter.
static int syn_is_bracket(char c) {
    return c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}';
}

/// @brief True for an operator punctuation character.
/// @details Deliberately excludes `.`, `,`, `;` (kept at the default color so
///          member access and separators do not become visually noisy).
/// @param c Character to classify.
/// @return Non-zero for punctuation treated as an operator.
static int syn_is_operator(char c) {
    return c == '+' || c == '-' || c == '*' || c == '/' || c == '%' || c == '=' || c == '<' ||
           c == '>' || c == '!' || c == '&' || c == '|' || c == '^' || c == '~' || c == '?' ||
           c == ':';
}

/// @brief Compare `len` chars of `a` and `b` ignoring ASCII case.
///
/// Folds lowercase letters via the bit-flip trick (`a..z` differ from
/// `A..Z` by exactly 0x20). Used for BASIC keyword matching, which is
/// case-insensitive.
/// @param a First byte sequence.
/// @param b Second byte sequence.
/// @param len Number of bytes to compare.
/// @return `1` when the sequences are equal ignoring ASCII case; otherwise `0`.
static int syn_word_eq_ci(const char *a, const char *b, size_t len) {
    for (size_t i = 0; i < len; i++) {
        char ca = (a[i] >= 'a' && a[i] <= 'z') ? a[i] - 32 : a[i];
        char cb = (b[i] >= 'a' && b[i] <= 'z') ? b[i] - 32 : b[i];
        if (ca != cb)
            return 0;
    }
    return 1;
}

/// @brief Match `word[0..wlen]` against a NULL-terminated keyword table (case-sensitive).
///
/// Used for Zia keywords/types — Zia is case-sensitive. Linear scan;
/// keyword tables are short enough that hashing isn't worth it.
/// @param word Candidate token bytes.
/// @param wlen Candidate token length.
/// @param table Null-terminated table of case-sensitive keywords.
/// @return `1` when the token exactly matches a table entry; otherwise `0`.
static int syn_is_keyword(const char *word, size_t wlen, const char *const *table) {
    for (int i = 0; table[i]; i++) {
        size_t klen = strlen(table[i]);
        if (klen == wlen && memcmp(word, table[i], wlen) == 0)
            return 1;
    }
    return 0;
}

/// @brief Case-insensitive variant of `syn_is_keyword` for BASIC.
/// @param word Candidate token bytes.
/// @param wlen Candidate token length.
/// @param table Null-terminated table of keywords.
/// @return `1` when the token matches an entry ignoring ASCII case; otherwise `0`.
static int syn_is_keyword_ci(const char *word, size_t wlen, const char *const *table) {
    for (int i = 0; table[i]; i++) {
        size_t klen = strlen(table[i]);
        if (klen == wlen && syn_word_eq_ci(word, table[i], wlen))
            return 1;
    }
    return 0;
}

// ─── Zia language tokenizer ────────────────────────────────────────────────

// Bridge into zia_editor_services' authoritative kKeywordTable (52 entries).
// Strong impl in src/frontends/zia/rt_zia_highlight.cpp; weak fallback in
// src/runtime/core/rt_zia_highlight_stub.c returns 0 (no keyword) when editor
// services are not linked. The zia binary force-loads zia_editor_services
// (see src/CMakeLists.txt) so the strong implementation always wins for the IDE.
extern int rt_zia_is_keyword(const char *name, int64_t len);

// Type names highlighted as types (teal). The real lexer doesn't separate
// these from identifiers, but VS Code-style coloring distinguishes the
// common runtime-class names visually. Keep this list curated; kept short
// because the user's biggest pain was missing keywords, not missing types.
static const char *const zia_types[] = {"Integer",
                                        "Boolean",
                                        "String",
                                        "Number",
                                        "Byte",
                                        "List",
                                        "Seq",
                                        "Map",
                                        "Set",
                                        "Stack",
                                        "Queue",
                                        NULL};

/// @brief Scan one Zia line and update nested block-comment depth.
/// @param line Null-terminated source line; `NULL` leaves the depth unchanged.
/// @param depth Open block-comment depth on entry.
/// @return Block-comment depth after the line, ignoring comment markers in strings.
static int rt_zia_comment_depth_after_text(const char *line, int depth) {
    if (!line)
        return depth;

    size_t len = strlen(line);
    for (size_t i = 0; i < len;) {
        if (depth > 0) {
            if (i + 1 < len && line[i] == '/' && line[i + 1] == '*') {
                depth++;
                i += 2;
            } else if (i + 1 < len && line[i] == '*' && line[i + 1] == '/') {
                depth--;
                i += 2;
            } else {
                i++;
            }
            continue;
        }

        if (i + 1 < len && line[i] == '/' && line[i + 1] == '/')
            break;
        if (i + 1 < len && line[i] == '/' && line[i + 1] == '*') {
            depth = 1;
            i += 2;
            continue;
        }
        if (line[i] == '"') {
            i++;
            while (i < len) {
                if (line[i] == '\\' && i + 1 < len) {
                    i += 2;
                    continue;
                }
                if (line[i++] == '"')
                    break;
            }
            continue;
        }
        i++;
    }

    return depth;
}

/// @brief Compute the open Zia block-comment nesting depth at the start of
///        @p line_num.
/// @details Caches per-line lexical state so painting a viewport near the end
///          of a large file does not rescan all preceding lines for every
///          visible row.
/// @param ce CodeEditor providing line text and cached lexical state.
/// @param line_num Zero-based line whose incoming state is requested.
/// @return Open nested block-comment depth immediately before @p line_num.
static int rt_zia_comment_depth_before_line(vg_codeeditor_t *ce, int line_num) {
    if (!ce || line_num <= 0 || !ce->lines)
        return 0;

    int limit = line_num < ce->line_count ? line_num : ce->line_count;
    uint64_t generation = ce->highlight_generation;
    int start = 0;
    int depth = 0;

    for (int i = limit - 1; i >= 0; i--) {
        if (ce->lines[i].syntax_state_generation == generation) {
            start = i + 1;
            depth = ce->lines[i].syntax_state_out;
            break;
        }
    }

    for (int line_index = start; line_index < limit; line_index++) {
        vg_code_line_t *line = &ce->lines[line_index];
        line->syntax_state_in = depth;
        depth = rt_zia_comment_depth_after_text(line->text ? line->text : "", depth);
        line->syntax_state_out = depth;
        line->syntax_state_generation = generation;
        ce->perf_stats.syntax_state_line_scans++;
    }

    return depth;
}

/// @brief Tokenize a Zia source line and write per-character color codes.
///
/// Walks the line once, classifying each span:
///   - `// ...` line comment → green
///   - `/* ... */` block comment → green. Multi-line depth is derived from
///     prior buffer lines so highlighting is independent of render order.
///   - `"..."` string (with `\\` escape handling) → orange
///   - `[0-9]+` number → light green
///   - identifier → keyword (via real Zia keyword table) / type / custom
///     keyword / default lookup
///   - everything else → default color
/// @param editor Widget-layer editor invoking the callback.
/// @param line_num Zero-based logical line being highlighted.
/// @param text Null-terminated UTF-8 line text.
/// @param[out] colors Per-byte color array populated by the tokenizer.
/// @param user_data CodeEditor instance supplying theme and cached syntax state.
static void rt_zia_syntax_cb(
    vg_widget_t *editor, int line_num, const char *text, uint32_t *colors, void *user_data) {
    vg_codeeditor_t *ce = (vg_codeeditor_t *)user_data;
    (void)editor;

    uint32_t c_default = syn_color(ce, VG_SYN_TOKEN_DEFAULT, SYN_COLOR_DEFAULT);
    uint32_t c_keyword = syn_color(ce, VG_SYN_TOKEN_KEYWORD, SYN_COLOR_KEYWORD);
    uint32_t c_type = syn_color(ce, VG_SYN_TOKEN_TYPE, SYN_COLOR_TYPE);
    uint32_t c_string = syn_color(ce, VG_SYN_TOKEN_STRING, SYN_COLOR_STRING);
    uint32_t c_comment = syn_color(ce, VG_SYN_TOKEN_COMMENT, SYN_COLOR_COMMENT);
    uint32_t c_number = syn_color(ce, VG_SYN_TOKEN_NUMBER, SYN_COLOR_NUMBER);
    uint32_t c_function = syn_color(ce, VG_SYN_TOKEN_FUNCTION, SYN_COLOR_FUNCTION);
    uint32_t c_operator = syn_color(ce, VG_SYN_TOKEN_OPERATOR, SYN_COLOR_OPERATOR);
    uint32_t c_bracket = syn_color(ce, VG_SYN_TOKEN_BRACKET, SYN_COLOR_BRACKET);

    /* `len` bounds both the text scan and the colors[] writes. strlen(text) <= the line's stored
     * byte length, and the vg layer sizes colors[] to that stored length, so writes stay in
     * bounds (an embedded NUL only shortens the scan — harmless). Passing the line byte-length
     * and the colors capacity into the callback signature would be more robust against a line
     * buffer that is ever not NUL-terminated exactly at its length, but that is a lib-layer
     * (vg syntax-callback) contract change, out of scope for this runtime callback. */
    size_t len = strlen(text);
    size_t i = 0;
    int block_depth = rt_zia_comment_depth_before_line(ce, line_num);
    if (ce && line_num >= 0 && line_num < ce->line_count) {
        ce->lines[line_num].syntax_state_in = block_depth;
    }

    // If we entered this line inside a block comment, paint until we find */
    // (with proper nesting).
    if (block_depth > 0) {
        size_t comment_start = 0;
        while (i < len && block_depth > 0) {
            if (i + 1 < len && text[i] == '/' && text[i + 1] == '*') {
                block_depth++;
                i += 2;
            } else if (i + 1 < len && text[i] == '*' && text[i + 1] == '/') {
                block_depth--;
                i += 2;
            } else {
                i++;
            }
        }
        syn_fill(colors, comment_start, i - comment_start, c_comment);
        if (block_depth > 0) {
            // Block comment continues past this line — done with line.
            if (ce) {
                ce->zia_block_comment_depth = block_depth;
                if (line_num >= 0 && line_num < ce->line_count) {
                    ce->lines[line_num].syntax_state_out = block_depth;
                    ce->lines[line_num].syntax_state_generation = ce->highlight_generation;
                }
            }
            return;
        }
    }

    while (i < len) {
        // Line comment
        if (text[i] == '/' && i + 1 < len && text[i + 1] == '/') {
            syn_fill(colors, i, len - i, c_comment);
            if (ce && line_num >= 0 && line_num < ce->line_count) {
                ce->lines[line_num].syntax_state_out = block_depth;
                ce->lines[line_num].syntax_state_generation = ce->highlight_generation;
            }
            return;
        }

        // Block comment (may continue past end of line)
        if (text[i] == '/' && i + 1 < len && text[i + 1] == '*') {
            size_t start = i;
            block_depth = 1;
            i += 2;
            while (i < len && block_depth > 0) {
                if (i + 1 < len && text[i] == '/' && text[i + 1] == '*') {
                    block_depth++;
                    i += 2;
                } else if (i + 1 < len && text[i] == '*' && text[i + 1] == '/') {
                    block_depth--;
                    i += 2;
                } else {
                    i++;
                }
            }
            syn_fill(colors, start, i - start, c_comment);
            continue;
        }

        // String literal
        if (text[i] == '"') {
            size_t start = i++;
            while (i < len && text[i] != '"') {
                if (text[i] == '\\' && i + 1 < len)
                    i++; // skip escaped character
                i++;
            }
            if (i < len)
                i++; // closing quote
            syn_fill(colors, start, i - start, c_string);
            continue;
        }

        // Number literal (decimal/hex/binary, underscores, exponent).
        if (text[i] >= '0' && text[i] <= '9') {
            size_t start = i;
            i = syn_scan_number(text, len, i);
            syn_fill(colors, start, i - start, c_number);
            continue;
        }

        // Identifier or keyword
        if (syn_is_id_start(text[i])) {
            size_t start = i;
            while (i < len && syn_is_id_cont(text[i]))
                i++;
            size_t wlen = i - start;
            uint32_t color = c_default;
            if (rt_zia_is_keyword(text + start, (int64_t)wlen)) {
                color = c_keyword;
            } else if (syn_is_keyword(text + start, wlen, zia_types)) {
                color = c_type;
            } else if (syn_is_custom_keyword(text + start, wlen, ce)) {
                color = c_keyword;
            } else {
                // Peek past whitespace for an opening paren — identifies
                // function and method calls regardless of capitalization
                // (`packPixel(`, `Canvas3D.New(`, `Math.Sin(` all match).
                size_t peek = i;
                while (peek < len && (text[peek] == ' ' || text[peek] == '\t'))
                    peek++;
                if (peek < len && text[peek] == '(') {
                    color = c_function;
                } else if (wlen > 0 && text[start] >= 'A' && text[start] <= 'Z') {
                    // Identifiers starting with an uppercase letter are
                    // type / class / module names (Foo, MyClass, Math, Zanna).
                    // Matches VS Code / IntelliJ convention; gives the
                    // highlighter a much richer feel than a hand-curated
                    // 11-name type list.
                    color = c_type;
                }
            }
            syn_fill(colors, start, wlen, color);
            continue;
        }

        // Operators / brackets / punctuation
        if (syn_is_bracket(text[i]))
            colors[i] = c_bracket;
        else if (syn_is_operator(text[i]))
            colors[i] = c_operator;
        else
            colors[i] = c_default;
        i++;
    }
    if (ce) {
        ce->zia_block_comment_depth = block_depth;
        if (line_num >= 0 && line_num < ce->line_count) {
            ce->lines[line_num].syntax_state_out = block_depth;
            ce->lines[line_num].syntax_state_generation = ce->highlight_generation;
        }
    }
}

// ─── Zanna BASIC language tokenizer ───────────────────────────────────────

static const char *const basic_keywords[] = {
    "AND",       "AS",        "CALL",      "CASE",   "CLASS",  "CLOSE",     "CONST",
    "DECLARE",   "DIM",       "DO",        "EACH",   "ELSE",   "ELSEIF",    "END",
    "ENDIF",     "ENUM",      "ERASE",     "ERROR",  "EXIT",   "FALSE",     "FOR",
    "FUNCTION",  "GET",       "GOSUB",     "GOTO",   "IF",     "IMPLEMENTS","IN",
    "INPUT",     "INHERITS",  "INTERFACE", "LET",    "LINE",   "LOOP",      "MOD",
    "NAMESPACE", "NEW",       "NEXT",      "NOT",    "ON",     "OPEN",      "OR",
    "PRINT",     "PRIVATE",   "PROPERTY",  "PUBLIC", "PUT",    "RANDOMIZE", "REDIM",
    "RESUME",    "RETURN",    "SELECT",    "SHARED", "STATIC", "STEP",      "SUB",
    "SWAP",      "THEN",      "TO",        "TRUE",   "TYPE",   "UNTIL",     "USING",
    "WEND",      "WHILE",     "XOR",       NULL};

/// @brief Zanna BASIC built-in type names — coloured as types, not keywords.
static const char *const basic_types[] = {"BOOLEAN", "DOUBLE",  "FLOAT", "INT", "INTEGER",
                                          "LONG",    "SINGLE",  "STRING", NULL};

/// @brief Tokenize a Zanna BASIC source line.
///
/// Differences from Zia:
///   - Comments use `'` (apostrophe) or the `REM` keyword.
///   - Keyword matching is case-insensitive (`PRINT`, `print`, `Print`
///     all highlight).
///   - Built-in type names (INTEGER, DOUBLE, STRING, …) colour as types.
/// @param editor Widget-layer editor invoking the callback.
/// @param line_num Zero-based logical line being highlighted.
/// @param text Null-terminated BASIC source line.
/// @param[out] colors Per-byte color array populated by the tokenizer.
/// @param user_data CodeEditor instance supplying theme and custom keywords.
static void rt_basic_syntax_cb(
    vg_widget_t *editor, int line_num, const char *text, uint32_t *colors, void *user_data) {
    (void)line_num;
    vg_codeeditor_t *ce = (vg_codeeditor_t *)user_data;
    (void)editor;

    uint32_t c_default = syn_color(ce, VG_SYN_TOKEN_DEFAULT, SYN_COLOR_DEFAULT);
    uint32_t c_keyword = syn_color(ce, VG_SYN_TOKEN_KEYWORD, SYN_COLOR_KEYWORD);
    uint32_t c_type = syn_color(ce, VG_SYN_TOKEN_TYPE, SYN_COLOR_TYPE);
    uint32_t c_string = syn_color(ce, VG_SYN_TOKEN_STRING, SYN_COLOR_STRING);
    uint32_t c_comment = syn_color(ce, VG_SYN_TOKEN_COMMENT, SYN_COLOR_COMMENT);
    uint32_t c_number = syn_color(ce, VG_SYN_TOKEN_NUMBER, SYN_COLOR_NUMBER);
    uint32_t c_operator = syn_color(ce, VG_SYN_TOKEN_OPERATOR, SYN_COLOR_OPERATOR);
    uint32_t c_bracket = syn_color(ce, VG_SYN_TOKEN_BRACKET, SYN_COLOR_BRACKET);

    size_t len = strlen(text);
    size_t i = 0;

    while (i < len) {
        // Single-quote comment
        if (text[i] == '\'') {
            syn_fill(colors, i, len - i, c_comment);
            return;
        }

        // String literal
        if (text[i] == '"') {
            size_t start = i++;
            while (i < len && text[i] != '"')
                i++;
            if (i < len)
                i++;
            syn_fill(colors, start, i - start, c_string);
            continue;
        }

        // Number literal (decimal/hex/binary, underscores, exponent).
        if (text[i] >= '0' && text[i] <= '9') {
            size_t start = i;
            i = syn_scan_number(text, len, i);
            syn_fill(colors, start, i - start, c_number);
            continue;
        }

        // Identifier or keyword (case-insensitive for BASIC)
        if (syn_is_id_start(text[i])) {
            size_t start = i;
            while (i < len && syn_is_id_cont(text[i]))
                i++;
            size_t wlen = i - start;

            // REM comment: rest of line is a comment
            if (wlen == 3 && syn_word_eq_ci(text + start, "REM", 3)) {
                syn_fill(colors, start, len - start, c_comment);
                return;
            }

            uint32_t color = c_default;
            if (syn_is_keyword_ci(text + start, wlen, basic_keywords))
                color = c_keyword;
            else if (syn_is_keyword_ci(text + start, wlen, basic_types))
                color = c_type;
            else if (syn_is_custom_keyword(text + start, wlen, ce))
                color = c_keyword;
            syn_fill(colors, start, wlen, color);
            continue;
        }

        // Operators / brackets / punctuation
        if (syn_is_bracket(text[i]))
            colors[i] = c_bracket;
        else if (syn_is_operator(text[i]))
            colors[i] = c_operator;
        else
            colors[i] = c_default;
        i++;
    }
}

// ─── Zanna IL language tokenizer ──────────────────────────────────────────

// Structural keywords of the IL textual format (header, declarations, blocks).
static const char *const zanna_keywords[] = {
    "il", "extern", "global", "func", "const", "block", NULL};

// IL value types.
static const char *const zanna_types[] = {"i1",
                                          "i8",
                                          "i16",
                                          "i32",
                                          "i64",
                                          "f32",
                                          "f64",
                                          "ptr",
                                          "str",
                                          "void",
                                          "error",
                                          "resume_tok",
                                          NULL};

// IL opcode mnemonics (kept in sync with `zanna --dump-opcodes`). Opcodes embed
// '.' and '_', so the IL identifier scan treats '.' as a continuation char.
static const char *const zanna_opcodes[] = {"add",
                                            "sub",
                                            "mul",
                                            "iadd.ovf",
                                            "isub.ovf",
                                            "imul.ovf",
                                            "sdiv",
                                            "udiv",
                                            "srem",
                                            "urem",
                                            "sdiv.chk0",
                                            "udiv.chk0",
                                            "srem.chk0",
                                            "urem.chk0",
                                            "idx.chk",
                                            "and",
                                            "or",
                                            "xor",
                                            "shl",
                                            "lshr",
                                            "ashr",
                                            "fadd",
                                            "fsub",
                                            "fmul",
                                            "fdiv",
                                            "icmp_eq",
                                            "icmp_ne",
                                            "scmp_lt",
                                            "scmp_le",
                                            "scmp_gt",
                                            "scmp_ge",
                                            "ucmp_lt",
                                            "ucmp_le",
                                            "ucmp_gt",
                                            "ucmp_ge",
                                            "fcmp_eq",
                                            "fcmp_ne",
                                            "fcmp_lt",
                                            "fcmp_le",
                                            "fcmp_gt",
                                            "fcmp_ge",
                                            "fcmp_ord",
                                            "fcmp_uno",
                                            "sitofp",
                                            "fptosi",
                                            "cast.fp_to_si.rte.chk",
                                            "cast.fp_to_ui.rte.chk",
                                            "cast.si_narrow.chk",
                                            "cast.ui_narrow.chk",
                                            "cast.si_to_fp",
                                            "cast.ui_to_fp",
                                            "zext1",
                                            "trunc1",
                                            "alloca",
                                            "gep",
                                            "load",
                                            "store",
                                            "addr_of",
                                            "const_str",
                                            "gaddr",
                                            "const_null",
                                            "const.f64",
                                            "call",
                                            "call.indirect",
                                            "switch.i32",
                                            "br",
                                            "cbr",
                                            "ret",
                                            "trap.kind",
                                            "trap.from_err",
                                            "trap.err",
                                            "err.get_kind",
                                            "err.get_code",
                                            "err.get_ip",
                                            "err.get_line",
                                            "err.get_msg",
                                            "eh.push",
                                            "eh.pop",
                                            "resume.same",
                                            "resume.next",
                                            "resume.label",
                                            "eh.entry",
                                            "trap",
                                            NULL};

/// @brief True if `c` continues an IL identifier/opcode token (id chars + '.').
/// @param c Character to classify.
/// @return Non-zero for an identifier continuation or period.
static int syn_is_il_id_cont(char c) {
    return syn_is_id_cont(c) || c == '.';
}

/// @brief Tokenize a Zanna IL source line.
///
/// IL specifics:
///   - `#` or `//` introduce a line comment.
///   - `@name` is a global/function reference (function color); `%name` is an
///     SSA temp (parameter color).
///   - Opcode mnemonics may embed `.`/`_` (`iadd.ovf`, `cast.si_to_fp`), so the
///     identifier scan treats `.` as a continuation character.
/// @param editor Widget-layer editor invoking the callback.
/// @param line_num Zero-based logical line being highlighted.
/// @param text Null-terminated Zanna IL source line.
/// @param[out] colors Per-byte color array populated by the tokenizer.
/// @param user_data CodeEditor instance supplying theme overrides.
static void rt_zanna_syntax_cb(
    vg_widget_t *editor, int line_num, const char *text, uint32_t *colors, void *user_data) {
    (void)line_num;
    vg_codeeditor_t *ce = (vg_codeeditor_t *)user_data;
    (void)editor;

    uint32_t c_default = syn_color(ce, VG_SYN_TOKEN_DEFAULT, SYN_COLOR_DEFAULT);
    uint32_t c_keyword = syn_color(ce, VG_SYN_TOKEN_KEYWORD, SYN_COLOR_KEYWORD);
    uint32_t c_type = syn_color(ce, VG_SYN_TOKEN_TYPE, SYN_COLOR_TYPE);
    uint32_t c_string = syn_color(ce, VG_SYN_TOKEN_STRING, SYN_COLOR_STRING);
    uint32_t c_comment = syn_color(ce, VG_SYN_TOKEN_COMMENT, SYN_COLOR_COMMENT);
    uint32_t c_number = syn_color(ce, VG_SYN_TOKEN_NUMBER, SYN_COLOR_NUMBER);
    uint32_t c_global = syn_color(ce, VG_SYN_TOKEN_FUNCTION, SYN_COLOR_FUNCTION);
    uint32_t c_temp = syn_color(ce, VG_SYN_TOKEN_PARAMETER, SYN_COLOR_PARAMETER);
    uint32_t c_operator = syn_color(ce, VG_SYN_TOKEN_OPERATOR, SYN_COLOR_OPERATOR);
    uint32_t c_bracket = syn_color(ce, VG_SYN_TOKEN_BRACKET, SYN_COLOR_BRACKET);

    size_t len = strlen(text);
    size_t i = 0;

    while (i < len) {
        // Line comment: `#…` or `//…`
        if (text[i] == '#' || (text[i] == '/' && i + 1 < len && text[i + 1] == '/')) {
            syn_fill(colors, i, len - i, c_comment);
            return;
        }

        // String literal
        if (text[i] == '"') {
            size_t start = i++;
            while (i < len && text[i] != '"') {
                if (text[i] == '\\' && i + 1 < len)
                    i++;
                i++;
            }
            if (i < len)
                i++;
            syn_fill(colors, start, i - start, c_string);
            continue;
        }

        // Global/function (@) or SSA temp (%) reference
        if (text[i] == '@' || text[i] == '%') {
            uint32_t sig_color = (text[i] == '@') ? c_global : c_temp;
            size_t start = i++;
            while (i < len && syn_is_il_id_cont(text[i]))
                i++;
            syn_fill(colors, start, i - start, sig_color);
            continue;
        }

        // Number literal (decimal/hex/binary, underscores, exponent).
        if (text[i] >= '0' && text[i] <= '9') {
            size_t start = i;
            i = syn_scan_number(text, len, i);
            syn_fill(colors, start, i - start, c_number);
            continue;
        }

        // Identifier / opcode / keyword / type
        if (syn_is_id_start(text[i])) {
            size_t start = i;
            while (i < len && syn_is_il_id_cont(text[i]))
                i++;
            size_t wlen = i - start;
            uint32_t color = c_default;
            if (syn_is_keyword(text + start, wlen, zanna_keywords))
                color = c_keyword;
            else if (syn_is_keyword(text + start, wlen, zanna_types))
                color = c_type;
            else if (syn_is_keyword(text + start, wlen, zanna_opcodes))
                color = c_keyword;
            syn_fill(colors, start, wlen, color);
            continue;
        }

        // Operators / brackets / punctuation
        if (syn_is_bracket(text[i]))
            colors[i] = c_bracket;
        else if (syn_is_operator(text[i]))
            colors[i] = c_operator;
        else
            colors[i] = c_default;
        i++;
    }
}

// ─── Public: set language ─────────────────────────────────────────────────

/// @brief `CodeEditor.SetLanguage(language)` — install a syntax-highlight callback.
///
/// Recognized values: `"zia"`, `"basic"`, `"zanna"`/`"il"`. Anything else
/// (including empty string) installs the no-op highlighter (plain text).
/// The editor pointer itself is the `user_data` for the callback so
/// the tokenizer can read the per-editor color overrides + custom
/// keyword list.
/// @param editor CodeEditor widget handle.
/// @param language Runtime language name selecting the tokenizer.
void rt_codeeditor_set_language(void *editor, rt_string language) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce)
        return;
    char *clang = rt_string_to_cstr_no_nul(language);
    if (language && !clang)
        return;
    if (!clang)
        return;

    // Pass the editor itself as user_data so the syntax callback can read
    // per-editor token_colors[] and custom_keywords[].
    if (strcmp(clang, "zia") == 0)
        vg_codeeditor_set_syntax(ce, rt_zia_syntax_cb, ce);
    else if (strcmp(clang, "basic") == 0)
        vg_codeeditor_set_syntax(ce, rt_basic_syntax_cb, ce);
    else if (strcmp(clang, "zanna") == 0 || strcmp(clang, "il") == 0)
        vg_codeeditor_set_syntax(ce, rt_zanna_syntax_cb, ce);
    else
        vg_codeeditor_set_syntax(ce, NULL, NULL); // plain text

    free(clang);
}

/// @brief `CodeEditor.SetTokenColor(tokenType, color)` — override one theme color.
///
/// `tokenType` is a `vg_syntax_token_type` value: 0=default, 1=keyword, 2=type,
/// 3=string, 4=comment, 5=number, 6=function, 7=operator, 8=bracket,
/// 9=parameter, 10=property, 11=constant, 12=decorator. `color`: ARGB
/// 0xAARRGGBB. Out-of-range types are ignored. Triggers a repaint.
/// @param editor CodeEditor widget handle.
/// @param token_type Syntax token category to override.
/// @param color Packed ARGB replacement color.
void rt_codeeditor_set_token_color(void *editor, int64_t token_type, int64_t color) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce)
        return;
    if (token_type >= 0 && token_type < VG_SYN_TOKEN_COUNT) {
        ce->token_colors[token_type] = (uint32_t)color;
        rt_codeeditor_invalidate_syntax_cache(ce);
    }
}

/// @brief `CodeEditor.SetCustomKeywords(keywords)` — install a comma-separated keyword list.
///
/// The list is parsed into `ce->custom_keywords` (newly allocated copy
/// of each token, leading/trailing whitespace trimmed). Replaces any
/// previous list (no append). Empty input clears the list. Doubling
/// growth from an initial capacity of 8.
/// @param editor CodeEditor widget handle.
/// @param keywords Comma-separated runtime string; empty input clears the list.
void rt_codeeditor_set_custom_keywords(void *editor, rt_string keywords) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce)
        return;

    // Parse comma-separated keywords into array
    char *ckw = rt_string_to_cstr_no_nul(keywords);
    if (keywords && !ckw)
        return;
    if (!ckw || !ckw[0]) {
        free(ckw);
        for (int i = 0; i < ce->custom_keyword_count; i++)
            free(ce->custom_keywords[i]);
        free(ce->custom_keywords);
        ce->custom_keywords = NULL;
        ce->custom_keyword_count = 0;
        rt_codeeditor_invalidate_syntax_cache(ce);
        return;
    }

    // Count commas to estimate capacity
    int cap = 8;
    char **new_keywords = (char **)malloc((size_t)cap * sizeof(char *));
    if (!new_keywords) {
        free(ckw);
        return;
    }
    int new_count = 0;
    int ok = 1;

    char *saveptr = NULL;
    char *token = rt_strtok_r(ckw, ",", &saveptr);
    while (token) {
        // Trim whitespace
        while (isspace((unsigned char)*token))
            token++;
        char *end = token + strlen(token);
        while (end > token && isspace((unsigned char)end[-1]))
            *--end = '\0';

        if (*token) {
            if (new_count >= cap) {
                if (cap > INT_MAX / 2 || (size_t)cap * 2 > SIZE_MAX / sizeof(char *)) {
                    ok = 0;
                    break;
                }
                cap *= 2;
                char **p = (char **)realloc(new_keywords, (size_t)cap * sizeof(char *));
                if (!p) {
                    ok = 0;
                    break;
                }
                new_keywords = p;
            }
            char *copy = rt_codeeditor_syntax_strdup(token);
            if (!copy) {
                ok = 0;
                break;
            }
            new_keywords[new_count++] = copy;
        }
        token = rt_strtok_r(NULL, ",", &saveptr);
    }
    free(ckw);

    if (!ok) {
        for (int i = 0; i < new_count; i++)
            free(new_keywords[i]);
        free(new_keywords);
        return;
    }

    for (int i = 0; i < ce->custom_keyword_count; i++)
        free(ce->custom_keywords[i]);
    free(ce->custom_keywords);
    ce->custom_keywords = new_keywords;
    ce->custom_keyword_count = new_count;
    rt_codeeditor_invalidate_syntax_cache(ce);
}

/// @brief `CodeEditor.ClearHighlights()` — remove every custom highlight span.
///
/// Highlights are user-defined colored ranges painted on top of the
/// syntax highlighting (e.g., for diagnostics, find-results, etc.).
/// @param editor CodeEditor widget handle.
void rt_codeeditor_clear_highlights(void *editor) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce)
        return;
    free(ce->highlight_spans);
    ce->highlight_spans = NULL;
    ce->highlight_span_count = 0;
    ce->highlight_span_cap = 0;
    ce->highlight_spans_sorted = true;
    free(ce->highlight_line_offsets);
    free(ce->highlight_line_span_indices);
    ce->highlight_line_offsets = NULL;
    ce->highlight_line_span_indices = NULL;
    ce->highlight_line_offsets_cap = 0;
    ce->highlight_line_span_indices_cap = 0;
    ce->highlight_line_index_line_count = 0;
    ce->highlight_line_index_span_count = 0;
    ce->highlight_line_index_valid = false;
    ce->base.needs_paint = true;
}

/// @brief `CodeEditor.AddHighlight(line0, col0, line1, col1, color)` — add a colored range.
///
/// Spans are inclusive on start, exclusive on end. Geometric growth
/// for the spans array (cap doubles, starting at 8). Silently no-ops
/// on OOM (better than trapping the editor).
/// @param editor CodeEditor widget handle.
/// @param start_line Zero-based inclusive start line.
/// @param start_col Zero-based inclusive start column.
/// @param end_line Zero-based exclusive-end line.
/// @param end_col Zero-based exclusive end column.
/// @param color Packed ARGB overlay color.
void rt_codeeditor_add_highlight(void *editor,
                                 int64_t start_line,
                                 int64_t start_col,
                                 int64_t end_line,
                                 int64_t end_col,
                                 int64_t color) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce)
        return;
    int32_t sl = rt_gui_clamp_i64_to_i32(start_line, 0, INT32_MAX);
    int32_t sc = rt_gui_clamp_i64_to_i32(start_col, 0, INT32_MAX);
    int32_t el = rt_gui_clamp_i64_to_i32(end_line, 0, INT32_MAX);
    int32_t ec = rt_gui_clamp_i64_to_i32(end_col, 0, INT32_MAX);
    if (el < sl || (el == sl && ec <= sc))
        return;
    if (ce->highlight_span_count >= ce->highlight_span_cap) {
        if (ce->highlight_span_cap > INT_MAX / 2)
            return;
        int new_cap = ce->highlight_span_cap ? ce->highlight_span_cap * 2 : 8;
        void *p = realloc(ce->highlight_spans, (size_t)new_cap * sizeof(*ce->highlight_spans));
        if (!p)
            return;
        ce->highlight_spans = p;
        ce->highlight_span_cap = new_cap;
    }
    struct vg_highlight_span *s = &ce->highlight_spans[ce->highlight_span_count++];
    s->start_line = sl;
    s->start_col = sc;
    s->end_line = el;
    s->end_col = ec;
    s->color = (uint32_t)color;
    ce->highlight_spans_sorted = false;
    ce->highlight_line_index_valid = false;
    ce->base.needs_paint = true;
}

/// @brief `CodeEditor.RefreshHighlights()` — schedule a repaint.
///
/// Useful when callers mutate highlight state through other means
/// and need the editor to redraw on the next frame.
/// @param editor CodeEditor widget handle.
void rt_codeeditor_refresh_highlights(void *editor) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce)
        return;
    ce->base.needs_paint = true;
}

/// @brief Default foreground color for a semantic token type (mirrors the
///        lexical palette so semantic and lexical coloring stay consistent).
/// @param token_type Widget-layer semantic token category.
/// @return Packed ARGB default color for the category.
static uint32_t semantic_default_color(int token_type) {
    switch (token_type) {
        case VG_SYN_TOKEN_KEYWORD:
            return SYN_COLOR_KEYWORD;
        case VG_SYN_TOKEN_TYPE:
            return SYN_COLOR_TYPE;
        case VG_SYN_TOKEN_FUNCTION:
            return SYN_COLOR_FUNCTION;
        case VG_SYN_TOKEN_OPERATOR:
            return SYN_COLOR_OPERATOR;
        case VG_SYN_TOKEN_BRACKET:
            return SYN_COLOR_BRACKET;
        case VG_SYN_TOKEN_PARAMETER:
            return SYN_COLOR_PARAMETER;
        case VG_SYN_TOKEN_PROPERTY:
            return SYN_COLOR_PROPERTY;
        case VG_SYN_TOKEN_CONSTANT:
            return SYN_COLOR_CONSTANT;
        case VG_SYN_TOKEN_DECORATOR:
            return SYN_COLOR_DECORATOR;
        default:
            return SYN_COLOR_DEFAULT;
    }
}

/// @brief `CodeEditor.AddSemanticToken(line, start, end, tokenType)` — overlay a
///        compiler-classified color on an identifier (0-based, end exclusive).
///
/// The color honors any per-editor SetTokenColor override, else the lexical
/// default for the type. Applied on top of the lexical highlighter in
/// highlight_line(). Geometric growth (cap doubles from 16); no-ops on OOM.
/// @param editor CodeEditor widget handle.
/// @param line Zero-based logical line.
/// @param start Inclusive character-column start.
/// @param end Exclusive character-column end.
/// @param token_type Semantic token category controlling the overlay color.
void rt_codeeditor_add_semantic_token(
    void *editor, int64_t line, int64_t start, int64_t end, int64_t token_type) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce)
        return;
    int32_t l = rt_gui_clamp_i64_to_i32(line, 0, INT32_MAX);
    int32_t s = rt_gui_clamp_i64_to_i32(start, 0, INT32_MAX);
    int32_t e = rt_gui_clamp_i64_to_i32(end, 0, INT32_MAX);
    if (e <= s)
        return;
    if (ce->semantic_token_count >= ce->semantic_token_cap) {
        if (ce->semantic_token_cap > INT_MAX / 2)
            return;
        int new_cap = ce->semantic_token_cap ? ce->semantic_token_cap * 2 : 16;
        void *p = realloc(ce->semantic_tokens, (size_t)new_cap * sizeof(*ce->semantic_tokens));
        if (!p)
            return;
        ce->semantic_tokens = p;
        ce->semantic_token_cap = new_cap;
    }
    struct vg_semantic_token *st = &ce->semantic_tokens[ce->semantic_token_count++];
    st->line = l;
    st->start_col = s;
    st->end_col = e;
    st->color = syn_color(ce, (int)token_type, semantic_default_color((int)token_type));
    ce->semantic_tokens_sorted = false;
    // Invalidate this line's cached colors so the overlay is reapplied on paint.
    if (l < ce->line_count)
        ce->lines[l].highlight_generation = 0;
    ce->base.needs_paint = true;
}

/// @brief `CodeEditor.ClearSemanticTokens()` — drop the semantic overlay and
///        re-highlight so lexical colors are restored.
/// @param editor CodeEditor widget handle.
void rt_codeeditor_clear_semantic_tokens(void *editor) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce)
        return;
    ce->semantic_token_count = 0;
    ce->semantic_tokens_sorted = true;
    rt_codeeditor_invalidate_syntax_cache(ce);
}

/// @brief `CodeEditor.AddInlayHint(line, col, text, color)` — add ghost annotation text.
/// @param editor CodeEditor widget handle.
/// @param line Zero-based logical line for the hint.
/// @param col Zero-based character column for the hint.
/// @param text Runtime annotation text copied into widget storage.
/// @param color Packed ARGB hint color.
void rt_codeeditor_add_inlay_hint(
    void *editor, int64_t line, int64_t col, rt_string text, int64_t color) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce)
        return;
    char *ctext = rt_string_to_gui_cstr(text);
    if (!ctext)
        return;
    int32_t line_i = rt_gui_clamp_i64_to_i32(line, 0, INT32_MAX);
    int32_t col_i = rt_gui_clamp_i64_to_i32(col, 0, INT32_MAX);
    vg_codeeditor_add_inlay_hint(ce, line_i, col_i, ctext, (uint32_t)color);
    free(ctext);
}

/// @brief `CodeEditor.ClearInlayHints()` — remove every inlay hint.
/// @param editor CodeEditor widget handle.
void rt_codeeditor_clear_inlay_hints(void *editor) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce)
        return;
    vg_codeeditor_clear_inlay_hints(ce);
}

/// @brief `CodeEditor.GetInlayHintCount()` — return active inlay hints.
/// @param editor CodeEditor widget handle.
/// @return Number of active inlay hints, or zero for an invalid handle.
int64_t rt_codeeditor_get_inlay_hint_count(void *editor) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce)
        return 0;
    return vg_codeeditor_get_inlay_hint_count(ce);
}

//=============================================================================

#else /* !ZANNA_ENABLE_GRAPHICS */

/// @brief Stub: `CodeEditor.SetLanguage` is a no-op without graphics.
/// @param editor Ignored CodeEditor handle.
/// @param language Ignored language name.
void rt_codeeditor_set_language(void *editor, rt_string language) {
    (void)editor;
    (void)language;
}

/// @brief Stub: `CodeEditor.SetTokenColor` is a no-op without graphics.
/// @param editor Ignored CodeEditor handle.
/// @param token_type Ignored syntax token category.
/// @param color Ignored packed ARGB color.
void rt_codeeditor_set_token_color(void *editor, int64_t token_type, int64_t color) {
    (void)editor;
    (void)token_type;
    (void)color;
}

/// @brief Stub: `CodeEditor.SetCustomKeywords` is a no-op without graphics.
/// @param editor Ignored CodeEditor handle.
/// @param keywords Ignored comma-separated keyword string.
void rt_codeeditor_set_custom_keywords(void *editor, rt_string keywords) {
    (void)editor;
    (void)keywords;
}

/// @brief Stub: `CodeEditor.ClearHighlights` is a no-op without graphics.
/// @param editor Ignored CodeEditor handle.
void rt_codeeditor_clear_highlights(void *editor) {
    (void)editor;
}

/// @brief Stub: `CodeEditor.AddHighlight` is a no-op without graphics.
/// @param editor Ignored CodeEditor handle.
/// @param start_line Ignored inclusive start line.
/// @param start_col Ignored inclusive start column.
/// @param end_line Ignored exclusive-end line.
/// @param end_col Ignored exclusive end column.
/// @param color Ignored packed ARGB color.
void rt_codeeditor_add_highlight(void *editor,
                                 int64_t start_line,
                                 int64_t start_col,
                                 int64_t end_line,
                                 int64_t end_col,
                                 int64_t color) {
    (void)editor;
    (void)start_line;
    (void)start_col;
    (void)end_line;
    (void)end_col;
    (void)color;
}

/// @brief Stub: `CodeEditor.RefreshHighlights` is a no-op without graphics.
/// @param editor Ignored CodeEditor handle.
void rt_codeeditor_refresh_highlights(void *editor) {
    (void)editor;
}

/// @brief Stub: `CodeEditor.AddSemanticToken` is a no-op without graphics.
/// @param editor Ignored CodeEditor handle.
/// @param line Ignored zero-based logical line.
/// @param start Ignored inclusive start column.
/// @param end Ignored exclusive end column.
/// @param token_type Ignored semantic token category.
void rt_codeeditor_add_semantic_token(
    void *editor, int64_t line, int64_t start, int64_t end, int64_t token_type) {
    (void)editor;
    (void)line;
    (void)start;
    (void)end;
    (void)token_type;
}

/// @brief Stub: `CodeEditor.ClearSemanticTokens` is a no-op without graphics.
/// @param editor Ignored CodeEditor handle.
void rt_codeeditor_clear_semantic_tokens(void *editor) {
    (void)editor;
}

/// @brief Stub: `CodeEditor.AddInlayHint` is a no-op without graphics.
/// @param editor Ignored CodeEditor handle.
/// @param line Ignored zero-based logical line.
/// @param col Ignored zero-based character column.
/// @param text Ignored hint text.
/// @param color Ignored packed ARGB color.
void rt_codeeditor_add_inlay_hint(
    void *editor, int64_t line, int64_t col, rt_string text, int64_t color) {
    (void)editor;
    (void)line;
    (void)col;
    (void)text;
    (void)color;
}

/// @brief Stub: `CodeEditor.ClearInlayHints` is a no-op without graphics.
/// @param editor Ignored CodeEditor handle.
void rt_codeeditor_clear_inlay_hints(void *editor) {
    (void)editor;
}

/// @brief Stub: returns 0 (no inlay hint state without graphics).
/// @param editor Ignored CodeEditor handle.
/// @return Always `0`.
int64_t rt_codeeditor_get_inlay_hint_count(void *editor) {
    (void)editor;
    return 0;
}

#endif /* ZANNA_ENABLE_GRAPHICS */
