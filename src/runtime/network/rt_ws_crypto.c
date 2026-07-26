//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/network/rt_ws_crypto.c
// Purpose: WebSocket handshake crypto/encoding: UTF-8 validation, SHA-1, the
//          client nonce generator, and the Sec-WebSocket-Accept key computation.
// Key invariants:
//   - UTF-8 validation rejects overlong, surrogate, truncated, and out-of-range forms.
//   - SHA-1 is used only for the RFC 6455 handshake transform, never as a security hash.
//   - Fixed handshake Base64 conversion uses native bounded buffers and no managed staging.
// Ownership/Lifetime:
//   - Generated runtime Strings are caller-owned.
//   - rt_ws_compute_accept_key returns native heap storage released with free().
// Links: src/runtime/network/rt_websocket.h,
//        src/runtime/network/rt_websocket_internal.h,
//        src/runtime/network/rt_websocket.c
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_ws_crypto.c
 * @brief Implements WebSocket handshake transforms and text validation.
 * @details The module provides strict UTF-8 validation, the RFC-mandated
 *          handshake-only SHA-1 transform, unpredictable client nonce
 *          generation, Base64 encoding, and Sec-WebSocket-Accept computation
 *          using bounded native intermediate storage.
 */

//===----------------------------------------------------------------------===//

#include "rt_websocket.h"

#include "rt_crypto.h"
#include "rt_string.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Forward declarations (defined in rt_io.c).
#include "rt_trap.h"
extern void rt_trap_net(const char *msg, int err_code);

#include "rt_error.h"

#include "rt_websocket_internal.h"

/// @brief Validate a byte sequence as canonical RFC 3629 UTF-8.
/// @details Rejects overlong encodings, UTF-16 surrogate halves, truncated or
///          malformed continuation sequences, and code points above U+10FFFF.
///          ASCII bytes, including U+0000, are valid.
/// @param[in] data Input bytes; it may be NULL only when @p len is zero.
/// @param[in] len Number of bytes to validate.
/// @return 1 when the entire sequence is valid UTF-8; otherwise 0.
int ws_is_valid_utf8(const uint8_t *data, size_t len) {
    size_t i = 0;
    while (i < len) {
        uint8_t c = data[i++];
        if (c <= 0x7F)
            continue;

        if (c >= 0xC2 && c <= 0xDF) {
            if (i >= len || (data[i] & 0xC0) != 0x80)
                return 0;
            i++;
            continue;
        }

        if (c == 0xE0) {
            if (i + 1 >= len || data[i] < 0xA0 || data[i] > 0xBF || (data[i + 1] & 0xC0) != 0x80)
                return 0;
            i += 2;
            continue;
        }

        if (c >= 0xE1 && c <= 0xEC) {
            if (i + 1 >= len || (data[i] & 0xC0) != 0x80 || (data[i + 1] & 0xC0) != 0x80)
                return 0;
            i += 2;
            continue;
        }

        if (c == 0xED) {
            if (i + 1 >= len || data[i] < 0x80 || data[i] > 0x9F || (data[i + 1] & 0xC0) != 0x80)
                return 0;
            i += 2;
            continue;
        }

        if (c >= 0xEE && c <= 0xEF) {
            if (i + 1 >= len || (data[i] & 0xC0) != 0x80 || (data[i + 1] & 0xC0) != 0x80)
                return 0;
            i += 2;
            continue;
        }

        if (c == 0xF0) {
            if (i + 2 >= len || data[i] < 0x90 || data[i] > 0xBF || (data[i + 1] & 0xC0) != 0x80 ||
                (data[i + 2] & 0xC0) != 0x80)
                return 0;
            i += 3;
            continue;
        }

        if (c >= 0xF1 && c <= 0xF3) {
            if (i + 2 >= len || (data[i] & 0xC0) != 0x80 || (data[i + 1] & 0xC0) != 0x80 ||
                (data[i + 2] & 0xC0) != 0x80)
                return 0;
            i += 3;
            continue;
        }

        if (c == 0xF4) {
            if (i + 2 >= len || data[i] < 0x80 || data[i] > 0x8F || (data[i + 1] & 0xC0) != 0x80 ||
                (data[i + 2] & 0xC0) != 0x80)
                return 0;
            i += 3;
            continue;
        }

        return 0;
    }
    return 1;
}

