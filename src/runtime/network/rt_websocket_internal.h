//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/network/rt_websocket_internal.h
// Purpose: WebSocket crypto/key helpers (SHA-1 accept key, nonce) shared
//   between rt_ws_crypto.c and the client logic in rt_websocket.c.
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_websocket_internal.h
 * @brief Declares shared WebSocket handshake and UTF-8 helpers.
 * @details These internal routines generate unpredictable opening-handshake
 *          keys, compute the protocol-mandated SHA-1 digest, and validate
 *          canonical UTF-8 for text messages. Returned managed keys are owned
 *          by the caller; all input byte spans remain borrowed.
 */

#pragma once

#include "rt_string.h"

#include <stddef.h>
#include <stdint.h>

/// @brief Generate a fresh RFC 6455 Sec-WebSocket-Key value.
/// @details Draws a 16-byte cryptographically random nonce and Base64-encodes
///          it into a managed 24-byte String.
/// @return Caller-owned managed key String, or NULL after a returning
///         allocation trap. Entropy-source failure may trap.
rt_string generate_ws_key(void);

/// @brief Compute the RFC 3174 SHA-1 digest required by the WebSocket opening handshake.
/// @details This protocol-mandated SHA-1 helper is scoped to
///          Sec-WebSocket-Accept computation and allocates temporary padded storage.
/// @param[in] data Input bytes; NULL is permitted only when @p len is zero.
/// @param[in] len Input length in bytes.
/// @param[out] digest Buffer that receives exactly 20 digest bytes.
/// @return 1 on success, or 0 for invalid arguments, length overflow, or allocation failure.
int ws_sha1(const uint8_t *data, size_t len, uint8_t digest[20]);

/// @brief Validate a byte sequence as canonical RFC 3629 UTF-8.
/// @details Rejects overlong encodings, surrogate halves, code points above
///          U+10FFFF, invalid continuation bytes, and truncated sequences.
/// @param[in] data Input bytes; NULL is permitted only when @p len is zero.
/// @param[in] len Number of bytes to validate.
/// @return 1 when the complete sequence is valid UTF-8; otherwise 0.
int ws_is_valid_utf8(const uint8_t *data, size_t len);
