//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/text/rt_scanner.c
// Purpose: Implements a cursor-based string scanner for the Zanna.Text.Scanner
//          class. Registered surface: Position/IsEnd/Remaining/Length props,
//          Reset, Peek/PeekAt/PeekStr, Read/ReadStr/ReadUntil/ReadUntilAny,
//          Match/MatchStr (non-consuming), Accept/AcceptStr/AcceptAny
//          (consuming), Skip, SkipWhitespace, and the token readers
//          ReadIdent/ReadIntToken/ReadNumberToken/ReadQuoted/ReadLine.
//
// Key invariants:
//   - The scanner maintains a byte-position cursor into the source string.
//   - Peek returns the byte at the current position without advancing.
//   - Read returns the current byte and advances the cursor by one.
//   - IsEnd returns true when position >= length; Peek/Read at end return -1.
//   - Match/MatchStr test without consuming; Accept/AcceptStr consume on
//     success and return whether they matched. There is no trapping Expect.
//   - The source string is retained by the scanner for the scanner lifetime.
//
// Ownership/Lifetime:
//   - Scanner objects are heap-allocated and managed by the runtime GC.
//   - Scanner finalization releases the retained source string.
//
// Links: src/runtime/text/rt_scanner.h (public API),
//        src/runtime/text/rt_compiled_pattern.h (regex pattern, related scanning)
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_scanner.c
 * @brief Implements a managed byte-oriented String cursor.
 * @details A Scanner retains its source and tracks a clamped byte position,
 *          providing non-consuming lookahead and matches, conditional
 *          consuming accepts, skipping, and copied token readers for
 *          identifiers, numbers, quoted text, delimiters, and lines.
 */

#include "rt_scanner.h"
#include "rt_internal.h"
#include "rt_object.h"
#include "rt_string.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

//=============================================================================
// Internal Structure
//=============================================================================

typedef struct {
    rt_string source;
    const char *data;
    int64_t len;
    int64_t pos;
} Scanner;

/// @brief Release the source retained by a Scanner runtime object.
/// @param obj Scanner being finalized; null is ignored.
static void scanner_finalizer(void *obj) {
    Scanner *s = (Scanner *)obj;
    if (s && s->source) {
        rt_string_unref(s->source);
        s->source = NULL;
        s->data = NULL;
    }
}

//=============================================================================
// Scanner Creation
//=============================================================================

/// @brief Create a scanner positioned at the start of a runtime string.
/// @details Retains a non-null source for the object's lifetime. A null source
///          creates an empty scanner.
/// @param source Borrowed runtime string to scan, or null.
/// @return Newly allocated runtime-managed Scanner object.
void *rt_scanner_new(rt_string source) {
    Scanner *s = (Scanner *)rt_obj_new_i64(0, (int64_t)sizeof(Scanner));

    s->source = source ? rt_string_ref(source) : NULL;
    s->data = source ? rt_string_cstr(source) : "";
    s->len = source ? rt_str_len(source) : 0;
    s->pos = 0;
    rt_obj_set_finalizer(s, scanner_finalizer);
    return s;
}

//=============================================================================
// Position and State
//=============================================================================

/// @brief Return the scanner's current byte offset into the input.
/// @param obj Borrowed Scanner object; null is treated as position zero.
/// @return Current zero-based byte position.
int64_t rt_scanner_pos(void *obj) {
    if (!obj)
        return 0;
    Scanner *s = (Scanner *)obj;
    return s->pos;
}

/// @brief Seek to an absolute position in the input, clamped to [0, len].
/// @param obj Borrowed Scanner object; null is ignored.
/// @param pos Requested absolute byte position.
void rt_scanner_set_pos(void *obj, int64_t pos) {
    if (!obj)
        return;
    Scanner *s = (Scanner *)obj;
    if (pos < 0)
        pos = 0;
    if (pos > s->len)
        pos = s->len;
    s->pos = pos;
}

/// @brief Check whether the scanner has reached the end of input.
/// @param obj Borrowed Scanner object; null is treated as ended.
/// @return `1` at or beyond the source length, otherwise `0`.
int8_t rt_scanner_is_end(void *obj) {
    if (!obj)
        return 1;
    Scanner *s = (Scanner *)obj;
    return s->pos >= s->len ? 1 : 0;
}

