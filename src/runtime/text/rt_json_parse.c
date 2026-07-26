//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE in the project root for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/text/rt_json_parse.c
// Purpose: JSON parsing for the Zanna.Text.Json class per ECMA-404 / RFC 8259.
//          Recursive-descent parser that maps JSON types to Zanna runtime
//          types: null→NULL, bool→Box.I1, number→Box.F64, string→String,
//          array→Seq, object→Map<String,*>.
//
// Key invariants:
//   - All JSON numbers are parsed as IEEE 754 double (Box.F64).
//   - Unicode escape sequences (\uXXXX), including surrogate pairs, are decoded.
//   - Nesting beyond JSON_MAX_DEPTH first unwinds without trapping; the public
//     entry point then reports the error through its selected trap/status mode.
//   - Invalid input traps with a line/column diagnostic unless trap_errors is
//     cleared (rt_json_try_parse), in which case the error is reported via out
//     parameters.
//
// Ownership/Lifetime:
//   - Returned Map/Seq/String trees are fresh allocations owned by the caller.
//
// Links: src/runtime/text/rt_json.h (public API),
//        src/runtime/text/rt_json_internal.h (shared parser cursor),
//        src/runtime/text/rt_json_validate.c (non-allocating validator),
//        src/runtime/text/rt_json_format.c (inverse: value → JSON text)
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_json_parse.c
 * @brief Implements allocating recursive-descent JSON parsing.
 * @details The parser decodes validated UTF-8 and Unicode escapes, constructs
 *          caller-owned Maps, element-owning sequences, Strings, and boxed
 *          primitives, replaces duplicate object members, enforces finite
 *          nesting, and supports either trapping or structured diagnostics.
 */

#include "rt_json.h"

#include "rt_box.h"
#include "rt_internal.h"
#include "rt_json_internal.h"
#include "rt_map.h"
#include "rt_numeric.h"
#include "rt_object.h"
#include "rt_seq.h"
#include "rt_string.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/// @brief Release one locally owned parsed-value reference.
/// @details Frees the runtime object if dropping this reference reaches zero.
///          JSON null is represented by a null pointer and needs no action.
/// @param value Parsed runtime object owned by the current scope.
static void json_release_value(void *value) {
    if (value && rt_obj_release_check0(value))
        rt_obj_free(value);
}

/// @brief Discard a parsed value using the correct string/object release path.
/// @param value Parsed runtime string, boxed value, container, or JSON null.
static void json_discard_value(void *value) {
    if (!value)
        return;
    if (rt_string_is_handle(value))
        rt_string_unref((rt_string)value);
    else
        json_release_value(value);
}

static void parser_error(json_parser *p, const char *msg);

/// @brief Validate and expose the byte view for a JSON input string.
/// @details The parser uses `size_t` cursor arithmetic internally. Runtime
///          strings should not report negative lengths, but this guard keeps
///          malformed or partially-constructed handles from turning `-1` into
///          a huge `size_t` and making the parser walk out of bounds.
/// @param text Runtime string handle to inspect.
/// @param context Diagnostic prefix for traps.
/// @param out_input Receives the borrowed UTF-8 byte pointer.
/// @param out_len Receives the byte length as `size_t`.
/// @return 1 when the string is non-empty and valid for parsing; otherwise 0.
static int json_input_view(rt_string text,
                           const char *context,
                           const char **out_input,
                           size_t *out_len) {
    int64_t raw_len = rt_str_len(text);
    if (raw_len < 0) {
        char buf[128];
        snprintf(buf, sizeof(buf), "%s: invalid input length", context);
        rt_trap(buf);
        return 0;
    }
    if (raw_len == 0) {
        char buf[128];
        snprintf(buf, sizeof(buf), "%s: empty input", context);
        rt_trap(buf);
        return 0;
    }

    *out_input = rt_string_cstr(text);
    *out_len = (size_t)raw_len;
    if (!*out_input) {
        char buf[128];
        snprintf(buf, sizeof(buf), "%s: invalid input", context);
        rt_trap(buf);
        return 0;
    }
    return 1;
}

