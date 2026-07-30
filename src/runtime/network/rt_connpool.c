//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/network/rt_connpool.c
// Purpose: Thread-safe TCP connection pooling for HTTP keep-alive.
// Key invariants:
//   - Connections are keyed by an allocation-free hash plus exact comparison
//     of the immutable endpoint stored by each TCP handle.
//   - Internal mutex protects all pool operations.
//   - A TCP lease token prevents the same socket from being tracked by two pools.
//   - Idle connections closed after 60 seconds.
// Ownership/Lifetime:
//   - Pool objects are GC-managed. Tracked entries hold their own retained
//     reference; Acquire returns a caller-owned (retained) handle.
//   - Clear/finalize close idle entries only; checked-out handles are
//     detached, never closed out from under their holder.
// Links: rt_connpool.h (API)
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Implements synchronized plain-TCP connection pooling.
 * @details Keys reusable transports by exact immutable endpoints, uses atomic
 * lease tokens to prevent cross-pool aliasing, probes idle socket health,
 * expires entries on a monotonic clock, and safely detaches checked-out
 * transports during clear and finalization.
 */

#include "rt_connpool.h"

#include "rt_heap.h"
#include "rt_network_internal.h"
#include "rt_object.h"
#include "rt_string.h"
#include "rt_time.h"
#include "rt_trap.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if RT_PLATFORM_WINDOWS
typedef CRITICAL_SECTION pool_mutex_t;
#else
#include <pthread.h>
typedef pthread_mutex_t pool_mutex_t;
#endif

//=============================================================================
// Internal Structures
//=============================================================================

/** Compile-time capacity of the fixed ConnectionPool entry array. */
#define POOL_MAX_ENTRIES 128
/** Maximum permitted idle age before an entry is evicted. */
#define POOL_IDLE_TIMEOUT_MS (60LL * 1000LL)
/** Payload sentinel used with class and size checks to reject forged handles. */
#define RT_CONNPOOL_MAGIC UINT64_C(0x5A434F4E4E504F4C)

/** One pool-owned TCP reference and its reuse metadata. */
typedef struct {
    void *tcp;              ///< Pool-retained TCP connection object.
    uint64_t endpoint_hash; ///< Hash of exact host bytes plus remote port.
    int64_t last_used_ms;   ///< Monotonic tick when the connection was returned.
    bool in_use;            ///< Whether a caller currently has the entry checked out.
} pooled_entry_t;

/** Pool-owned TCP reference detached for cleanup after unlocking. */
typedef struct {
    void *tcp;            ///< Pool-retained reference to release.
    uint64_t owner_token; ///< Lease token to release after optional close.
    bool close_transport; ///< Whether cleanup closes the TCP first.
    bool owns_claim;      ///< Whether cleanup still holds @ref owner_token.
} connpool_discard_t;

/** Complete managed ConnectionPool payload and native synchronization state. */
typedef struct {
    uint64_t magic;                           ///< Initialized payload sentinel.
    uint64_t owner_token;                     ///< Exclusive TCP lease identity.
    pooled_entry_t entries[POOL_MAX_ENTRIES]; ///< Fixed tracked-entry storage.
    int count;                                ///< Number of active array slots.
    int max_size;                             ///< Configured active-slot ceiling.
    pool_mutex_t lock;                        ///< Native bookkeeping mutex.
    bool lock_initialized;                    ///< Whether @ref lock may be used.
} rt_connpool_impl;

//=============================================================================
// Helpers
//=============================================================================

/** Atomic source of nonzero process-local pool lease identities. */
static volatile uint64_t g_connpool_owner_sequence = 1;

/// @brief Initialize one platform mutex for a ConnectionPool.
/// @details Windows uses a modest spin count before kernel waiting; POSIX uses
///          the default non-recursive mutex attributes. Unlike the former
///          macro, this helper reports initialization failure to the
///          constructor instead of publishing an unusable pool.
/// @param mutex Native mutex storage to initialize.
/// @return True on success, false otherwise.
static bool connpool_mutex_init(pool_mutex_t *mutex) {
    if (!mutex)
        return false;
#if RT_PLATFORM_WINDOWS
    return InitializeCriticalSectionAndSpinCount(mutex, 4000) != 0;
#else
    return pthread_mutex_init(mutex, NULL) == 0;
#endif
}

