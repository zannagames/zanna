//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/network/rt_websocket.h
// Purpose: WebSocket client implementing RFC 6455 for real-time bidirectional text and binary
// communication over persistent connections.
//
// Key invariants:
//   - Implements the RFC 6455 WebSocket protocol including framing and masking.
//   - Supports both text and binary message types.
//   - Ping/pong keepalive is handled transparently.
//   - Opening responses and frames are parsed strictly and with bounded sizes.
//   - Native descriptors remain pointer-width across TCP and TLS ownership.
//   - Connection objects have stable identity; send/receive are serialized and blocking.
//
// Ownership/Lifetime:
//   - Constructors return one caller-owned managed opaque reference.
//   - Explicit Close retires the transport; finalization is partial-safe and idempotent.
//   - Returned String and Bytes values are caller-owned managed references.
//
// Links: src/runtime/network/rt_websocket.c (implementation), src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_websocket.h
 * @brief Declares the managed RFC 6455 WebSocket client API.
 * @details The interface connects with optional timeouts and subprotocols,
 *          exposes synchronized connection metadata, sends text, binary, ping,
 *          and close frames, and receives caller-owned managed messages while
 *          enforcing protocol framing and aggregate-size limits.
 */

#pragma once

#include "rt_string.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Stable managed identity for `Zanna.Network.WebSocket` connections.
/// @details Every non-null public receiver validates this identifier and the
///          complete private payload before reading socket, TLS, URL, or frame
///          state. It is part of the native runtime ABI.
#define RT_WEBSOCKET_CLASS_ID INT64_C(-0x720214)

//=========================================================================
// WebSocket - Creation
//=========================================================================

/// @brief Connect to a WebSocket server.
/// @details Uses the default 30-second timeout and performs TCP, optional TLS,
///          and RFC 6455 opening handshakes transactionally.
/// @param[in] url Managed WebSocket URL using `ws://` or `wss://`.
/// @return Caller-owned managed WebSocket connection, or NULL after a returning trap hook.
/// @note Traps on an invalid URL, connection error, TLS error, or handshake failure.
void *rt_ws_connect(rt_string url);

/// @brief Connect to a WebSocket server with timeout.
/// @param[in] url Managed WebSocket URL using `ws://` or `wss://`.
/// @param[in] timeout_ms Nonnegative connection and per-I/O timeout in milliseconds.
/// @return Caller-owned managed WebSocket connection, or NULL after a returning trap hook.
/// @note Traps on invalid input, connection error, timeout, TLS error, or handshake failure.
void *rt_ws_connect_for(rt_string url, int64_t timeout_ms);

/// @brief Connect to a WebSocket server and request a specific subprotocol.
/// @param[in] url Managed WebSocket URL using `ws://` or `wss://`.
/// @param[in] subprotocol Managed RFC token requested through `Sec-WebSocket-Protocol`.
/// @return Caller-owned managed WebSocket connection, or NULL after a returning trap hook.
/// @note Traps on invalid subprotocol, connection error, or handshake mismatch.
void *rt_ws_connect_protocol(rt_string url, rt_string subprotocol);

/// @brief Connect with timeout and request a specific WebSocket subprotocol.
/// @param[in] url Managed WebSocket URL using `ws://` or `wss://`.
/// @param[in] timeout_ms Nonnegative connection and per-I/O timeout in milliseconds.
/// @param[in] subprotocol Optional managed RFC token requested during the upgrade.
/// @return Caller-owned managed WebSocket connection, or NULL after a returning trap hook.
/// @note Traps on invalid subprotocol, timeout, or handshake mismatch.
void *rt_ws_connect_for_protocol(rt_string url, int64_t timeout_ms, rt_string subprotocol);

//=========================================================================
// WebSocket - Properties
//=========================================================================

/// @brief Get the connected URL.
/// @param[in] obj Managed WebSocket receiver, or NULL.
/// @return Caller-owned managed URL string; empty for NULL, or NULL after an
///         invalid-object trap.
rt_string rt_ws_url(void *obj);

/// @brief Check if connection is open.
/// @param[in] obj Managed WebSocket receiver, or NULL.
/// @return 1 if open; otherwise 0.
int8_t rt_ws_is_open(void *obj);

/// @brief Get the close code if connection was closed.
/// @param[in] obj Managed WebSocket receiver, or NULL.
/// @return Recorded close status, or 0 when no close status is available.
int64_t rt_ws_close_code(void *obj);

