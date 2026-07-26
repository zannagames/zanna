//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/text/rt_json_stream.c
// Purpose: Implements a SAX-style pull-based streaming JSON parser for the
//          Zanna.Data.JsonStream class. Emits tokens one at a time: ObjectStart,
//          ObjectEnd, ArrayStart, ArrayEnd, Key, String, Number, Bool, Null.
//
// Key invariants:
//   - The fixed state arrays have MAX_DEPTH (256) slots; the current pre-increment
//     guard admits at most 255 simultaneously open containers.
//   - The parser advances by one token per call to Next; state is maintained in
//     the stream object between calls.
//   - String token values are unescaped (\\, \", \n etc. processed).
//   - Number tokens are parsed as IEEE 754 double.
//   - Invalid JSON causes an Error token; the stream is not recoverable after error.
//   - The input string is retained so the source bytes stay alive while streaming.
//
// Ownership/Lifetime:
//   - The stream object is reference-counted and owned by its caller.
//   - An internal string buffer is grown dynamically and freed with the stream.
//   - Key and String token values are returned as fresh rt_string allocations.
//
// Links: src/runtime/text/rt_json_stream.h (public API),
//        src/runtime/text/rt_json.h (document-mode JSON parser for small inputs)
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_json_stream.c
 * @brief Implements an in-memory pull-token JSON parser.
 * @details Each managed stream retains its complete source and incremental
 *          object/array state, emits one structural or scalar token per call,
 *          decodes String and key escapes into reusable scratch storage,
 *          preserves raw numeric lexemes, and enters terminal error or end
 *          states after one complete document.
 */

#include "rt_json_stream.h"

#include "rt_internal.h"
#include "rt_json_internal.h"
#include "rt_numeric.h"
#include "rt_object.h"
#include "rt_result.h"
#include "rt_string.h"

#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/// Fixed number of container-state slots, including the unused depth-zero slot.
#define MAX_DEPTH 256

/// State-machine phase for the currently open object or array.
typedef enum {
    JSON_CTX_OBJECT_KEY_OR_END = 1,
    JSON_CTX_OBJECT_KEY = 2,
    JSON_CTX_OBJECT_COLON = 3,
    JSON_CTX_OBJECT_VALUE = 4,
    JSON_CTX_OBJECT_AFTER_VALUE = 5,
    JSON_CTX_ARRAY_VALUE_OR_END = 6,
    JSON_CTX_ARRAY_VALUE = 7,
    JSON_CTX_ARRAY_AFTER_VALUE = 8
} json_stream_ctx_state_t;

/// Mutable state retained by one in-memory pull parser.
typedef struct {
    rt_string input_owner;               ///< Retained source string.
    const char *input;                   ///< Borrowed bytes from @c input_owner.
    size_t len;                          ///< Source length in bytes.
    size_t pos;                          ///< Current source byte offset.
    rt_json_tok_type_t current_type;     ///< Most recently emitted token.
    char *str_buf;                       ///< Decoded key/string scratch bytes.
    size_t str_buf_len;                  ///< Decoded content length.
    size_t str_buf_cap;                  ///< Scratch allocation capacity.
    double num_value;                    ///< Most recently parsed numeric value.
    size_t num_start;                    ///< Source offset of latest number.
    size_t num_len;                      ///< Raw byte length of latest number.
    int8_t bool_value;                   ///< Most recently parsed Boolean value.
    int64_t depth;                       ///< Number of currently open containers.
    char *error_msg;                     ///< Heap-owned parse diagnostic.
    int8_t expect_key;                   ///< Whether the current object expects a key.
    int8_t in_object[MAX_DEPTH];         ///< Container-kind flags by depth.
    int8_t first_value[MAX_DEPTH];       ///< First-value markers by depth.
    uint8_t state[MAX_DEPTH];            ///< @ref json_stream_ctx_state_t by depth.
    int8_t top_value_seen;               ///< Whether the single root value began.
} rt_json_stream_impl;

