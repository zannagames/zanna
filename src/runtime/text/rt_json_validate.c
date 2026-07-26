//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE in the project root for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/text/rt_json_validate.c
// Purpose: JSON syntax validation for the Zanna.Data.Json class
//          per ECMA-404 / RFC 8259. Mirrors the structure of the recursive
//          descent parser without constructing a runtime value tree, returning
//          a Boolean verdict before callers commit to a full parse.
//
// Key invariants:
//   - Shares the json_parser cursor with rt_json_parse.c (rt_json_internal.h)
//     but allocates no Zanna objects; finite-number checks use a temporary C
//     string allocation.
//   - Returns 0 on any malformed input; 1 only when a complete JSON value is
//     consumed with no trailing content.
//
// Ownership/Lifetime:
//   - The json_parser borrows the input buffer.
//   - Temporary number buffers are freed before the validator returns.
//
// Links: src/runtime/text/rt_json.h (public API),
//        src/runtime/text/rt_json_internal.h (shared parser cursor),
//        src/runtime/text/rt_json_parse.c (allocating parser)
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_json_validate.c
 * @brief Implements non-tree-building JSON syntax validation.
 * @details The validator mirrors the allocating parser over the shared cursor,
 *          checking UTF-8, String escapes, finite numeric lexemes, containers,
 *          depth, and complete input consumption without creating managed JSON
 *          values. Only bounded native numeric scratch allocation is used.
 */

#include "rt_json.h"

#include "rt_internal.h"
#include "rt_json_internal.h"
#include "rt_string.h"

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/// @brief True if the span [start, start+len) is a finite double under strtod.
/// @details Used by the number validator to reject values that overflow to
///          infinity, leave unconsumed bytes, or set `ERANGE`. Copies the span
///          to a temporary NUL-terminated buffer because `strtod` is not
///          length-bounded; allocation failure traps and reports false.
/// @param start Borrowed start of the number lexeme.
/// @param len Number of bytes in the lexeme.
/// @return `1` when the entire span converts to a finite double, otherwise `0`.
static int json_number_is_finite_span(const char *start, size_t len) {
    if (!start || len == 0)
        return 0;

    char *num_str = (char *)malloc(len + 1);
    if (!num_str) {
        rt_trap("Json: memory allocation failed");
        return 0;
    }
    memcpy(num_str, start, len);
    num_str[len] = '\0';

    errno = 0;
    char *endptr = NULL;
    double value = strtod(num_str, &endptr);
    int ok = endptr == num_str + len && errno != ERANGE && isfinite(value);
    free(num_str);
    return ok;
}

/// @brief Consume exactly four hex digits, accumulating their value into `*out`.
/// @details Invalid input may leave the parser cursor partway through the
///          four-byte sequence; callers abandon validation immediately.
/// @param p Mutable parser cursor.
/// @param out Non-null output receiving the decoded 16-bit value on success.
/// @return 1 if four hex digits were consumed, 0 otherwise.
static int validate_hex4(json_parser *p, unsigned int *out) {
    if (p->pos + 4 > p->len)
        return 0;
    unsigned int codepoint = 0;
    for (int i = 0; i < 4; i++) {
        int digit = rt_hex_digit_value(parser_consume(p));
        if (digit < 0)
            return 0;
        codepoint = (codepoint << 4) | (unsigned int)digit;
    }
    *out = codepoint;
    return 1;
}

