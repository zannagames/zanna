//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/network/rt_ws_server.h
// Purpose: WebSocket server that accepts upgrade requests on a TCP port.
// Key invariants:
//   - Performs HTTP upgrade handshake per RFC 6455.
//   - Every public server receiver validates stable managed identity before access.
//   - Background workers drain and validate client frames, including control traffic.
//   - Per-client serialization prevents broadcast/control-frame byte interleaving.
// Ownership/Lifetime:
//   - Constructors return one caller-owned managed server reference.
//   - Accepted client handles use the stable Tcp managed identity.
//   - Stop and finalization retire listeners, pending handshakes, and active clients.
// Links: rt_websocket.h (client-side framing), rt_network.h (TCP)
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_ws_server.h
 * @brief Declares the managed plaintext WebSocket server API.
 * @details Servers support restartable listener lifecycles, optional required
 *          subprotocols, synchronized client counts, broadcast, compatibility
 *          accept, and per-client receive and send operations. Stop retires
 *          pending handshakes and active clients before returning.
 */

#pragma once

#include "rt_string.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Stable managed identity for `Zanna.Network.WsServer` objects.
/// @details Every non-null public server receiver validates this identifier and
///          the complete private payload before reading lifecycle, listener, or
///          client-table state. It is part of the native runtime ABI.
#define RT_WS_SERVER_CLASS_ID INT64_C(-0x720215)

/// @brief Create a WebSocket server configured for the given local port.
/// @param[in] port Port in 0..65535; zero requests an ephemeral port at Start.
/// @return Caller-owned managed server object, or NULL after a returning trap.
void *rt_ws_server_new(int64_t port);
/// @brief Begin listening and create the bounded background worker pool.
/// @details Idempotent while running. Port zero is replaced atomically with
///          the assigned port. Bind, pool, or accept-thread failure restores a
///          stopped state before propagating its trap.
/// @param[in] server Managed WsServer receiver; NULL is a no-op.
void rt_ws_server_start(void *server);
/// @brief Stop acceptance and synchronously retire queued and active clients.
/// @details Interrupts incomplete upgrades, joins the accept thread and every
///          task, shuts down/releases the worker pool, and permits a later Start.
/// @param[in] server Managed WsServer receiver; NULL is a no-op.
void rt_ws_server_stop(void *server);
/// @brief Require one exact HTTP token for future upgrades while stopped.
/// @param[in] server Managed WsServer receiver.
/// @param[in] subprotocol Managed token String, or NULL/empty to clear the policy.
void rt_ws_server_set_subprotocol(void *server, rt_string subprotocol);
/// @brief Send one exact managed String as an unmasked text frame to active clients.
/// @param[in] server Managed WsServer receiver.
/// @param[in] message Managed String; embedded NUL bytes remain payload data.
void rt_ws_server_broadcast(void *server, rt_string message);
/// @brief Send one exact managed Bytes value as an unmasked binary frame.
/// @param[in] server Managed WsServer receiver.
/// @param[in] data Managed Bytes payload.
void rt_ws_server_broadcast_bytes(void *server, void *data);
/// @brief Return the synchronized count of fully upgraded active clients.
/// @param[in] server Managed WsServer receiver.
/// @return Number of active upgraded clients, or zero for NULL or invalid receivers.
int64_t rt_ws_server_client_count(void *server);
/// @brief Return a caller-owned managed snapshot of the required subprotocol.
/// @param[in] server Managed WsServer receiver.
/// @return Required token or an empty managed String; NULL after an
///         invalid-object or allocation trap.
rt_string rt_ws_server_subprotocol(void *server);
/// @brief Return the configured or currently assigned listening port.
/// @param[in] server Managed WsServer receiver.
/// @return Configured port before Start or actual bound port afterward; zero
///         for NULL or invalid receivers.
int64_t rt_ws_server_port(void *server);
/// @brief Return one while the serialized background lifecycle is running.
/// @param[in] server Managed WsServer receiver.
/// @return 1 while running; otherwise 0.
int8_t rt_ws_server_is_running(void *server);
/// @brief Compatibility helper that blocks for and upgrades one plain client.
/// @details The listener and subprotocol are retained snapshots, so concurrent
///          Stop cannot invalidate their storage. Do not race it with the
///          background accept loop when deterministic connection ownership is required.
/// @param[in] server Managed WsServer receiver.
/// @return Caller-owned managed TCP client, or NULL on rejection/shutdown.
void *rt_ws_server_accept(void *server);
/// @brief Receive and assemble the next text or binary message from a client.
/// @details Handles interleaved control frames, fragmentation, aggregate size,
///          and text UTF-8 validation. Result publication is allocation-transactional.
/// @param[in] client Managed TCP handle returned by Accept.
/// @return Caller-owned message String, empty on close, or NULL after validation trap.
rt_string rt_ws_server_client_recv(void *client);
/// @brief Send one exact text message to a synchronously accepted client.
/// @param[in] client Valid managed TCP handle.
/// @param[in] message Valid managed String; embedded NUL bytes are preserved.
void rt_ws_server_client_send(void *client, rt_string message);
/// @brief Best-effort send an empty Close frame and close one accepted TCP client.
/// @param[in] client Valid managed TCP handle; NULL is a no-op.
void rt_ws_server_client_close(void *client);

#ifdef __cplusplus
}
#endif
