//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/text/rt_scanner.h
// Purpose: Declares a byte-oriented cursor scanner with lookahead, conditional
//          consumption, skipping, token extraction, and reusable predicates.
//
// Key invariants:
//   - Scanner maintains a position within the source string.
//   - rt_scanner_peek returns the current character without advancing.
//   - Match operations do not consume; Accept operations consume on success.
//   - rt_scanner_is_end returns 1 when all source bytes have been consumed.
//   - Null scanner arguments return neutral values and do not trap.
//
// Ownership/Lifetime:
//   - Scanner objects are runtime-managed and finalized with their object.
//   - The source string is retained by the scanner and released at finalization.
//
// Links: src/runtime/text/rt_scanner.c (implementation), src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_scanner.h
 * @brief Declares cursor-based byte scanning over retained runtime Strings.
 * @details The API exposes position and remaining state, byte or String
 *          lookahead, conditional matches and accepts, skipping, and common
 *          token readers. Scanner handles own their source reference, while
 *          extracted token Strings are caller-owned.
 */

#pragma once

#include "rt_string.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//=========================================================================
// Scanner Creation
//=========================================================================

/// @brief Create a new scanner for the given string.
/// @details Retains @p source until object finalization; null creates an empty scanner.
/// @param source Borrowed runtime string to scan.
/// @return Newly allocated runtime-managed opaque Scanner object.
void *rt_scanner_new(rt_string source);

//=========================================================================
// Position and State
//=========================================================================

/// @brief Get the current position in the string.
/// @param obj Borrowed Scanner object; null reports zero.
/// @return Current byte position (0-indexed).
int64_t rt_scanner_pos(void *obj);

/// @brief Set the current position.
/// @param obj Borrowed Scanner object; null is ignored.
/// @param pos New byte position, clamped to `[0, length]`.
void rt_scanner_set_pos(void *obj, int64_t pos);

/// @brief Check if at end of string.
/// @param obj Borrowed Scanner object; null is treated as ended.
/// @return `1` if at end, otherwise `0`.
int8_t rt_scanner_is_end(void *obj);

/// @brief Get the remaining length to scan.
/// @param obj Borrowed Scanner object; null reports zero.
/// @return Number of source bytes remaining.
int64_t rt_scanner_remaining(void *obj);

/// @brief Get the total length of the source string.
/// @param obj Borrowed Scanner object; null reports zero.
/// @return Total source length in bytes.
int64_t rt_scanner_len(void *obj);

/// @brief Reset scanner to beginning.
/// @param obj Borrowed Scanner object; null is ignored.
void rt_scanner_reset(void *obj);

//=========================================================================
// Peeking (without advancing)
//=========================================================================

/// @brief Peek at the current character.
/// @param obj Borrowed Scanner object.
/// @return Current unsigned byte, or `-1` if null or at end.
int64_t rt_scanner_peek(void *obj);

/// @brief Peek at character at offset from current position.
/// @param obj Borrowed Scanner object.
/// @param offset Signed byte offset from the current position.
/// @return Unsigned byte at the relative position, or `-1` if out of bounds.
int64_t rt_scanner_peek_at(void *obj, int64_t offset);

/// @brief Peek at the next n characters as a string.
/// @param obj Borrowed Scanner object.
/// @param n Maximum number of bytes to peek.
/// @return Newly allocated preview clamped to the remainder, or an empty sentinel.
rt_string rt_scanner_peek_str(void *obj, int64_t n);

//=========================================================================
// Reading (with advancing)
//=========================================================================

/// @brief Read and advance past the current character.
/// @param obj Borrowed Scanner object.
/// @return Consumed unsigned byte, or `-1` if null or at end.
int64_t rt_scanner_read(void *obj);

/// @brief Read the next n characters and advance.
/// @param obj Borrowed Scanner object.
/// @param n Maximum number of bytes to consume.
/// @return Newly allocated consumed bytes clamped to the remainder, or an empty sentinel.
rt_string rt_scanner_read_str(void *obj, int64_t n);

/// @brief Read until a delimiter character is found.
/// @details Leaves the matching delimiter at the cursor.
/// @param obj Borrowed Scanner object.
/// @param delim Integer whose low unsigned byte is the delimiter.
/// @return Newly allocated bytes before the delimiter, or an empty sentinel.
rt_string rt_scanner_read_until(void *obj, int64_t delim);

/// @brief Read until any of the delimiter characters is found.
/// @details Leaves the delimiter unconsumed; null delimiters consume the remainder.
/// @param obj Borrowed Scanner object.
/// @param delims Borrowed delimiter-byte set.
/// @return Newly allocated bytes before a delimiter, or an empty sentinel.
rt_string rt_scanner_read_until_any(void *obj, rt_string delims);

/// @brief Read while predicate is true.
/// @param obj Borrowed Scanner object.
/// @param pred Borrowed callback receiving each next unsigned byte.
/// @return Newly allocated accepted run, or an empty sentinel.
rt_string rt_scanner_read_while(void *obj, int8_t (*pred)(int64_t));