/// @brief Push a parsed JSON array element and verify that it was appended.
/// @details `rt_seq_push` reports allocation failure via traps and returns
///          `void`; when a trap recovery handler resumes execution, the caller
///          needs a second signal that parsing must stop. Comparing the length
///          before and after the push gives that signal without changing the
///          public Seq API.
/// @param p Parser state used for diagnostics.
/// @param seq Destination array sequence.
/// @param value Parsed element, including NULL for JSON null.
/// @return 1 when the value was appended; 0 when the parse should abort.
static int json_array_push_checked(json_parser *p, void *seq, void *value) {
    int64_t before = rt_seq_len(seq);
    rt_seq_push(seq, value);
    if (rt_seq_len(seq) != before + 1) {
        parser_error(p, "memory allocation failed");
        return 0;
    }
    return 1;
}

/// @brief Store a parsed JSON object member and verify that it is retrievable.
/// @details Map insertion also reports allocation failures by trapping. This
///          helper confirms either insertion or replacement happened before the
///          parser releases its local references.
/// @param p Parser state used for diagnostics.
/// @param map Destination object map.
/// @param key Object member key.
/// @param value Parsed member value.
/// @return 1 when the map contains `key` after the call; otherwise 0.
static int json_object_set_checked(json_parser *p, void *map, rt_string key, void *value) {
    rt_map_set(map, key, value);
    if (!rt_map_has(map, key)) {
        parser_error(p, "memory allocation failed");
        return 0;
    }
    return 1;
}

/// @brief Record a line/column parse diagnostic and optionally trap.
///
/// Computes the location lazily by walking from the start; cheap
/// for typical-sized JSON inputs and avoids tracking line/col on
/// every byte advance. Stores the detail and one-based location, moves the
/// cursor to EOF, and traps only when `p->trap_errors` is set.
/// @param p Mutable parser state to mark failed.
/// @param msg Diagnostic detail, or null for `"parse error"`.
static void parser_error(json_parser *p, const char *msg) {
    const char *detail = msg ? msg : "parse error";

    // Calculate line and column for error message
    size_t line = 1;
    size_t col = 1;
    for (size_t i = 0; i < p->pos && i < p->len; i++) {
        if (p->input[i] == '\n') {
            line++;
            col = 1;
        } else {
            col++;
        }
    }

    char buf[256];
    snprintf(buf, sizeof(buf), "Json.Parse: %s at line %zu, column %zu", detail, line, col);
    p->has_error = 1;
    p->error_line = (int64_t)line;
    p->error_column = (int64_t)col;
    snprintf(p->error_message, sizeof(p->error_message), "%s", detail);
    p->pos = p->len;
    if (!p->trap_errors) {
        return;
    }
    rt_trap(buf);
}

//=============================================================================
// Forward Declarations
//=============================================================================

/// @brief Forward declaration: parse any JSON value (object/array/string/number/literal).
/// @details JSON `null` is represented by a C `NULL` value in the runtime tree.
///          Callers must inspect the parser error state to distinguish a valid
///          root `null` from parse failure.
/// @param p Mutable parser positioned before a value.
/// @return Newly allocated parsed value, or null for JSON null/error/depth.
static void *parse_value(json_parser *p);

//=============================================================================
// String Parsing
//=============================================================================

/// @brief Ensure a JSON string decode buffer can append @p extra bytes plus NUL.
/// @details Grows geometrically with explicit overflow checks instead of raw
///          `cap *= 2`, so extremely large string inputs fail safely rather
///          than wrapping the allocation size.
/// @param buf_io Buffer pointer to grow in place.
/// @param cap_io Current capacity, updated on success.
/// @param len Current used length.
/// @param extra Number of bytes the caller needs to append.
/// @return 1 on success, 0 on allocation or overflow failure.
static int json_string_ensure_capacity(char **buf_io, size_t *cap_io, size_t len, size_t extra) {
    if (!buf_io || !*buf_io || !cap_io)
        return 0;
    if (len > SIZE_MAX - 1 || extra > SIZE_MAX - len - 1)
        return 0;
    size_t needed = len + extra + 1;
    if (needed <= *cap_io)
        return 1;
    size_t new_cap = *cap_io;
    while (new_cap < needed) {
        if (new_cap > SIZE_MAX / 2) {
            new_cap = needed;
            break;
        }
        new_cap *= 2;
    }
    char *tmp = (char *)realloc(*buf_io, new_cap);
    if (!tmp)
        return 0;
    *buf_io = tmp;
    *cap_io = new_cap;
    return 1;
}


