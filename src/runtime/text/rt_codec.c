//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE in the project root for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/text/rt_codec.c
// Purpose: Implements encoding and decoding utilities for the Zanna.Text.Codec
//          class. Provides Base64 (RFC 4648), hexadecimal, and URL percent-
//          encoding representations of strings and binary data.
//
// Key invariants:
//   - Base64 encoding follows RFC 4648 with '=' padding; output is always a
//     multiple of 4 characters.
//   - Hex encoding produces lowercase hex pairs; decoding accepts upper or lower.
//   - URL encoding percent-encodes all characters except A-Z, a-z, 0-9, -, _, ., ~.
//   - Invalid Base64 or Hex input during decoding traps with a diagnostic.
//   - All functions are thread-safe with no global mutable state.
//
// Ownership/Lifetime:
//   - All returned rt_string values are fresh allocations owned by the caller.
//   - Input strings are borrowed read-only for the duration of the call.
//
// Links: src/runtime/text/rt_codec.h (public API),
//        src/runtime/text/rt_hash.h (produces hex output, related codec)
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_codec.c
 * @brief Implements Base64, hexadecimal, and URL byte-string codecs.
 * @details Stateless helpers encode runtime String bytes with canonical RFC
 *          alphabets, strictly validate Base64 and hexadecimal input, and
 *          provide forgiving percent decoding for form-compatible URL text.
 *          Every successful transformation returns fresh managed storage.
 */


#include "rt_codec.h"

#include "rt_internal.h"
#include "rt_string.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/// @brief Base64 character lookup table for encoding (RFC 4648 standard alphabet).
static const char b64_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/// @brief Convert Base64 character to value (0-63).
/// @param c Candidate byte from the standard padded Base64 alphabet.
/// @return Value 0-63, -2 for '=', or -1 if invalid.
static int b64_digit_value(char c) {
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;
    if (c == '=')
        return -2;
    return -1;
}

/// @brief Check if character is unreserved in URL encoding.
/// @details Unreserved: A-Z a-z 0-9 - _ . ~
/// @param c Byte to classify.
/// @return 1 for an RFC 3986 unreserved ASCII byte, otherwise 0.
static int is_url_unreserved(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
}

//=============================================================================
// URL Encoding/Decoding
//=============================================================================

/// @brief Encodes a string for safe use in URLs (percent encoding).
///
/// Replaces unsafe characters with percent-encoded equivalents (%XX where XX
/// is the hexadecimal byte value). Safe characters are left unchanged.
///
/// **Unreserved (safe) characters:**
/// - Letters: A-Z, a-z
/// - Digits: 0-9
/// - Special: - _ . ~
///
/// **Encoding examples:**
/// ```
/// Input:  "Hello World"     Output: "Hello%20World"
/// Input:  "100% done!"      Output: "100%25%20done%21"
/// Input:  "a=1&b=2"         Output: "a%3D1%26b%3D2"
/// Input:  "café"            Output: "caf%C3%A9" (UTF-8 bytes encoded)
/// ```
///
/// **Usage example:**
/// ```
/// Dim query = "search term with spaces"
/// Dim encoded = Codec.UrlEncode(query)
/// Dim url = "https://example.com/search?q=" & encoded
/// ' url = "https://example.com/search?q=search%20term%20with%20spaces"
/// ```
///
/// @param str The string to encode. NULL or empty returns empty string.
///
/// @return A newly allocated URL-encoded string. Traps on allocation failure.
///
/// @note O(n) time complexity where n is input length.
/// @note Output length is at most 3× input length (all bytes encoded).
/// @note UTF-8 strings have each byte encoded separately.
///
/// @see rt_codec_url_decode For decoding URL-encoded strings
rt_string rt_codec_url_encode(rt_string str) {
    if (!str)
        return rt_string_from_bytes("", 0);

    const char *input = rt_string_cstr(str);
    size_t input_len = (size_t)rt_str_len(str);
    if (input_len == 0)
        return rt_string_from_bytes("", 0);

    // Calculate output length (worst case: every char needs encoding)
    size_t out_len = 0;
    for (size_t i = 0; i < input_len; i++) {
        unsigned char c = (unsigned char)input[i];
        if (is_url_unreserved(c))
            out_len++;
        else {
            if (out_len > SIZE_MAX - 3) {
                rt_trap("Codec.UrlEncode: output length overflow");
                return rt_string_from_bytes("", 0);
            }
            out_len += 3; // %XX
            continue;
        }
        if (out_len == 0) {
            rt_trap("Codec.UrlEncode: output length overflow");
            return rt_string_from_bytes("", 0);
        }
    }
    if (out_len == SIZE_MAX) {
        rt_trap("Codec.UrlEncode: output length overflow");
        return rt_string_from_bytes("", 0);
    }

    char *out = (char *)malloc(out_len + 1);
    if (!out) {
        rt_trap("Codec.UrlEncode: memory allocation failed");
        return rt_string_from_bytes("", 0);
    }

    size_t o = 0;
    for (size_t i = 0; i < input_len; i++) {
        unsigned char c = (unsigned char)input[i];
        if (is_url_unreserved(c)) {
            out[o++] = (char)c;
        } else {
            out[o++] = '%';
            out[o++] = rt_hex_chars[(c >> 4) & 0xF];
            out[o++] = rt_hex_chars[c & 0xF];
        }
    }
    out[o] = '\0';

    rt_string result = rt_string_from_bytes(out, o);
    free(out);
    return result;
}