//=========================================================================
// Matching
//=========================================================================

/// @brief Check if current position matches the given character.
/// @param obj Borrowed Scanner object.
/// @param c Integer whose low unsigned byte is compared.
/// @return `1` if the current byte matches, otherwise `0`.
int8_t rt_scanner_match(void *obj, int64_t c);

/// @brief Check if current position matches the given string.
/// @details An empty non-null string matches without consuming.
/// @param obj Borrowed Scanner object.
/// @param s Borrowed byte string to match.
/// @return `1` if all upcoming bytes match, otherwise `0`.
int8_t rt_scanner_match_str(void *obj, rt_string s);

/// @brief Match and consume a character if it matches.
/// @param obj Borrowed Scanner object.
/// @param c Integer whose low unsigned byte is matched.
/// @return `1` if one byte matched and was consumed, otherwise `0`.
int8_t rt_scanner_accept(void *obj, int64_t c);

/// @brief Match and consume a string if it matches.
/// @param obj Borrowed Scanner object.
/// @param s Borrowed byte string to match.
/// @return `1` if the complete string matched and was consumed, otherwise `0`.
int8_t rt_scanner_accept_str(void *obj, rt_string s);

/// @brief Match and consume any of the given characters.
/// @param obj Borrowed Scanner object.
/// @param chars Borrowed byte set.
/// @return `1` if one member byte matched and was consumed, otherwise `0`.
int8_t rt_scanner_accept_any(void *obj, rt_string chars);

//=========================================================================
// Skipping
//=========================================================================

/// @brief Skip n characters.
/// @details Nonpositive values do nothing; positive values clamp at end-of-input.
/// @param obj Borrowed Scanner object.
/// @param n Number of bytes to skip.
void rt_scanner_skip(void *obj, int64_t n);

/// @brief Skip whitespace characters.
/// @param obj Borrowed Scanner object.
/// @return Number of space, tab, line-feed, or carriage-return bytes skipped.
int64_t rt_scanner_skip_whitespace(void *obj);

/// @brief Skip while predicate is true.
/// @param obj Borrowed Scanner object.
/// @param pred Borrowed callback receiving each next unsigned byte.
/// @return Number of bytes skipped.
int64_t rt_scanner_skip_while(void *obj, int8_t (*pred)(int64_t));

//=========================================================================
// Token Helpers
//=========================================================================

/// @brief Read an identifier (letters, digits, underscore, starting with letter/underscore).
/// @param obj Borrowed Scanner object.
/// @return Newly allocated ASCII identifier token, or an empty sentinel without consuming.
rt_string rt_scanner_read_ident(void *obj);

/// @brief Read an integer (optional sign, digits).
/// @param obj Borrowed Scanner object.
/// @return Newly allocated signed decimal token, or an empty sentinel without consuming.
rt_string rt_scanner_read_int(void *obj);

/// @brief Read a number (integer or float).
/// @details Accepts a decimal point and exponent; an incomplete exponent is
///          rolled back while preserving a valid mantissa.
/// @param obj Borrowed Scanner object.
/// @return Newly allocated decimal token, or an empty sentinel without consuming.
rt_string rt_scanner_read_number(void *obj);

/// @brief Read a quoted string (handles escape sequences).
/// @details Consumes both delimiters and decodes common single-byte escapes.
///          Unterminated input restores the cursor and traps.
/// @param obj Borrowed Scanner object.
/// @param quote Integer whose low byte is the quote delimiter.
/// @return Newly allocated decoded contents, or an empty string on mismatch.
rt_string rt_scanner_read_quoted(void *obj, int64_t quote);

/// @brief Read until end of line.
/// @details Consumes LF, CR, or CRLF after the returned content.
/// @param obj Borrowed Scanner object.
/// @return Newly allocated line bytes excluding the terminator.
rt_string rt_scanner_read_line(void *obj);

//=========================================================================
// Character Class Predicates
//=========================================================================

/// @brief Check if character is a digit.
/// @param c Integer value to classify.
/// @return `1` for an ASCII decimal digit, otherwise `0`.
int8_t rt_scanner_is_digit(int64_t c);

/// @brief Check if character is a letter.
/// @param c Integer value to classify.
/// @return `1` for an ASCII letter, otherwise `0`.
int8_t rt_scanner_is_alpha(int64_t c);

/// @brief Check if character is alphanumeric.
/// @param c Integer value to classify.
/// @return `1` for an ASCII letter or decimal digit, otherwise `0`.
int8_t rt_scanner_is_alnum(int64_t c);

/// @brief Check if character is whitespace.
/// @param c Integer value to classify.
/// @return `1` for space, tab, line feed, or carriage return; otherwise `0`.
int8_t rt_scanner_is_space(int64_t c);

#ifdef __cplusplus
}
#endif