/// @brief Parse a JSON string (starting after the opening quote).
/// @details Consumes both quotes, decodes all RFC 8259 escapes, combines valid
///          UTF-16 surrogate pairs, emits UTF-8, rejects unescaped controls,
///          and validates raw UTF-8 sequences. Syntax errors mark @p p and
///          return an empty fallback string for caller cleanup.
/// @param p Mutable parser positioned at the opening quote.
/// @return Newly allocated decoded runtime string, or an empty fallback after
///         a parse/allocation error.
static rt_string parse_string(json_parser *p) {
    if (parser_consume(p) != '"') {
        parser_error(p, "expected string");
        return rt_string_from_bytes("", 0);
    }

    size_t cap = 64;
    size_t len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) {
        rt_trap("Json.Parse: memory allocation failed");
        return rt_string_from_bytes("", 0);
    }

    while (!parser_eof(p)) {
        char c = parser_consume(p);

        if (c == '"') {
            // End of string
            buf[len] = '\0';
            rt_string result = rt_string_from_bytes(buf, len);
            free(buf);
            if (!result)
                parser_error(p, "memory allocation failed");
            return result;
        }

        if (c == '\\') {
            // Escape sequence
            if (parser_eof(p)) {
                free(buf);
                parser_error(p, "unexpected end of string");
                return rt_string_from_bytes("", 0);
            }

            char esc = parser_consume(p);
            char decoded;
            switch (esc) {
                case '"':
                    decoded = '"';
                    break;
                case '\\':
                    decoded = '\\';
                    break;
                case '/':
                    decoded = '/';
                    break;
                case 'b':
                    decoded = '\b';
                    break;
                case 'f':
                    decoded = '\f';
                    break;
                case 'n':
                    decoded = '\n';
                    break;
                case 'r':
                    decoded = '\r';
                    break;
                case 't':
                    decoded = '\t';
                    break;
                case 'u': {
                    // Unicode escape: \uXXXX
                    if (p->pos + 4 > p->len) {
                        free(buf);
                        parser_error(p, "incomplete unicode escape");
                        return rt_string_from_bytes("", 0);
                    }

                    unsigned int codepoint = 0;
                    for (int i = 0; i < 4; i++) {
                        char hex = parser_consume(p);
                        unsigned int digit;
                        if (hex >= '0' && hex <= '9')
                            digit = hex - '0';
                        else if (hex >= 'a' && hex <= 'f')
                            digit = 10 + hex - 'a';
                        else if (hex >= 'A' && hex <= 'F')
                            digit = 10 + hex - 'A';
                        else {
                            free(buf);
                            parser_error(p, "invalid unicode escape");
                            return rt_string_from_bytes("", 0);
                        }
                        codepoint = (codepoint << 4) | digit;
                    }

                    // Handle UTF-16 surrogate pairs for codepoints above U+FFFF.
                    // High surrogate (D800-DBFF) must be followed by \uDC00-\uDFFF.
                    if (codepoint >= 0xD800 && codepoint <= 0xDBFF) {
                        // Expect \u followed by low surrogate
                        if (p->pos + 6 > p->len || parser_peek(p) != '\\' ||
                            p->input[p->pos + 1] != 'u') {
                            free(buf);
                            parser_error(p, "missing low surrogate in unicode escape");
                            return rt_string_from_bytes("", 0);
                        }
                        parser_consume(p); // consume '\'
                        parser_consume(p); // consume 'u'

                        unsigned int low = 0;
                        for (int i = 0; i < 4; i++) {
                            char hex = parser_consume(p);
                            unsigned int digit;
                            if (hex >= '0' && hex <= '9')
                                digit = hex - '0';
                            else if (hex >= 'a' && hex <= 'f')
                                digit = 10 + hex - 'a';
                            else if (hex >= 'A' && hex <= 'F')
                                digit = 10 + hex - 'A';
                            else {
                                free(buf);
                                parser_error(p, "invalid unicode escape");
                                return rt_string_from_bytes("", 0);
                            }
                            low = (low << 4) | digit;
                        }

                        if (low < 0xDC00 || low > 0xDFFF) {
                            free(buf);
                            parser_error(p, "invalid low surrogate in unicode escape");
                            return rt_string_from_bytes("", 0);
                        }

                        // Combine surrogates: U+10000 + (hi - D800) * 400 + (lo - DC00)
                        codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (low - 0xDC00);
                    } else if (codepoint >= 0xDC00 && codepoint <= 0xDFFF) {
                        // Lone low surrogate is invalid
                        free(buf);
                        parser_error(p, "unexpected low surrogate in unicode escape");
                        return rt_string_from_bytes("", 0);
                    }

                    // Encode codepoint as UTF-8
                    if (codepoint < 0x80) {
                        if (!json_string_ensure_capacity(&buf, &cap, len, 1)) {
                            free(buf);
                            rt_trap("Json.Parse: memory allocation failed");
                            return rt_string_from_bytes("", 0);
                        }
                        buf[len++] = (char)codepoint;
                    } else if (codepoint < 0x800) {
                        if (!json_string_ensure_capacity(&buf, &cap, len, 2)) {
                            free(buf);
                            rt_trap("Json.Parse: memory allocation failed");
                            return rt_string_from_bytes("", 0);
                        }
                        buf[len++] = (char)(0xC0 | (codepoint >> 6));
                        buf[len++] = (char)(0x80 | (codepoint & 0x3F));
                    } else if (codepoint < 0x10000) {
                        if (!json_string_ensure_capacity(&buf, &cap, len, 3)) {
                            free(buf);
                            rt_trap("Json.Parse: memory allocation failed");
                            return rt_string_from_bytes("", 0);
                        }
                        buf[len++] = (char)(0xE0 | (codepoint >> 12));
                        buf[len++] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
                        buf[len++] = (char)(0x80 | (codepoint & 0x3F));
                    } else {
                        // 4-byte UTF-8 for supplementary plane (U+10000 to U+10FFFF)
                        if (!json_string_ensure_capacity(&buf, &cap, len, 4)) {
                            free(buf);
                            rt_trap("Json.Parse: memory allocation failed");
                            return rt_string_from_bytes("", 0);
                        }
                        buf[len++] = (char)(0xF0 | (codepoint >> 18));
                        buf[len++] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
                        buf[len++] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
                        buf[len++] = (char)(0x80 | (codepoint & 0x3F));
                    }
                    continue;
                }
                default:
                    free(buf);
                    parser_error(p, "invalid escape sequence");
                    return rt_string_from_bytes("", 0);
            }

            if (!json_string_ensure_capacity(&buf, &cap, len, 1)) {
                free(buf);
                rt_trap("Json.Parse: memory allocation failed");
                return rt_string_from_bytes("", 0);
            }
            buf[len++] = decoded;
        } else if ((unsigned char)c < 0x20) {
            // Control characters not allowed in strings
            free(buf);
            parser_error(p, "control character in string");
            return rt_string_from_bytes("", 0);
        } else {
            // Regular character
            size_t extra = 0;
            if (!json_raw_utf8_sequence_valid(
                    (unsigned char)c, p->input + p->pos, p->len - p->pos, &extra)) {
                free(buf);
                parser_error(p, "invalid UTF-8 in string");
                return rt_string_from_bytes("", 0);
            }
            if (!json_string_ensure_capacity(&buf, &cap, len, 1 + extra)) {
                free(buf);
                rt_trap("Json.Parse: memory allocation failed");
                return rt_string_from_bytes("", 0);
            }
            buf[len++] = c;
            for (size_t i = 0; i < extra; i++)
                buf[len++] = parser_consume(p);
        }
    }

    free(buf);
    parser_error(p, "unterminated string");
    return rt_string_from_bytes("", 0);
}