/// @brief Decodes a URL-encoded (percent-encoded) string.
///
/// Converts percent-encoded sequences (%XX) back to their original byte values.
/// Also converts '+' to space (form encoding convention).
///
/// **Decoding examples:**
/// ```
/// Input:  "Hello%20World"           Output: "Hello World"
/// Input:  "100%25%20done%21"        Output: "100% done!"
/// Input:  "caf%C3%A9"               Output: "café"
/// Input:  "a+b"                     Output: "a b" (+ becomes space)
/// ```
///
/// **Error handling:**
/// - Invalid sequences (e.g., "%GG") are passed through unchanged
/// - Incomplete sequences at end (e.g., "%2") are passed through unchanged
///
/// **Usage example:**
/// ```
/// Dim encoded = "Hello%20World%21"
/// Dim decoded = Codec.UrlDecode(encoded)
/// Print decoded   ' Outputs: Hello World!
/// ```
///
/// @param str The URL-encoded string to decode. NULL or empty returns empty string.
///
/// @return A newly allocated decoded string. Traps on allocation failure.
///
/// @note O(n) time complexity where n is input length.
/// @note Output length is at most input length (no expansion possible).
/// @note Forgiving: invalid sequences pass through unchanged.
///
/// @see rt_codec_url_encode For encoding strings for URLs
rt_string rt_codec_url_decode(rt_string str) {
    if (!str)
        return rt_string_from_bytes("", 0);

    const char *input = rt_string_cstr(str);
    size_t input_len = (size_t)rt_str_len(str);
    if (input_len == 0)
        return rt_string_from_bytes("", 0);

    // Output is at most as long as input
    char *out = (char *)malloc(input_len + 1);
    if (!out) {
        rt_trap("Codec.UrlDecode: memory allocation failed");
        return rt_string_from_bytes("", 0);
    }

    size_t o = 0;
    for (size_t i = 0; i < input_len; i++) {
        char c = input[i];
        if (c == '%' && i + 2 < input_len) {
            int hi = rt_hex_digit_value(input[i + 1]);
            int lo = rt_hex_digit_value(input[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out[o++] = (char)((hi << 4) | lo);
                i += 2;
                continue;
            }
        } else if (c == '+') {
            // Treat + as space (form encoding convention)
            out[o++] = ' ';
            continue;
        }
        out[o++] = c;
    }
    out[o] = '\0';

    rt_string result = rt_string_from_bytes(out, o);
    free(out);
    return result;
}

//=============================================================================
// Base64 Encoding/Decoding
//=============================================================================

/// @brief Encodes a string to Base64 format (RFC 4648).
///
/// Converts binary data to a text representation using the Base64 alphabet.
/// The output uses only printable ASCII characters and is suitable for
/// embedding in JSON, XML, email, and other text formats.
///
/// **Base64 encoding process:**
/// 1. Take 3 bytes (24 bits) of input
/// 2. Split into 4 groups of 6 bits each
/// 3. Map each 6-bit value to a character (A-Z, a-z, 0-9, +, /)
/// 4. Pad with '=' if input length is not divisible by 3
///
/// **Padding:**
/// - 0 remainder bytes: no padding
/// - 1 remainder byte: "==" padding
/// - 2 remainder bytes: "=" padding
///
/// **Examples:**
/// ```
/// Input:  "Man"      Output: "TWFu"      (no padding)
/// Input:  "Ma"       Output: "TWE="      (1 pad char)
/// Input:  "M"        Output: "TQ=="      (2 pad chars)
/// Input:  "Hello"    Output: "SGVsbG8="
/// ```
///
/// **Usage example:**
/// ```
/// Dim data = "Hello, World!"
/// Dim encoded = Codec.Base64Enc(data)
/// Print encoded   ' Outputs: SGVsbG8sIFdvcmxkIQ==
/// ```
///
/// @param str The string to encode. NULL or empty returns empty string.
///
/// @return A newly allocated Base64-encoded string. Traps on allocation failure.
///
/// @note O(n) time complexity where n is input length.
/// @note Output length is ceil(n/3)*4 (33% expansion plus padding).
///
/// @see rt_codec_base64_dec For decoding Base64 strings
rt_string rt_codec_base64_enc(rt_string str) {
    if (!str)
        return rt_string_from_bytes("", 0);

    const char *input = rt_string_cstr(str);
    size_t input_len = (size_t)rt_str_len(str);
    if (input_len == 0)
        return rt_string_from_bytes("", 0);

    if (input_len > SIZE_MAX - 2) {
        rt_trap("Codec.Base64Enc: output length overflow");
        return rt_string_from_bytes("", 0);
    }
    size_t groups = (input_len + 2) / 3;
    if (groups > SIZE_MAX / 4) {
        rt_trap("Codec.Base64Enc: output length overflow");
        return rt_string_from_bytes("", 0);
    }
    size_t output_len = groups * 4;
    if (output_len == SIZE_MAX) {
        rt_trap("Codec.Base64Enc: output length overflow");
        return rt_string_from_bytes("", 0);
    }

    char *out = (char *)malloc(output_len + 1);
    if (!out) {
        rt_trap("Codec.Base64Enc: memory allocation failed");
        return rt_string_from_bytes("", 0);
    }

    const uint8_t *data = (const uint8_t *)input;
    size_t i = 0;
    size_t o = 0;

    // Process 3-byte groups
    while (i + 3 <= input_len) {
        uint32_t triple = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8) | data[i + 2];
        out[o++] = b64_chars[(triple >> 18) & 0x3F];
        out[o++] = b64_chars[(triple >> 12) & 0x3F];
        out[o++] = b64_chars[(triple >> 6) & 0x3F];
        out[o++] = b64_chars[triple & 0x3F];
        i += 3;
    }

    // Handle remaining bytes
    if (i < input_len) {
        uint32_t triple = (uint32_t)data[i] << 16;
        int two = 0;
        if (i + 1 < input_len) {
            triple |= (uint32_t)data[i + 1] << 8;
            two = 1;
        }

        out[o++] = b64_chars[(triple >> 18) & 0x3F];
        out[o++] = b64_chars[(triple >> 12) & 0x3F];
        if (two) {
            out[o++] = b64_chars[(triple >> 6) & 0x3F];
            out[o++] = '=';
        } else {
            out[o++] = '=';
            out[o++] = '=';
        }
    }

    out[o] = '\0';
    rt_string result = rt_string_from_bytes(out, o);
    free(out);
    return result;
}