/// @brief Return the number of bytes remaining from the current position to end.
/// @param obj Borrowed Scanner object; null reports zero.
/// @return Remaining source byte count.
int64_t rt_scanner_remaining(void *obj) {
    if (!obj)
        return 0;
    Scanner *s = (Scanner *)obj;
    return s->len - s->pos;
}

/// @brief Return the total length of the input in bytes (not affected by current position).
/// @param obj Borrowed Scanner object; null reports zero.
/// @return Retained source length in bytes.
int64_t rt_scanner_len(void *obj) {
    if (!obj)
        return 0;
    Scanner *s = (Scanner *)obj;
    return s->len;
}

/// @brief Reset the scanner position to the beginning of the input.
/// @param obj Borrowed Scanner object; null is ignored.
void rt_scanner_reset(void *obj) {
    if (!obj)
        return;
    Scanner *s = (Scanner *)obj;
    s->pos = 0;
}

//=============================================================================
// Peeking
//=============================================================================

/// @brief Return the byte at the current position without consuming (-1 at end).
/// @param obj Borrowed Scanner object.
/// @return Unsigned byte value in `[0, 255]`, or `-1` for null/end-of-input.
int64_t rt_scanner_peek(void *obj) {
    if (!obj)
        return -1;
    Scanner *s = (Scanner *)obj;
    if (s->pos >= s->len)
        return -1;
    return (unsigned char)s->data[s->pos];
}

/// @brief Look at a character at a relative offset from the current position without advancing.
/// @param obj Borrowed Scanner object.
/// @param offset Signed byte displacement from the current cursor.
/// @return Unsigned byte value, or `-1` for overflow or an out-of-range index.
int64_t rt_scanner_peek_at(void *obj, int64_t offset) {
    if (!obj)
        return -1;
    Scanner *s = (Scanner *)obj;
    if ((offset > 0 && s->pos > INT64_MAX - offset) || (offset < 0 && s->pos < INT64_MIN - offset))
        return -1;
    int64_t idx = s->pos + offset;
    if (idx < 0 || idx >= s->len)
        return -1;
    return (unsigned char)s->data[idx];
}

/// @brief Preview the next n characters as a string without advancing the position.
/// @details Clamps @p n to the remaining byte count. Nonpositive requests and
///          null scanners return the empty-string sentinel.
/// @param obj Borrowed Scanner object.
/// @param n Maximum number of bytes to preview.
/// @return Newly allocated preview when nonempty, otherwise an empty sentinel.
rt_string rt_scanner_peek_str(void *obj, int64_t n) {
    if (!obj || n <= 0)
        return rt_const_cstr("");
    Scanner *s = (Scanner *)obj;

    int64_t avail = s->len - s->pos;
    if (n > avail)
        n = avail;
    if (n <= 0)
        return rt_const_cstr("");

    return rt_string_from_bytes(s->data + s->pos, n);
}

//=============================================================================
// Reading
//=============================================================================

/// @brief Read and consume one character, returning it as a byte value (-1 at end).
/// @param obj Borrowed Scanner object.
/// @return Consumed unsigned byte value, or `-1` for null/end-of-input.
int64_t rt_scanner_read(void *obj) {
    if (!obj)
        return -1;
    Scanner *s = (Scanner *)obj;
    if (s->pos >= s->len)
        return -1;
    return (unsigned char)s->data[s->pos++];
}

/// @brief Read and consume up to n characters, returning them as a string.
/// @details Clamps @p n to the remaining byte count and advances only by bytes
///          returned. Nonpositive requests return the empty sentinel.
/// @param obj Borrowed Scanner object.
/// @param n Maximum number of bytes to consume.
/// @return Newly allocated consumed substring, or an empty sentinel.
rt_string rt_scanner_read_str(void *obj, int64_t n) {
    if (!obj || n <= 0)
        return rt_const_cstr("");
    Scanner *s = (Scanner *)obj;

    int64_t avail = s->len - s->pos;
    if (n > avail)
        n = avail;
    if (n <= 0)
        return rt_const_cstr("");

    rt_string result = rt_string_from_bytes(s->data + s->pos, n);
    s->pos += n;
    return result;
}