/// @brief Acquire a ConnectionPool mutex.
/// @param mutex Initialized native mutex.
/// @return True when the caller owns the mutex, false on a native error.
static bool connpool_mutex_lock(pool_mutex_t *mutex) {
    if (!mutex)
        return false;
#if RT_PLATFORM_WINDOWS
    EnterCriticalSection(mutex);
    return true;
#else
    return pthread_mutex_lock(mutex) == 0;
#endif
}

/// @brief Release a ConnectionPool mutex or terminate on invariant failure.
/// @details An unlock failure means ownership bookkeeping is already corrupt,
///          so continuing under a returning trap hook would permit concurrent
///          unsynchronized mutation. Such an internal failure is therefore
///          non-recoverable.
/// @param mutex Locked native mutex owned by the current thread.
static void connpool_mutex_unlock(pool_mutex_t *mutex) {
#if RT_PLATFORM_WINDOWS
    LeaveCriticalSection(mutex);
#else
    if (!mutex || pthread_mutex_unlock(mutex) != 0)
        rt_abort("ConnectionPool: mutex unlock failed");
#endif
}

/// @brief Destroy a successfully initialized ConnectionPool mutex.
/// @param mutex Initialized, unlocked native mutex.
static void connpool_mutex_destroy(pool_mutex_t *mutex) {
    if (!mutex)
        return;
#if RT_PLATFORM_WINDOWS
    DeleteCriticalSection(mutex);
#else
    if (pthread_mutex_destroy(mutex) != 0)
        rt_abort("ConnectionPool: mutex destroy failed");
#endif
}

/// @brief Allocate a nonzero process-local identity for a new pool.
/// @details The unsigned counter deliberately wraps. Zero is reserved for an
///          unleased TCP and is skipped; exhausting the remaining 2^64-1
///          identities within one process lifetime is not operationally
///          reachable.
/// @return Nonzero token used for atomic TCP lease ownership.
static uint64_t connpool_next_owner_token(void) {
    uint64_t token;
    do {
        token = rt_atomic_fetch_add_u64(&g_connpool_owner_sequence, 1, __ATOMIC_RELAXED);
    } while (token == 0);
    return token;
}

/// @brief Release one owned managed object reference.
/// @param obj Owned object reference; NULL is a no-op.
static void connpool_release_object(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief Validate and retain a non-null ConnectionPool for one public call.
/// @details Class id, payload size, initialization magic, and lock state are
///          checked before the native mutex is touched. A non-trapping live
///          retain prevents finalization from destroying the mutex while the
///          call is active. Callers preserve their historical NULL semantics
///          before invoking this helper.
/// @param obj Caller-supplied pool handle.
/// @param function_name API name included in invalid-handle diagnostics.
/// @return Retained pool implementation, or NULL after one trap.
static rt_connpool_impl *connpool_require_retained(void *obj, const char *function_name) {
    if (!obj)
        return NULL;
    int32_t retained = rt_heap_try_retain_live(obj);
    if (retained != 1) {
        rt_trap(retained < 0 ? "ConnectionPool: reference count overflow"
                             : "ConnectionPool: stale object");
        return NULL;
    }
    if (!rt_obj_is_instance(obj, RT_CONNPOOL_CLASS_ID, sizeof(rt_connpool_impl)) ||
        ((rt_connpool_impl *)obj)->magic != RT_CONNPOOL_MAGIC ||
        !((rt_connpool_impl *)obj)->lock_initialized) {
        char error[160];
        snprintf(error,
                 sizeof(error),
                 "%s: invalid ConnectionPool object",
                 function_name ? function_name : "ConnectionPool");
        connpool_release_object(obj);
        rt_trap(error);
        return NULL;
    }
    return (rt_connpool_impl *)obj;
}

/// @brief Hash exact endpoint bytes and port for a fast pool prefilter.
/// @details FNV-1a is sufficient because equality is always confirmed against
///          the immutable TCP endpoint; collisions affect only comparison cost.
/// @param host Exact host bytes.
/// @param host_len Number of host bytes.
/// @param port Remote port in [1, 65535].
/// @return Deterministic 64-bit endpoint hash.
static uint64_t connpool_endpoint_hash(const char *host, size_t host_len, int port) {
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < host_len; i++) {
        hash ^= (uint8_t)host[i];
        hash *= UINT64_C(1099511628211);
    }
    hash ^= (uint8_t)(port >> 8);
    hash *= UINT64_C(1099511628211);
    hash ^= (uint8_t)port;
    hash *= UINT64_C(1099511628211);
    return hash;
}