//=============================================================================
// Number Parsing
//=============================================================================

/// @brief Parse a JSON number into a boxed Float64.
///
/// Per RFC 8259 §6: optional `-`, integer part (no leading zeros
/// except `0` itself), optional `.fraction`, optional
/// `e[+-]exponent`. All forms are converted through the shared double parser;
/// non-finite or out-of-range results are rejected.
/// @param p Mutable parser positioned at the number's first byte.
/// @return Newly allocated boxed F64 value, including an owned zero fallback
///         after an error.
static void *parse_number(json_parser *p) {
    size_t start = p->pos;

    // Optional minus sign
    if (parser_peek(p) == '-')
        parser_consume(p);

    // Integer part
    if (parser_peek(p) == '0') {
        parser_consume(p);
    } else if (parser_peek(p) >= '1' && parser_peek(p) <= '9') {
        while (parser_peek(p) >= '0' && parser_peek(p) <= '9')
            parser_consume(p);
    } else {
        parser_error(p, "invalid number");
        return rt_box_f64(0.0);
    }

    // Fractional part
    if (parser_peek(p) == '.') {
        parser_consume(p);
        if (parser_peek(p) < '0' || parser_peek(p) > '9') {
            parser_error(p, "invalid number: expected digit after decimal point");
            return rt_box_f64(0.0);
        }
        while (parser_peek(p) >= '0' && parser_peek(p) <= '9')
            parser_consume(p);
    }

    // Exponent part
    if (parser_peek(p) == 'e' || parser_peek(p) == 'E') {
        parser_consume(p);
        if (parser_peek(p) == '+' || parser_peek(p) == '-')
            parser_consume(p);
        if (parser_peek(p) < '0' || parser_peek(p) > '9') {
            parser_error(p, "invalid number: expected digit in exponent");
            return rt_box_f64(0.0);
        }
        while (parser_peek(p) >= '0' && parser_peek(p) <= '9')
            parser_consume(p);
    }

    // Extract the number string and parse it
    size_t num_len = p->pos - start;
    char *num_str = (char *)malloc(num_len + 1);
    if (!num_str) {
        rt_trap("Json.Parse: memory allocation failed");
        return rt_box_f64(0.0);
    }
    memcpy(num_str, p->input + start, num_len);
    num_str[num_len] = '\0';

    double value = 0.0;
    if (rt_parse_double(num_str, &value) != (int32_t)Err_None || !isfinite(value)) {
        free(num_str);
        parser_error(p, "number out of range");
        return rt_box_f64(0.0);
    }
    free(num_str);

    return rt_box_f64(value);
}