/// @brief Decodes a Base64-encoded string back to its original form.
///
/// Converts a Base64 string back to the original binary data. The input
/// must be valid Base64 with proper padding.
///
/// **Decoding examples:**
/// ```
/// Input:  "TWFu"              Output: "Man"
/// Input:  "SGVsbG8sIFdvcmxkIQ==" Output: "Hello, World!"
/// Input:  "TQ=="              Output: "M"
/// ```
///
/// **Validation:**
/// - Input length must be a multiple of 4
/// - Padding characters must only appear at the end
/// - Only valid Base64 characters are allowed
///
/// **Usage example:**
/// ```
/// Dim encoded = "SGVsbG8sIFdvcmxkIQ=="
/// Dim decoded = Codec.Base64Dec(encoded)
/// Print decoded   ' Outputs: Hello, World!
/// ```
///
/// @param str The Base64-encoded string to decode. NULL or empty returns empty string.
///
/// @return A newly allocated decoded string. Traps on invalid input or allocation failure.
///
/// @note O(n) time complexity where n is input length.
/// @note Output length is approximately 3/4 of input length.
/// @note Traps on invalid Base64 format (not forgiving like URL decode).
///
/// @see rt_codec_base64_enc For encoding strings to Base64
rt_string rt_codec_base64_dec(rt_string str) {
    if (!str)
        return rt_string_from_bytes("", 0);

    const char *input = rt_string_cstr(str);
    size_t b64_len = (size_t)rt_str_len(str);
    if (b64_len == 0)
        return rt_string_from_bytes("", 0);

    if (b64_len % 4 != 0) {
        rt_trap("Codec.Base64Dec: base64 length must be a multiple of 4");
        return rt_string_from_bytes("", 0);
    }

    size_t padding = 0;
    if (input[b64_len - 1] == '=') {
        padding = 1;
        if (b64_len >= 2 && input[b64_len - 2] == '=')
            padding = 2;
    }

    // Check for invalid padding position
    for (size_t i = 0; i < b64_len - padding; ++i) {
        if (input[i] == '=') {
            rt_trap("Codec.Base64Dec: invalid padding");
            return rt_string_from_bytes("", 0);
        }
    }

    size_t out_len = (b64_len / 4) * 3 - padding;
    if (out_len == 0)
        return rt_string_from_bytes("", 0);

    char *out = (char *)malloc(out_len + 1);
    if (!out) {
        rt_trap("Codec.Base64Dec: memory allocation failed");
        return rt_string_from_bytes("", 0);
    }

    size_t out_pos = 0;
    for (size_t i = 0; i < b64_len; i += 4) {
        char c0 = input[i];
        char c1 = input[i + 1];
        char c2 = input[i + 2];
        char c3 = input[i + 3];

        int v0 = b64_digit_value(c0);
        int v1 = b64_digit_value(c1);
        int v2 = b64_digit_value(c2);
        int v3 = b64_digit_value(c3);

        if (v0 < 0 || v1 < 0) {
            free(out);
            if (v0 == -2 || v1 == -2)
                rt_trap("Codec.Base64Dec: invalid padding");
            else
                rt_trap("Codec.Base64Dec: invalid base64 character");
            return rt_string_from_bytes("", 0);
        }

        if (v2 == -1 || v3 == -1) {
            free(out);
            rt_trap("Codec.Base64Dec: invalid base64 character");
            return rt_string_from_bytes("", 0);
        }

        if (v2 == -2) {
            if (v3 != -2 || i + 4 != b64_len) {
                free(out);
                rt_trap("Codec.Base64Dec: invalid padding");
                return rt_string_from_bytes("", 0);
            }
            if ((v1 & 0x0F) != 0) {
                free(out);
                rt_trap("Codec.Base64Dec: invalid padding");
                return rt_string_from_bytes("", 0);
            }

            uint32_t triple = ((uint32_t)v0 << 18) | ((uint32_t)v1 << 12);
            out[out_pos++] = (char)((triple >> 16) & 0xFF);
            break;
        }

        if (v3 == -2) {
            if (i + 4 != b64_len) {
                free(out);
                rt_trap("Codec.Base64Dec: invalid padding");
                return rt_string_from_bytes("", 0);
            }
            if ((v2 & 0x03) != 0) {
                free(out);
                rt_trap("Codec.Base64Dec: invalid padding");
                return rt_string_from_bytes("", 0);
            }

            uint32_t triple = ((uint32_t)v0 << 18) | ((uint32_t)v1 << 12) | ((uint32_t)v2 << 6);
            out[out_pos++] = (char)((triple >> 16) & 0xFF);
            out[out_pos++] = (char)((triple >> 8) & 0xFF);
            break;
        }

        uint32_t triple =
            ((uint32_t)v0 << 18) | ((uint32_t)v1 << 12) | ((uint32_t)v2 << 6) | (uint32_t)v3;
        out[out_pos++] = (char)((triple >> 16) & 0xFF);
        out[out_pos++] = (char)((triple >> 8) & 0xFF);
        out[out_pos++] = (char)(triple & 0xFF);
    }

    if (out_pos != out_len) {
        free(out);
        rt_trap("Codec.Base64Dec: invalid padding");
        return rt_string_from_bytes("", 0);
    }

    out[out_pos] = '\0';
    rt_string result = rt_string_from_bytes(out, out_pos);
    free(out);
    return result;
}