/// @brief Compare one pooled TCP against an exact requested endpoint.
/// @details The cached hash rejects most mismatches before borrowing the TCP's
///          immutable host view. Final equality remains byte-exact and
///          case-sensitive, preserving the existing pool-key contract.
/// @param entry Candidate pooled entry.
/// @param endpoint_hash Precomputed request endpoint hash.
/// @param host Exact request host bytes.
/// @param host_len Number of request host bytes.
/// @param port Requested remote port.
/// @return True only for an exact endpoint match.
static bool connpool_entry_matches(const pooled_entry_t *entry,
                                   uint64_t endpoint_hash,
                                   const char *host,
                                   size_t host_len,
                                   int port) {
    if (!entry || !entry->tcp || entry->endpoint_hash != endpoint_hash)
        return false;
    const char *entry_host = NULL;
    size_t entry_host_len = 0;
    int entry_port = 0;
    return rt_tcp_endpoint_view(entry->tcp, &entry_host, &entry_host_len, &entry_port) &&
           entry_port == port && entry_host_len == host_len &&
           memcmp(entry_host, host, host_len) == 0;
}

/// @brief Read the monotonic millisecond clock used for idle-age bookkeeping.
/// @details Connection reuse decisions must not depend on wall-clock time:
///          NTP adjustments, daylight-saving changes, or manual clock edits
///          should not make idle sockets live forever or expire immediately.
/// @return Monotonic milliseconds from the runtime clock's unspecified epoch.
static int64_t connpool_now_ms(void) {
    return rt_clock_ticks_us() / 1000;
}

/// @brief Return whether an idle entry has exceeded the pool timeout.
/// @details Backward clock deltas are clamped to zero defensively even though
///          the source is monotonic.
/// @param entry Entry being considered for eviction.
/// @param now_ms Current monotonic tick in milliseconds.
/// @return True when the entry is idle long enough to close.
static bool connpool_entry_is_idle_expired(const pooled_entry_t *entry, int64_t now_ms) {
    if (!entry || entry->in_use)
        return false;
    int64_t age_ms = now_ms >= entry->last_used_ms ? now_ms - entry->last_used_ms : 0;
    return age_ms >= POOL_IDLE_TIMEOUT_MS;
}

/// @brief Detach a pool entry for destruction after the bookkeeping unlock.
/// @details An idle transport that will be closed keeps the pool's lease until
///          deferred cleanup, preventing another pool from adopting it in the
///          unlock-to-close interval. A checked-out transport is unclaimed
///          immediately so its borrower can return it after a concurrent clear.
/// @param pool Pool that owns @p entry.
/// @param entry Entry to detach and zero.
/// @param close_transport Whether an exclusively owned transport should close.
/// @param discard Destination that assumes the entry's managed reference.
static void detach_entry(rt_connpool_impl *pool,
                         pooled_entry_t *entry,
                         bool close_transport,
                         connpool_discard_t *discard) {
    if (!pool || !entry || !discard)
        return;
    memset(discard, 0, sizeof(*discard));
    discard->tcp = entry->tcp;
    if (discard->tcp) {
        uint64_t owner = rt_tcp_pool_owner(discard->tcp);
        if (owner == pool->owner_token) {
            discard->owner_token = pool->owner_token;
            discard->close_transport = close_transport;
            discard->owns_claim = close_transport;
            if (!close_transport)
                (void)rt_tcp_pool_release_claim(discard->tcp, pool->owner_token);
        }
    }
    memset(entry, 0, sizeof(*entry));
}

/// @brief Finish one detached TCP cleanup without the pool mutex held.
/// @param discard Detached ownership record to consume.
static void connpool_discard_release(connpool_discard_t *discard) {
    if (!discard)
        return;
    if (discard->tcp) {
        if (discard->close_transport && discard->owns_claim)
            rt_tcp_close(discard->tcp);
        if (discard->owns_claim)
            (void)rt_tcp_pool_release_claim(discard->tcp, discard->owner_token);
        connpool_release_object(discard->tcp);
    }
    memset(discard, 0, sizeof(*discard));
}

