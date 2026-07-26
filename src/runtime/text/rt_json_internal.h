//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE in the project root for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/text/rt_json_internal.h
// Purpose: Shared parser state and cursor primitives for the JSON runtime.
//          Used by the parsing (rt_json_parse.c) and validation
//          (rt_json_validate.c) translation units, which both walk a flat
//          input buffer through the same json_parser cursor.
//
// Key invariants:
//   - Cursor primitives are static inline so each translation unit gets an
//     internal-linkage copy; no external symbol is exported (avoids link-time
//     collisions with the identically named static helpers in rt_yaml.c /
//     rt_xml.c) and no source is duplicated.
//   - JSON_MAX_DEPTH bounds nesting for both parsing and formatting (S-16).
//
// Ownership/Lifetime:
//   - json_parser borrows the input buffer; it owns no heap state.
//
// Links: src/runtime/text/rt_json.h (public API),
//        src/runtime/text/rt_json_parse.c (allocating parser),
//        src/runtime/text/rt_json_format.c (depth-limited serializer),
//        src/runtime/text/rt_json_validate.c (non-allocating validator)
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_json_internal.h
 * @brief Defines shared JSON cursor, diagnostic, UTF-8, and depth helpers.
 * @details Static inline primitives initialize and advance a borrowed flat
 *          input range, track line and column diagnostics, skip JSON
 *          whitespace, validate UTF-8 scalar encodings, and enforce the common
 *          nesting bound used by parsing, validation, and formatting.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

//=============================================================================
// Parser limits
//=============================================================================

/// Maximum nested container depth accepted by JSON parsers and formatters.
/// @details This denial-of-service guard bounds recursive C stack use and the
///          serializer's active-container stack.
#define JSON_MAX_DEPTH 200

//=============================================================================
// Parser State
//=============================================================================

/// Shared cursor and diagnostic state for JSON parsing and validation.
typedef struct {
    const char *input;       ///< Borrowed start of the JSON byte range.
    size_t len;              ///< Total number of input bytes.
    size_t pos;              ///< Current byte offset.
    int depth;               ///< Current recursive container depth.
    int depth_exceeded;      ///< Set when `JSON_MAX_DEPTH` is reached.
    int trap_errors;         ///< Whether parser diagnostics call `rt_trap`.
    int has_error;           ///< Sticky parse-error flag.
    char error_message[160]; ///< NUL-terminated diagnostic detail.
    int64_t error_line;      ///< One-based line, or zero when unavailable.
    int64_t error_column;    ///< One-based column, or zero when unavailable.
} json_parser;

// ---------------------------------------------------------------------------
// Parser primitives over a flat byte buffer. Tracks nesting depth so callers
// can fail gracefully on adversarial inputs (S-16) instead of blowing the C
// stack via recursion.
// ---------------------------------------------------------------------------

/// @brief Initialise a parser onto fresh input. depth=0, no errors.
/// @details The input range is borrowed, diagnostics are cleared, and trapping
///          is enabled by default.
/// @param p Writable parser state.
/// @param input Borrowed input byte range.
/// @param len Number of readable bytes at @p input.
static inline void parser_init(json_parser *p, const char *input, size_t len) {
    p->input = input;
    p->len = len;
    p->pos = 0;
    p->depth = 0;
    p->depth_exceeded = 0;
    p->trap_errors = 1;
    p->has_error = 0;
    p->error_message[0] = '\0';
    p->error_line = 0;
    p->error_column = 0;
}

/// @brief True if the cursor has consumed all input bytes.
/// @details An error state is treated as EOF so cursor loops terminate during
///          unwinding.
/// @param p Borrowed parser state.
/// @return `true` when an error is set or the cursor reached the input end.
static inline bool parser_eof(json_parser *p) {
    if (p->has_error)
        return true;
    return p->pos >= p->len;
}

/// @brief Look at the byte under the cursor; returns `\0` at EOF.
/// @param p Borrowed parser state.
/// @return Current byte without advancing, or NUL at EOF/error.
static inline char parser_peek(json_parser *p) {
    if (p->has_error)
        return '\0';
    if (p->pos >= p->len)
        return '\0';
    return p->input[p->pos];
}

/// @brief Consume and return the byte under the cursor.
/// @param p Mutable parser state.
/// @return Current byte while advancing one position, or NUL at EOF/error.
static inline char parser_consume(json_parser *p) {
    if (p->has_error)
        return '\0';
    if (p->pos >= p->len)
        return '\0';
    return p->input[p->pos++];
}

/// @brief Validate one raw (unescaped) UTF-8 sequence beginning at `lead`.
/// @details Shared by the parser, validator, and tokenizer so IsValid/Parse
///          agree byte-for-byte on what constitutes interoperable JSON text.
///          Overlong encodings, surrogate codepoints, and values above
///          U+10FFFF are rejected.
/// @param lead First byte already consumed.
/// @param rest Remaining unconsumed input bytes.
/// @param rest_len Number of bytes available in @p rest.
/// @param extra_out Receives the number of continuation bytes to consume.
/// @return 1 when the sequence is valid, 0 otherwise.
static inline int json_raw_utf8_sequence_valid(unsigned char lead,
                                               const char *rest,
                                               size_t rest_len,
                                               size_t *extra_out) {
    size_t extra = 0;
    uint32_t cp = 0;
    if (extra_out)
        *extra_out = 0;
    if (lead < 0x80)
        return 1;
    if (lead >= 0xC2 && lead <= 0xDF) {
        extra = 1;
        cp = lead & 0x1Fu;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
        extra = 2;
        cp = lead & 0x0Fu;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        extra = 3;
        cp = lead & 0x07u;
    } else {
        return 0;
    }
    if (rest_len < extra)
        return 0;
    for (size_t i = 0; i < extra; i++) {
        unsigned char ch = (unsigned char)rest[i];
        if ((ch & 0xC0u) != 0x80u)
            return 0;
        cp = (cp << 6) | (uint32_t)(ch & 0x3Fu);
    }
    if ((extra == 2 && cp < 0x800u) || (extra == 3 && cp < 0x10000u))
        return 0;
    if (cp >= 0xD800u && cp <= 0xDFFFu)
        return 0;
    if (cp > 0x10FFFFu)
        return 0;
    if (extra_out)
        *extra_out = extra;
    return 1;
}

/// @brief Skip JSON whitespace bytes defined by RFC 8259 section 2.
/// @details Advances over space, horizontal tab, line feed, and carriage
///          return, stopping at the first other byte or parser error.
/// @param p Mutable parser state.
static inline void parser_skip_whitespace(json_parser *p) {
    while (!parser_eof(p)) {
        char c = parser_peek(p);
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            p->pos++;
        else
            break;
    }
}
