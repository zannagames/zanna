//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/text/rt_json_stream.h
// Purpose: SAX-style pull parser over one complete in-memory JSON string. It
// avoids building a value tree (lower allocation overhead than Json.Parse) but
// is NOT incremental: the sole constructor retains the whole input string and
// there is no feed/resume API for network or file chunks.
//
// Key invariants:
//   - Stateful, pull-based: call rt_json_stream_next to advance to the next token.
//   - Token types: object_start, object_end, array_start, array_end, key, string, number, bool,
//   null.
//   - The stream retains the input and allocates scratch storage for decoded strings.
//   - Malformed JSON returns a terminal error token rather than a syntax trap;
//     runtime allocation failures may still trap.
//   - At most 255 containers may be simultaneously open.
//
// Ownership/Lifetime:
//   - Stream objects are reference-counted; the caller owns the returned handle.
//   - Input string is retained for the lifetime of the stream.
//   - Accessor strings and Result objects are fresh references owned by callers.
//
// Links: src/runtime/text/rt_json_stream.c (implementation),
//        src/runtime/text/rt_json_internal.h (shared UTF-8 validation),
//        src/runtime/core/rt_string.h (retained input and accessor strings)
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_json_stream.h
 * @brief Declares the managed pull-based JSON token stream API.
 * @details A stream retains one complete input String and exposes structural,
 *          key, String, numeric, Boolean, null, error, and end tokens.
 *          Accessors return caller-owned scalar data or Results while malformed
 *          syntax remains a terminal token state rather than a syntax trap.
 */

#pragma once

#include "rt_string.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Token types emitted by the streaming parser.
typedef enum {
    RT_JSON_TOK_NONE = 0,         ///< Initial state before the first pull.
    RT_JSON_TOK_OBJECT_START = 1, ///< Opening `{`.
    RT_JSON_TOK_OBJECT_END = 2,   ///< Closing `}`.
    RT_JSON_TOK_ARRAY_START = 3,  ///< Opening `[`.
    RT_JSON_TOK_ARRAY_END = 4,    ///< Closing `]`.
    RT_JSON_TOK_KEY = 5,          ///< Decoded object member name.
    RT_JSON_TOK_STRING = 6,       ///< Decoded string value.
    RT_JSON_TOK_NUMBER = 7,       ///< Finite double plus retained raw lexeme.
    RT_JSON_TOK_BOOL = 8,         ///< JSON `true` or `false`.
    RT_JSON_TOK_NULL = 9,         ///< JSON `null`.
    RT_JSON_TOK_ERROR = 10,       ///< Terminal parse-error state.
    RT_JSON_TOK_END = 11          ///< Terminal end after one complete value.
} rt_json_tok_type_t;

/// @brief Create a new streaming JSON parser from a string.
/// @details Retains the entire source; this API does not accept incremental
///          chunks. Null input creates an empty stream that reports ERROR on
///          its first pull.
/// @param json Borrowed source string retained by the new stream.
/// @return Newly allocated opaque stream handle owned by the caller, or null
///         after an allocation trap.
void *rt_json_stream_new(rt_string json);

/// @brief Advance to the next token.
/// @details Emits one token per call. END and ERROR are terminal and returned
///          again by subsequent calls.
/// @param parser Borrowed mutable stream handle; null returns ERROR.
/// @return Token type of the next token as an `int64_t`.
int64_t rt_json_stream_next(void *parser);

/// @brief Advance to the next token and return a Zanna.Result.
/// @details Returns `Ok(tokenType)` for normal tokens and end-of-input, or
///          `Err(message)` when malformed JSON produces an error token. This
///          keeps tokenizer failure attached to the returned value.
/// @param parser Borrowed mutable stream handle.
/// @return Newly allocated Zanna.Result containing the token type or error,
///         owned by the caller.
void *rt_json_stream_next_result(void *parser);

/// @brief Get the current token type.
/// @param parser Borrowed stream handle.
/// @return Current token type, or ERROR for a null handle.
int64_t rt_json_stream_token_type(void *parser);

/// @brief Get the current token's string value (for key, string tokens).
/// @details Copies the current decoded scratch buffer. Call only while
///          positioned on KEY or STRING; other tokens may expose the previous
///          decoded value.
/// @param parser Borrowed stream handle.
/// @return Newly allocated decoded string, or an owned empty string for null or
///         empty content.
rt_string rt_json_stream_string_value(void *parser);

/// @brief Get the current token's numeric value (for number tokens).
/// @details Does not verify the current token and retains the last parsed
///          numeric value.
/// @param parser Borrowed stream handle.
/// @return Most recently parsed finite double, or `0.0` for a null handle or
///         before the first number.
double rt_json_stream_number_value(void *parser);

/// @brief Get the current token's raw number lexeme (for number tokens).
/// @param parser Borrowed stream handle.
/// @return Newly allocated exact source lexeme, or an owned empty string when
///         not positioned on a valid NUMBER span.
rt_string rt_json_stream_number_text(void *parser);

/// @brief Get the current token's boolean value (for bool tokens).
/// @details Does not verify the current token and retains the last parsed
///          Boolean value.
/// @param parser Borrowed stream handle.
/// @return `1` for the latest true token, otherwise `0`.
int8_t rt_json_stream_bool_value(void *parser);

/// @brief Get the current nesting depth.
/// @param parser Borrowed stream handle.
/// @return Number of currently open containers, or zero for a null handle.
int64_t rt_json_stream_depth(void *parser);

/// @brief Skip the current value (object, array, or primitive).
/// @details Container-start tokens are consumed through their matching end;
///          primitive values are already consumed and require no work.
/// @param parser Borrowed mutable stream handle; null is ignored.
void rt_json_stream_skip(void *parser);

/// @brief Check if the parser has more tokens.
/// @details Trailing invalid content counts as remaining work because the next
///          pull must emit ERROR.
/// @param parser Borrowed stream handle.
/// @return `1` when source/container state remains, otherwise `0`.
int8_t rt_json_stream_has_next(void *parser);

/// @brief Get error message if current token is ERROR.
/// @param parser Borrowed stream handle.
/// @return Newly allocated error-description copy, or an owned empty string.
rt_string rt_json_stream_error(void *parser);

#ifdef __cplusplus
}
#endif