//=============================================================================
// Array Parsing
//=============================================================================

/// @brief Parse a JSON array `[…]` into a `Seq[Any]`.
///
/// Increments `p->depth` on `[` and decrements on `]`; if the
/// depth-cap is exceeded the parser sets `depth_exceeded` and
/// unwinds without trapping (S-16). Comma-separated values; empty
/// array is allowed; trailing commas are not. The returned Seq owns appended
/// element references, including null entries.
/// @param p Mutable parser positioned at `[`.
/// @return Newly allocated element-owning Seq, possibly partial when the
///         parser error/depth flags are set, or null on allocation/depth error.
static void *parse_array(json_parser *p) {
    if (p->has_error)
        return NULL;
    /* S-16: Reject deeply nested documents */
    if (p->depth >= JSON_MAX_DEPTH) {
        p->depth_exceeded = 1;
        return NULL;
    }
    p->depth++;

    if (parser_consume(p) != '[') {
        p->depth--;
        parser_error(p, "expected array");
        return rt_seq_new();
    }

    void *seq = rt_seq_new();
    if (!seq) {
        p->depth--;
        rt_trap("Json.Parse: array allocation failed");
        return NULL;
    }
    rt_seq_set_owns_elements(seq, 1);
    parser_skip_whitespace(p);

    // Empty array
    if (parser_peek(p) == ']') {
        parser_consume(p);
        p->depth--;
        return seq;
    }

    // Parse elements
    while (true) {
        if (p->has_error)
            break;
        parser_skip_whitespace(p);
        void *value = parse_value(p);
        if (p->has_error) {
            json_release_value(value);
            p->depth--;
            return seq;
        }
        /* S-16: depth limit hit inside nested value — bail out cleanly */
        if (p->depth_exceeded) {
            json_release_value(value);
            p->depth--;
            return seq;
        }
        if (!json_array_push_checked(p, seq, value)) {
            json_release_value(value);
            p->depth--;
            return seq;
        }
        json_release_value(value);

        parser_skip_whitespace(p);
        char c = parser_peek(p);

        if (c == ']') {
            parser_consume(p);
            break;
        } else if (c == ',') {
            parser_consume(p);
        } else {
            parser_error(p, "expected ',' or ']' in array");
            p->depth--;
            return seq;
        }
    }

    p->depth--;
    return seq;
}

//=============================================================================
// Object Parsing
//=============================================================================