//=============================================================================
// Hex Encoding/Decoding
//=============================================================================

/// @brief Encodes a string to hexadecimal representation.
///
/// Converts each byte of the input to its two-character hexadecimal
/// representation using lowercase letters (0-9, a-f).
///
/// **Encoding process:**
/// ```
/// Input byte:  0x48 ('H')
/// Output:      "48"
///
/// Input:  "Hi"
/// Bytes:  0x48 0x69
/// Output: "4869"
/// ```
///
/// **Examples:**
/// ```
/// Input:  "Hello"     Output: "48656c6c6f"
/// Input:  "\x00\xFF"  Output: "00ff"
/// Input:  "ABC"       Output: "414243"
/// ```
///
/// **Usage example:**
/// ```
/// Dim text = "Hello"
/// Dim hex = Codec.HexEnc(text)
/// Print hex   ' Outputs: 48656c6c6f
///
/// ' Useful for debugging binary data
/// Dim binaryData = Chr(0) & Chr(255) & Chr(128)
/// Print Codec.HexEnc(binaryData)  ' Outputs: 00ff80
/// ```
///
/// @param str The string to encode. NULL or empty returns empty string.
///
/// @return A newly allocated hex-encoded string (lowercase). Traps on allocation failure.
///
/// @note O(n) time complexity where n is input length.
/// @note Output length is exactly 2× input length.
/// @note Uses lowercase hex digits (a-f, not A-F).
///
/// @see rt_codec_hex_dec For decoding hex strings
/// @see rt_hash_md5 For hashing to hex output
rt_string rt_codec_hex_enc(rt_string str) {
    if (!str)
        return rt_string_from_bytes("", 0);

    const char *input = rt_string_cstr(str);
    size_t input_len = (size_t)rt_str_len(str);
    if (input_len == 0)
        return rt_string_from_bytes("", 0);

    if (input_len > (SIZE_MAX - 1) / 2) {
        rt_trap("Codec.HexEnc: output length overflow");
        return rt_string_from_bytes("", 0);
    }
    size_t output_len = input_len * 2;
    char *out = (char *)malloc(output_len + 1);
    if (!out) {
        rt_trap("Codec.HexEnc: memory allocation failed");
        return rt_string_from_bytes("", 0);
    }

    const uint8_t *data = (const uint8_t *)input;
    for (size_t i = 0; i < input_len; i++) {
        out[i * 2] = rt_hex_chars[(data[i] >> 4) & 0xF];
        out[i * 2 + 1] = rt_hex_chars[data[i] & 0xF];
    }
    out[output_len] = '\0';

    rt_string result = rt_string_from_bytes(out, output_len);
    free(out);
    return result;
}