/// @brief Drop a pool entry's lease and managed reference immediately.
/// @details Reserved for quiescent finalization; synchronized public paths
///          detach under the mutex and invoke cleanup only after unlocking.
/// @param pool Pool that owns @p entry.
/// @param entry Entry to discard.
/// @param close_transport Whether an exclusively owned transport should close.
static void discard_entry(rt_connpool_impl *pool, pooled_entry_t *entry, bool close_transport) {
    if (!pool || !entry)
        return;
    connpool_discard_t discard;
    detach_entry(pool, entry, close_transport, &discard);
    connpool_discard_release(&discard);
}

/// @brief Remove the entry at @p index using swap-with-last for O(1) deletion.
/// @details Order within the @c entries array is not meaningful — slots are
///          looked up linearly by `(key, in_use)` — so the cheap swap is
///          preferred over a memmove. Ownership is detached into @p discard
///          for cleanup after the caller releases the pool mutex.
/// @param pool Locked pool containing the entry.
/// @param index Zero-based entry index to remove; invalid values are ignored.
/// @param close_transport Whether to close a transport still leased to @p pool.
/// @param discard Destination for the removed entry's ownership.
static void remove_entry_at(rt_connpool_impl *pool,
                            int index,
                            bool close_transport,
                            connpool_discard_t *discard) {
    if (!pool || !discard || index < 0 || index >= pool->count)
        return;
    detach_entry(pool, &pool->entries[index], close_transport, discard);
    pool->entries[index] = pool->entries[pool->count - 1];
    memset(&pool->entries[pool->count - 1], 0, sizeof(pooled_entry_t));
    pool->count--;
}

/// @brief Insert a pre-retained TCP reference as a new tracked entry.
/// @details Used by both `acquire` (when opening a fresh connection that
///          should immediately appear as checked-out) and `release` (when
///          a caller returns an unknown connection that the pool may
///          want to keep). @p tcp_ref is already an owned reference and is
///          consumed only on success. Claiming the TCP lease is atomic, so a
///          connection owned by another pool is rejected without mutation.
/// @param pool Destination pool, locked by the caller.
/// @param tcp_ref Pre-retained TCP reference to consume on success.
/// @param endpoint_hash Exact endpoint hash cached in the new entry.
/// @param in_use Whether the connection is immediately checked out.
/// @param last_used_ms Idle timestamp; ignored while @p in_use is true.
/// @return True when the reference was consumed and entry appended.
static bool track_connection(rt_connpool_impl *pool,
                             void *tcp_ref,
                             uint64_t endpoint_hash,
                             bool in_use,
                             int64_t last_used_ms) {
    if (!pool || !tcp_ref || pool->count >= pool->max_size ||
        !rt_tcp_pool_try_claim(tcp_ref, pool->owner_token))
        return false;

    pooled_entry_t *entry = &pool->entries[pool->count++];
    memset(entry, 0, sizeof(*entry));
    entry->tcp = tcp_ref;
    entry->endpoint_hash = endpoint_hash;
    entry->last_used_ms = last_used_ms;
    entry->in_use = in_use;
    return true;
}

/// @brief Probe a pooled TCP connection to see if the peer has silently closed it.
/// @details A connection sitting idle in the pool can be closed by the peer
///          (server timeout, NAT tear-down, etc.) without our side noticing
///          until the next write fails. Detecting that *before* handing the
///          connection to a caller avoids spurious request failures.
///
///          The probe uses a non-blocking `select` + `MSG_PEEK` to look at
///          whatever bytes (if any) are sitting in the receive buffer:
///          - `wait_socket(..., 0, ...)` with a zero timeout returns >0 if
///            the socket has readable data (peer wrote, *or* peer closed —
///            both produce a readable signal).
///          - If nothing's readable, the connection is healthy (idle but
///            still open).
///          - If something *is* readable, peek one byte without consuming it:
///            * `peek > 0` — stale protocol bytes are queued; reusing the
///              socket would cross-contaminate the next request, so it is
///              rejected and closed.
///            * `peek == 0` — orderly close from the peer; the connection
///              is dead.
///            * `peek < 0` with EAGAIN/EWOULDBLOCK — race between select
///              and peek; treat as healthy.
///            * `peek < 0` with any other errno — connection is broken.
/// @param tcp Pooled TCP connection object.
/// @return 1 if the connection is healthy and reusable, 0 if it should be
///         closed and removed from the pool.
static int tcp_connection_healthy(void *tcp) {
    if (!rt_tcp_is_handle(tcp) || !rt_tcp_is_open(tcp))
        return 0;

    socket_t sock = rt_tcp_socket_fd(tcp);
    if (sock == INVALID_SOCK)
        return 0;

    int ready = wait_socket(sock, 0, false);
    if (ready < 0)
        return 0;
    if (ready == 0)
        return 1;

    uint8_t byte = 0;
    int peeked;
    do {
        peeked = recv(sock, (char *)&byte, 1, MSG_PEEK);
    } while (peeked < 0 && rt_socket_error_is_interrupted(GET_LAST_ERROR()));
    if (peeked > 0)
        return 0; // pending unread bytes: stale protocol data, do not reuse
    if (peeked == 0)
        return 0;
    return rt_socket_error_is_would_block(GET_LAST_ERROR()) ? 1 : 0;
}