/// @brief Read characters until the delimiter byte is found (delimiter not consumed).
/// @param obj Borrowed Scanner object.
/// @param delim Integer whose low unsigned byte is the delimiter.
/// @return Newly allocated bytes before the delimiter, or an empty sentinel
///         when no bytes were consumed.
rt_string rt_scanner_read_until(void *obj, int64_t delim) {
    if (!obj)
        return rt_const_cstr("");
    Scanner *s = (Scanner *)obj;

    int64_t start = s->pos;
    while (s->pos < s->len && (unsigned char)s->data[s->pos] != (unsigned char)delim) {
        s->pos++;
    }

    if (start == s->pos)
        return rt_const_cstr("");
    return rt_string_from_bytes(s->data + start, s->pos - start);
}

/// @brief Read characters until any character in the delimiter set is found.
/// @details The matching delimiter remains unconsumed. A null delimiter set
///          consumes the complete remainder.
/// @param obj Borrowed Scanner object.
/// @param delims Borrowed byte set, or null.
/// @return Newly allocated consumed bytes, or an empty sentinel.
rt_string rt_scanner_read_until_any(void *obj, rt_string delims) {
    if (!obj)
        return rt_const_cstr("");
    Scanner *s = (Scanner *)obj;

    if (!delims)
        return rt_scanner_read_str(obj, s->len - s->pos);
    const char *delim_str = rt_string_cstr(delims);
    int64_t delim_len = rt_str_len(delims);

    int64_t start = s->pos;
    while (s->pos < s->len) {
        char c = s->data[s->pos];
        int found = 0;
        for (int64_t i = 0; i < delim_len; i++) {
            if (c == delim_str[i]) {
                found = 1;
                break;
            }
        }
        if (found)
            break;
        s->pos++;
    }

    if (start == s->pos)
        return rt_const_cstr("");
    return rt_string_from_bytes(s->data + start, s->pos - start);
}

/// @brief Read characters while a predicate function returns true.
/// @details Invokes @p pred with each next unsigned byte and stops before the
///          first false result. A null predicate consumes nothing.
/// @param obj Borrowed Scanner object.
/// @param pred Borrowed callback returning nonzero for bytes to consume.
/// @return Newly allocated consumed bytes, or an empty sentinel.
rt_string rt_scanner_read_while(void *obj, int8_t (*pred)(int64_t)) {
    if (!obj || !pred)
        return rt_const_cstr("");
    Scanner *s = (Scanner *)obj;

    int64_t start = s->pos;
    while (s->pos < s->len && pred((unsigned char)s->data[s->pos])) {
        s->pos++;
    }

    if (start == s->pos)
        return rt_const_cstr("");
    return rt_string_from_bytes(s->data + start, s->pos - start);
}

//=============================================================================
// Matching
//=============================================================================

/// @brief Check whether the current character matches the given byte (without consuming).
/// @param obj Borrowed Scanner object.
/// @param c Integer whose low unsigned byte is compared.
/// @return `1` on a current-byte match, otherwise `0`.
int8_t rt_scanner_match(void *obj, int64_t c) {
    if (!obj)
        return 0;
    Scanner *s = (Scanner *)obj;
    if (s->pos >= s->len)
        return 0;
    return (unsigned char)s->data[s->pos] == (unsigned char)c ? 1 : 0;
}

/// @brief Check whether the upcoming bytes match a string (without consuming).
/// @details An empty non-null string matches at every cursor position,
///          including end-of-input.
/// @param obj Borrowed Scanner object.
/// @param str Borrowed runtime byte string to compare.
/// @return `1` when all bytes match, otherwise `0`.
int8_t rt_scanner_match_str(void *obj, rt_string str) {
    if (!obj)
        return 0;
    Scanner *s = (Scanner *)obj;

    if (!str)
        return 0;
    const char *str_data = rt_string_cstr(str);
    int64_t str_len = rt_str_len(str);

    if (str_len < 0 || str_len > s->len - s->pos)
        return 0;

    return memcmp(s->data + s->pos, str_data, (size_t)str_len) == 0 ? 1 : 0;
}

/// @brief If the current character matches, consume it and return 1; else return 0.
/// @param obj Borrowed Scanner object.
/// @param c Integer whose low unsigned byte is matched.
/// @return `1` when one byte matched and was consumed, otherwise `0`.
int8_t rt_scanner_accept(void *obj, int64_t c) {
    if (!rt_scanner_match(obj, c))
        return 0;
    Scanner *s = (Scanner *)obj;
    s->pos++;
    return 1;
}