/// @brief Encodes raw byte data to hexadecimal representation.
///
/// Lower-level version of rt_codec_hex_enc that takes raw byte data
/// instead of an rt_string. Used internally by rt_hash for hash output.
///
/// @param data Pointer to byte data to encode.
/// @param len Length of data in bytes.
/// @return Newly allocated lowercase hex string. Traps on allocation failure.
rt_string rt_codec_hex_enc_bytes(const uint8_t *data, size_t len) {
    if (!data || len == 0)
        return rt_string_from_bytes("", 0);

    if (len > (SIZE_MAX - 1) / 2) {
        rt_trap("Codec.HexEncBytes: output length overflow");
        return rt_string_from_bytes("", 0);
    }
    size_t output_len = len * 2;
    char *out = (char *)malloc(output_len + 1);
    if (!out) {
        rt_trap("Codec.HexEncBytes: memory allocation failed");
        return rt_string_from_bytes("", 0);
    }

    for (size_t i = 0; i < len; i++) {
        out[i * 2] = rt_hex_chars[(data[i] >> 4) & 0xF];
        out[i * 2 + 1] = rt_hex_chars[data[i] & 0xF];
    }
    out[output_len] = '\0';

    rt_string result = rt_string_from_bytes(out, output_len);
    free(out);
    return result;
}

