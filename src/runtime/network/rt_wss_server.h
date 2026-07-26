//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/network/rt_wss_server.h
// Purpose: TLS-backed WebSocket server using the in-tree TLS 1.3 runtime.
// Key invariants:
//   - Every public receiver validates stable managed identity before payload access.
//   - TLS credentials are parsed once and remain immutable across Start/Stop cycles.
//   - Client frame writes are serialized per connection and server frames are unmasked.
// Ownership/Lifetime:
//   - Constructors return one caller-owned managed server reference.
//   - Stop/finalization retire the listener, handshakes, TLS sessions, and workers.
// Links: rt_wss_server.c, rt_tls_server_internal.h, rt_websocket.h
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_wss_server.h
 * @brief Declares the managed TLS-backed WebSocket server API.
 * @details Construction copies certificate and key paths and prepares an
 *          immutable TLS context. The API provides restartable serving,
 *          subprotocol policy, synchronized client state, and text or binary
 *          broadcast while Stop drains every pending and active connection.
 */

#pragma once

#include "rt_string.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Stable managed identity for `Zanna.Network.WssServer` objects.
/// @details The identifier and complete private payload are validated before
///          lifecycle, TLS-context, credential, or client-table state is read.
///          It is part of the native runtime ABI.
#define RT_WSS_SERVER_CLASS_ID INT64_C(-0x720216)

/// @brief Create a TLS-backed WebSocket server without starting its listener.
/// @details Copies the paths and creates the immutable TLS server context before
///          returning; listener binding remains lazy until Start.
/// @param[in] port Port in 0..65535; zero requests an ephemeral port at Start.
/// @param[in] cert_file Non-empty managed path to a PEM certificate chain.
/// @param[in] key_file Non-empty managed path to the matching PEM private key.
/// @return Caller-owned managed server object, or NULL after a returning trap.
void *rt_wss_server_new(int64_t port, rt_string cert_file, rt_string key_file);
/// @brief Start accepting secure WebSocket connections; idempotent while running.
/// @details Lazily creates a bounded pool and transactionally restores stopped
///          state after bind, pool, or accept-thread failure.
/// @param[in] server Managed WssServer receiver; NULL is a no-op.
void rt_wss_server_start(void *server);
/// @brief Stop acceptance and synchronously retire pending and active connections.
/// @details Interrupts raw, detached-handshake, and TLS descriptors, joins all
///          tasks, and releases the worker pool before returning.
/// @param[in] server Managed WssServer receiver; NULL is a no-op.
void rt_wss_server_stop(void *server);
/// @brief Require one subprotocol token for future upgrades while stopped.
/// @param[in] server Managed WssServer receiver.
/// @param[in] subprotocol Managed HTTP token, or NULL/empty to clear the policy.
void rt_wss_server_set_subprotocol(void *server, rt_string subprotocol);
/// @brief Broadcast one exact managed String payload as an unmasked text frame.
/// @param[in] server Managed WssServer receiver.
/// @param[in] message Managed String; embedded NUL bytes remain payload data.
void rt_wss_server_broadcast(void *server, rt_string message);
/// @brief Broadcast one exact managed Bytes payload as an unmasked binary frame.
/// @param[in] server Managed WssServer receiver.
/// @param[in] data Managed Bytes payload.
void rt_wss_server_broadcast_bytes(void *server, void *data);
/// @brief Return a synchronized count of upgraded active clients.
/// @param[in] server Managed WssServer receiver.
/// @return Number of fully upgraded active TLS clients, or zero for NULL or invalid receivers.
int64_t rt_wss_server_client_count(void *server);
/// @brief Return a caller-owned copy of the required subprotocol, or empty String.
/// @param[in] server Managed WssServer receiver.
/// @return Required token or an empty managed String; NULL after an
///         invalid-object or allocation trap.
rt_string rt_wss_server_subprotocol(void *server);
/// @brief Return the configured or currently bound port.
/// @param[in] server Managed WssServer receiver.
/// @return Configured port before Start or actual bound port afterward; zero
///         for NULL or invalid receivers.
int64_t rt_wss_server_port(void *server);
/// @brief Return one while the background accept lifecycle is running.
/// @param[in] server Managed WssServer receiver.
/// @return 1 while running; otherwise 0.
int8_t rt_wss_server_is_running(void *server);

#ifdef __cplusplus
}
#endif
