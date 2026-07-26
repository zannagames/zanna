//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/network/rt_connpool.h
// Purpose: Thread-safe plain-TCP connection pooling for HTTP keep-alive.
// Key invariants:
//   - Connections are keyed by exact host bytes and port.
//   - Pool is thread-safe (internal mutex).
//   - Idle connections are evicted after 60 seconds during Acquire().
// Ownership/Lifetime:
//   - Pool objects are GC-managed via rt_obj_set_finalizer.
//   - Tracked entries retain an independent TCP reference. Release does not
//     consume the caller's reference, and Clear detaches checked-out entries.
// Links: rt_network.h (TCP)
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Declares the managed plain-TCP ConnectionPool API.
 * @details Defines validated pool identity, bounded construction, exact
 * endpoint acquisition, exclusive connection return, safe clearing, and
 * synchronized tracked and idle entry counts.
 */

#pragma once

#include "rt_string.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Stable runtime class identifier for managed ConnectionPool handles.
/// @details Public pool methods validate this identity and the complete native
///          payload size before locking or dereferencing a caller-supplied
///          object.
#define RT_CONNPOOL_CLASS_ID INT64_C(-0x720205)

/// @brief Create a new connection pool.
/// @details The requested capacity is clamped to the inclusive range [1, 128].
///          The returned pool serializes all bookkeeping internally and owns
///          an independent reference to every tracked TCP connection.
/// @param max_size Maximum number of pooled connections.
/// @return New GC-managed ConnectionPool, or NULL after an allocation or
///         synchronization-initialization trap.
void *rt_connpool_new(int64_t max_size);

/// @brief Acquire a plain TCP connection from the pool (or create new).
/// @details Reuses an exact endpoint match only when it is idle, within the
///          idle-age limit, open, and free of unread protocol bytes. A fresh
///          connection beyond tracked capacity is still returned but remains
///          untracked until a later Release can adopt it.
/// @param pool Live ConnectionPool handle.
/// @param host Non-empty host string without embedded NUL bytes.
/// @param port Remote port in the inclusive range [1, 65535].
/// @return Caller-owned TCP handle, or NULL after a connection trap.
void *rt_connpool_acquire(void *pool, rt_string host, int64_t port);

/// @brief Transfer a connection back to the pool for reuse (or closure when full/unhealthy).
/// @details The caller keeps its managed reference but must stop using the
///          transport after release. Adoption is exclusive: a TCP currently
///          leased to another pool is rejected rather than aliased between
///          pools.
/// @param pool Destination ConnectionPool; NULL is a no-op.
/// @param conn TCP handle to return; NULL is a no-op.
void rt_connpool_release(void *pool, void *conn);

/// @brief Remove every tracked connection without invalidating active borrowers.
/// @details Idle transports are closed. Checked-out transports are detached
///          and remain usable through their caller-owned references.
/// @param pool ConnectionPool to clear; NULL is a no-op.
void rt_connpool_clear(void *pool);

/// @brief Get number of connections currently in the pool.
/// @param pool ConnectionPool to inspect; NULL returns zero.
/// @return Number of tracked idle plus checked-out entries.
int64_t rt_connpool_size(void *pool);

/// @brief Get number of idle (available) connections.
/// @param pool ConnectionPool to inspect; NULL returns zero.
/// @return Number of tracked entries eligible for a later health probe.
int64_t rt_connpool_available(void *pool);

#ifdef __cplusplus
}
#endif