/// @brief Minimal SHA-1 (RFC 3174) for Sec-WebSocket-Accept validation (RFC 6455 §4.1).
/// @details SHA-1 is used only for the protocol-mandated opening-handshake
///          transform, not as a general security primitive. Temporary padded
///          message storage is allocated internally and released before return.
/// @param[in] data Input bytes; NULL is permitted only when @p len is zero.
/// @param[in] len Number of input bytes.
/// @param[out] digest Buffer that receives exactly 20 digest bytes.
/// @return 1 on success, or 0 for invalid arguments, length overflow, or allocation failure.
int ws_sha1(const uint8_t *data, size_t len, uint8_t digest[20]) {
    if (!digest || (len > 0 && !data) || len > SIZE_MAX - 72u || len > UINT64_MAX / 8u)
        return 0;
    uint32_t h0 = 0x67452301u, h1 = 0xEFCDAB89u, h2 = 0x98BADCFEu;
    uint32_t h3 = 0x10325476u, h4 = 0xC3D2E1F0u;

    size_t padded_len = ((len + 9 + 63) / 64) * 64;
    uint8_t *padded = (uint8_t *)calloc(padded_len, 1);
    if (!padded)
        return 0;
    memcpy(padded, data, len);
    padded[len] = 0x80;
    uint64_t bit_len = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++)
        padded[padded_len - 8 + i] = (uint8_t)(bit_len >> (56 - i * 8));

    for (size_t chunk = 0; chunk < padded_len; chunk += 64) {
        uint32_t w[80];
        for (int j = 0; j < 16; j++) {
            w[j] = ((uint32_t)padded[chunk + j * 4] << 24) |
                   ((uint32_t)padded[chunk + j * 4 + 1] << 16) |
                   ((uint32_t)padded[chunk + j * 4 + 2] << 8) |
                   ((uint32_t)padded[chunk + j * 4 + 3]);
        }
        for (int j = 16; j < 80; j++) {
            uint32_t t = w[j - 3] ^ w[j - 8] ^ w[j - 14] ^ w[j - 16];
            w[j] = (t << 1) | (t >> 31);
        }
        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int j = 0; j < 80; j++) {
            uint32_t f, k;
            if (j < 20) {
                f = (b & c) | (~b & d);
                k = 0x5A827999u;
            } else if (j < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1u;
            } else if (j < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCu;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6u;
            }
            uint32_t temp = ((a << 5) | (a >> 27)) + f + e + k + w[j];
            e = d;
            d = c;
            c = (b << 30) | (b >> 2);
            b = a;
            a = temp;
        }
        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
    }
    free(padded);

    for (int i = 0; i < 4; i++) {
        digest[i] = (uint8_t)(h0 >> (24 - i * 8));
        digest[4 + i] = (uint8_t)(h1 >> (24 - i * 8));
        digest[8 + i] = (uint8_t)(h2 >> (24 - i * 8));
        digest[12 + i] = (uint8_t)(h3 >> (24 - i * 8));
        digest[16 + i] = (uint8_t)(h4 >> (24 - i * 8));
    }
    return 1;
}