/// @brief Parse a JSON object `{…}` into a `Map[String, Any]`.
///
/// Each member is `"key" : value`; commas separate members; the
/// empty object `{}` is allowed. Same depth-tracking story as
/// `parse_array`. RFC 8259 doesn't require unique keys but we
/// follow the convention of "last wins".
/// @param p Mutable parser positioned at `{`.
/// @return Newly allocated Map owning parsed keys and values, possibly partial
///         when parser error/depth flags are set, or null on allocation/depth
///         error.
static void *parse_object(json_parser *p) {
    if (p->has_error)
        return NULL;
    /* S-16: Reject deeply nested documents */
    if (p->depth >= JSON_MAX_DEPTH) {
        p->depth_exceeded = 1;
        return NULL;
    }
    p->depth++;

    if (parser_consume(p) != '{') {
        p->depth--;
        parser_error(p, "expected object");
        return rt_map_new();
    }

    void *map = rt_map_new();
    if (!map) {
        p->depth--;
        rt_trap("Json.Parse: object allocation failed");
        return NULL;
    }
    parser_skip_whitespace(p);

    // Empty object
    if (parser_peek(p) == '}') {
        parser_consume(p);
        p->depth--;
        return map;
    }

    // Parse key-value pairs
    while (true) {
        if (p->has_error)
            break;
        parser_skip_whitespace(p);

        if (parser_peek(p) != '"') {
            parser_error(p, "expected string key in object");
            p->depth--;
            return map;
        }

        rt_string key = parse_string(p);
        if (p->has_error) {
            rt_str_release_maybe(key);
            p->depth--;
            return map;
        }
        parser_skip_whitespace(p);

        if (parser_consume(p) != ':') {
            rt_str_release_maybe(key);
            parser_error(p, "expected ':' after key in object");
            p->depth--;
            return map;
        }

        parser_skip_whitespace(p);
        void *value = parse_value(p);
        if (p->has_error) {
            rt_str_release_maybe(key);
            json_release_value(value);
            p->depth--;
            return map;
        }
        /* S-16: depth limit hit inside nested value — bail out cleanly */
        if (p->depth_exceeded) {
            rt_str_release_maybe(key);
            json_release_value(value);
            p->depth--;
            return map;
        }

        if (!json_object_set_checked(p, map, key, value)) {
            rt_str_release_maybe(key);
            json_release_value(value);
            p->depth--;
            return map;
        }
        json_release_value(value);
        rt_str_release_maybe(key);

        parser_skip_whitespace(p);
        char c = parser_peek(p);

        if (c == '}') {
            parser_consume(p);
            break;
        } else if (c == ',') {
            parser_consume(p);
        } else {
            parser_error(p, "expected ',' or '}' in object");
            p->depth--;
            return map;
        }
    }

    p->depth--;
    return map;
}

//=============================================================================
// Value Parsing
//=============================================================================

/// @brief Top-level value dispatch — picks the parser by the next non-space character.
///
///   - `{` → `parse_object`
///   - `[` → `parse_array`
///   - `"` → `parse_string`
///   - digit, `-` → `parse_number`
///   - `t`, `f`, `n` → boolean / null literal (must match `true`/`false`/`null`)
/// Traps or records an error on any other character, according to parser mode.
/// @param p Mutable parser positioned before a JSON value.
/// @return Newly allocated runtime value, or null for JSON null, parse failure,
///         or propagated depth overflow; inspect @p p to distinguish them.
static void *parse_value(json_parser *p) {
    if (p->has_error)
        return NULL;
    /* S-16: Propagate depth-exceeded without trapping */
    if (p->depth_exceeded)
        return NULL;

    parser_skip_whitespace(p);

    if (parser_eof(p)) {
        parser_error(p, "unexpected end of input");
        return NULL;
    }

    char c = parser_peek(p);

    // String
    if (c == '"') {
        return (void *)parse_string(p);
    }

    // Number
    if (c == '-' || (c >= '0' && c <= '9')) {
        return parse_number(p);
    }

    // Array
    if (c == '[') {
        return parse_array(p);
    }

    // Object
    if (c == '{') {
        return parse_object(p);
    }

    // true
    if (p->pos + 4 <= p->len && strncmp(p->input + p->pos, "true", 4) == 0) {
        p->pos += 4;
        return rt_box_i1(1);
    }

    // false
    if (p->pos + 5 <= p->len && strncmp(p->input + p->pos, "false", 5) == 0) {
        p->pos += 5;
        return rt_box_i1(0);
    }

    // null
    if (p->pos + 4 <= p->len && strncmp(p->input + p->pos, "null", 4) == 0) {
        p->pos += 4;
        // Return NULL for JSON null
        return NULL;
    }

    parser_error(p, "unexpected character");
    return NULL;
}