/// @brief Claim an untracked TCP for deferred close outside the pool mutex.
/// @details A zero owner is first claimed for this pool, preventing another
///          pool from adopting the connection between the ownership check and
///          deferred close. An existing same-pool token is accepted. A foreign
///          token is never overwritten and leaves the transport untouched.
/// @param pool Pool attempting to close the untracked handle.
/// @param tcp Valid, retained TCP handle.
/// @return True when this pool owns the close lease; false for a foreign race.
static bool connpool_claim_untracked(rt_connpool_impl *pool, void *tcp) {
    return pool && tcp && rt_tcp_pool_try_claim(tcp, pool->owner_token);
}

/// @brief Close a TCP while holding this pool's previously acquired lease.
/// @param pool Pool whose token protects @p tcp.
/// @param tcp Valid, retained TCP handle.
static void connpool_close_claimed(rt_connpool_impl *pool, void *tcp) {
    if (!pool || !tcp)
        return;
    rt_tcp_close(tcp);
    (void)rt_tcp_pool_release_claim(tcp, pool->owner_token);
}

//=============================================================================
// Finalizer
//=============================================================================

/// @brief GC finalizer: close idle pooled connections, detach checked-out
/// ones (their holders keep working handles), and destroy the pool's mutex.
/// @details Every public call retains the pool through its final unlock, so a
///          finalizer cannot overlap live mutex users. The sweep therefore
///          needs no lock and may safely destroy it afterward.
/// @param obj Fully initialized ConnectionPool payload at zero references.
static void rt_connpool_finalize(void *obj) {
    if (!obj)
        return;
    rt_connpool_impl *pool = (rt_connpool_impl *)obj;
    if (pool->magic != RT_CONNPOOL_MAGIC)
        return;
    for (int i = 0; i < pool->count; i++)
        discard_entry(pool, &pool->entries[i], !pool->entries[i].in_use);
    pool->count = 0;
    if (pool->lock_initialized) {
        connpool_mutex_destroy(&pool->lock);
        pool->lock_initialized = false;
    }
    pool->magic = 0;
}

//=============================================================================
// Public API
//=============================================================================

/// @brief Construct a TCP connection pool with a bounded tracked capacity.
/// @details `max_size` is clamped to `[1, POOL_MAX_ENTRIES]`. Mutex
///          initialization must succeed before class magic/finalizer
///          publication, so no caller can observe a partially synchronized
///          instance.
/// @param max_size Requested number of tracked idle plus checked-out entries.
/// @return New GC-managed pool, or NULL after one allocation/init trap.
void *rt_connpool_new(int64_t max_size) {
    rt_connpool_impl *pool =
        (rt_connpool_impl *)rt_obj_new_i64(RT_CONNPOOL_CLASS_ID, (int64_t)sizeof(rt_connpool_impl));
    if (!pool)
        return NULL;
    if (max_size < 1)
        max_size = 1;
    if (max_size > POOL_MAX_ENTRIES)
        max_size = POOL_MAX_ENTRIES;
    pool->max_size = (int)max_size;
    if (!connpool_mutex_init(&pool->lock)) {
        connpool_release_object(pool);
        rt_trap("ConnectionPool: mutex initialization failed");
        return NULL;
    }
    pool->lock_initialized = true;
    pool->owner_token = connpool_next_owner_token();
    pool->magic = RT_CONNPOOL_MAGIC;
    rt_obj_set_finalizer(pool, rt_connpool_finalize);
    return pool;
}