/// @brief If the upcoming bytes match a string, consume them and return 1; else return 0.
/// @details Accepting an empty non-null string succeeds without moving the cursor.
/// @param obj Borrowed Scanner object.
/// @param str Borrowed runtime byte string to match.
/// @return `1` when the complete string matched, otherwise `0`.
int8_t rt_scanner_accept_str(void *obj, rt_string str) {
    if (!rt_scanner_match_str(obj, str))
        return 0;
    Scanner *s = (Scanner *)obj;
    int64_t len = rt_str_len(str);
    if (len > s->len - s->pos)
        return 0;
    s->pos += len;
    return 1;
}

/// @brief If the current character is in the given character set, consume it; else return 0.
/// @param obj Borrowed Scanner object.
/// @param chars Borrowed runtime string whose bytes form the accepted set.
/// @return `1` when one matching byte was consumed, otherwise `0`.
int8_t rt_scanner_accept_any(void *obj, rt_string chars) {
    if (!obj)
        return 0;
    Scanner *s = (Scanner *)obj;
    if (s->pos >= s->len)
        return 0;

    if (!chars)
        return 0;
    const char *char_str = rt_string_cstr(chars);
    int64_t char_len = rt_str_len(chars);
    char c = s->data[s->pos];

    for (int64_t i = 0; i < char_len; i++) {
        if (c == char_str[i]) {
            s->pos++;
            return 1;
        }
    }
    return 0;
}

//=============================================================================
// Skipping
//=============================================================================

/// @brief Advance the position by n characters (clamped to end of input).
/// @details Nonpositive distances do nothing; this operation never moves
///          backward.
/// @param obj Borrowed Scanner object.
/// @param n Number of bytes to skip.
void rt_scanner_skip(void *obj, int64_t n) {
    if (!obj || n <= 0)
        return;
    Scanner *s = (Scanner *)obj;
    if (n > s->len - s->pos)
        s->pos = s->len;
    else
        s->pos += n;
}

