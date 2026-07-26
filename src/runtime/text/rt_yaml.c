//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/text/rt_yaml.c
// Purpose: Implements a practical YAML 1.2 subset parser and formatter for the
//          Zanna.Data.Yaml class. Supports scalars (string, int, float, bool,
//          null), block/flow sequences, block/flow mappings, quoted strings,
//          comments (#), and block scalar parsing (| and >).
//
// Key invariants:
//   - YAML types map to: null→NULL, bool→Box.I1, int→Box.I64,
//     float→Box.F64, string→String, sequence→Seq, mapping→Map.
//   - Indentation determines nesting; tabs are not permitted as indentation.
//   - Parse returns NULL on invalid YAML (not a trap).
//   - Anchors (&) and aliases (*) are not supported in this implementation.
//   - All functions are thread-safe with no global mutable state.
//
// Ownership/Lifetime:
//   - Returned rt_map and rt_seq trees are fresh allocations owned by the caller.
//   - Formatted YAML strings are fresh rt_string allocations owned by caller.
//
// Links: src/runtime/text/rt_yaml.h (public API),
//        src/runtime/rt_map.h, rt_seq.h, rt_box.h (container types)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Parser for the runtime's bounded, dependency-free YAML 1.2 subset.
/// @details The implementation maps scalar and collection syntax directly to
///          runtime objects, supports block and flow collections plus simple
///          literal/folded blocks, and reports non-trapping syntax failures
///          through a thread-local diagnostic.

#include "rt_yaml.h"

#include "rt_string_internal.h"

#include "rt_box.h"
#include "rt_internal.h"
#include "rt_map.h"
#include "rt_numeric.h"
#include "rt_object.h"
#include "rt_result.h"
#include "rt_seq.h"
#include "rt_string.h"
#include "rt_yaml_internal.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rt_trap.h"

/// Minimum payload bytes read by Seq/Map public APIs when formatting YAML values.

//=============================================================================
// Type Constants
//=============================================================================

// We use the standard rt_box_type values:
// RT_BOX_I64 = 0 (integers)
// RT_BOX_F64 = 1 (floats)
// RT_BOX_I1 = 2 (booleans)
// RT_BOX_STR = 3 (strings)
// NULL pointer represents YAML null

//=============================================================================
// Parser State
//=============================================================================

/* S-18: Maximum nesting depth before aborting */
#define YAML_MAX_DEPTH 200

/// @brief Mutable byte cursor and nesting state for one YAML parse.
typedef struct {
    const char *input; ///< Borrowed UTF-8 source buffer.
    size_t len;        ///< Total source length in bytes.
    size_t pos;        ///< Current zero-based byte offset.
    int line;          ///< Current one-based source line.
    int col;           ///< Current one-based byte column.
    int depth;         ///< Active collection nesting depth.
} yaml_parser;

/// @brief Last parse error message (thread-local to avoid concurrent parse clobbering).
static _Thread_local char yaml_last_error[256];

/// @brief Record a parse error with line number into the thread-local buffer.
/// Subsequent reads of `yaml_last_error` from the same thread see this message.
/// @param msg Required null-terminated diagnostic text.
/// @param line One-based source line to prefix.
static void set_error(const char *msg, int line) {
    snprintf(yaml_last_error, sizeof(yaml_last_error), "Line %d: %s", line, msg);
}

/// @brief Reset the thread-local error message to empty (start-of-parse cleanup).
static void clear_error(void) {
    yaml_last_error[0] = '\0';
}

//=============================================================================
// Parser Helpers
//=============================================================================

// ---------------------------------------------------------------------------
// Cursor + lexer primitives — pure functions over the parser state.
// All advance through the input one byte at a time, tracking line /
// column for diagnostics. UTF-8 sequences pass through unchanged
// since YAML lexing only cares about ASCII control characters.
// ---------------------------------------------------------------------------

/// @brief Reset a parser onto fresh input at line and column one.
/// @param p Parser state to initialize.
/// @param input Borrowed UTF-8 source buffer.
/// @param len Source length in bytes.
static void parser_init(yaml_parser *p, const char *input, size_t len) {
    p->input = input;
    p->len = len;
    p->pos = 0;
    p->line = 1;
    p->col = 1;
    p->depth = 0;
}

/// @brief Test whether the cursor has consumed every input byte.
/// @param p Parser state to inspect.
/// @return `true` at or beyond the end of the source buffer.
static bool parser_eof(yaml_parser *p) {
    return p->pos >= p->len;
}

/// @brief Peek at the byte under the cursor; returns `\0` at EOF.
/// @param p Parser state to inspect.
/// @return Current source byte, or the null sentinel at end-of-input.
static char parser_peek(yaml_parser *p) {
    if (p->pos >= p->len)
        return '\0';
    return p->input[p->pos];
}

/// @brief Peek at `offset` bytes past the cursor; returns `\0` past EOF.
/// @param p Parser state to inspect.
/// @param offset Byte displacement from the current cursor.
/// @return Requested source byte, or the null sentinel past end-of-input.
static char parser_peek_at(yaml_parser *p, size_t offset) {
    if (p->pos + offset >= p->len)
        return '\0';
    return p->input[p->pos + offset];
}

/// @brief Consume and return the byte under the cursor; updates line/col.
/// @param p Parser state to advance.
/// @return Consumed byte, or the null sentinel at end-of-input.
static char parser_advance(yaml_parser *p) {
    if (p->pos >= p->len)
        return '\0';
    char c = p->input[p->pos++];
    if (c == '\n') {
        p->line++;
        p->col = 1;
    } else {
        p->col++;
    }
    return c;
}

/// @brief Skip horizontal whitespace (space, tab) on the current line.
/// @param p Parser state to advance.
static void parser_skip_spaces(yaml_parser *p) {
    while (!parser_eof(p) && (parser_peek(p) == ' ' || parser_peek(p) == '\t'))
        parser_advance(p);
}

/// @brief Advance to (but not past) the next newline.
/// @param p Parser state to advance.
static void parser_skip_to_eol(yaml_parser *p) {
    while (!parser_eof(p) && parser_peek(p) != '\n')
        parser_advance(p);
}