/// @brief Encode one bounded native byte span with the RFC 4648 Base64 alphabet.
/// @details This helper is used only for 16-byte client nonces and 20-byte
///          SHA-1 handshake digests. It nevertheless checks the general output
///          size formula, preserves padding, and never writes a partial result.
/// @param input Source bytes; NULL is accepted only for an empty input.
/// @param input_len Source byte count.
/// @param output Destination buffer.
/// @param output_capacity Destination capacity including the NUL terminator.
/// @param output_len Receives the encoded byte count, excluding the terminator.
/// @return 1 on success, otherwise 0 without publishing an output string.
static int ws_base64_encode_native(const uint8_t *input,
                                   size_t input_len,
                                   char *output,
                                   size_t output_capacity,
                                   size_t *output_len) {
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (output_len)
        *output_len = 0;
    if (!output || (input_len > 0 && !input) || input_len > (SIZE_MAX - 2u) / 3u)
        return 0;
    size_t encoded_len = ((input_len + 2u) / 3u) * 4u;
    if (encoded_len >= output_capacity)
        return 0;

    size_t source = 0;
    size_t destination = 0;
    while (source + 3u <= input_len) {
        uint32_t value = ((uint32_t)input[source] << 16) | ((uint32_t)input[source + 1u] << 8) |
                         input[source + 2u];
        output[destination++] = alphabet[(value >> 18) & 0x3fu];
        output[destination++] = alphabet[(value >> 12) & 0x3fu];
        output[destination++] = alphabet[(value >> 6) & 0x3fu];
        output[destination++] = alphabet[value & 0x3fu];
        source += 3u;
    }
    size_t remaining = input_len - source;
    if (remaining == 1u) {
        uint32_t value = (uint32_t)input[source] << 16;
        output[destination++] = alphabet[(value >> 18) & 0x3fu];
        output[destination++] = alphabet[(value >> 12) & 0x3fu];
        output[destination++] = '=';
        output[destination++] = '=';
    } else if (remaining == 2u) {
        uint32_t value = ((uint32_t)input[source] << 16) | ((uint32_t)input[source + 1u] << 8);
        output[destination++] = alphabet[(value >> 18) & 0x3fu];
        output[destination++] = alphabet[(value >> 12) & 0x3fu];
        output[destination++] = alphabet[(value >> 6) & 0x3fu];
        output[destination++] = '=';
    }
    output[destination] = '\0';
    if (output_len)
        *output_len = destination;
    return destination == encoded_len ? 1 : 0;
}

/// @brief Generate a random WebSocket key without an intermediate Bytes object.
/// @details RFC 6455 requires a fresh unpredictable 16-byte nonce. It is
///          encoded into a fixed 24-byte Base64 representation and copied once
///          into the returned managed String.
/// @return Caller-owned managed key String, or NULL after a returning allocation trap.
rt_string generate_ws_key(void) {
    uint8_t raw[16];
    char encoded[25];
    size_t encoded_len = 0;
    rt_crypto_random_bytes(raw, sizeof(raw));
    if (!ws_base64_encode_native(raw, sizeof(raw), encoded, sizeof(encoded), &encoded_len))
        return NULL;
    return rt_string_from_bytes(encoded, encoded_len);
}

/// @brief Compute the Sec-WebSocket-Accept header value for a given key.
/// @details Computes Base64(SHA1(key + RFC 6455 GUID)) and returns the
///          28-character result as native heap storage.
/// @param[in] key_cstr Null-terminated Sec-WebSocket-Key header value.
/// @return Newly allocated null-terminated accept value, or NULL for null
///         input, length overflow, hashing failure, or allocation failure. The
///         caller must release the result with free().
char *rt_ws_compute_accept_key(const char *key_cstr) {
    if (!key_cstr)
        return NULL;

    static const char WS_MAGIC[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    size_t key_len = strlen(key_cstr);
    size_t magic_len = sizeof(WS_MAGIC) - 1;
    if (key_len > SIZE_MAX - magic_len - 1)
        return NULL;

    char *concat = (char *)malloc(key_len + magic_len + 1);
    if (!concat)
        return NULL;

    memcpy(concat, key_cstr, key_len);
    memcpy(concat + key_len, WS_MAGIC, magic_len);
    concat[key_len + magic_len] = '\0';

    uint8_t sha1_digest[20];
    if (!ws_sha1((const uint8_t *)concat, key_len + magic_len, sha1_digest)) {
        free(concat);
        return NULL;
    }
    free(concat);

    char encoded[29];
    size_t encoded_len = 0;
    if (!ws_base64_encode_native(
            sha1_digest, sizeof(sha1_digest), encoded, sizeof(encoded), &encoded_len))
        return NULL;
    char *result = (char *)malloc(encoded_len + 1u);
    if (!result)
        return NULL;
    memcpy(result, encoded, encoded_len + 1u);
    return result;
}