/// @brief Acquire a TCP connection to `(host, port)`. Walk-the-pool flow:
///   1. Evict expired idle entries (`POOL_IDLE_TIMEOUT_MS` since last_used_ms).
///   2. Find an idle exact endpoint match; if its TCP is healthy, reuse it.
///   3. If unhealthy, close + remove and keep searching.
///   4. If nothing matches, open a fresh TCP connection and immediately track
///      it as checked out when pool capacity allows.
/// @details Pool bookkeeping is locked, while the potentially blocking connect
///          runs unlocked. Reuse takes a non-trapping live retain before
///          publication, preventing a refcount trap from stranding `in_use`.
/// @param obj Required ConnectionPool receiver.
/// @param host Non-empty complete runtime string without embedded NUL bytes.
/// @param port Remote port in [1, 65535].
/// @return Caller-owned TCP handle, or NULL after one trap.
void *rt_connpool_acquire(void *obj, rt_string host, int64_t port) {
    if (!obj) {
        rt_trap("ConnectionPool: NULL pool");
        return NULL;
    }

    rt_connpool_impl *pool = connpool_require_retained(obj, "ConnectionPool.Acquire");
    if (!pool)
        return NULL;

    const char *host_str = NULL;
    size_t host_len = 0;
    if (!host || !rt_string_is_handle((const void *)host) ||
        !rt_net_cstr_no_embedded_nul(host, &host_str, &host_len) || host_len == 0 || port < 1 ||
        port > 65535) {
        connpool_release_object(pool);
        rt_trap("ConnectionPool: invalid host or port");
        return NULL;
    }
    uint64_t endpoint_hash = connpool_endpoint_hash(host_str, host_len, (int)port);

    for (;;) {
        void *candidate = NULL;
        connpool_discard_t discards[POOL_MAX_ENTRIES];
        size_t discard_count = 0;
        if (!connpool_mutex_lock(&pool->lock)) {
            connpool_release_object(pool);
            rt_trap("ConnectionPool: mutex lock failed");
            return NULL;
        }

        // Evict expired idle connections.
        int64_t now_ms = connpool_now_ms();
        for (int i = 0; i < pool->count; i++) {
            if (connpool_entry_is_idle_expired(&pool->entries[i], now_ms)) {
                if (discard_count >= POOL_MAX_ENTRIES)
                    rt_abort("ConnectionPool: discard capacity exhausted");
                remove_entry_at(pool, i, true, &discards[discard_count]);
                discard_count++;
                i--;
            }
        }

        // Reserve and retain one endpoint match. Socket health is probed only
        // after unlocking so a select/peek readiness race cannot convoy the
        // complete pool.
        for (int i = 0; i < pool->count; i++) {
            pooled_entry_t *entry = &pool->entries[i];
            if (entry->in_use ||
                !connpool_entry_matches(entry, endpoint_hash, host_str, host_len, (int)port)) {
                continue;
            }
            int32_t retained = rt_heap_try_retain_live(entry->tcp);
            if (retained > 0) {
                entry->in_use = true;
                candidate = entry->tcp;
                break;
            }
            if (retained < 0) {
                connpool_mutex_unlock(&pool->lock);
                for (size_t j = 0; j < discard_count; j++)
                    connpool_discard_release(&discards[j]);
                connpool_release_object(pool);
                rt_trap("ConnectionPool.Acquire: TCP reference count overflow");
                return NULL;
            }
            if (discard_count >= POOL_MAX_ENTRIES)
                rt_abort("ConnectionPool: discard capacity exhausted");
            remove_entry_at(pool, i, true, &discards[discard_count]);
            discard_count++;
            i--;
        }
        connpool_mutex_unlock(&pool->lock);
        for (size_t i = 0; i < discard_count; i++)
            connpool_discard_release(&discards[i]);

        if (!candidate)
            break;
        if (tcp_connection_healthy(candidate)) {
            connpool_release_object(pool);
            return candidate;
        }

        if (!connpool_mutex_lock(&pool->lock)) {
            rt_tcp_close(candidate);
            connpool_release_object(candidate);
            connpool_release_object(pool);
            rt_trap("ConnectionPool: mutex lock failed");
            return NULL;
        }
        connpool_discard_t discard;
        bool detached = false;
        for (int i = 0; i < pool->count; i++) {
            if (pool->entries[i].tcp == candidate && pool->entries[i].in_use) {
                remove_entry_at(pool, i, false, &discard);
                detached = true;
                break;
            }
        }
        connpool_mutex_unlock(&pool->lock);
        if (detached)
            connpool_discard_release(&discard);
        rt_tcp_close(candidate);
        connpool_release_object(candidate);
    }

    connpool_release_object(pool);

    // No pooled connection available; create new one
    void *tcp = rt_tcp_connect(host, port);
    if (!tcp)
        return NULL;

    pool = connpool_require_retained(obj, "ConnectionPool.Acquire");
    if (!pool) {
        rt_tcp_close(tcp);
        connpool_release_object(tcp);
        return NULL;
    }

    int32_t pool_tcp_ref = rt_heap_try_retain_live(tcp);
    if (pool_tcp_ref == 1) {
        if (!connpool_mutex_lock(&pool->lock)) {
            connpool_release_object(tcp); // candidate pool reference
            connpool_release_object(pool);
            rt_tcp_close(tcp);
            connpool_release_object(tcp); // original result reference
            rt_trap("ConnectionPool: mutex lock failed");
            return NULL;
        }
        bool tracked = track_connection(pool, tcp, endpoint_hash, true, 0);
        connpool_mutex_unlock(&pool->lock);
        if (!tracked)
            connpool_release_object(tcp); // unused candidate pool reference
    }
    connpool_release_object(pool);
    return tcp;
}