/// @brief Skip a `#` line comment if one starts at the current position.
/// @param p Parser state to conditionally advance.
static void parser_skip_comment(yaml_parser *p) {
    if (parser_peek(p) == '#')
        parser_skip_to_eol(p);
}

/// @brief Advance over runs of empty lines and comment-only lines.
///
/// Blank lines (just whitespace + newline) and lines whose first
/// non-space character is `#` are equally inert in YAML. This loop
/// chews through them so the caller can land on real content.
/// @param p Parser state positioned at the beginning of an ignorable run.
static void parser_skip_blank_lines_and_comments(yaml_parser *p) {
    while (!parser_eof(p)) {
        size_t pos = p->pos;
        while (pos < p->len && p->input[pos] == ' ')
            pos++;

        if (pos < p->len && p->input[pos] == '#') {
            while (!parser_eof(p) && parser_peek(p) != '\n')
                parser_advance(p);
            if (parser_peek(p) == '\n')
                parser_advance(p);
            continue;
        }

        if (parser_peek(p) == '\n') {
            parser_advance(p);
            continue;
        }

        break;
    }
}

/// @brief Count leading spaces on the current line and return the indent depth.
///        Returns -1 and records an error if a tab is found in the indent region
///        (YAML 1.2 forbids tabs as indentation).
/// @param p Parser state positioned at the start of a line.
/// @return Space count without consuming it, or -1 for tab indentation.
static int get_indent_checked(yaml_parser *p) {
    int indent = 0;
    size_t pos = p->pos;
    while (pos < p->len) {
        if (p->input[pos] == ' ') {
            indent++;
            pos++;
            continue;
        }
        if (p->input[pos] == '\t') {
            set_error("tabs are not allowed in indentation", p->line);
            return -1;
        }
        break;
    }
    return indent;
}

/// @brief Consume trailing spaces, an optional `#` comment, and the newline after an inline value.
/// @param p Parser state positioned after an inline value.
static void parser_finish_value_line(yaml_parser *p) {
    parser_skip_spaces(p);
    parser_skip_comment(p);
    if (parser_peek(p) == '\n')
        parser_advance(p);
}

/// @brief True if (after leading spaces) the current line is exactly `marker`.
///
/// Used to detect document boundaries (`---`, `...`) without silently
/// discarding inline content on lines like `--- value`.
/// @param p Parser state whose current line is inspected without modification.
/// @param marker Required null-terminated marker text.
/// @return `true` when the marker occupies the line apart from whitespace and
///         an optional comment.
static bool parser_at_line_marker(yaml_parser *p, const char *marker) {
    size_t pos = p->pos;
    size_t marker_len = strlen(marker);

    while (pos < p->len && p->input[pos] == ' ')
        pos++;
    if (pos + marker_len > p->len)
        return false;
    if (strncmp(p->input + pos, marker, marker_len) != 0)
        return false;

    pos += marker_len;
    while (pos < p->len && (p->input[pos] == ' ' || p->input[pos] == '\t'))
        pos++;
    char next = (pos < p->len) ? p->input[pos] : '\0';
    return next == '\0' || next == '\n' || next == '#';
}

/// @brief If the current line starts with `marker`, consume it (and the rest of the line).
/// @param p Parser state to inspect and conditionally advance.
/// @param marker Required null-terminated marker text.
/// @return True if a marker was consumed, false if the line didn't match.
static bool parser_consume_line_marker(yaml_parser *p, const char *marker) {
    if (!parser_at_line_marker(p, marker))
        return false;

    while (parser_peek(p) == ' ')
        parser_advance(p);
    for (const char *m = marker; *m; ++m)
        parser_advance(p);
    parser_skip_spaces(p);
    parser_skip_comment(p);
    if (!parser_eof(p) && parser_peek(p) != '\n') {
        set_error("unexpected content after document marker", p->line);
        return false;
    }
    if (parser_peek(p) == '\n')
        parser_advance(p);
    return true;
}

//=============================================================================
// Scalar Parsing
//=============================================================================

/// @brief Case-insensitive whole-string compare against a YAML keyword.
///
/// Used to match the YAML 1.2 spellings we accept for null/true/false
/// and special float values.
/// @param str Candidate byte span.
/// @param len Candidate length in bytes.
/// @param value Required null-terminated keyword.
/// @return `true` when the complete spans match ignoring ASCII case.
static bool is_special_value(const char *str, size_t len, const char *value) {
    size_t vlen = strlen(value);
    if (len != vlen)
        return false;
    for (size_t i = 0; i < len; i++) {
        if (tolower((unsigned char)str[i]) != tolower((unsigned char)value[i]))
            return false;
    }
    return true;
}

/// @brief Test whether a byte span has a finite decimal-float token shape.
/// @details Accepts a decimal point or exponent with optional signs, but does
///          not accept C-only `inf` or `nan` spellings.
/// @param str Candidate byte span.
/// @param len Candidate length in bytes.
/// @return `true` when the entire span matches the supported shape.
static bool looks_like_decimal_float(const char *str, size_t len) {
    size_t i = 0;
    if (i < len && (str[i] == '+' || str[i] == '-'))
        i++;

    bool digits_before = false;
    while (i < len && isdigit((unsigned char)str[i])) {
        digits_before = true;
        i++;
    }

    bool has_dot = false;
    bool digits_after = false;
    if (i < len && str[i] == '.') {
        has_dot = true;
        i++;
        while (i < len && isdigit((unsigned char)str[i])) {
            digits_after = true;
            i++;
        }
    }

    bool has_exp = false;
    if (i < len && (str[i] == 'e' || str[i] == 'E')) {
        has_exp = true;
        i++;
        if (i < len && (str[i] == '+' || str[i] == '-'))
            i++;
        bool exp_digits = false;
        while (i < len && isdigit((unsigned char)str[i])) {
            exp_digits = true;
            i++;
        }
        if (!exp_digits)
            return false;
    }

    return i == len && (has_dot || has_exp) && (digits_before || digits_after);
}