/// @brief Skip over whitespace characters (space, tab, newline, carriage return).
/// @param obj Borrowed Scanner object.
/// @return Number of bytes consumed from the fixed four-byte whitespace set.
int64_t rt_scanner_skip_whitespace(void *obj) {
    if (!obj)
        return 0;
    Scanner *s = (Scanner *)obj;

    int64_t start = s->pos;
    while (s->pos < s->len) {
        char c = s->data[s->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            s->pos++;
        else
            break;
    }
    return s->pos - start;
}

/// @brief Skip characters while a predicate function returns true; returns count skipped.
/// @param obj Borrowed Scanner object.
/// @param pred Borrowed callback receiving each next unsigned byte.
/// @return Number of bytes consumed; zero for a null scanner or callback.
int64_t rt_scanner_skip_while(void *obj, int8_t (*pred)(int64_t)) {
    if (!obj || !pred)
        return 0;
    Scanner *s = (Scanner *)obj;

    int64_t start = s->pos;
    while (s->pos < s->len && pred((unsigned char)s->data[s->pos])) {
        s->pos++;
    }
    return s->pos - start;
}

//=============================================================================
// Token Helpers
//=============================================================================

/// @brief Read an identifier token (letter/underscore start, then alphanumeric/underscore).
/// @details The grammar is ASCII `[A-Za-z_][A-Za-z0-9_]*`. Failure leaves the
///          cursor unchanged.
/// @param obj Borrowed Scanner object.
/// @return Newly allocated identifier token, or an empty sentinel on failure.
rt_string rt_scanner_read_ident(void *obj) {
    if (!obj)
        return rt_const_cstr("");
    Scanner *s = (Scanner *)obj;

    if (s->pos >= s->len)
        return rt_const_cstr("");

    char c = s->data[s->pos];
    // Must start with letter or underscore
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'))
        return rt_const_cstr("");

    int64_t start = s->pos;
    s->pos++;

    while (s->pos < s->len) {
        c = s->data[s->pos];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')
            s->pos++;
        else
            break;
    }

    return rt_string_from_bytes(s->data + start, s->pos - start);
}

/// @brief Read an integer literal (optional sign followed by digits) as a string.
/// @details Requires at least one ASCII decimal digit. A sign without a digit
///          rolls the cursor back to its original position.
/// @param obj Borrowed Scanner object.
/// @return Newly allocated integer token, or an empty sentinel on failure.
rt_string rt_scanner_read_int(void *obj) {
    if (!obj)
        return rt_const_cstr("");
    Scanner *s = (Scanner *)obj;

    if (s->pos >= s->len)
        return rt_const_cstr("");

    int64_t start = s->pos;

    // Optional sign
    if (s->data[s->pos] == '+' || s->data[s->pos] == '-') {
        s->pos++;
        if (s->pos >= s->len || !(s->data[s->pos] >= '0' && s->data[s->pos] <= '9')) {
            s->pos = start;
            return rt_const_cstr("");
        }
    }

    // Must have at least one digit
    if (!(s->data[s->pos] >= '0' && s->data[s->pos] <= '9')) {
        s->pos = start;
        return rt_const_cstr("");
    }

    while (s->pos < s->len && s->data[s->pos] >= '0' && s->data[s->pos] <= '9') {
        s->pos++;
    }

    return rt_string_from_bytes(s->data + start, s->pos - start);
}

/// @brief Read a numeric literal (integer or float with optional exponent) as a string.
/// @details Accepts an optional sign, digits around an optional decimal point,
///          and an optional exponent with at least one digit. At least one
///          mantissa digit is required. An incomplete exponent is excluded
///          from the token; complete failure restores the original cursor.
/// @param obj Borrowed Scanner object.
/// @return Newly allocated numeric token, or an empty sentinel on failure.
rt_string rt_scanner_read_number(void *obj) {
    if (!obj)
        return rt_const_cstr("");
    Scanner *s = (Scanner *)obj;

    if (s->pos >= s->len)
        return rt_const_cstr("");

    int64_t start = s->pos;

    // Optional sign
    if (s->data[s->pos] == '+' || s->data[s->pos] == '-') {
        s->pos++;
    }

    // Integer part
    int has_digits = 0;
    while (s->pos < s->len && s->data[s->pos] >= '0' && s->data[s->pos] <= '9') {
        s->pos++;
        has_digits = 1;
    }

    // Decimal point and fraction
    if (s->pos < s->len && s->data[s->pos] == '.') {
        s->pos++;
        while (s->pos < s->len && s->data[s->pos] >= '0' && s->data[s->pos] <= '9') {
            s->pos++;
            has_digits = 1;
        }
    }

    // Exponent - only consume if there are digits after e/E and optional sign
    if (s->pos < s->len && (s->data[s->pos] == 'e' || s->data[s->pos] == 'E')) {
        int64_t exp_start = s->pos;
        s->pos++;
        if (s->pos < s->len && (s->data[s->pos] == '+' || s->data[s->pos] == '-'))
            s->pos++;
        // Must have at least one digit after exponent
        if (s->pos < s->len && s->data[s->pos] >= '0' && s->data[s->pos] <= '9') {
            while (s->pos < s->len && s->data[s->pos] >= '0' && s->data[s->pos] <= '9') {
                s->pos++;
            }
        } else {
            // No digits after exponent, rollback
            s->pos = exp_start;
        }
    }

    if (!has_digits) {
        s->pos = start;
        return rt_const_cstr("");
    }

    return rt_string_from_bytes(s->data + start, s->pos - start);
}

/// @brief Read a quoted string literal (consumes opening/closing quote, handles escape sequences).
/// @details Decodes `\\n`, `\\t`, `\\r`, escaped slash, and escaped quote
///          bytes; other escapes discard the slash and preserve the following
///          byte. Missing opening quote is a non-consuming failure. Missing
///          closing quote restores the original cursor and traps.
/// @param obj Borrowed Scanner object.
/// @param quote Integer whose low byte is used as both quote delimiter.
/// @return Newly allocated decoded contents without delimiters, or an empty
///         string on mismatch or after a recoverable trap.
rt_string rt_scanner_read_quoted(void *obj, int64_t quote) {
    if (!obj)
        return rt_const_cstr("");
    Scanner *s = (Scanner *)obj;

    if (s->pos >= s->len || s->data[s->pos] != (char)quote)
        return rt_const_cstr("");

    int64_t start = s->pos;
    s->pos++; // Skip opening quote

    // Build result, handling escapes
    size_t cap = 64;
    size_t buf_pos = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) {
        rt_trap("Scanner.ReadQuoted: memory allocation failed");
        return rt_string_from_bytes("", 0);
    }

    while (s->pos < s->len && s->data[s->pos] != (char)quote) {
        char actual;
        if (s->data[s->pos] == '\\' && s->pos + 1 < s->len) {
            s->pos++;
            char esc = s->data[s->pos++];
            switch (esc) {
                case 'n':
                    actual = '\n';
                    break;
                case 't':
                    actual = '\t';
                    break;
                case 'r':
                    actual = '\r';
                    break;
                case '\\':
                    actual = '\\';
                    break;
                case '"':
                    actual = '"';
                    break;
                case '\'':
                    actual = '\'';
                    break;
                default:
                    actual = esc;
                    break;
            }
        } else {
            actual = s->data[s->pos];
            s->pos++;
        }
        if (buf_pos + 1 >= cap) {
            if (cap > SIZE_MAX / 2) {
                free(buf);
                rt_trap("Scanner.ReadQuoted: string length overflow");
                return rt_string_from_bytes("", 0);
            }
            cap *= 2;
            char *tmp = (char *)realloc(buf, cap);
            if (!tmp) {
                free(buf);
                rt_trap("Scanner.ReadQuoted: memory allocation failed");
                return rt_string_from_bytes("", 0);
            }
            buf = tmp;
        }
        buf[buf_pos++] = actual;
    }

    // Skip closing quote
    if (s->pos < s->len && s->data[s->pos] == (char)quote) {
        s->pos++;
    } else {
        free(buf);
        s->pos = start;
        rt_trap("Scanner.ReadQuoted: unterminated quoted string");
        return rt_string_from_bytes("", 0);
    }

    rt_string result = rt_string_from_bytes(buf, buf_pos);
    free(buf);
    return result;
}