/// @brief Get the close reason if connection was closed.
/// @param[in] obj Managed WebSocket receiver, or NULL.
/// @return Caller-owned managed close reason; empty when absent, or NULL after
///         an invalid-object trap.
rt_string rt_ws_close_reason(void *obj);

/// @brief Get the negotiated subprotocol, if any.
/// @param[in] obj Managed WebSocket receiver, or NULL.
/// @return Caller-owned selected `Sec-WebSocket-Protocol` token; empty when
///         none was negotiated, or NULL after an invalid-object trap.
rt_string rt_ws_subprotocol(void *obj);

//=========================================================================
// WebSocket - Send Methods
//=========================================================================

/// @brief Send a text message.
/// @param[in] obj Managed WebSocket receiver; NULL is a no-op.
/// @param[in] text Managed UTF-8 payload; NULL sends an empty text frame.
/// @note Traps for an invalid receiver, invalid UTF-8, a closed connection, or send failure.
void rt_ws_send(void *obj, rt_string text);

/// @brief Send binary data.
/// @param[in] obj Managed WebSocket receiver; NULL is a no-op.
/// @param[in] data Managed Bytes payload.
/// @note Traps for invalid input, a closed connection, or send failure.
void rt_ws_send_bytes(void *obj, void *data);

/// @brief Send a ping frame.
/// @details Sends an empty Ping. Closed or NULL connections are ignored; send
///          failure records abnormal closure and retires the transport.
/// @param[in] obj Managed WebSocket receiver, or NULL.
void rt_ws_ping(void *obj);

//=========================================================================
// WebSocket - Receive Methods
//=========================================================================

/// @brief Receive a message (blocks until message arrives).
/// @param[in] obj Managed WebSocket receiver, or NULL.
/// @return Caller-owned managed String containing the next text or binary
///         payload bytes; empty on close/error, or NULL after an invalid-object trap.
/// @note Control frames (ping/pong/close) are handled automatically.
rt_string rt_ws_recv(void *obj);

/// @brief Receive a message with timeout.
/// @param[in] obj Managed WebSocket receiver.
/// @param[in] timeout_ms Nonnegative timeout in milliseconds; zero retains
///                       current blocking transport behavior.
/// @return Caller-owned managed String, or NULL on timeout, closure, invalid input,
///         or a returning trap hook.
rt_string rt_ws_recv_for(void *obj, int64_t timeout_ms);

/// @brief Receive binary data (blocks until message arrives).
/// @param[in] obj Managed WebSocket receiver, or NULL.
/// @return Caller-owned managed Bytes containing the next text or binary
///         payload; empty on close/error, or NULL after an invalid-object trap.
void *rt_ws_recv_bytes(void *obj);

/// @brief Receive binary data with timeout.
/// @param[in] obj Managed WebSocket receiver.
/// @param[in] timeout_ms Nonnegative timeout in milliseconds; zero retains
///                       current blocking transport behavior.
/// @return Caller-owned managed Bytes, or NULL on timeout, closure, invalid input,
///         or a returning trap hook.
void *rt_ws_recv_bytes_for(void *obj, int64_t timeout_ms);

//=========================================================================
// WebSocket - Close
//=========================================================================

/// @brief Close the connection gracefully.
/// @details Sends status 1000 with an empty reason and performs a bounded close handshake.
/// @param[in] obj Managed WebSocket receiver; NULL is a no-op.
void rt_ws_close(void *obj);

/// @brief Close the connection with a code and reason.
/// @details Sends the close frame, waits briefly for the peer reply, drains a
///          bounded number of in-flight frames, and deterministically retires
///          the TCP or TLS transport.
/// @param[in] obj Managed WebSocket receiver; NULL or an already closed receiver is a no-op.
/// @param[in] code Valid RFC 6455 or application close status.
/// @param[in] reason Optional managed UTF-8 reason of at most 123 bytes.
void rt_ws_close_with(void *obj, int64_t code, rt_string reason);

//=========================================================================
// WebSocket - Utilities (for testing)
//=========================================================================

/// @brief Compute the Sec-WebSocket-Accept header value for a given key.
///
/// Returns Base64(SHA1(key + WS_MAGIC)) as a C string that the caller
/// must free. Returns NULL on allocation failure. This function is exposed
/// for use by test servers that need to produce a valid handshake response.
///
/// @param[in] key_cstr Null-terminated Sec-WebSocket-Key value.
/// @return Newly allocated Sec-WebSocket-Accept string, or NULL for invalid
///         input or allocation failure. The caller must free the result.
char *rt_ws_compute_accept_key(const char *key_cstr);

#ifdef __cplusplus
}
#endif
