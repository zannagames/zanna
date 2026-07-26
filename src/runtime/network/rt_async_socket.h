//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/network/rt_async_socket.h
// Purpose: Future-based adapters that execute blocking TCP and one-shot HTTP
//          operations on a configurable shared worker pool.
// Key invariants:
//   - Every accepted operation owns a Promise plus immutable argument snapshots
//     or retained managed handles until its worker finishes.
//   - The shared pool is initialized once on success and may retry after failed
//     initialization; configuration is serialized with first use.
//   - Worker traps always settle their Future, even when copying the diagnostic
//     itself runs out of memory.
// Ownership/Lifetime:
//   - Each returned Future is a caller-owned managed reference.
//   - Result objects stored in Futures are managed and retained according to the
//     Promise transfer contract; callers release values obtained from Future.Get.
// Links: rt_async_socket.c (implementation), rt_network.h (blocking sockets),
//        rt_future.h (result transport), rt_threadpool.h (worker executor)
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Declares Future-based asynchronous TCP and HTTP adapters.
 * @details Defines pre-use shared-pool configuration and caller-owned Future
 * contracts for bounded connect, retained send/receive handles, and
 * snapshotted one-shot HTTP request arguments.
 */

#pragma once

#include "rt_string.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Configure the process-wide AsyncSocket worker count before first use.
/// @details The requested size replaces any earlier pre-initialization setting.
///          Once initialization begins or succeeds, the setting is immutable and
///          a later call traps. Without an explicit setting, the runtime uses
///          twice the detected CPU count with a floor of eight and cap of 1024.
/// @param size Worker count in the inclusive range 1..1024.
void rt_async_socket_set_pool_size(int64_t size);

/// @brief Asynchronously connect to a TCP endpoint with the default timeout.
/// @details Equivalent to @ref rt_async_connect_for with a 30-second overall
///          timeout. Host bytes are snapshotted before queueing, so the caller
///          may release @p host after this function returns.
/// @param host Non-empty managed String without embedded NUL bytes.
/// @param port TCP port in the inclusive range 1..65535.
/// @return Caller-owned Future resolving to a managed TCP connection object.
void *rt_async_connect(rt_string host, int64_t port);

/// @brief Asynchronously connect to host:port with an explicit timeout.
/// @details Uses one bounded connect operation on the shared worker Pool. Input
///          validation traps synchronously; recoverable Pool/setup failures are
///          represented by an already-failed Future.
/// @param host Non-empty managed String without embedded NUL bytes.
/// @param port TCP port in the inclusive range 1..65535.
/// @param timeout_ms Overall timeout in the range 0..INT_MAX milliseconds.
/// @return Caller-owned Future resolving to a managed TCP connection object.
void *rt_async_connect_for(rt_string host, int64_t port, int64_t timeout_ms);

/// @brief Asynchronously send one Bytes payload over a TCP connection.
/// @details The TCP and Bytes handles are retained and revalidated before they
///          cross the worker boundary. The operation mirrors @c Tcp.Send and may
///          therefore report a partial write. Its numeric result is a managed
///          boxed Integer, never a pointer-cast ABI scalar.
/// @param tcp Live managed TCP connection object.
/// @param data Live managed Bytes object; an empty payload is permitted.
/// @return Caller-owned Future resolving to a boxed Integer byte count.
void *rt_async_send(void *tcp, void *data);

/// @brief Asynchronously receive data. Returns a Future[Bytes].
/// @details Retains and revalidates @p tcp before queueing. A zero byte limit
///          resolves to an empty Bytes object without reading the socket.
/// @param tcp Live managed TCP connection object.
/// @param max_bytes Maximum receive size in the range 0..INT_MAX.
/// @return Caller-owned Future resolving to a managed Bytes object.
void *rt_async_recv(void *tcp, int64_t max_bytes);

/// @brief Asynchronously perform HTTP GET. Returns a Future[String].
/// @details Snapshots the exact URL bytes before dispatching the blocking
///          one-shot HTTP client. Empty and embedded-NUL URLs trap synchronously.
/// @param url Non-empty managed URL String without embedded NUL bytes.
/// @return Caller-owned Future resolving to the response body String.
void *rt_async_http_get(rt_string url);

/// @brief Asynchronously perform HTTP POST. Returns a Future[String].
/// @details Snapshots URL and body bytes before queueing. NULL @p body is
///          normalized to an empty String; a managed body preserves its exact
///          length, including embedded NUL bytes.
/// @param url Non-empty managed URL String without embedded NUL bytes.
/// @param body Managed request-body String, or NULL for an empty body.
/// @return Caller-owned Future resolving to the response body String.
void *rt_async_http_post(rt_string url, rt_string body);

#ifdef __cplusplus
}
#endif