/// @brief Return a single connection to the pool, or close it.
/// @details Locates the entry by pointer identity under the pool mutex.
///          - Tracked + healthy: cleared `in_use` and stamped with the
///            monotonic clock so the idle-timeout sweep can later reclaim it.
///          - Tracked + unhealthy: removed via @ref remove_entry_at so
///            the slot doesn't keep a dead fd around.
///          - Untracked + healthy: exclusively adopted into a free slot.
///          - Foreign-pool lease: rejected without closing the transport.
///          - Untracked + unhealthy or pool full: closed, caller ref retained.
/// @param obj Destination ConnectionPool; NULL is a no-op.
/// @param conn TCP handle to release; NULL is a no-op.
void rt_connpool_release(void *obj, void *conn) {
    if (!obj || !conn)
        return;

    rt_connpool_impl *pool = connpool_require_retained(obj, "ConnectionPool.Release");
    if (!pool)
        return;
    if (!rt_tcp_is_handle(conn)) {
        connpool_release_object(pool);
        rt_trap("ConnectionPool.Release: invalid TCP connection");
        return;
    }
    int32_t retained = rt_heap_try_retain_live(conn);
    if (retained != 1) {
        connpool_release_object(pool);
        rt_trap(retained < 0 ? "ConnectionPool.Release: TCP reference count overflow"
                             : "ConnectionPool.Release: stale TCP connection");
        return;
    }

    int healthy = tcp_connection_healthy(conn);
    if (!connpool_mutex_lock(&pool->lock)) {
        connpool_release_object(conn);
        connpool_release_object(pool);
        rt_trap("ConnectionPool: mutex lock failed");
        return;
    }

    // Find the entry for this connection (if it was tracked)
    for (int i = 0; i < pool->count; i++) {
        if (pool->entries[i].tcp == conn) {
            if (!healthy) {
                connpool_discard_t discard;
                remove_entry_at(pool, i, true, &discard);
                connpool_mutex_unlock(&pool->lock);
                connpool_discard_release(&discard);
                connpool_release_object(conn);
                connpool_release_object(pool);
                return;
            }
            pool->entries[i].in_use = false;
            pool->entries[i].last_used_ms = connpool_now_ms();
            connpool_mutex_unlock(&pool->lock);
            connpool_release_object(conn);
            connpool_release_object(pool);
            return;
        }
    }

    uint64_t owner = rt_tcp_pool_owner(conn);
    if (owner != 0 && owner != pool->owner_token) {
        connpool_mutex_unlock(&pool->lock);
        connpool_release_object(conn);
        connpool_release_object(pool);
        rt_trap("ConnectionPool.Release: TCP belongs to another pool");
        return;
    }

    if (!healthy) {
        bool claimed = connpool_claim_untracked(pool, conn);
        connpool_mutex_unlock(&pool->lock);
        if (claimed)
            connpool_close_claimed(pool, conn);
        connpool_release_object(conn);
        connpool_release_object(pool);
        if (!claimed)
            rt_trap("ConnectionPool.Release: TCP belongs to another pool");
        return;
    }

    bool tracked = false;
    if (pool->count < pool->max_size) {
        const char *host_str = NULL;
        size_t host_len = 0;
        int port = 0;
        if (rt_tcp_endpoint_view(conn, &host_str, &host_len, &port) && host_len > 0 && port >= 1 &&
            port <= 65535) {
            uint64_t endpoint_hash = connpool_endpoint_hash(host_str, host_len, port);
            tracked = track_connection(pool, conn, endpoint_hash, false, connpool_now_ms());
        }
    }

    if (tracked) {
        connpool_mutex_unlock(&pool->lock);
        // The temporary retained reference became the pool's entry reference.
        connpool_release_object(pool);
        return;
    }

    bool claimed = connpool_claim_untracked(pool, conn);
    connpool_mutex_unlock(&pool->lock);
    if (claimed)
        connpool_close_claimed(pool, conn);
    connpool_release_object(conn);
    connpool_release_object(pool);
    if (!claimed)
        rt_trap("ConnectionPool.Release: TCP belongs to another pool");
}