/// @brief Read until end-of-line and consume the line terminator (\r\n or \n).
/// @details Stops at either carriage return or line feed. A carriage return
///          consumes one following line feed; a lone line feed is also consumed.
/// @param obj Borrowed Scanner object.
/// @return Newly allocated line bytes excluding the terminator, or an empty
///         sentinel for a null scanner.
rt_string rt_scanner_read_line(void *obj) {
    if (!obj)
        return rt_const_cstr("");
    Scanner *s = (Scanner *)obj;

    int64_t start = s->pos;
    while (s->pos < s->len && s->data[s->pos] != '\n' && s->data[s->pos] != '\r') {
        s->pos++;
    }

    rt_string result = rt_string_from_bytes(s->data + start, s->pos - start);

    // Skip newline(s)
    if (s->pos < s->len && s->data[s->pos] == '\r')
        s->pos++;
    if (s->pos < s->len && s->data[s->pos] == '\n')
        s->pos++;

    return result;
}

//=============================================================================
// Character Class Predicates
//=============================================================================

/// @brief Character predicate: returns 1 if c is an ASCII digit ('0'-'9').
/// @param c Integer value to classify.
/// @return `1` for an ASCII decimal digit, otherwise `0`.
int8_t rt_scanner_is_digit(int64_t c) {
    return (c >= '0' && c <= '9') ? 1 : 0;
}

/// @brief Character predicate: returns 1 if c is an ASCII letter (a-z or A-Z).
/// @param c Integer value to classify.
/// @return `1` for an ASCII letter, otherwise `0`.
int8_t rt_scanner_is_alpha(int64_t c) {
    return ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) ? 1 : 0;
}

/// @brief Character predicate: returns 1 if c is alphanumeric (letter or digit).
/// @param c Integer value to classify.
/// @return `1` for an ASCII letter or decimal digit, otherwise `0`.
int8_t rt_scanner_is_alnum(int64_t c) {
    return ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) ? 1 : 0;
}

/// @brief Character predicate: returns 1 if c is whitespace (space, tab, newline, CR).
/// @param c Integer value to classify.
/// @return `1` for space, tab, line feed, or carriage return; otherwise `0`.
int8_t rt_scanner_is_space(int64_t c) {
    return (c == ' ' || c == '\t' || c == '\n' || c == '\r') ? 1 : 0;
}
