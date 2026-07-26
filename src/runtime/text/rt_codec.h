//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/text/rt_codec.h
// Purpose: Base64, Hex, and URL encoding/decoding utilities for string-based transformations used
// in network protocols, configuration, and data serialization.
//
// Key invariants:
//   - Base64 encoding uses the standard alphabet (A-Z, a-z, 0-9, +, /); output includes padding.
//   - URL encoding uses percent-encoding for reserved characters.
//   - Hex encoding uses lowercase hexadecimal digits.
//   - Base64 and Hex decoders trap on invalid input; URL decoding is forgiving.
//
// Ownership/Lifetime:
//   - Returned strings are newly allocated; caller must release.
//   - Input strings are borrowed; callers retain ownership.
//
// Links: src/runtime/text/rt_codec.c (implementation), src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_codec.h
 * @brief Declares Base64, hexadecimal, and URL String transformations.
 * @details Encoding routines operate on exact borrowed runtime bytes and
 *          return caller-owned Strings. Strict Base64 and hexadecimal decoders
 *          reject malformed or noncanonical input, while URL decoding preserves
 *          invalid percent syntax and supports form-style plus characters.
 */

#pragma once

#include "rt_string.h"

#ifdef __cplusplus
extern "C" {
#endif

/// @brief URL-encode a string (percent-encoding).
/// @details Leaves RFC 3986 unreserved ASCII bytes `A-Z`, `a-z`, `0-9`, `-`,
///          `_`, `.`, and `~` unchanged; encodes every other input byte as
///          uppercase `%XX`, including individual bytes of UTF-8 sequences.
/// @param str Borrowed runtime byte string; NULL and empty yield empty output.
/// @return Newly allocated encoded string, or an empty fallback after an
///         overflow/allocation trap.
rt_string rt_codec_url_encode(rt_string str);

/// @brief URL-decode a string (percent-decoding).
/// @details Decodes valid `%XX` byte sequences and maps `+` to a space for form
///          compatibility. Invalid or incomplete percent sequences pass
///          through unchanged rather than trapping.
/// @param str Borrowed encoded string; NULL and empty yield empty output.
/// @return Newly allocated decoded byte string, which may contain embedded NUL,
///         or an empty fallback after an allocation trap.
rt_string rt_codec_url_decode(rt_string str);

/// @brief Base64-encode a string's bytes.
/// @details Uses the RFC 4648 standard alphabet with canonical `=` padding; the
///          output length is always a multiple of four.
/// @param str Borrowed runtime byte string; NULL and empty yield empty output.
/// @return Newly allocated Base64 string, or an empty fallback after an
///         overflow/allocation trap.
rt_string rt_codec_base64_enc(rt_string str);

/// @brief Base64-decode a string.
/// @details Requires a multiple-of-four length, standard alphabet, canonical
///          trailing padding placement, and zero unused bits. Whitespace and
///          unpadded variants are rejected.
/// @param str Borrowed Base64 text; NULL and empty yield empty output.
/// @return Newly allocated decoded byte string, which may contain embedded NUL,
///         or an empty fallback after a validation/allocation trap.
rt_string rt_codec_base64_dec(rt_string str);

/// @brief Hex-encode a string's bytes.
/// @param str Borrowed runtime byte string; NULL and empty yield empty output.
/// @return Newly allocated lowercase hexadecimal string with exactly twice the
///         input length, or an empty fallback after an overflow/allocation trap.
rt_string rt_codec_hex_enc(rt_string str);

/// @brief Hex-encode raw byte data.
/// @param data Borrowed byte span; NULL yields empty output.
/// @param len Length of data in bytes.
/// @details Used by rt_hash for hash output formatting.
/// @return Newly allocated lowercase hexadecimal string, or an empty string for
///         NULL/zero input or after an overflow/allocation trap.
rt_string rt_codec_hex_enc_bytes(const uint8_t *data, size_t len);

/// @brief Hex-decode a string.
/// @details Accepts uppercase or lowercase digits but requires an even input
///          length and no separators or whitespace.
/// @param str Borrowed hexadecimal string; NULL and empty yield empty output.
/// @return Newly allocated decoded byte string, which may contain embedded NUL,
///         or an empty fallback after a validation/allocation trap.
rt_string rt_codec_hex_dec(rt_string str);

#ifdef __cplusplus
}
#endif