/// @brief Parse a raw scalar token into its strongest matching Zanna type.
///
/// Tries, in order: null (special tokens / empty) → bool → integer
/// (decimal, 0x hex, 0o octal) → float → string. The first
/// successful conversion wins. This is YAML's "core schema" (1.2)
/// approximation — quoted strings are handled separately by
/// `parse_quoted_string` and never reach here.
/// @param str Plain-scalar byte span.
/// @param len Scalar length in bytes.
/// @return Boxed primitive (Int/Bool/Float) or `rt_string`, or NULL for YAML null.
void *parse_scalar(const char *str, size_t len) {
    if (len == 0)
        return NULL; // YAML null

    // An embedded NUL can never be part of a numeric/boolean/null token, and
    // the C-string converters below would silently stop at it (VDOC-039):
    // treat the whole span as an ordinary string scalar instead.
    if (memchr(str, '\0', len) != NULL)
        return rt_string_from_bytes(str, (int64_t)len);

    // Null
    if (is_special_value(str, len, "null") || is_special_value(str, len, "~") ||
        (len == 4 && strncmp(str, "Null", 4) == 0) || (len == 4 && strncmp(str, "NULL", 4) == 0)) {
        return NULL;
    }

    // Boolean (YAML 1.2 core schema)
    if (is_special_value(str, len, "true")) {
        return rt_box_i1(1);
    }
    if (is_special_value(str, len, "false")) {
        return rt_box_i1(0);
    }

    // Try integer
    char *end;
    char *buf = malloc(len + 1);
    if (!buf)
        return rt_string_from_bytes(str, (int64_t)len);
    memcpy(buf, str, len);
    buf[len] = '\0';

    // Check for hex
    if (len > 2 && buf[0] == '0' && (buf[1] == 'x' || buf[1] == 'X')) {
        errno = 0;
        long long val = strtoll(buf, &end, 16);
        if (*end == '\0' && errno != ERANGE) {
            free(buf);
            return rt_box_i64(val);
        }
        if (*end == '\0') {
            free(buf);
            return rt_string_from_bytes(str, (int64_t)len);
        }
    }
    // Check for octal
    else if (len > 2 && buf[0] == '0' && (buf[1] == 'o' || buf[1] == 'O')) {
        errno = 0;
        long long val = strtoll(buf + 2, &end, 8);
        if (*end == '\0' && errno != ERANGE) {
            free(buf);
            return rt_box_i64(val);
        }
        if (*end == '\0') {
            free(buf);
            return rt_string_from_bytes(str, (int64_t)len);
        }
    } else {
        // Decimal integer
        errno = 0;
        long long val = strtoll(buf, &end, 10);
        if (*end == '\0' && errno != ERANGE) {
            free(buf);
            return rt_box_i64(val);
        }
        if (*end == '\0') {
            free(buf);
            return rt_string_from_bytes(str, (int64_t)len);
        }

        // Try numeric floats. Gate the C-locale parser so C-only tokens like `inf`/`nan`
        // are not accepted as YAML scalars; YAML special floats are handled below.
        if (looks_like_decimal_float(buf, len)) {
            double fval = 0.0;
            if (rt_parse_double(buf, &fval) == (int32_t)Err_None && isfinite(fval)) {
                free(buf);
                return rt_box_f64(fval);
            }
        }

        // Check for special floats
        if (is_special_value(str, len, ".inf") || is_special_value(str, len, ".Inf") ||
            is_special_value(str, len, ".INF")) {
            free(buf);
            return rt_box_f64(INFINITY);
        }
        if (is_special_value(str, len, "-.inf") || is_special_value(str, len, "-.Inf") ||
            is_special_value(str, len, "-.INF")) {
            free(buf);
            return rt_box_f64(-INFINITY);
        }
        if (is_special_value(str, len, ".nan") || is_special_value(str, len, ".NaN") ||
            is_special_value(str, len, ".NAN")) {
            free(buf);
            return rt_box_f64(NAN);
        }
    }

    free(buf);

    // Default to string
    return rt_string_from_bytes(str, (int64_t)len);
}