/// @brief Decodes a hexadecimal string back to binary data.
///
/// Converts pairs of hexadecimal characters back to their byte values.
/// Accepts both uppercase (A-F) and lowercase (a-f) hex digits.
///
/// **Decoding examples:**
/// ```
/// Input:  "48656c6c6f"    Output: "Hello"
/// Input:  "00FF80"        Output: "\x00\xFF\x80"
/// Input:  "414243"        Output: "ABC"
/// ```
///
/// **Validation:**
/// - Input length must be even (2 chars per byte)
/// - Only valid hex characters allowed (0-9, A-F, a-f)
///
/// **Usage example:**
/// ```
/// Dim hex = "48656c6c6f"
/// Dim text = Codec.HexDec(hex)
/// Print text   ' Outputs: Hello
///
/// ' Case insensitive
/// Print Codec.HexDec("ABCD") = Codec.HexDec("abcd")  ' True
/// ```
///
/// @param str The hex-encoded string to decode. NULL or empty returns empty string.
///
/// @return A newly allocated decoded string. Traps on invalid input or allocation failure.
///
/// @note O(n) time complexity where n is input length.
/// @note Output length is exactly input length / 2.
/// @note Traps if length is odd or contains invalid hex characters.
/// @note Accepts both uppercase and lowercase hex digits.
///
/// @see rt_codec_hex_enc For encoding strings to hex
rt_string rt_codec_hex_dec(rt_string str) {
    if (!str)
        return rt_string_from_bytes("", 0);

    const char *input = rt_string_cstr(str);
    size_t hex_len = (size_t)rt_str_len(str);
    if (hex_len == 0)
        return rt_string_from_bytes("", 0);

    if (hex_len % 2 != 0) {
        rt_trap("Codec.HexDec: hex string length must be even");
        return rt_string_from_bytes("", 0);
    }

    size_t out_len = hex_len / 2;
    char *out = (char *)malloc(out_len + 1);
    if (!out) {
        rt_trap("Codec.HexDec: memory allocation failed");
        return rt_string_from_bytes("", 0);
    }

    for (size_t i = 0; i < out_len; i++) {
        int hi = rt_hex_digit_value(input[i * 2]);
        int lo = rt_hex_digit_value(input[i * 2 + 1]);

        if (hi < 0 || lo < 0) {
            free(out);
            rt_trap("Codec.HexDec: invalid hex character");
            return rt_string_from_bytes("", 0);
        }

        out[i] = (char)((hi << 4) | lo);
    }
    out[out_len] = '\0';

    rt_string result = rt_string_from_bytes(out, out_len);
    free(out);
    return result;
}