//=============================================================================
// Public API
//=============================================================================

/// @brief Parses a JSON string into a Zanna value.
///
/// Parses any valid JSON text and returns the corresponding Zanna value.
/// The return type depends on the JSON content:
/// - Objects become Map (string-keyed)
/// - Arrays become Seq
/// - Strings stay as String
/// - Numbers become boxed f64
/// - Booleans become boxed i1
/// - null becomes NULL
///
/// **Example:**
/// ```
/// Dim text = "{\"name\": \"Alice\", \"items\": [1, 2, 3]}"
/// Dim obj = Json.Parse(text)
/// Print obj.Get("name")           ' "Alice"
/// Dim items = obj.Get("items")
/// Print items.Get(0)              ' 1.0
/// ```
///
/// **Error handling:**
/// Traps with descriptive error message on invalid JSON, including line
/// and column information. A null input returns null directly; an empty
/// runtime string is an error.
///
/// @param text Borrowed JSON text to parse.
///
/// @return Caller-owned parsed value (Map, Seq, String, boxed number, boxed
///         bool), or null for JSON null, null input, or a trapped error.
///
/// @note O(n) time complexity where n is the text length.
/// @note Thread-safe (no global state).
///
/// @see rt_json_parse_object For parsing specifically as object
/// @see rt_json_parse_array For parsing specifically as array
/// @see rt_json_format For the inverse operation
void *rt_json_parse(rt_string text) {
    if (!text)
        return NULL;

    const char *input = rt_string_cstr(text);
    int64_t len64 = rt_str_len(text);
    if (!input || len64 < 0) {
        rt_trap("Json.Parse: invalid input");
        return NULL;
    }
    size_t len = (size_t)len64;
    if (len == 0) {
        rt_trap("Json.Parse: empty input");
        return NULL;
    }

    json_parser p;
    parser_init(&p, input, len);

    void *result = parse_value(&p);

    if (p.has_error) {
        json_discard_value(result);
        return NULL;
    }

    if (p.depth_exceeded) {
        json_discard_value(result);
        parser_error(&p, "maximum nesting depth exceeded");
        return NULL;
    }

    // Check for trailing content
    parser_skip_whitespace(&p);
    if (!parser_eof(&p)) {
        parser_error(&p, "unexpected content after JSON value");
        json_discard_value(result);
        return NULL;
    }

    return result;
}

/// @brief Parse one complete JSON value while reporting syntax errors by status.
/// @details Initializes every supplied output before parsing. Successful JSON
///          null is distinguished by return value one with a null value output.
///          When @p out_value is omitted, a successfully parsed non-null value
///          is released. Syntax and depth errors do not call the parser trap
///          path, though lower-level allocation failures may still trap.
/// @param text Borrowed JSON text; null and empty input report `"empty input"`.
/// @param out_value Optional output receiving the caller-owned parsed value.
/// @param out_message Optional output receiving a caller-owned failure detail.
/// @param out_line Optional output receiving a one-based line or zero.
/// @param out_column Optional output receiving a one-based column or zero.
/// @return `1` on success, including JSON null, otherwise `0`.
int8_t rt_json_try_parse(rt_string text,
                         void **out_value,
                         rt_string *out_message,
                         int64_t *out_line,
                         int64_t *out_column) {
    if (out_value)
        *out_value = NULL;
    if (out_message)
        *out_message = NULL;
    if (out_line)
        *out_line = 0;
    if (out_column)
        *out_column = 0;

    int64_t len64 = text ? rt_str_len(text) : 0;
    if (!text || len64 <= 0) {
        if (out_message)
            *out_message = rt_string_from_bytes("empty input", strlen("empty input"));
        if (out_line)
            *out_line = 1;
        if (out_column)
            *out_column = 1;
        return 0;
    }

    const char *input = rt_string_cstr(text);
    if (!input) {
        if (out_message)
            *out_message = rt_string_from_bytes("invalid input", strlen("invalid input"));
        if (out_line)
            *out_line = 1;
        if (out_column)
            *out_column = 1;
        return 0;
    }
    size_t len = (size_t)len64;
    json_parser p;
    parser_init(&p, input, len);
    p.trap_errors = 0;

    void *result = parse_value(&p);

    if (!p.has_error && p.depth_exceeded) {
        p.has_error = 1;
        snprintf(p.error_message, sizeof(p.error_message), "%s", "maximum nesting depth exceeded");
        p.error_line = 0;
        p.error_column = 0;
    }

    if (!p.has_error) {
        parser_skip_whitespace(&p);
        if (!parser_eof(&p))
            parser_error(&p, "unexpected content after JSON value");
    }

    if (p.has_error) {
        if (result) {
            json_discard_value(result);
        }
        if (out_message)
            *out_message =
                rt_string_from_bytes(p.error_message[0] ? p.error_message : "parse error",
                                     strlen(p.error_message[0] ? p.error_message : "parse error"));
        if (out_line)
            *out_line = p.error_line;
        if (out_column)
            *out_column = p.error_column;
        return 0;
    }

    if (out_value)
        *out_value = result;
    else if (result) {
        json_discard_value(result);
    }
    return 1;
}