/// @brief Convert one ASCII hexadecimal digit to its numeric value.
/// @param c Candidate hexadecimal digit.
/// @return Value from 0 through 15, or -1 for a non-hexadecimal byte.
static int hex_value(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/// @brief Encode Unicode codepoint `cp` as UTF-8 and append it to `*buf`, growing as needed.
///        Returns false and leaves the buffer unchanged if `cp` is a surrogate or out of range.
/// @param cp Unicode scalar value to encode.
/// @param buf Address of the heap-buffer pointer.
/// @param len Address of the populated byte count.
/// @param capacity Address of the allocation capacity.
/// @return `true` after appending one to four bytes, otherwise `false`.
static bool append_utf8(uint32_t cp, char **buf, size_t *len, size_t *capacity) {
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
        return false;

    char bytes[4];
    size_t n = 0;
    if (cp < 0x80) {
        bytes[n++] = (char)cp;
    } else if (cp < 0x800) {
        bytes[n++] = (char)(0xC0 | (cp >> 6));
        bytes[n++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        bytes[n++] = (char)(0xE0 | (cp >> 12));
        bytes[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        bytes[n++] = (char)(0x80 | (cp & 0x3F));
    } else {
        bytes[n++] = (char)(0xF0 | (cp >> 18));
        bytes[n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        bytes[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        bytes[n++] = (char)(0x80 | (cp & 0x3F));
    }

    if (n > SIZE_MAX - *len - 1) {
        rt_trap("rt_yaml: output length overflow");
        return false;
    }
    size_t required = *len + n + 1u;
    while (required > *capacity) {
        size_t next_capacity = *capacity ? (*capacity * 2u) : 256u;
        if (next_capacity <= *capacity || next_capacity < required)
            next_capacity = required;
        char *tmp = realloc(*buf, next_capacity);
        if (!tmp) {
            rt_trap("rt_yaml: memory allocation failed");
            return false;
        }
        *buf = tmp;
        *capacity = next_capacity;
    }
    memcpy(*buf + *len, bytes, n);
    *len += n;
    return true;
}

/// @brief Parse a single- or double-quoted YAML string with escape processing.
///
/// `quote` is `'` (no escapes except `''` for a literal quote) or
/// `"` (full set of `\\`, `\n`, `\t`, `\u00XX`, etc. per YAML
/// §5.7). Cursor is positioned on the opening quote and advances
/// past the matching close. Sets the parser error and returns
/// NULL on any malformed escape or unterminated string.
/// @param p Parser positioned at the opening quote.
/// @param quote Opening delimiter, either single or double quote.
/// @return Newly allocated decoded runtime string, or null on failure.
static rt_string parse_quoted_string(yaml_parser *p, char quote) {
    parser_advance(p); // Skip opening quote

    size_t capacity = 64;
    char *buf = malloc(capacity);
    if (!buf) {
        rt_trap("rt_yaml: memory allocation failed");
        return NULL;
    }
    size_t len = 0;

    while (!parser_eof(p)) {
        char c = parser_peek(p);
        if (c == quote) {
            if (quote == '\'' && parser_peek_at(p, 1) == '\'') {
                parser_advance(p);
                parser_advance(p);
                c = '\'';
            } else {
                break;
            }
        } else if (c == '\\' && quote == '"') {
            parser_advance(p);
            if (parser_eof(p)) {
                free(buf);
                set_error("unterminated escape sequence", p->line);
                return NULL;
            }
            c = parser_advance(p);
            uint32_t cp = 0;
            int digits = 0;
            switch (c) {
                case '0':
                    c = '\0';
                    break;
                case 'a':
                    c = '\a';
                    break;
                case 'b':
                    c = '\b';
                    break;
                case 't':
                case '\t':
                    c = '\t';
                    break;
                case 'n':
                    c = '\n';
                    break;
                case 'v':
                    c = '\v';
                    break;
                case 'f':
                    c = '\f';
                    break;
                case 'r':
                    c = '\r';
                    break;
                case 'e':
                    c = '\x1B';
                    break;
                case ' ':
                    c = ' ';
                    break;
                case '"':
                    c = '"';
                    break;
                case '/':
                    c = '/';
                    break;
                case '\\':
                    c = '\\';
                    break;
                case 'N':
                    cp = 0x85;
                    digits = -1;
                    break;
                case '_':
                    cp = 0xA0;
                    digits = -1;
                    break;
                case 'L':
                    cp = 0x2028;
                    digits = -1;
                    break;
                case 'P':
                    cp = 0x2029;
                    digits = -1;
                    break;
                case 'x':
                    digits = 2;
                    break;
                case 'u':
                    digits = 4;
                    break;
                case 'U':
                    digits = 8;
                    break;
                default:
                    free(buf);
                    set_error("invalid escape sequence", p->line);
                    return NULL;
            }

            if (digits > 0) {
                cp = 0;
                for (int i = 0; i < digits; i++) {
                    if (parser_eof(p)) {
                        free(buf);
                        set_error("unterminated unicode escape", p->line);
                        return NULL;
                    }
                    int hv = hex_value(parser_advance(p));
                    if (hv < 0) {
                        free(buf);
                        set_error("invalid unicode escape", p->line);
                        return NULL;
                    }
                    cp = (cp << 4) | (uint32_t)hv;
                }
                if (!append_utf8(cp, &buf, &len, &capacity)) {
                    free(buf);
                    set_error("invalid unicode codepoint", p->line);
                    return NULL;
                }
                continue;
            }
            if (digits == -1) {
                if (!append_utf8(cp, &buf, &len, &capacity)) {
                    free(buf);
                    set_error("invalid unicode codepoint", p->line);
                    return NULL;
                }
                continue;
            }
        } else {
            parser_advance(p);
        }

        if (len >= capacity - 1) {
            capacity *= 2;
            char *tmp = (char *)realloc(buf, capacity);
            if (!tmp) {
                rt_trap("rt_yaml: memory allocation failed");
                free(buf);
                return NULL;
            }
            buf = tmp;
        }
        buf[len++] = c;
    }

    if (parser_peek(p) != quote) {
        free(buf);
        set_error("unterminated quoted string", p->line);
        return NULL;
    }
    parser_advance(p); // Skip closing quote

    buf[len] = '\0';
    rt_string result = rt_string_from_bytes(buf, (int64_t)len);
    free(buf);
    return result;
}

//=============================================================================
// Value Parsing
//=============================================================================

/// @brief Parse any supported block, flow, quoted, or plain YAML value.
/// @param p Parser positioned at a value or preceding blank lines.
/// @param base_indent Minimum indentation allowed for the value.
/// @return Owned runtime value, or null for YAML null, absence, or failure.
static void *parse_value(yaml_parser *p, int base_indent);

/// @brief Reject an unsupported anchor or alias at the current token start.
/// @param p Parser state to inspect.
/// @return 1 after recording an unsupported-feature error, otherwise 0.
static int reject_anchor_alias(yaml_parser *p);

/// @brief Parse a block-style sequence (`- item` lines at `base_indent`).
///
/// Each iteration consumes one `- ` lead-in, recurses into
/// `parse_value` for the item, and stops at EOF or when the next content line
/// is less indented than `base_indent`. A same-level line without an item
/// marker is a syntax error.
/// YAML null items are represented by null entries. Collection recursion is
/// bounded by @c YAML_MAX_DEPTH.
/// @param p Parser positioned at the sequence's first line.
/// @param base_indent Required indentation of each item marker.
/// @return Owned element-owning sequence, or null on syntax or allocation failure.
static void *parse_block_sequence(yaml_parser *p, int base_indent) {
    /* S-18: Guard against deeply nested documents */
    if (p->depth >= YAML_MAX_DEPTH) {
        set_error("sequence nesting depth limit exceeded", p->line);
        return NULL;
    }
    p->depth++;

    void *seq = rt_seq_new();
    if (!seq) {
        p->depth--;
        return NULL;
    }
    rt_seq_set_owns_elements(seq, 1);

    while (!parser_eof(p)) {
        parser_skip_blank_lines_and_comments(p);
        if (parser_eof(p))
            break;

        int indent = get_indent_checked(p);
        if (indent < 0) {
            yaml_release(seq);
            p->depth--;
            return NULL;
        }
        if (indent < base_indent)
            break;
        if (indent > base_indent) {
            set_error("unexpected indentation in sequence", p->line);
            yaml_release(seq);
            p->depth--;
            return NULL;
        }

        // Skip to the '-'
        while (parser_peek(p) == ' ')
            parser_advance(p);

        if (parser_peek(p) != '-') {
            set_error("expected sequence item", p->line);
            yaml_release(seq);
            p->depth--;
            return NULL;
        }

        parser_advance(p); // Skip '-'

        // Check for nested content
        if (parser_peek(p) == ' ' || parser_peek(p) == '\n') {
            void *item = NULL;
            if (parser_peek(p) == ' ') {
                parser_advance(p); // Skip space after '-'
                item = parse_value(p, 0);
            } else {
                parser_advance(p); // Empty item, or nested value on following indented lines.
                parser_skip_blank_lines_and_comments(p);
                if (!parser_eof(p)) {
                    int next_indent = get_indent_checked(p);
                    if (next_indent < 0) {
                        yaml_release(seq);
                        p->depth--;
                        return NULL;
                    }
                    if (next_indent > indent)
                        item = parse_value(p, next_indent);
                }
            }
            if (!item && yaml_last_error[0] != '\0') {
                yaml_release(seq);
                p->depth--;
                return NULL;
            }
            rt_seq_push(seq, item);
            yaml_release(item);
        } else {
            set_error("expected sequence value", p->line);
            yaml_release(seq);
            p->depth--;
            return NULL;
        }
    }

    p->depth--;
    return seq;
}

/// @brief Parse a block-style mapping (`key: value` lines at `base_indent`).
///
/// Reads `key:` (the key may itself be quoted or a plain scalar),
/// optionally consumes a same-line scalar value, then either uses
/// it directly or recurses into `parse_value` for a nested
/// block. Stops at the first line below `base_indent`.
/// Keys are runtime strings, duplicate keys are rejected, and missing values
/// represent YAML null. Collection recursion is bounded by
/// @c YAML_MAX_DEPTH.
/// @param p Parser positioned at the mapping's first line.
/// @param base_indent Required indentation of each key.
/// @return Owned mapping, or null on syntax or allocation failure.
static void *parse_block_mapping(yaml_parser *p, int base_indent) {
    /* S-18: Guard against deeply nested documents */
    if (p->depth >= YAML_MAX_DEPTH) {
        set_error("mapping nesting depth limit exceeded", p->line);
        return NULL;
    }
    p->depth++;

    void *map = rt_map_new();
    if (!map) {
        p->depth--;
        return NULL;
    }

    while (!parser_eof(p)) {
        parser_skip_blank_lines_and_comments(p);
        if (parser_eof(p))
            break;

        int indent = get_indent_checked(p);
        if (indent < 0) {
            yaml_release(map);
            p->depth--;
            return NULL;
        }
        if (indent < base_indent)
            break;
        if (indent > base_indent) {
            set_error("unexpected indentation in mapping", p->line);
            yaml_release(map);
            p->depth--;
            return NULL;
        }

        // Skip indentation
        while (parser_peek(p) == ' ')
            parser_advance(p);

        // Check for sequence item
        if (parser_peek(p) == '-')
            break;

        // Parse key
        rt_string key = NULL;
        if (parser_peek(p) == '"' || parser_peek(p) == '\'') {
            key = parse_quoted_string(p, parser_peek(p));
            if (!key) {
                yaml_release(map);
                p->depth--;
                return NULL;
            }
            parser_skip_spaces(p);
        } else {
            if (reject_anchor_alias(p)) {
                yaml_release(map);
                p->depth--;
                return NULL;
            }
            size_t key_start = p->pos;
            while (!parser_eof(p) && parser_peek(p) != ':' && parser_peek(p) != '\n')
                parser_advance(p);

            size_t key_len = p->pos - key_start;
            // Trim trailing spaces from key
            while (key_len > 0 && p->input[key_start + key_len - 1] == ' ')
                key_len--;

            key = rt_string_from_bytes(p->input + key_start, (int64_t)key_len);
        }

        if (parser_peek(p) != ':') {
            set_error("expected ':' after mapping key", p->line);
            yaml_release((void *)key);
            yaml_release(map);
            p->depth--;
            return NULL;
        }

        parser_advance(p); // Skip ':'
        parser_skip_spaces(p);

        void *value;
        if (parser_peek(p) == '\n' || parser_peek(p) == '#') {
            // Value on next line(s)
            parser_skip_comment(p);
            if (parser_peek(p) == '\n')
                parser_advance(p);

            value = NULL;
            parser_skip_blank_lines_and_comments(p);
            if (!parser_eof(p)) {
                int next_indent = get_indent_checked(p);
                if (next_indent < 0) {
                    yaml_release((void *)key);
                    yaml_release(map);
                    p->depth--;
                    return NULL;
                }
                if (next_indent > indent)
                    value = parse_value(p, next_indent);
            }
        } else {
            // Inline value
            value = parse_value(p, 0);
        }

        if (!value && yaml_last_error[0] != '\0') {
            yaml_release((void *)key);
            yaml_release(map);
            p->depth--;
            return NULL;
        }
        if (rt_map_has(map, key)) {
            set_error("duplicate mapping key", p->line);
            yaml_release(value);
            yaml_release((void *)key);
            yaml_release(map);
            p->depth--;
            return NULL;
        }
        rt_map_set(map, key, value);
        yaml_release(value);

        yaml_release((void *)key);
    }

    p->depth--;
    return map;
}

/// @brief Parse one scalar or nested collection inside flow syntax.
/// @param p Parser positioned before a flow value.
/// @return Owned runtime value, or null for YAML null or failure.
static void *parse_flow_value(yaml_parser *p);

/// @brief Skip spaces, tabs, CR, and LF within a flow collection context.
/// @param p Parser state to advance.
static void skip_flow_ws(yaml_parser *p) {
    while (!parser_eof(p) && (parser_peek(p) == ' ' || parser_peek(p) == '\t' ||
                              parser_peek(p) == '\n' || parser_peek(p) == '\r'))
        parser_advance(p);
}

/// @brief Parse a flow-mapping key: either a quoted string or a plain scalar up to `:` or `}`.
///        Sets an error and returns NULL if no key material is found.
/// @param p Parser positioned before a flow-mapping key.
/// @return Newly allocated string key, or null on failure.
static rt_string parse_flow_key(yaml_parser *p) {
    skip_flow_ws(p);
    char c = parser_peek(p);
    if (c == '"' || c == '\'')
        return parse_quoted_string(p, c);

    if (reject_anchor_alias(p))
        return NULL;
    size_t start = p->pos;
    while (!parser_eof(p) && parser_peek(p) != ':' && parser_peek(p) != '}' &&
           parser_peek(p) != '\n')
        parser_advance(p);
    size_t len = p->pos - start;
    while (len > 0 && isspace((unsigned char)p->input[start + len - 1]))
        len--;
    if (len == 0) {
        set_error("expected flow mapping key", p->line);
        return NULL;
    }
    return rt_string_from_bytes(p->input + start, (int64_t)len);
}

/// @brief Parse a flow sequence `[ item, item, ... ]`, returning an owned `Seq[Any]`.
///        Rejects trailing commas and unterminated sequences.
/// @param p Parser positioned at the opening `[`.
/// @return Owned element-owning sequence, or null on failure.
static void *parse_flow_sequence(yaml_parser *p) {
    if (p->depth >= YAML_MAX_DEPTH) {
        set_error("sequence nesting depth limit exceeded", p->line);
        return NULL;
    }
    p->depth++;
    parser_advance(p); // [
    void *seq = rt_seq_new();
    rt_seq_set_owns_elements(seq, 1);

    skip_flow_ws(p);
    if (parser_peek(p) == ']') {
        parser_advance(p);
        p->depth--;
        return seq;
    }

    while (!parser_eof(p)) {
        if (parser_peek(p) == ',' || parser_peek(p) == ']' || parser_peek(p) == '}') {
            set_error("expected flow sequence value", p->line);
            yaml_release(seq);
            p->depth--;
            return NULL;
        }
        void *item = parse_flow_value(p);
        if (!item && yaml_last_error[0] != '\0') {
            yaml_release(seq);
            p->depth--;
            return NULL;
        }
        rt_seq_push(seq, item);
        yaml_release(item);

        skip_flow_ws(p);
        if (parser_peek(p) == ',') {
            parser_advance(p);
            skip_flow_ws(p);
            if (parser_peek(p) == ']') {
                set_error("trailing comma in flow sequence", p->line);
                yaml_release(seq);
                p->depth--;
                return NULL;
            }
            continue;
        }
        if (parser_peek(p) == ']') {
            parser_advance(p);
            p->depth--;
            return seq;
        }
        set_error("expected ',' or ']' in flow sequence", p->line);
        yaml_release(seq);
        p->depth--;
        return NULL;
    }

    set_error("unterminated flow sequence", p->line);
    yaml_release(seq);
    p->depth--;
    return NULL;
}

/// @brief Parse a flow mapping `{ key: value, ... }`, returning an owned `Map[Any,Any]`.
///        Rejects trailing commas, duplicate keys, and unterminated mappings.
/// @param p Parser positioned at the opening `{`.
/// @return Owned string-keyed mapping, or null on failure.
static void *parse_flow_mapping(yaml_parser *p) {
    if (p->depth >= YAML_MAX_DEPTH) {
        set_error("mapping nesting depth limit exceeded", p->line);
        return NULL;
    }
    p->depth++;
    parser_advance(p); // {
    void *map = rt_map_new();

    skip_flow_ws(p);
    if (parser_peek(p) == '}') {
        parser_advance(p);
        p->depth--;
        return map;
    }

    while (!parser_eof(p)) {
        rt_string key = parse_flow_key(p);
        if (!key) {
            yaml_release(map);
            p->depth--;
            return NULL;
        }

        skip_flow_ws(p);
        if (parser_peek(p) != ':') {
            set_error("expected ':' in flow mapping", p->line);
            yaml_release((void *)key);
            yaml_release(map);
            p->depth--;
            return NULL;
        }
        parser_advance(p);

        void *value = parse_flow_value(p);
        if (!value && yaml_last_error[0] != '\0') {
            yaml_release((void *)key);
            yaml_release(map);
            p->depth--;
            return NULL;
        }
        if (rt_map_has(map, key)) {
            set_error("duplicate mapping key", p->line);
            yaml_release(value);
            yaml_release((void *)key);
            yaml_release(map);
            p->depth--;
            return NULL;
        }
        rt_map_set(map, key, value);
        yaml_release(value);
        yaml_release((void *)key);

        skip_flow_ws(p);
        if (parser_peek(p) == ',') {
            parser_advance(p);
            skip_flow_ws(p);
            if (parser_peek(p) == '}') {
                set_error("trailing comma in flow mapping", p->line);
                yaml_release(map);
                p->depth--;
                return NULL;
            }
            continue;
        }
        if (parser_peek(p) == '}') {
            parser_advance(p);
            p->depth--;
            return map;
        }
        set_error("expected ',' or '}' in flow mapping", p->line);
        yaml_release(map);
        p->depth--;
        return NULL;
    }

    set_error("unterminated flow mapping", p->line);
    yaml_release(map);
    p->depth--;
    return NULL;
}

/// @brief Reject unsupported YAML anchor/alias indicators at a plain-token start.
/// @details Anchors (&name) and aliases (*name) are not implemented; accepting
///          them silently produces a materially different data model, so they
///          invalidate the document with an explicit diagnostic instead.
/// @param p Parser positioned at the beginning of a plain token.
/// @return 1 when the current position starts an anchor/alias (error set), else 0.
static int reject_anchor_alias(yaml_parser *p) {
    char c = parser_peek(p);
    if (c == '&' || c == '*') {
        set_error("YAML anchors and aliases are not supported", p->line);
        return 1;
    }
    return 0;
}

/// @brief Parse a single value inside a flow collection context (scalar, quoted string,
///        or nested `[`/`{`). Stops before `,`, `]`, or `}` without consuming them.
/// @param p Parser positioned before a flow value.
/// @return Owned runtime value, null for the YAML null scalar, or null on failure.
static void *parse_flow_value(yaml_parser *p) {
    skip_flow_ws(p);
    char c = parser_peek(p);
    if (c == '[')
        return parse_flow_sequence(p);
    if (c == '{')
        return parse_flow_mapping(p);
    if (c == '"' || c == '\'')
        return (void *)parse_quoted_string(p, c);
    if (reject_anchor_alias(p))
        return NULL;

    size_t start = p->pos;
    while (!parser_eof(p) && parser_peek(p) != ',' && parser_peek(p) != ']' &&
           parser_peek(p) != '}')
        parser_advance(p);
    size_t len = p->pos - start;
    while (len > 0 && isspace((unsigned char)p->input[start + len - 1]))
        len--;
    if (len == 0) {
        set_error("expected flow value", p->line);
        return NULL;
    }
    return parse_scalar(p->input + start, len);
}

/// @brief Top-level value dispatch — picks the right block/flow/scalar parser.
///
/// Looks at the first non-space character on the current line:
///   - `-` followed by space → block sequence
///   - `[` → flow sequence
///   - `{` → flow mapping
///   - quote (`'`, `"`) → quoted string scalar
///   - anything else → block mapping (if a colon follows the first
///     token) or plain scalar (if the line ends after one token).
/// Literal (`|`) and folded (`>`) block scalars are also supported with a
/// simplified trim policy. Recurses into the appropriate sub-parser at the
/// detected indentation.
/// @param p Parser positioned at a value or preceding blank lines.
/// @param base_indent Minimum indentation allowed for this value.
/// @return Owned runtime value, or null for YAML null, absence, or failure.
static void *parse_value(yaml_parser *p, int base_indent) {
    parser_skip_blank_lines_and_comments(p);

    if (parser_eof(p))
        return NULL; // YAML null

    int indent = get_indent_checked(p);
    if (indent < 0)
        return NULL;
    if (indent < base_indent)
        return NULL;

    size_t content_pos = p->pos;
    while (content_pos < p->len && p->input[content_pos] == ' ')
        content_pos++;
    char c = (content_pos < p->len) ? p->input[content_pos] : '\0';

    // Sequence
    if (c == '-' && (content_pos + 1 >= p->len || p->input[content_pos + 1] == ' ' ||
                     p->input[content_pos + 1] == '\n')) {
        return parse_block_sequence(p, indent);
    }

    if (c == '[' || c == '{') {
        while (parser_peek(p) == ' ')
            parser_advance(p);
        void *flow = (c == '[') ? parse_flow_sequence(p) : parse_flow_mapping(p);
        parser_finish_value_line(p);
        return flow;
    }

    // Check if it's a mapping (look for ':')
    {
        size_t scan = content_pos;
        char quote = '\0';
        while (scan < p->len && p->input[scan] != '\n' && p->input[scan] != '#') {
            char sc = p->input[scan];
            if (quote) {
                if (sc == '\\' && quote == '"' && scan + 1 < p->len) {
                    scan += 2;
                    continue;
                }
                if (sc == quote)
                    quote = '\0';
                scan++;
                continue;
            }
            if (sc == '"' || sc == '\'') {
                quote = sc;
                scan++;
                continue;
            }
            if (sc == ':' &&
                (scan + 1 >= p->len || p->input[scan + 1] == ' ' || p->input[scan + 1] == '\n')) {
                return parse_block_mapping(p, indent);
            }
            scan++;
        }
    }

    // Skip indentation now that block constructs were handled.
    while (parser_peek(p) == ' ')
        parser_advance(p);

    c = parser_peek(p);

    // Quoted string
    if (c == '"' || c == '\'') {
        rt_string quoted = parse_quoted_string(p, c);
        parser_finish_value_line(p);
        return (void *)quoted;
    }

    if (c == '[') {
        void *seq = parse_flow_sequence(p);
        parser_finish_value_line(p);
        return seq;
    }

    if (c == '{') {
        void *map = parse_flow_mapping(p);
        parser_finish_value_line(p);
        return map;
    }

    // Literal block scalar
    if (c == '|' || c == '>') {
        bool folded = (c == '>');
        parser_advance(p);
        parser_skip_to_eol(p);
        if (parser_peek(p) == '\n')
            parser_advance(p);

        // Get block indent
        int block_indent = get_indent_checked(p);
        if (block_indent < 0)
            return NULL;
        if (block_indent <= indent)
            return (void *)rt_str_empty();

        size_t capacity = 256;
        char *buf = malloc(capacity);
        if (!buf) {
            rt_trap("rt_yaml: memory allocation failed");
            return NULL;
        }
        size_t len = 0;

        while (!parser_eof(p)) {
            int line_indent = get_indent_checked(p);
            if (line_indent < 0) {
                free(buf);
                return NULL;
            }
            if (line_indent < block_indent && p->input[p->pos + line_indent] != '\n')
                break;

            // Skip block indent
            for (int i = 0; i < block_indent && parser_peek(p) == ' '; i++)
                parser_advance(p);

            // Read line
            while (!parser_eof(p) && parser_peek(p) != '\n') {
                if (len >= capacity - 2) {
                    capacity *= 2;
                    char *tmp = (char *)realloc(buf, capacity);
                    if (!tmp) {
                        rt_trap("rt_yaml: memory allocation failed");
                        free(buf);
                        return NULL;
                    }
                    buf = tmp;
                }
                buf[len++] = parser_advance(p);
            }

            if (parser_peek(p) == '\n') {
                parser_advance(p);
                if (folded)
                    buf[len++] = ' ';
                else
                    buf[len++] = '\n';
            }
        }

        // Trim trailing newline/space
        while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == ' '))
            len--;

        buf[len] = '\0';
        rt_string result = rt_string_from_bytes(buf, (int64_t)len);
        free(buf);
        return (void *)result;
    }

    // Plain scalar
    if (reject_anchor_alias(p))
        return NULL;
    size_t start = p->pos;
    while (!parser_eof(p) && parser_peek(p) != '\n') {
        if (parser_peek(p) == '#' &&
            (p->pos == start || p->input[p->pos - 1] == ' ' || p->input[p->pos - 1] == '\t'))
            break;
        if (parser_peek(p) == ':' && (parser_peek_at(p, 1) == ' ' || parser_peek_at(p, 1) == '\n'))
            break;
        parser_advance(p);
    }

    size_t len = p->pos - start;
    // Trim trailing spaces
    while (len > 0 && p->input[start + len - 1] == ' ')
        len--;

    void *scalar = parse_scalar(p->input + start, len);
    parser_finish_value_line(p);
    return scalar;
}

//=============================================================================
// Public API - Parsing
//=============================================================================

/// @brief `Yaml.Parse(text)` — parse a YAML 1.2 document into Zanna values.
///
/// YAML types map to: null→NULL, bool→Box.I1, int→Box.I64, float→Box.F64,
/// string→rt_string, sequence→Seq, mapping→Map. Multi-document streams
/// (separated by `---`) return a Seq containing one value per document.
/// Returns NULL both for YAML `null` and for parse failures; call
/// `rt_yaml_error()` to distinguish the two cases.
///
/// @param text UTF-8 YAML source to parse.
/// @return Owned Zanna value, or NULL on empty/null/error.
void *rt_yaml_parse(rt_string text) {
    clear_error();

    if (!text || rt_str_len(text) == 0)
        return NULL; // YAML null

    const char *cstr = rt_string_cstr(text);
    int64_t len = rt_str_len(text);

    // YAML requires a valid UTF-8 character stream (VDOC-040).
    if (!cstr || len < 0 || !rt_utf8_span_valid(cstr, (size_t)len)) {
        set_error("invalid UTF-8 in document", 1);
        return NULL;
    }

    yaml_parser p;
    parser_init(&p, cstr, (size_t)len);
    parser_skip_blank_lines_and_comments(&p);
    if (parser_eof(&p))
        return NULL;

    const bool explicit_documents = parser_at_line_marker(&p, "---");
    void *documents = NULL;

    if (explicit_documents) {
        documents = rt_seq_new();
        if (!documents)
            return NULL;
        rt_seq_set_owns_elements(documents, 1);
    }

    void *first_document = NULL;
    int64_t document_count = 0;

    while (!parser_eof(&p)) {
        parser_skip_blank_lines_and_comments(&p);
        if (parser_eof(&p))
            break;

        if (parser_consume_line_marker(&p, "...")) {
            parser_skip_blank_lines_and_comments(&p);
            continue;
        }

        if (explicit_documents) {
            if (!parser_consume_line_marker(&p, "---")) {
                set_error("expected document separator '---'", p.line);
                yaml_release(documents);
                return NULL;
            }
            parser_skip_blank_lines_and_comments(&p);
            if (parser_eof(&p))
                break;
        }

        void *document = parse_value(&p, 0);
        if (!document && yaml_last_error[0] != '\0') {
            yaml_release(first_document);
            yaml_release(documents);
            return NULL;
        }

        if (explicit_documents) {
            rt_seq_push(documents, document);
            yaml_release(document);
        } else {
            first_document = document;
        }
        document_count++;

        parser_skip_blank_lines_and_comments(&p);
        if (parser_eof(&p))
            break;

        if (parser_consume_line_marker(&p, "...")) {
            parser_skip_blank_lines_and_comments(&p);
            if (parser_eof(&p))
                break;
        }

        if (explicit_documents) {
            if (!parser_at_line_marker(&p, "---")) {
                set_error("unexpected trailing content after YAML document", p.line);
                yaml_release(documents);
                return NULL;
            }
            continue;
        }

        set_error("unexpected trailing content after YAML value", p.line);
        yaml_release(first_document);
        return NULL;
    }

    if (document_count == 0) {
        yaml_release(documents);
        return NULL;
    }

    if (explicit_documents)
        return documents;
    return first_document;
}

/// @brief `Yaml.ParseResult(text)` — parse YAML source into a Result.
///
/// A valid YAML document returns `Ok(value)`. YAML null and empty documents are
/// valid and therefore return `Ok(NULL)`. Invalid YAML returns `Err(message)`.
///
/// @param text YAML source text.
/// @return Owned `Zanna.Result` carrying either a parsed value or an error string.
void *rt_yaml_parse_result(rt_string text) {
    void *value = rt_yaml_parse(text);
    rt_string err = rt_yaml_error();
    const int has_parse_error = !value && err && rt_str_len(err) > 0;
    if (has_parse_error) {
        void *result = rt_result_err_str(err);
        rt_str_release_maybe(err);
        return result;
    }
    rt_str_release_maybe(err);

    void *result = rt_result_ok(value);
    yaml_release(value);
    return result;
}

/// @brief `Yaml.Error()` — return the last parse error on this thread, or "" if none.
///        Errors are thread-local so concurrent parses don't clobber each other.
/// @return Owned diagnostic string, or an owned empty string when no error exists.
rt_string rt_yaml_error(void) {
    return rt_string_from_bytes(yaml_last_error, strlen(yaml_last_error));
}

/// @brief `Yaml.IsValid(text)` — return 1 if `text` parses as valid YAML, 0 otherwise.
///        Empty input is considered valid (maps to YAML null). Internally calls Parse
///        and discards the result so no document object is retained.
/// @param text Candidate UTF-8 YAML source.
/// @return 1 for accepted YAML, including empty and null documents, otherwise 0.
int8_t rt_yaml_is_valid(rt_string text) {
    clear_error();

    if (!text || rt_str_len(text) == 0)
        return 1; // Empty is valid

    void *result = rt_yaml_parse(text);
    if (result) {
        if (rt_obj_release_check0(result))
            rt_obj_free(result);
        return 1;
    }
    return yaml_last_error[0] == '\0' ? 1 : 0;
}