/// @brief Non-trapping JSON validator: advance the cursor past one JSON value.
/// @details Mirrors the structure of `parse_value` but never allocates
///          runtime values and returns 0 on malformed syntax. It is mutually
///          recursive for objects and arrays, validates raw UTF-8 and surrogate
///          pairs, rejects non-finite numbers, and enforces `JSON_MAX_DEPTH`.
///          Number validation may allocate a temporary C buffer.
/// @param p Mutable parser cursor positioned before one value.
/// @return 1 if a complete JSON value was consumed, 0 otherwise.
static int validate_value(json_parser *p) {
    parser_skip_whitespace(p);
    if (parser_eof(p))
        return 0;

    char c = parser_peek(p);

    if (c == '"') {
        // String: consume opening quote, scan to closing quote handling escapes
        parser_consume(p);
        while (!parser_eof(p)) {
            unsigned char ch = (unsigned char)parser_consume(p);
            if (ch == '"')
                return 1;
            if (ch < 0x20)
                return 0;
            if (ch >= 0x80) {
                // Match the parser: raw non-ASCII bytes must form valid UTF-8,
                // so IsValid never accepts text the trapping parser rejects.
                size_t extra = 0;
                if (!json_raw_utf8_sequence_valid(
                        ch, p->input + p->pos, p->len - p->pos, &extra))
                    return 0;
                p->pos += extra;
                continue;
            }
            if (ch == '\\') {
                if (parser_eof(p))
                    return 0;
                char esc = parser_consume(p);
                switch (esc) {
                    case '"':
                    case '\\':
                    case '/':
                    case 'b':
                    case 'f':
                    case 'n':
                    case 'r':
                    case 't':
                        break;
                    case 'u': {
                        unsigned int codepoint = 0;
                        if (!validate_hex4(p, &codepoint))
                            return 0;
                        if (codepoint >= 0xD800 && codepoint <= 0xDBFF) {
                            if (p->pos + 6 > p->len || parser_peek(p) != '\\' ||
                                p->input[p->pos + 1] != 'u')
                                return 0;
                            parser_consume(p);
                            parser_consume(p);
                            unsigned int low = 0;
                            if (!validate_hex4(p, &low))
                                return 0;
                            if (low < 0xDC00 || low > 0xDFFF)
                                return 0;
                        }
                        if (codepoint >= 0xDC00 && codepoint <= 0xDFFF)
                            return 0;
                        break;
                    }
                    default:
                        return 0;
                }
            }
        }
        return 0; // unterminated string
    }

    if (c == '-' || (c >= '0' && c <= '9')) {
        // Number
        size_t number_start = p->pos;
        if (c == '-')
            parser_consume(p);
        if (parser_eof(p))
            return 0;
        c = parser_peek(p);
        if (c == '0') {
            parser_consume(p);
            if (!parser_eof(p) && parser_peek(p) >= '0' && parser_peek(p) <= '9')
                return 0;
        } else if (c >= '1' && c <= '9') {
            parser_consume(p);
            while (!parser_eof(p) && parser_peek(p) >= '0' && parser_peek(p) <= '9')
                parser_consume(p);
        } else {
            return 0;
        }
        if (!parser_eof(p) && parser_peek(p) == '.') {
            parser_consume(p);
            if (parser_eof(p) || !(parser_peek(p) >= '0' && parser_peek(p) <= '9'))
                return 0;
            while (!parser_eof(p) && parser_peek(p) >= '0' && parser_peek(p) <= '9')
                parser_consume(p);
        }
        if (!parser_eof(p) && (parser_peek(p) == 'e' || parser_peek(p) == 'E')) {
            parser_consume(p);
            if (!parser_eof(p) && (parser_peek(p) == '+' || parser_peek(p) == '-'))
                parser_consume(p);
            if (parser_eof(p) || !(parser_peek(p) >= '0' && parser_peek(p) <= '9'))
                return 0;
            while (!parser_eof(p) && parser_peek(p) >= '0' && parser_peek(p) <= '9')
                parser_consume(p);
        }
        if (!json_number_is_finite_span(p->input + number_start, p->pos - number_start))
            return 0;
        return 1;
    }

    if (c == '{') {
        if (p->depth >= JSON_MAX_DEPTH)
            return 0;
        p->depth++;
        parser_consume(p);
        parser_skip_whitespace(p);
        if (!parser_eof(p) && parser_peek(p) == '}') {
            parser_consume(p);
            p->depth--;
            return 1;
        }
        int ok = 0;
        for (;;) {
            parser_skip_whitespace(p);
            if (parser_eof(p) || parser_peek(p) != '"')
                break;
            if (!validate_value(p))
                break; // key
            parser_skip_whitespace(p);
            if (parser_eof(p) || parser_consume(p) != ':')
                break;
            if (!validate_value(p))
                break; // value
            parser_skip_whitespace(p);
            if (parser_eof(p))
                break;
            c = parser_consume(p);
            if (c == '}') {
                ok = 1;
                break;
            }
            if (c != ',')
                break;
        }
        p->depth--;
        return ok;
    }

    if (c == '[') {
        if (p->depth >= JSON_MAX_DEPTH)
            return 0;
        p->depth++;
        parser_consume(p);
        parser_skip_whitespace(p);
        if (!parser_eof(p) && parser_peek(p) == ']') {
            parser_consume(p);
            p->depth--;
            return 1;
        }
        int ok = 0;
        for (;;) {
            if (!validate_value(p))
                break;
            parser_skip_whitespace(p);
            if (parser_eof(p))
                break;
            c = parser_consume(p);
            if (c == ']') {
                ok = 1;
                break;
            }
            if (c != ',')
                break;
        }
        p->depth--;
        return ok;
    }

    // Keywords: true, false, null
    if (c == 't') {
        if (p->pos + 4 <= p->len && strncmp(p->input + p->pos, "true", 4) == 0) {
            p->pos += 4;
            return 1;
        }
        return 0;
    }
    if (c == 'f') {
        if (p->pos + 5 <= p->len && strncmp(p->input + p->pos, "false", 5) == 0) {
            p->pos += 5;
            return 1;
        }
        return 0;
    }
    if (c == 'n') {
        if (p->pos + 4 <= p->len && strncmp(p->input + p->pos, "null", 4) == 0) {
            p->pos += 4;
            return 1;
        }
        return 0;
    }

    return 0;
}

/// @brief Quick syntactic check: is `text` parseable as JSON?
///
/// Runs a tree-free validator that mirrors the real parser,
/// returning a boolean instead of either the value or a trap.
/// Useful for input validation before committing to a parse. No runtime value
/// tree is allocated, although numeric lexemes use a temporary C buffer.
/// @param text Borrowed JSON text; null and empty strings are invalid.
/// @return `1` only when one complete value plus optional surrounding
///         whitespace consumes the input, otherwise `0`.
int8_t rt_json_is_valid(rt_string text) {
    if (!text || rt_str_len(text) == 0)
        return 0;

    const char *input = rt_string_cstr(text);
    size_t len = (size_t)rt_str_len(text);
    json_parser p;
    parser_init(&p, input, len);

    if (!validate_value(&p))
        return 0;

    // Ensure no trailing content after the JSON value
    parser_skip_whitespace(&p);
    return parser_eof(&p) ? 1 : 0;
}