/// @brief Parses a JSON string expecting an object at the root.
///
/// Like rt_json_parse, but validates that the root value is a JSON object.
/// Returns a Map containing the object's key-value pairs.
///
/// **Example:**
/// ```
/// Dim config = Json.ParseObject("{\"debug\": true, \"port\": 8080}")
/// Print config.Get("port")  ' 8080.0
/// ```
///
/// @param text The JSON text to parse.
///
/// @return Map containing the object properties.
///
/// @note Traps if the root value is not an object.
///
/// @see rt_json_parse For parsing any JSON value
/// @see rt_json_parse_array For parsing specifically as array
void *rt_json_parse_object(rt_string text) {
    if (!text) {
        rt_trap("Json.ParseObject: null input");
        return NULL;
    }

    const char *input = NULL;
    size_t len = 0;
    if (!json_input_view(text, "Json.ParseObject", &input, &len))
        return NULL;

    json_parser p;
    parser_init(&p, input, len);
    parser_skip_whitespace(&p);

    if (parser_peek(&p) != '{') {
        rt_trap("Json.ParseObject: expected object at root");
        return NULL;
    }

    void *result = parse_object(&p);

    if (p.has_error) {
        json_discard_value(result);
        return NULL;
    }

    if (p.depth_exceeded) {
        json_discard_value(result);
        parser_error(&p, "maximum nesting depth exceeded");
        return NULL;
    }

    parser_skip_whitespace(&p);
    if (!parser_eof(&p)) {
        parser_error(&p, "unexpected content after JSON object");
        json_discard_value(result);
        return NULL;
    }

    return result;
}

/// @brief Parses a JSON string expecting an array at the root.
///
/// Like rt_json_parse, but validates that the root value is a JSON array.
/// Returns a Seq containing the array's elements.
///
/// **Example:**
/// ```
/// Dim items = Json.ParseArray("[1, 2, 3, \"four\"]")
/// Print items.Len()    ' 4
/// Print items.Get(3)   ' "four"
/// ```
///
/// @param text The JSON text to parse.
///
/// @return Seq containing the array elements.
///
/// @note Traps if the root value is not an array.
///
/// @see rt_json_parse For parsing any JSON value
/// @see rt_json_parse_object For parsing specifically as object
void *rt_json_parse_array(rt_string text) {
    if (!text) {
        rt_trap("Json.ParseArray: null input");
        return NULL;
    }

    const char *input = NULL;
    size_t len = 0;
    if (!json_input_view(text, "Json.ParseArray", &input, &len))
        return NULL;

    json_parser p;
    parser_init(&p, input, len);
    parser_skip_whitespace(&p);

    if (parser_peek(&p) != '[') {
        rt_trap("Json.ParseArray: expected array at root");
        return NULL;
    }

    void *result = parse_array(&p);

    if (p.has_error) {
        json_discard_value(result);
        return NULL;
    }

    if (p.depth_exceeded) {
        json_discard_value(result);
        parser_error(&p, "maximum nesting depth exceeded");
        return NULL;
    }

    parser_skip_whitespace(&p);
    if (!parser_eof(&p)) {
        parser_error(&p, "unexpected content after JSON array");
        json_discard_value(result);
        return NULL;
    }

    return result;
}