/// @brief GC finalizer — release the input ref, scratch string buffer, and error string.
/// @details The streaming parser holds a reference to its input string
///          (`input_owner`) so the underlying bytes don't disappear
///          mid-iteration. Two heap-allocated scratches (`str_buf`
///          for value accumulation, `error_msg` for the most recent
///          error) are freed here. All pointers nulled afterwards
///          so a double finalize is safe.
/// @param obj Stream payload being finalized; null is ignored.
static void stream_finalizer(void *obj) {
    rt_json_stream_impl *s = (rt_json_stream_impl *)obj;
    if (s) {
        if (s->input_owner) {
            rt_string_unref(s->input_owner);
            s->input_owner = NULL;
        }
        free(s->str_buf);
        s->str_buf = NULL;
        free(s->error_msg);
        s->error_msg = NULL;
    }
}

/// @brief Advance the cursor past any RFC 8259 whitespace (`space`, `tab`, `\n`, `\r`).
/// @param s Mutable stream state.
static void skip_whitespace(rt_json_stream_impl *s) {
    while (s->pos < s->len) {
        char c = s->input[s->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            s->pos++;
        else
            break;
    }
}

/// @brief Skip whitespace and return the next non-whitespace byte (`\0` at EOF).
/// @details Combines whitespace skipping with peeking — most state-
///          machine transitions need both, so this saves a separate
///          call at every dispatch point.
/// @param s Mutable stream state whose whitespace is consumed.
/// @return Next non-whitespace byte without consuming it, or NUL at EOF.
static char peek(rt_json_stream_impl *s) {
    skip_whitespace(s);
    if (s->pos >= s->len)
        return '\0';
    return s->input[s->pos];
}

/// @brief Record a parse error: switch token type to ERROR and stash a message string.
/// @details Frees any previously stored message before strdup-style
///          duplicating the new one. On allocation failure for the
///          message copy, the type still flips to ERROR but the
///          message becomes NULL — so callers should not assume a
///          non-NULL `error_msg` whenever `current_type == ERROR`.
/// @param s Mutable stream to mark failed.
/// @param msg Borrowed NUL-terminated detail, or null for no stored message.
static void set_error(rt_json_stream_impl *s, const char *msg) {
    s->current_type = RT_JSON_TOK_ERROR;
    free(s->error_msg);
    s->error_msg = NULL;
    if (msg) {
        size_t len = strlen(msg);
        s->error_msg = (char *)malloc(len + 1);
        if (s->error_msg) {
            memcpy(s->error_msg, msg, len + 1);
        }
    }
}

/// @brief Reset the value-accumulator buffer to empty (without freeing capacity).
/// @param s Mutable stream whose decoded-string length is reset.
static void str_buf_clear(rt_json_stream_impl *s) {
    s->str_buf_len = 0;
}

/// @brief Append one byte to the value buffer, growing capacity when needed.
/// @details Initial allocation grows from 0 to ≥64; subsequent growth
///          doubles. Records an error and bails on allocation failure
///          rather than crashing.
/// @param s Mutable stream owning the scratch buffer.
/// @param c Byte to append.
static void str_buf_push(rt_json_stream_impl *s, char c) {
    if (s->str_buf_len + 1 >= s->str_buf_cap) {
        if (s->str_buf_cap > SIZE_MAX / 2) {
            set_error(s, "string too long");
            return;
        }
        size_t new_cap = s->str_buf_cap * 2;
        if (new_cap < 64)
            new_cap = 64;
        char *new_buf = (char *)realloc(s->str_buf, new_cap);
        if (!new_buf) {
            set_error(s, "out of memory");
            return;
        }
        s->str_buf = new_buf;
        s->str_buf_cap = new_cap;
    }
    s->str_buf[s->str_buf_len++] = c;
    s->str_buf[s->str_buf_len] = '\0';
}

/// @brief Parse exactly four hex digits into a 16-bit codepoint (used by `\uXXXX` escapes).
/// @details Accepts both upper and lower case hex. Returns 0 on EOF
///          mid-sequence or any non-hex byte; advances the cursor by
///          exactly 4 on success.
/// @param s Mutable stream positioned at the first hexadecimal byte.
/// @param out Non-null output receiving the decoded 16-bit value on success.
/// @return `1` after four valid digits, otherwise `0`.
static int parse_hex4(rt_json_stream_impl *s, uint32_t *out) {
    uint32_t val = 0;
    for (int i = 0; i < 4; i++) {
        if (s->pos >= s->len)
            return 0;
        char c = s->input[s->pos++];
        val <<= 4;
        if (c >= '0' && c <= '9')
            val |= (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f')
            val |= (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            val |= (uint32_t)(c - 'A' + 10);
        else
            return 0;
    }
    *out = val;
    return 1;
}

/// @brief Append `cp` (a Unicode scalar) into the value buffer as 1–4 UTF-8 bytes.
/// @details Standard UTF-8 layout:
///          - U+0000..U+007F → 1 byte (`0xxxxxxx`)
///          - U+0080..U+07FF → 2 bytes (`110xxxxx 10xxxxxx`)
///          - U+0800..U+FFFF → 3 bytes (`1110xxxx 10xxxxxx ×2`)
///          - U+10000..U+10FFFF → 4 bytes (`11110xxx 10xxxxxx ×3`)
///          Caller is expected to have already resolved surrogate
///          pairs to a single scalar.
/// @param s Mutable stream receiving UTF-8 bytes in its string scratch buffer.
/// @param cp Unicode scalar value to encode.
static void encode_utf8(rt_json_stream_impl *s, uint32_t cp) {
    if (cp < 0x80) {
        str_buf_push(s, (char)cp);
    } else if (cp < 0x800) {
        str_buf_push(s, (char)(0xC0 | (cp >> 6)));
        str_buf_push(s, (char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        str_buf_push(s, (char)(0xE0 | (cp >> 12)));
        str_buf_push(s, (char)(0x80 | ((cp >> 6) & 0x3F)));
        str_buf_push(s, (char)(0x80 | (cp & 0x3F)));
    } else {
        str_buf_push(s, (char)(0xF0 | (cp >> 18)));
        str_buf_push(s, (char)(0x80 | ((cp >> 12) & 0x3F)));
        str_buf_push(s, (char)(0x80 | ((cp >> 6) & 0x3F)));
        str_buf_push(s, (char)(0x80 | (cp & 0x3F)));
    }
}

/// @brief Consume a JSON string literal into the value buffer (handles all escapes).
/// @details Steps from the opening `"` through to the matching close
///          quote, decoding every escape sequence:
///          - `\"`, `\\`, `\/`, `\b`, `\f`, `\n`, `\r`, `\t` → literal byte.
///          - `\uXXXX` → Unicode scalar via `parse_hex4` + `encode_utf8`.
///          - `\uD8XX\uDCXX` → surrogate pair → single supplementary
///            scalar (Plane 1+).
///          On any malformed escape, unterminated string, or
///          incomplete surrogate pair, sets the error flag and returns 0.
/// @param s Mutable stream positioned at the opening quote.
/// @return `1` after consuming a valid closing quote, otherwise `0`.
static int parse_string_content(rt_json_stream_impl *s) {
    str_buf_clear(s);
    if (s->pos >= s->len || s->input[s->pos] != '"') {
        set_error(s, "expected '\"'");
        return 0;
    }
    s->pos++; /* skip opening quote */

    while (s->pos < s->len) {
        char c = s->input[s->pos++];
        if (c == '"')
            return 1;
        if (c == '\\') {
            if (s->pos >= s->len) {
                set_error(s, "unterminated escape");
                return 0;
            }
            char esc = s->input[s->pos++];
            switch (esc) {
                case '"':
                    str_buf_push(s, '"');
                    break;
                case '\\':
                    str_buf_push(s, '\\');
                    break;
                case '/':
                    str_buf_push(s, '/');
                    break;
                case 'b':
                    str_buf_push(s, '\b');
                    break;
                case 'f':
                    str_buf_push(s, '\f');
                    break;
                case 'n':
                    str_buf_push(s, '\n');
                    break;
                case 'r':
                    str_buf_push(s, '\r');
                    break;
                case 't':
                    str_buf_push(s, '\t');
                    break;
                case 'u': {
                    uint32_t cp = 0;
                    if (!parse_hex4(s, &cp)) {
                        set_error(s, "invalid unicode escape");
                        return 0;
                    }
                    /* Handle surrogate pairs */
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        if (s->pos + 1 < s->len && s->input[s->pos] == '\\' &&
                            s->input[s->pos + 1] == 'u') {
                            s->pos += 2;
                            uint32_t lo = 0;
                            if (!parse_hex4(s, &lo) || lo < 0xDC00 || lo > 0xDFFF) {
                                set_error(s, "invalid surrogate pair");
                                return 0;
                            }
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        } else {
                            set_error(s, "invalid surrogate pair");
                            return 0;
                        }
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        set_error(s, "invalid surrogate pair");
                        return 0;
                    }
                    encode_utf8(s, cp);
                    break;
                }
                default:
                    set_error(s, "invalid escape character");
                    return 0;
            }
        } else {
            if ((unsigned char)c < 0x20) {
                set_error(s, "control character in string");
                return 0;
            }
            if ((unsigned char)c >= 0x80) {
                // Match Json.Parse/IsValid: raw non-ASCII bytes must form
                // valid UTF-8 (VDOC-034).
                size_t extra = 0;
                if (!json_raw_utf8_sequence_valid(
                        (unsigned char)c, s->input + s->pos, s->len - s->pos, &extra)) {
                    set_error(s, "invalid UTF-8 sequence in string");
                    return 0;
                }
                str_buf_push(s, c);
                for (size_t k = 0; k < extra; k++)
                    str_buf_push(s, s->input[s->pos++]);
                continue;
            }
            str_buf_push(s, c);
        }
    }
    set_error(s, "unterminated string");
    return 0;
}

/// @brief Parse a JSON number into `s->num_value`, returning 1 on success.
/// @details Accepts the JSON grammar: optional `-`, integer part
///          (`0` or `1-9` followed by digits), optional fraction
///          (`. digits`), optional exponent (`[eE] [+-]? digits`).
///          The matched span is then handed to the runtime C-locale double
///          parser for conversion. Sets an error and returns 0 on
///          malformed input, including leading zeroes, missing fraction digits,
///          and missing exponent digits.
/// @param s Mutable stream positioned at the number's first byte.
/// @return `1` after storing the finite double and raw span, otherwise `0`.
static int parse_number(rt_json_stream_impl *s) {
    size_t start = s->pos;
    if (s->pos < s->len && s->input[s->pos] == '-')
        s->pos++;
    if (s->pos >= s->len || !isdigit((unsigned char)s->input[s->pos])) {
        set_error(s, "invalid number");
        return 0;
    }
    if (s->input[s->pos] == '0') {
        s->pos++;
        if (s->pos < s->len && isdigit((unsigned char)s->input[s->pos])) {
            set_error(s, "invalid number");
            return 0;
        }
    } else {
        while (s->pos < s->len && isdigit((unsigned char)s->input[s->pos]))
            s->pos++;
    }
    if (s->pos < s->len && s->input[s->pos] == '.') {
        s->pos++;
        if (s->pos >= s->len || !isdigit((unsigned char)s->input[s->pos])) {
            set_error(s, "invalid number");
            return 0;
        }
        while (s->pos < s->len && isdigit((unsigned char)s->input[s->pos]))
            s->pos++;
    }
    if (s->pos < s->len && (s->input[s->pos] == 'e' || s->input[s->pos] == 'E')) {
        s->pos++;
        if (s->pos < s->len && (s->input[s->pos] == '+' || s->input[s->pos] == '-'))
            s->pos++;
        if (s->pos >= s->len || !isdigit((unsigned char)s->input[s->pos])) {
            set_error(s, "invalid number");
            return 0;
        }
        while (s->pos < s->len && isdigit((unsigned char)s->input[s->pos]))
            s->pos++;
    }

    /* Copy number text and parse */
    size_t nlen = s->pos - start;
    if (nlen == 0 || nlen == SIZE_MAX) {
        set_error(s, "invalid number");
        return 0;
    }
    char *buf = (char *)malloc(nlen + 1);
    if (!buf) {
        set_error(s, "out of memory");
        return 0;
    }
    memcpy(buf, s->input + start, nlen);
    buf[nlen] = '\0';
    if (rt_parse_double(buf, &s->num_value) != (int32_t)Err_None || !isfinite(s->num_value)) {
        free(buf);
        set_error(s, "invalid number");
        return 0;
    }
    s->num_start = start;
    s->num_len = nlen;
    free(buf);
    return 1;
}

/// @brief Match a literal byte sequence (`true`, `false`, `null`) at the cursor and advance.
/// @details Returns 1 and steps past the match on success, 0 (cursor
///          unchanged) on EOF before completion or mismatch. The
///          length is passed explicitly so the call site doesn't pay
///          for `strlen` at every literal check.
/// @param s Mutable stream positioned at a potential literal.
/// @param lit Borrowed literal bytes to compare.
/// @param len Number of bytes in @p lit.
/// @return `1` on a complete match, otherwise `0`.
static int match_literal(rt_json_stream_impl *s, const char *lit, size_t len) {
    if (s->pos + len > s->len)
        return 0;
    if (memcmp(s->input + s->pos, lit, len) != 0)
        return 0;
    s->pos += len;
    return 1;
}

//=============================================================================
// Public API
//=============================================================================

/// @brief Construct a streaming JSON parser positioned at the start of `json`. Returns a
/// reference-counted handle; advance through tokens via `_next` and read values via the type-specific
/// `_string_value` / `_number_value` / `_bool_value` accessors.
/// @details Retains the complete source string because this parser is pull-based
///          but not incrementally fed. Null input constructs an empty stream
///          whose first `Next` call returns an error token.
/// @param json Borrowed source string retained for the stream lifetime.
/// @return Newly allocated opaque stream object owned by the caller, or null
///         after an allocation trap.
void *rt_json_stream_new(rt_string json) {
    rt_json_stream_impl *s =
        (rt_json_stream_impl *)rt_obj_new_i64(0, (int64_t)sizeof(rt_json_stream_impl));
    if (!s) {
        rt_trap("JsonStream: memory allocation failed");
        return NULL;
    }
    s->input_owner = json ? rt_string_ref(json) : NULL;
    const char *cstr = json ? rt_string_cstr(json) : "";
    s->input = cstr ? cstr : "";
    s->len = json ? (size_t)rt_str_len(json) : 0;
    s->pos = 0;
    s->current_type = RT_JSON_TOK_NONE;
    s->str_buf = NULL;
    s->str_buf_len = 0;
    s->str_buf_cap = 0;
    s->num_value = 0.0;
    s->bool_value = 0;
    s->depth = 0;
    s->error_msg = NULL;
    s->expect_key = 0;
    memset(s->in_object, 0, sizeof(s->in_object));
    memset(s->first_value, 0, sizeof(s->first_value));
    memset(s->state, 0, sizeof(s->state));
    s->top_value_seen = 0;

    rt_obj_set_finalizer(s, stream_finalizer);
    return s;
}

/// @brief Advance to the next token. Returns the token type (RT_JSON_TOK_* enum). Use the
/// type-specific accessors below to read the value once positioned. Returns RT_JSON_TOK_END
/// at end of input or RT_JSON_TOK_ERROR on parse failure (call `_error` for diagnostic).
/// @details Emits exactly one structural, key, or scalar token per call. ERROR
///          and END states are terminal and are returned idempotently.
///          Malformed syntax, invalid UTF-8, non-finite numbers, trailing
///          content, and more than 255 open containers produce ERROR without a
///          syntax trap.
/// @param parser Borrowed mutable stream handle; null returns ERROR.
/// @return One `RT_JSON_TOK_*` value represented as `int64_t`.
int64_t rt_json_stream_next(void *parser) {
    if (!parser)
        return RT_JSON_TOK_ERROR;

    rt_json_stream_impl *s = (rt_json_stream_impl *)parser;
    if (s->current_type == RT_JSON_TOK_ERROR || s->current_type == RT_JSON_TOK_END)
        return s->current_type;

    char c = peek(s);

    if (s->depth == 0 && s->top_value_seen) {
        if (c == '\0') {
            s->current_type = RT_JSON_TOK_END;
            return RT_JSON_TOK_END;
        }
        set_error(s, "unexpected content after JSON value");
        return RT_JSON_TOK_ERROR;
    }

    if (s->depth == 0 && c == '\0') {
        set_error(s, "empty JSON input");
        return RT_JSON_TOK_ERROR;
    }

    while (s->depth > 0) {
        uint8_t state = s->state[s->depth];
        c = peek(s);
        if (c == '\0') {
            set_error(s, "unexpected end of JSON input");
            return RT_JSON_TOK_ERROR;
        }

        if (s->in_object[s->depth]) {
            if (state == JSON_CTX_OBJECT_KEY_OR_END) {
                if (c == '}') {
                    s->pos++;
                    s->in_object[s->depth] = 0;
                    s->state[s->depth] = 0;
                    s->depth--;
                    s->expect_key = (s->depth > 0 && s->in_object[s->depth]) ? 1 : 0;
                    s->current_type = RT_JSON_TOK_OBJECT_END;
                    return RT_JSON_TOK_OBJECT_END;
                }
                if (c != '"') {
                    set_error(s, "expected object key");
                    return RT_JSON_TOK_ERROR;
                }
                if (!parse_string_content(s))
                    return RT_JSON_TOK_ERROR;
                s->state[s->depth] = JSON_CTX_OBJECT_COLON;
                s->expect_key = 0;
                s->current_type = RT_JSON_TOK_KEY;
                return RT_JSON_TOK_KEY;
            }

            if (state == JSON_CTX_OBJECT_KEY) {
                if (c != '"') {
                    set_error(s, "expected object key");
                    return RT_JSON_TOK_ERROR;
                }
                if (!parse_string_content(s))
                    return RT_JSON_TOK_ERROR;
                s->state[s->depth] = JSON_CTX_OBJECT_COLON;
                s->expect_key = 0;
                s->current_type = RT_JSON_TOK_KEY;
                return RT_JSON_TOK_KEY;
            }

            if (state == JSON_CTX_OBJECT_COLON) {
                if (c != ':') {
                    set_error(s, "expected ':' after object key");
                    return RT_JSON_TOK_ERROR;
                }
                s->pos++;
                s->state[s->depth] = JSON_CTX_OBJECT_VALUE;
                continue;
            }

            if (state == JSON_CTX_OBJECT_AFTER_VALUE) {
                if (c == ',') {
                    s->pos++;
                    s->state[s->depth] = JSON_CTX_OBJECT_KEY;
                    continue;
                }
                if (c == '}') {
                    s->pos++;
                    s->in_object[s->depth] = 0;
                    s->state[s->depth] = 0;
                    s->depth--;
                    s->expect_key = (s->depth > 0 && s->in_object[s->depth]) ? 1 : 0;
                    s->current_type = RT_JSON_TOK_OBJECT_END;
                    return RT_JSON_TOK_OBJECT_END;
                }
                set_error(s, "expected ',' or '}' after object value");
                return RT_JSON_TOK_ERROR;
            }

            if (state != JSON_CTX_OBJECT_VALUE) {
                set_error(s, "invalid object parser state");
                return RT_JSON_TOK_ERROR;
            }
            break;
        }

        if (state == JSON_CTX_ARRAY_VALUE_OR_END) {
            if (c == ']') {
                s->pos++;
                s->state[s->depth] = 0;
                s->depth--;
                s->expect_key = (s->depth > 0 && s->in_object[s->depth]) ? 1 : 0;
                s->current_type = RT_JSON_TOK_ARRAY_END;
                return RT_JSON_TOK_ARRAY_END;
            }
            break;
        }

        if (state == JSON_CTX_ARRAY_AFTER_VALUE) {
            if (c == ',') {
                s->pos++;
                s->state[s->depth] = JSON_CTX_ARRAY_VALUE;
                continue;
            }
            if (c == ']') {
                s->pos++;
                s->state[s->depth] = 0;
                s->depth--;
                s->expect_key = (s->depth > 0 && s->in_object[s->depth]) ? 1 : 0;
                s->current_type = RT_JSON_TOK_ARRAY_END;
                return RT_JSON_TOK_ARRAY_END;
            }
            set_error(s, "expected ',' or ']' after array value");
            return RT_JSON_TOK_ERROR;
        }

        if (state != JSON_CTX_ARRAY_VALUE) {
            set_error(s, "invalid array parser state");
            return RT_JSON_TOK_ERROR;
        }
        break;
    }

    c = peek(s);
    if (c == '\0') {
        set_error(s, "unexpected end of JSON input");
        return RT_JSON_TOK_ERROR;
    }

    if (s->depth == 0) {
        s->top_value_seen = 1;
    } else if (s->in_object[s->depth]) {
        s->state[s->depth] = JSON_CTX_OBJECT_AFTER_VALUE;
        s->expect_key = 1;
    } else {
        s->state[s->depth] = JSON_CTX_ARRAY_AFTER_VALUE;
    }

    switch (c) {
        case '{':
            if (s->depth + 1 >= MAX_DEPTH) {
                set_error(s, "maximum JSON depth exceeded");
                return RT_JSON_TOK_ERROR;
            }
            s->pos++;
            s->depth++;
            s->in_object[s->depth] = 1;
            s->first_value[s->depth] = 1;
            s->state[s->depth] = JSON_CTX_OBJECT_KEY_OR_END;
            s->expect_key = 1;
            s->current_type = RT_JSON_TOK_OBJECT_START;
            return RT_JSON_TOK_OBJECT_START;

        case '}':
            set_error(s, "unexpected '}'");
            return RT_JSON_TOK_ERROR;

        case '[':
            if (s->depth + 1 >= MAX_DEPTH) {
                set_error(s, "maximum JSON depth exceeded");
                return RT_JSON_TOK_ERROR;
            }
            s->pos++;
            s->depth++;
            s->in_object[s->depth] = 0;
            s->first_value[s->depth] = 1;
            s->state[s->depth] = JSON_CTX_ARRAY_VALUE_OR_END;
            s->expect_key = 0;
            s->current_type = RT_JSON_TOK_ARRAY_START;
            return RT_JSON_TOK_ARRAY_START;

        case ']':
            set_error(s, "unexpected ']'");
            return RT_JSON_TOK_ERROR;

        case '"':
            if (!parse_string_content(s))
                return RT_JSON_TOK_ERROR;
            s->current_type = RT_JSON_TOK_STRING;
            return RT_JSON_TOK_STRING;

        case 't':
            if (match_literal(s, "true", 4)) {
                s->bool_value = 1;
                s->current_type = RT_JSON_TOK_BOOL;
                return RT_JSON_TOK_BOOL;
            }
            set_error(s, "invalid token");
            return RT_JSON_TOK_ERROR;

        case 'f':
            if (match_literal(s, "false", 5)) {
                s->bool_value = 0;
                s->current_type = RT_JSON_TOK_BOOL;
                return RT_JSON_TOK_BOOL;
            }
            set_error(s, "invalid token");
            return RT_JSON_TOK_ERROR;

        case 'n':
            if (match_literal(s, "null", 4)) {
                s->current_type = RT_JSON_TOK_NULL;
                return RT_JSON_TOK_NULL;
            }
            set_error(s, "invalid token");
            return RT_JSON_TOK_ERROR;

        default:
            if (c == '-' || isdigit((unsigned char)c)) {
                if (parse_number(s)) {
                    s->current_type = RT_JSON_TOK_NUMBER;
                    return RT_JSON_TOK_NUMBER;
                }
                return RT_JSON_TOK_ERROR;
            }
            set_error(s, "unexpected character");
            return RT_JSON_TOK_ERROR;
    }
}

/// @brief Advance to the next JSON token and return it as a Result.
///
/// Normal tokens, including END, are returned as `Ok(tokenType)`. Malformed
/// input returns `Err(message)` using the stream's current diagnostic text.
///
/// @param parser Parser handle.
/// @return Owned `Zanna.Result` carrying the token type or an error string.
void *rt_json_stream_next_result(void *parser) {
    int64_t token = rt_json_stream_next(parser);
    if (token != RT_JSON_TOK_ERROR)
        return rt_result_ok_i64(token);

    rt_string err = rt_json_stream_error(parser);
    if (!err || rt_str_len(err) == 0)
        return rt_result_err_str(rt_const_cstr("JsonStream.NextResult: parse error"));
    return rt_result_err_str(err);
}

/// @brief Return the type of the most-recently-consumed token (RT_JSON_TOK_* enum).
/// @param parser Borrowed stream handle.
/// @return Current token type, or ERROR for a null handle.
int64_t rt_json_stream_token_type(void *parser) {
    if (!parser)
        return RT_JSON_TOK_ERROR;
    return ((rt_json_stream_impl *)parser)->current_type;
}

/// @brief Copy the decoded bytes held for the most recent STRING or KEY token.
/// @details The accessor does not independently verify the current token; call
///          it only while positioned on STRING or KEY. Empty decoded values and
///          null handles both return an owned empty string.
/// @param parser Borrowed stream handle.
/// @return Newly allocated decoded string owned by the caller.
rt_string rt_json_stream_string_value(void *parser) {
    if (!parser)
        return rt_const_cstr("");
    rt_json_stream_impl *s = (rt_json_stream_impl *)parser;
    if (s->str_buf && s->str_buf_len > 0)
        return rt_string_from_bytes(s->str_buf, s->str_buf_len);
    return rt_const_cstr("");
}

/// @brief Read the finite double stored for the most recent NUMBER token.
/// @details The accessor does not verify the current token; before the first
///          number it returns the initialized value zero.
/// @param parser Borrowed stream handle.
/// @return Most recently parsed double, or `0.0` for a null handle.
double rt_json_stream_number_value(void *parser) {
    if (!parser)
        return 0.0;
    return ((rt_json_stream_impl *)parser)->num_value;
}

/// @brief Copy the exact source lexeme for the current NUMBER token.
/// @details Unlike `rt_json_stream_number_value`, this accessor verifies the
///          current token and validates the retained source span.
/// @param parser Borrowed stream handle.
/// @return Newly allocated raw number text, or an owned empty string when the
///         handle/token/span is unsuitable.
rt_string rt_json_stream_number_text(void *parser) {
    if (!parser)
        return rt_const_cstr("");
    rt_json_stream_impl *s = (rt_json_stream_impl *)parser;
    if (s->current_type != RT_JSON_TOK_NUMBER || s->num_start > s->len ||
        s->num_len > s->len - s->num_start)
        return rt_const_cstr("");
    return rt_string_from_bytes(s->input + s->num_start, s->num_len);
}

/// @brief Read the value stored for the most recent BOOL token.
/// @details The accessor does not verify the current token; before the first
///          Boolean it returns the initialized false value.
/// @param parser Borrowed stream handle.
/// @return `1` for the latest true token, otherwise `0`.
int8_t rt_json_stream_bool_value(void *parser) {
    if (!parser)
        return 0;
    return ((rt_json_stream_impl *)parser)->bool_value;
}

/// @brief Current nesting depth (number of open `[`/`{` minus close `]`/`}`). 0 = top level.
/// @param parser Borrowed stream handle.
/// @return Number of currently open containers, or zero for a null handle.
int64_t rt_json_stream_depth(void *parser) {
    if (!parser)
        return 0;
    return ((rt_json_stream_impl *)parser)->depth;
}

/// @brief Skip past the current value (including nested arrays/objects). Useful for selectively
/// parsing only certain fields and ignoring large irrelevant subtrees in big JSON documents.
/// @details When positioned on a container-start token, repeatedly advances
///          through its matching end token. Primitive tokens are already fully
///          consumed, so skipping them is a no-op. Errors and end-of-input stop
///          the loop.
/// @param parser Borrowed mutable stream handle; null is ignored.
void rt_json_stream_skip(void *parser) {
    if (!parser)
        return;
    rt_json_stream_impl *s = (rt_json_stream_impl *)parser;

    /* If current token is a container start, skip until matching end */
    if (s->current_type == RT_JSON_TOK_OBJECT_START || s->current_type == RT_JSON_TOK_ARRAY_START) {
        int64_t target_depth = s->depth - 1;
        while (s->current_type != RT_JSON_TOK_END && s->current_type != RT_JSON_TOK_ERROR) {
            rt_json_stream_next(parser);
            if ((s->current_type == RT_JSON_TOK_OBJECT_END ||
                 s->current_type == RT_JSON_TOK_ARRAY_END) &&
                s->depth == target_depth)
                return;
        }
    }
    /* Primitive values are already consumed */
}

/// @brief Check whether unprocessed source or an open container remains.
/// @details Returns false for null, terminal END/ERROR, and exhausted top-level
///          input. Trailing non-whitespace source counts as remaining work even
///          when the next call will classify it as ERROR.
/// @param parser Borrowed stream handle.
/// @return `1` when another call must process input/state, otherwise `0`.
int8_t rt_json_stream_has_next(void *parser) {
    if (!parser)
        return 0;
    rt_json_stream_impl *s = (rt_json_stream_impl *)parser;
    if (s->current_type == RT_JSON_TOK_END || s->current_type == RT_JSON_TOK_ERROR)
        return 0;
    if (s->depth > 0)
        return 1;
    char c = peek(s);
    if (!s->top_value_seen)
        return c != '\0' ? 1 : 0;
    return c != '\0' ? 1 : 0;
}

/// @brief Return the diagnostic message for the most recent parse error (empty if none).
/// @param parser Borrowed stream handle.
/// @return Newly allocated copy of the error detail, or an owned empty string.
rt_string rt_json_stream_error(void *parser) {
    if (!parser)
        return rt_const_cstr("");
    rt_json_stream_impl *s = (rt_json_stream_impl *)parser;
    if (s->error_msg)
        return rt_const_cstr(s->error_msg);
    return rt_const_cstr("");
}