/// @brief Drop every pooled entry and reset the entry count to zero.
/// @details Atomically detaches the complete entry set under the pool mutex,
///          then closes idle transports and releases managed references after
///          unlocking. Checked-out handles are detached without closing so an
///          `Acquire` caller keeps working; a later `Release` re-adopts or
///          closes the handle.
/// @param obj ConnectionPool handle to clear; NULL is a no-op.
void rt_connpool_clear(void *obj) {
    if (!obj)
        return;
    rt_connpool_impl *pool = connpool_require_retained(obj, "ConnectionPool.Clear");
    if (!pool)
        return;

    if (!connpool_mutex_lock(&pool->lock)) {
        connpool_release_object(pool);
        rt_trap("ConnectionPool: mutex lock failed");
        return;
    }
    connpool_discard_t discards[POOL_MAX_ENTRIES];
    size_t discard_count = 0;
    for (int i = 0; i < pool->count; i++) {
        detach_entry(pool, &pool->entries[i], !pool->entries[i].in_use, &discards[discard_count]);
        discard_count++;
    }
    pool->count = 0;
    connpool_mutex_unlock(&pool->lock);
    for (size_t i = 0; i < discard_count; i++)
        connpool_discard_release(&discards[i]);
    connpool_release_object(pool);
}

/// @brief Return a synchronized snapshot of the tracked entry count.
/// @param obj ConnectionPool to inspect; NULL returns zero.
/// @return Idle plus checked-out tracked entries.
int64_t rt_connpool_size(void *obj) {
    if (!obj)
        return 0;
    rt_connpool_impl *pool = connpool_require_retained(obj, "ConnectionPool.Size");
    if (!pool)
        return 0;

    if (!connpool_mutex_lock(&pool->lock)) {
        connpool_release_object(pool);
        rt_trap("ConnectionPool: mutex lock failed");
        return 0;
    }
    int64_t n = pool->count;
    connpool_mutex_unlock(&pool->lock);
    connpool_release_object(pool);
    return n;
}

/// @brief Count entries that are tracked but not currently checked out.
/// @details Takes the pool mutex and snapshots only the `in_use == false`
///          entries. It does not probe health or evict expired connections.
/// @param obj ConnectionPool handle to inspect; NULL returns zero.
/// @return Number of currently idle tracked entries.
int64_t rt_connpool_available(void *obj) {
    if (!obj)
        return 0;
    rt_connpool_impl *pool = connpool_require_retained(obj, "ConnectionPool.Available");
    if (!pool)
        return 0;

    if (!connpool_mutex_lock(&pool->lock)) {
        connpool_release_object(pool);
        rt_trap("ConnectionPool: mutex lock failed");
        return 0;
    }
    int64_t n = 0;
    for (int i = 0; i < pool->count; i++) {
        if (!pool->entries[i].in_use)
            n++;
    }
    connpool_mutex_unlock(&pool->lock);
    connpool_release_object(pool);
    return n;
}
