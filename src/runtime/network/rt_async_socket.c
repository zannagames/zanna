//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/network/rt_async_socket.c
// Purpose: Future wrapper that bridges blocking I/O through a configurable,
//          lazily initialized shared worker pool.
// Key invariants:
//   - Each async operation creates a Future, submits blocking work to the
//     thread pool, and resolves the Future when the operation completes.
//   - Pool initialization is atomic and retryable after allocation failure.
//   - Worker traps are captured, recovery is cleared, and only then is the
//     Promise rejected through an allocation-fallback completion primitive.
// Ownership/Lifetime:
//   - Returned Futures are caller-owned managed references.
//   - Closure args are heap-allocated and freed by the worker.
//   - Each queued closure owns one Promise reference until worker cleanup.
//   - TCP/Bytes arguments are retained and string arguments are snapshotted
//     before queueing; fresh worker results transfer into the Promise.
// Links: rt_async_socket.h (API), rt_network.h (blocking sockets),
//        rt_future.h (completion), rt_threadpool.h (executor)
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Implements Future-based adapters over blocking network operations.
 * @details Lazily configures a shared bounded worker pool, snapshots or retains
 * arguments across thread boundaries, contains worker traps, and settles every
 * Promise for asynchronous TCP connect/send/receive and one-shot HTTP work.
 */

#include "rt_async_socket.h"

#include "rt_box.h"
#include "rt_bytes.h"
#include "rt_future.h"
#include "system/rt_machine.h"

#include "rt_heap.h"
#include "rt_internal.h"
#include "rt_network.h"
#include "rt_network_internal.h"
#include "rt_object.h"
#include "rt_platform.h"
#include "rt_string.h"
#include "rt_threadpool.h"
#include "rt_threads.h"
#include "rt_trap.h"

#include <limits.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//=============================================================================
// Default Thread Pool (lazy singleton)
//=============================================================================

/** Published process-wide worker Pool after successful initialization. */
static void *default_pool = NULL;
/** Atomic state gate serializing Pool configuration and publication. */
static volatile int pool_init_state = 0;

/** States of the shared AsyncSocket Pool initialization state machine. */
enum {
    ASYNC_POOL_UNINITIALIZED = 0,
    ASYNC_POOL_INITIALIZING = 1,
    ASYNC_POOL_READY = 2,
    ASYNC_POOL_CONFIGURING = 3,
};

/// @brief Requested worker count for the shared Pool, or zero for CPU scaling.
/// @details Access is serialized by @ref pool_init_state. Configuration writes
///          only while holding ASYNC_POOL_CONFIGURING, and initialization reads
///          only after claiming ASYNC_POOL_INITIALIZING.
static int64_t requested_pool_size = 0;

/// @brief Compute the configured or CPU-scaled AsyncSocket worker count.
/// @details Each blocking operation occupies a worker for its duration, so the
///          default uses twice the logical CPU count with a floor of eight.
///          Explicit and derived values are capped at the Pool's 1,024-worker
///          constructor limit. The caller owns the initialization state gate.
/// @return Worker count in the inclusive range 1..1024.
static int64_t async_default_pool_size(void) {
    int64_t size = requested_pool_size;
    if (size <= 0) {
        int64_t cores = rt_machine_cores();
        if (cores < 1)
            cores = 1;
        size = cores * 2;
        if (size < 8)
            size = 8;
    }
    if (size > 1024)
        size = 1024;
    return size;
}

/// @brief Lazy-initialize and return the shared async-socket worker pool.
/// @details One portable atomic state machine coordinates configuration and
///          creation on every platform. The winning initializer catches pool
///          construction traps locally and restores the uninitialized state,
///          allowing a later operation to retry instead of publishing a
///          permanent NULL singleton. Contending callers yield while another
///          thread configures or initializes the pool. Publication uses release
///          ordering and readers observe it with acquire ordering.
/// @return Shared Pool object, or NULL when this initialization attempt fails.
static void *get_default_pool(void) {
    for (;;) {
        int state = rt_atomic_load_i32(&pool_init_state, __ATOMIC_ACQUIRE);
        if (state == ASYNC_POOL_READY)
            return default_pool;
        if (state == ASYNC_POOL_INITIALIZING || state == ASYNC_POOL_CONFIGURING) {
            rt_thread_yield();
            continue;
        }

        int expected = ASYNC_POOL_UNINITIALIZED;
        if (!rt_atomic_compare_exchange_i32(&pool_init_state,
                                            &expected,
                                            ASYNC_POOL_INITIALIZING,
                                            __ATOMIC_ACQ_REL,
                                            __ATOMIC_ACQUIRE)) {
            continue;
        }

        void *candidate = NULL;
        jmp_buf recovery;
        rt_trap_set_recovery(&recovery);
        if (setjmp(recovery) == 0) {
            candidate = rt_threadpool_new(async_default_pool_size());
            rt_trap_clear_recovery();
        } else {
            rt_trap_clear_recovery();
            candidate = NULL;
        }

        if (!candidate) {
            rt_atomic_store_i32(&pool_init_state, ASYNC_POOL_UNINITIALIZED, __ATOMIC_RELEASE);
            return NULL;
        }
        default_pool = candidate;
        rt_atomic_store_i32(&pool_init_state, ASYNC_POOL_READY, __ATOMIC_RELEASE);
        return candidate;
    }
}

/// @brief Configure the shared pool's worker count (the executor model is a
///        bounded blocking-worker pool).
/// @details Configuration takes the same atomic state gate as initialization,
///          preventing a data race with the first async operation. Sequential
///          pre-initialization calls may replace the requested size; once pool
///          creation begins or completes, configuration traps and returns.
/// @param size Requested worker count in the inclusive range 1..1024.
void rt_async_socket_set_pool_size(int64_t size) {
    if (size < 1 || size > 1024) {
        rt_trap("AsyncSocket.SetPoolSize: size must be 1..1024");
        return;
    }
    for (;;) {
        int state = rt_atomic_load_i32(&pool_init_state, __ATOMIC_ACQUIRE);
        if (state == ASYNC_POOL_INITIALIZING || state == ASYNC_POOL_READY) {
            rt_trap("AsyncSocket.SetPoolSize: pool already created; set the size "
                    "before the first async operation");
            return;
        }
        if (state == ASYNC_POOL_CONFIGURING) {
            rt_thread_yield();
            continue;
        }
        int expected = ASYNC_POOL_UNINITIALIZED;
        if (rt_atomic_compare_exchange_i32(&pool_init_state,
                                           &expected,
                                           ASYNC_POOL_CONFIGURING,
                                           __ATOMIC_ACQ_REL,
                                           __ATOMIC_ACQUIRE)) {
            break;
        }
    }
    requested_pool_size = size;
    rt_atomic_store_i32(&pool_init_state, ASYNC_POOL_UNINITIALIZED, __ATOMIC_RELEASE);
}

/// @brief Drop one owned managed reference and finalize it at refcount zero.
/// @details Handles runtime Strings as well as ordinary objects through the
///          common object-release dispatcher. All callers pass references they
///          already own; NULL is accepted so staged worker cleanup can remain
///          unconditional after a recovered trap.
/// @param obj Owned runtime-managed reference, or NULL.
static void async_release_owned(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief Reject a Promise after submission failure and release the producer reference.
/// @details Uses the no-throw C-string completion primitive so diagnostic OOM
///          still settles the Future. The function always consumes @p promise.
/// @param promise Producer-owned Promise reference.
/// @param message NUL-terminated rejection diagnostic.
static void async_fail_submit(void *promise, const char *message) {
    (void)rt_promise_try_set_error_cstr(promise, message);
    async_release_owned(promise);
}

/// @brief Allocate a Promise/Future pair with cleanup around non-local allocation traps.
/// @details Promise creation and cached-Future publication can each trap. A
///          local recovery frame owns both partial references, copies the
///          diagnostic before clearing recovery, releases everything already
///          created, then re-raises for ordinary VM behavior. If an embedder's
///          trap hook returns, the function returns zero with both outputs NULL.
/// @param promise_out Receives the producer-owned Promise reference.
/// @param future_out Receives the caller-owned Future reference.
/// @return One on success, zero after a recovered construction failure.
static int async_create_promise_pair(void **promise_out, void **future_out) {
    if (!promise_out || !future_out)
        return 0;
    *promise_out = NULL;
    *future_out = NULL;

    void *volatile promise = NULL;
    void *volatile future = NULL;
    char saved_error[256];
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        const char *error = rt_trap_get_error();
        snprintf(saved_error,
                 sizeof(saved_error),
                 "%s",
                 error && error[0] ? error : "AsyncSocket: Promise allocation failed");
        rt_trap_clear_recovery();
        async_release_owned((void *)future);
        async_release_owned((void *)promise);
        rt_trap(saved_error);
        return 0;
    }

    promise = rt_promise_new();
    if (!promise)
        rt_trap("AsyncSocket: Promise allocation failed");
    future = rt_promise_get_future((void *)promise);
    if (!future)
        rt_trap("AsyncSocket: Future allocation failed");
    rt_trap_clear_recovery();

    *promise_out = (void *)promise;
    *future_out = (void *)future;
    return 1;
}

/// @brief Create a Future that is already resolved as an error.
/// @details Used by public async entry points when validation fails before a
///          worker item can be built. Returning a failed Future preserves the
///          asynchronous API contract while preventing invalid handles or
///          strings from reaching the worker pool after a recoverable trap.
/// @param message Error text to store in the returned Future.
/// @return A Future object resolved with @p message, or NULL if the promise
///         allocation itself fails.
static void *async_failed_future(const char *message) {
    void *promise = NULL;
    void *future = NULL;
    if (!async_create_promise_pair(&promise, &future))
        return NULL;
    async_fail_submit(promise, message);
    return future;
}

/// @brief Leave a worker recovery frame and reject its Promise without recursive longjmp.
/// @details Copies the thread-local trap text into a bounded stack buffer before
///          clearing recovery. Promise rejection happens only after the frame is
///          removed and uses an allocation-fallback primitive, so a secondary
///          diagnostic OOM cannot jump back into the same handler. Callers must
///          invoke this exactly once from the nonzero setjmp branch and must not
///          clear that recovery frame themselves afterward.
/// @param promise Producer-owned Promise that must be settled as an error.
/// @param fallback Diagnostic used when the trapped operation supplied no text.
static void async_reject_caught_worker_trap(void *promise, const char *fallback) {
    char saved_error[512];
    const char *error = rt_trap_get_error();
    snprintf(saved_error,
             sizeof(saved_error),
             "%s",
             error && error[0] ? error : (fallback ? fallback : "AsyncSocket: worker failed"));
    rt_trap_clear_recovery();
    (void)rt_promise_try_set_error_cstr(promise, saved_error);
}

/// @brief Validate a runtime String and borrow its exact byte view.
/// @details Rejects NULL and forged handles before calling String accessors.
///          The returned length includes embedded NUL bytes; individual callers
///          decide whether their OS-facing field permits them. A success result
///          is distinct from a valid empty String, preventing callers from
///          issuing a second diagnostic after a returning trap hook.
/// @param str Candidate managed String.
/// @param invalid_msg Diagnostic used for NULL, invalid, or corrupt handles.
/// @param data_out Receives borrowed bytes owned by @p str.
/// @param len_out Receives the exact byte length.
/// @return One on success, zero immediately after exactly one trap.
static int async_string_view_or_trap(rt_string str,
                                     const char *invalid_msg,
                                     const char **data_out,
                                     size_t *len_out) {
    if (data_out)
        *data_out = NULL;
    if (len_out)
        *len_out = 0;
    if (!data_out || !len_out)
        return 0;
    if (!str || !rt_string_is_handle(str)) {
        rt_trap(invalid_msg);
        return 0;
    }
    const char *data = rt_string_cstr(str);
    int64_t len = rt_str_len(str);
    if (!data || len < 0) {
        rt_trap(invalid_msg);
        return 0;
    }
    if ((uint64_t)len > (uint64_t)SIZE_MAX) {
        rt_trap("AsyncSocket: string is too large");
        return 0;
    }
    *data_out = data;
    *len_out = (size_t)len;
    return 1;
}

/// @brief Test whether a length-delimited byte view contains an interior NUL.
/// @details Host names and URLs cross a C-string OS boundary and therefore
///          cannot preserve bytes after an embedded NUL. Empty and NULL views
///          do not report an embedded terminator.
/// @param data Borrowed byte view.
/// @param len Number of bytes to inspect.
/// @return Nonzero when one of the first @p len bytes is NUL.
static int async_has_embedded_nul(const char *data, size_t len) {
    return data && memchr(data, '\0', len) != NULL;
}

/// @brief Copy an exact byte view into independently owned NUL-terminated storage.
/// @details The trailing terminator is outside the logical @p len bytes, so the
///          helper also preserves embedded NULs for payload fields that permit
///          them. SIZE_MAX is rejected before the len-plus-one calculation.
/// @param data Source bytes; may be NULL only when @p len is zero.
/// @param len Exact number of source bytes.
/// @return Heap-owned copy for release with free(), or NULL on invalid input,
///         overflow, or allocation failure.
static char *async_copy_bytes(const char *data, size_t len) {
    if ((!data && len != 0) || len == SIZE_MAX)
        return NULL;
    char *copy = (char *)malloc(len + 1);
    if (!copy)
        return NULL;
    if (len > 0)
        memcpy(copy, data, len);
    copy[len] = '\0';
    return copy;
}

/// @brief Retain and revalidate a TCP handle for ownership by a queued request.
/// @details A registry-backed non-throwing retain closes the validation-to-share
///          race. The stable TCP identity is checked again after retention so an
///          address-reuse ABA or wrong-class object cannot enter a worker. A
///          failed post-retain validation releases the temporary reference.
/// @param tcp Candidate TCP object supplied at the public boundary.
/// @return One with one new owned reference, otherwise zero without trapping.
static int async_try_retain_tcp(void *tcp) {
    if (rt_heap_try_retain_live(tcp) != 1)
        return 0;
    if (rt_tcp_is_handle(tcp))
        return 1;
    async_release_owned(tcp);
    return 0;
}

/// @brief Retain and revalidate a Bytes handle for ownership by a queued request.
/// @details Mirrors @ref async_try_retain_tcp for the immutable send payload.
///          The second type check occurs while the newly acquired reference
///          prevents concurrent finalization.
/// @param data Candidate Bytes object supplied at the public boundary.
/// @return One with one new owned reference, otherwise zero without trapping.
static int async_try_retain_bytes(void *data) {
    if (rt_heap_try_retain_live(data) != 1)
        return 0;
    if (rt_bytes_is_bytes(data))
        return 1;
    async_release_owned(data);
    return 0;
}

/// @brief Submit one typed worker callback without propagating queueing traps.
/// @details Pool submission can reject for shutdown, backpressure, task-node
///          OOM, or trap on an internal invariant such as pending-count
///          overflow. This wrapper catches the latter, copies the diagnostic
///          before clearing recovery, and turns every refusal into a bounded
///          message that the caller can publish through its Promise after
///          releasing request-owned resources.
/// @param pool Shared worker Pool, or NULL after initialization failure.
/// @param callback Typed worker entry point.
/// @param arg Native request record borrowed until submission succeeds.
/// @param error_out Destination for a NUL-terminated failure diagnostic.
/// @param error_capacity Capacity of @p error_out in bytes.
/// @return One when the Pool owns the queued request, otherwise zero.
static int async_submit_worker(
    void *pool, void (*callback)(void *), void *arg, char *error_out, size_t error_capacity) {
    if (error_out && error_capacity > 0)
        error_out[0] = '\0';
    if (!pool || !callback) {
        if (error_out && error_capacity > 0)
            snprintf(error_out,
                     error_capacity,
                     "%s",
                     pool ? "AsyncSocket: invalid worker callback"
                          : "AsyncSocket: worker pool initialization failed");
        return 0;
    }

    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        const char *error = rt_trap_get_error();
        if (error_out && error_capacity > 0) {
            snprintf(error_out,
                     error_capacity,
                     "%s",
                     error && error[0] ? error : "AsyncSocket: worker submission failed");
        }
        rt_trap_clear_recovery();
        return 0;
    }

    int accepted = rt_threadpool_submit_fn(pool, callback, arg) ? 1 : 0;
    rt_trap_clear_recovery();
    if (!accepted && error_out && error_capacity > 0) {
        snprintf(error_out, error_capacity, "%s", "AsyncSocket: worker pool refused the request");
    }
    return accepted;
}

//=============================================================================
// Async Connect
//=============================================================================

typedef struct {
    char *host;
    size_t host_len;
    int64_t port;
    int64_t timeout_ms;
    void *promise;
} connect_args_t;

/// @brief Execute one blocking TCP connection request on a Pool worker.
/// @details Reconstitutes the caller's exact host snapshot as a managed String,
///          performs the bounded connect, and transfers the fresh TCP reference
///          into the Promise. Every object that can exist across longjmp is
///          volatile and is released only after the operation recovery frame is
///          cleared. A trapped connect is rejected through the allocation-
///          fallback Promise primitive before request cleanup.
/// @param arg Owned @ref connect_args_t request accepted by the worker Pool.
static void async_connect_worker(void *arg) {
    connect_args_t *a = (connect_args_t *)arg;
    rt_string volatile host = NULL;
    void *volatile tcp = NULL;
    jmp_buf recovery;

    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) == 0) {
        host = rt_string_from_bytes(a->host, a->host_len);
        if (!host)
            rt_trap("AsyncSocket: memory allocation failed");
        tcp = rt_tcp_connect_for((rt_string)host, a->port, a->timeout_ms);
        if (!tcp)
            rt_trap("AsyncSocket: connect failed without a result");

        void *transferred = (void *)tcp;
        tcp = NULL;
        (void)rt_promise_try_set_transferred(a->promise, transferred);
        rt_trap_clear_recovery();
    } else {
        async_reject_caught_worker_trap(a->promise, "AsyncSocket: connect failed");
    }

    async_release_owned((void *)tcp);
    async_release_owned((void *)host);
    async_release_owned(a->promise);
    free(a->host);
    free(a);
}

/// @brief Submit a blocking TCP connect to the worker pool with an explicit timeout.
/// @details Validates the complete host/port/timeout tuple before allocating
///          request state, then snapshots host bytes for the worker. The shared
///          Pool is initialized atomically and may retry after a previous OOM.
///          Native allocation, Pool construction, backpressure, shutdown, and
///          submission traps all settle the returned Future as an error while
///          releasing every partial request resource.
/// @param host Non-empty managed host String without embedded NUL bytes.
/// @param port TCP port in the inclusive range 1..65535.
/// @param timeout_ms Overall connect timeout in the range 0..INT_MAX milliseconds.
/// @return Caller-owned Future resolving to a managed TCP object, or an already-
///         failed Future after a recoverable setup failure.
void *rt_async_connect_for(rt_string host, int64_t port, int64_t timeout_ms) {
    const char *h = NULL;
    size_t host_len = 0;
    if (!async_string_view_or_trap(host, "AsyncSocket: invalid host String", &h, &host_len)) {
        return async_failed_future("AsyncSocket: invalid host String");
    }
    if (host_len == 0 || async_has_embedded_nul(h, host_len)) {
        rt_trap("AsyncSocket: invalid host");
        return async_failed_future("AsyncSocket: invalid host");
    }
    if (port < 1 || port > 65535) {
        rt_trap("AsyncSocket: port must be 1..65535");
        return async_failed_future("AsyncSocket: port must be 1..65535");
    }
    if (timeout_ms < 0 || timeout_ms > INT_MAX) {
        rt_trap("AsyncSocket: timeout must be 0..INT_MAX milliseconds");
        return async_failed_future("AsyncSocket: timeout must be 0..INT_MAX milliseconds");
    }

    void *promise = NULL;
    void *future = NULL;
    if (!async_create_promise_pair(&promise, &future))
        return NULL;

    void *pool = get_default_pool();
    if (!pool) {
        async_fail_submit(promise, "AsyncSocket: worker pool initialization failed");
        return future;
    }

    connect_args_t *args = (connect_args_t *)malloc(sizeof(connect_args_t));
    if (!args) {
        async_fail_submit(promise, "AsyncSocket: request allocation failed");
        return future;
    }
    args->host = async_copy_bytes(h, host_len);
    args->host_len = host_len;
    args->port = port;
    args->timeout_ms = timeout_ms;
    args->promise = promise;
    if (!args->host) {
        free(args);
        async_fail_submit(promise, "AsyncSocket: host snapshot allocation failed");
        return future;
    }

    char submit_error[512];
    if (!async_submit_worker(
            pool, async_connect_worker, args, submit_error, sizeof(submit_error))) {
        free(args->host);
        free(args);
        async_fail_submit(promise, submit_error);
    }
    return future;
}

/// @brief Submit a TCP connect using the default TCP connect timeout.
/// @details Equivalent to @ref rt_async_connect_for with a 30,000 millisecond
///          overall deadline. Host and port validation and Future ownership are
///          identical to the explicit-timeout variant.
/// @param host Non-empty managed host String without embedded NUL bytes.
/// @param port TCP port in the inclusive range 1..65535.
/// @return Caller-owned Future resolving to a managed TCP object.
void *rt_async_connect(rt_string host, int64_t port) {
    return rt_async_connect_for(host, port, 30000);
}

//=============================================================================
// Async Send
//=============================================================================

typedef struct {
    void *tcp;
    void *data;
    void *promise;
} send_args_t;

/// @brief Execute one blocking TCP send on a Pool worker.
/// @details The request owns retained TCP and Bytes references. A successful
///          byte count is boxed as a managed Integer object and transferred into
///          the Promise; a send or boxing trap rejects the Promise only after
///          leaving the worker recovery frame. All staged references are then
///          released exactly once.
/// @param arg Owned @ref send_args_t request accepted by the worker Pool.
static void async_send_worker(void *arg) {
    send_args_t *a = (send_args_t *)arg;
    void *volatile boxed = NULL;
    jmp_buf recovery;

    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) == 0) {
        int64_t sent = rt_tcp_send(a->tcp, a->data);
        boxed = rt_box_i64(sent);
        if (!boxed)
            rt_trap("AsyncSocket: send result allocation failed");

        void *transferred = (void *)boxed;
        boxed = NULL;
        (void)rt_promise_try_set_transferred(a->promise, transferred);
        rt_trap_clear_recovery();
    } else {
        async_reject_caught_worker_trap(a->promise, "AsyncSocket: send failed");
    }

    async_release_owned((void *)boxed);
    async_release_owned(a->data);
    async_release_owned(a->tcp);
    async_release_owned(a->promise);
    free(a);
}

/// @brief Submit a blocking TCP send and return its boxed byte count asynchronously.
/// @details Validates both stable class identities, creates the Future pair, and
///          then acquires live references with post-retain revalidation before
///          sharing them with a worker. Request OOM, initialization failure,
///          shutdown, backpressure, or submission traps settle an error Future
///          after releasing both retained arguments.
/// @param tcp Live managed TCP connection handle.
/// @param data Live managed Bytes payload; an empty payload is permitted.
/// @return Caller-owned Future resolving to a boxed Integer byte count.
void *rt_async_send(void *tcp, void *data) {
    if (!rt_tcp_is_handle(tcp)) {
        rt_trap("AsyncSocket: invalid TCP connection");
        return async_failed_future("AsyncSocket: invalid TCP connection");
    }
    if (!rt_bytes_is_bytes(data)) {
        rt_trap("AsyncSocket: invalid Bytes payload");
        return async_failed_future("AsyncSocket: invalid Bytes payload");
    }

    void *promise = NULL;
    void *future = NULL;
    if (!async_create_promise_pair(&promise, &future))
        return NULL;

    void *pool = get_default_pool();
    if (!pool) {
        async_fail_submit(promise, "AsyncSocket: worker pool initialization failed");
        return future;
    }
    if (!async_try_retain_tcp(tcp)) {
        async_fail_submit(promise, "AsyncSocket: TCP connection expired during submission");
        return future;
    }
    if (!async_try_retain_bytes(data)) {
        async_release_owned(tcp);
        async_fail_submit(promise, "AsyncSocket: Bytes payload expired during submission");
        return future;
    }

    send_args_t *args = (send_args_t *)malloc(sizeof(send_args_t));
    if (!args) {
        async_release_owned(data);
        async_release_owned(tcp);
        async_fail_submit(promise, "AsyncSocket: request allocation failed");
        return future;
    }
    args->tcp = tcp;
    args->data = data;
    args->promise = promise;

    char submit_error[512];
    if (!async_submit_worker(pool, async_send_worker, args, submit_error, sizeof(submit_error))) {
        async_release_owned(data);
        async_release_owned(tcp);
        free(args);
        async_fail_submit(promise, submit_error);
    }
    return future;
}

//=============================================================================
// Async Recv
//=============================================================================

typedef struct {
    void *tcp;
    int64_t max_bytes;
    void *promise;
} recv_args_t;

/// @brief Execute one blocking TCP receive on a Pool worker.
/// @details Transfers the fresh Bytes result into the Promise. A socket or
///          allocation trap is copied while the recovery frame is live, then
///          published only after that frame is cleared so diagnostic OOM cannot
///          recursively jump into the handler.
/// @param arg Owned @ref recv_args_t request accepted by the worker Pool.
static void async_recv_worker(void *arg) {
    recv_args_t *a = (recv_args_t *)arg;
    void *volatile data = NULL;
    jmp_buf recovery;

    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) == 0) {
        data = rt_tcp_recv(a->tcp, a->max_bytes);
        if (!data)
            rt_trap("AsyncSocket: receive failed without a result");

        void *transferred = (void *)data;
        data = NULL;
        (void)rt_promise_try_set_transferred(a->promise, transferred);
        rt_trap_clear_recovery();
    } else {
        async_reject_caught_worker_trap(a->promise, "AsyncSocket: receive failed");
    }

    async_release_owned((void *)data);
    async_release_owned(a->tcp);
    async_release_owned(a->promise);
    free(a);
}

/// @brief Submit a blocking TCP receive and return a Future[Bytes].
/// @details The byte limit is checked before setup, and the TCP identity is
///          retained and revalidated before queueing. Zero requests resolve to
///          empty Bytes without touching the socket. Any setup or submission
///          refusal settles the Future and releases the retained TCP handle.
/// @param tcp Live managed TCP connection handle.
/// @param max_bytes Maximum receive size in the range 0..INT_MAX.
/// @return Caller-owned Future resolving to a managed Bytes object.
void *rt_async_recv(void *tcp, int64_t max_bytes) {
    if (!rt_tcp_is_handle(tcp)) {
        rt_trap("AsyncSocket: invalid TCP connection");
        return async_failed_future("AsyncSocket: invalid TCP connection");
    }
    if (max_bytes < 0 || max_bytes > INT_MAX) {
        rt_trap("AsyncSocket: receive size must be 0..INT_MAX");
        return async_failed_future("AsyncSocket: receive size must be 0..INT_MAX");
    }

    void *promise = NULL;
    void *future = NULL;
    if (!async_create_promise_pair(&promise, &future))
        return NULL;

    void *pool = get_default_pool();
    if (!pool) {
        async_fail_submit(promise, "AsyncSocket: worker pool initialization failed");
        return future;
    }
    if (!async_try_retain_tcp(tcp)) {
        async_fail_submit(promise, "AsyncSocket: TCP connection expired during submission");
        return future;
    }

    recv_args_t *args = (recv_args_t *)malloc(sizeof(recv_args_t));
    if (!args) {
        async_release_owned(tcp);
        async_fail_submit(promise, "AsyncSocket: request allocation failed");
        return future;
    }
    args->tcp = tcp;
    args->max_bytes = max_bytes;
    args->promise = promise;

    char submit_error[512];
    if (!async_submit_worker(pool, async_recv_worker, args, submit_error, sizeof(submit_error))) {
        async_release_owned(tcp);
        free(args);
        async_fail_submit(promise, submit_error);
    }
    return future;
}

//=============================================================================
// Async HTTP
//=============================================================================

typedef struct {
    char *url;
    size_t url_len;
    char *body; // NULL for GET
    size_t body_len;
    void *promise;
} http_args_t;

/// @brief Execute one blocking one-shot HTTP GET on a Pool worker.
/// @details Reconstitutes the exact URL snapshot, transfers the fresh response
///          String into the Promise, and stages both managed values across the
///          recovery frame so partial allocation and HTTP traps are leak-free.
/// @param arg Owned @ref http_args_t GET request accepted by the worker Pool.
static void async_http_get_worker(void *arg) {
    http_args_t *a = (http_args_t *)arg;
    rt_string volatile url = NULL;
    rt_string volatile result = NULL;
    jmp_buf recovery;

    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) == 0) {
        url = rt_string_from_bytes(a->url, a->url_len);
        if (!url)
            rt_trap("AsyncSocket: memory allocation failed");
        result = rt_http_get((rt_string)url);
        if (!result)
            rt_trap("AsyncSocket: HTTP GET failed without a response");

        void *transferred = (void *)result;
        result = NULL;
        (void)rt_promise_try_set_transferred(a->promise, transferred);
        rt_trap_clear_recovery();
    } else {
        async_reject_caught_worker_trap(a->promise, "AsyncSocket: HTTP GET failed");
    }

    async_release_owned((void *)result);
    async_release_owned((void *)url);
    async_release_owned(a->promise);
    free(a->url);
    free(a);
}

/// @brief Initiate an HTTP GET and return a Future[String] for the response body.
/// @details Rejects empty or embedded-NUL URLs before setup and stores an exact
///          native snapshot that remains valid after the caller releases its
///          String. Pool initialization and submission failures resolve the
///          Future as an error without leaking the snapshot or Promise.
/// @param url Non-empty managed URL String without embedded NUL bytes.
/// @return Caller-owned Future resolving to the response body String.
void *rt_async_http_get(rt_string url) {
    const char *u = NULL;
    size_t url_len = 0;
    if (!async_string_view_or_trap(url, "AsyncSocket: invalid URL String", &u, &url_len))
        return async_failed_future("AsyncSocket: invalid URL String");
    if (url_len == 0 || async_has_embedded_nul(u, url_len)) {
        rt_trap("AsyncSocket: invalid URL");
        return async_failed_future("AsyncSocket: invalid URL");
    }

    void *promise = NULL;
    void *future = NULL;
    if (!async_create_promise_pair(&promise, &future))
        return NULL;

    void *pool = get_default_pool();
    if (!pool) {
        async_fail_submit(promise, "AsyncSocket: worker pool initialization failed");
        return future;
    }

    http_args_t *args = (http_args_t *)malloc(sizeof(http_args_t));
    if (!args) {
        async_fail_submit(promise, "AsyncSocket: request allocation failed");
        return future;
    }
    args->url = async_copy_bytes(u, url_len);
    args->url_len = url_len;
    args->body = NULL;
    args->body_len = 0;
    args->promise = promise;
    if (!args->url) {
        free(args);
        async_fail_submit(promise, "AsyncSocket: URL snapshot allocation failed");
        return future;
    }

    char submit_error[512];
    if (!async_submit_worker(
            pool, async_http_get_worker, args, submit_error, sizeof(submit_error))) {
        free(args->url);
        free(args);
        async_fail_submit(promise, submit_error);
    }
    return future;
}

/// @brief Execute one blocking one-shot HTTP POST on a Pool worker.
/// @details Reconstitutes length-delimited URL and body snapshots, preserving
///          embedded NUL bytes in the body while URL validation remains C-string
///          safe. The response reference is transferred to the Promise; every
///          partially constructed String is staged for post-recovery cleanup.
/// @param arg Owned @ref http_args_t POST request accepted by the worker Pool.
static void async_http_post_worker(void *arg) {
    http_args_t *a = (http_args_t *)arg;
    rt_string volatile url = NULL;
    rt_string volatile body = NULL;
    rt_string volatile result = NULL;
    jmp_buf recovery;

    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) == 0) {
        url = rt_string_from_bytes(a->url, a->url_len);
        body = rt_string_from_bytes(a->body ? a->body : "", a->body_len);
        if (!url || !body)
            rt_trap("AsyncSocket: memory allocation failed");
        result = rt_http_post((rt_string)url, (rt_string)body);
        if (!result)
            rt_trap("AsyncSocket: HTTP POST failed without a response");

        void *transferred = (void *)result;
        result = NULL;
        (void)rt_promise_try_set_transferred(a->promise, transferred);
        rt_trap_clear_recovery();
    } else {
        async_reject_caught_worker_trap(a->promise, "AsyncSocket: HTTP POST failed");
    }

    async_release_owned((void *)result);
    async_release_owned((void *)body);
    async_release_owned((void *)url);
    async_release_owned(a->promise);
    free(a->url);
    free(a->body);
    free(a);
}

/// @brief Initiate an HTTP POST and return a Future[String] for the response body.
/// @details URL and body bytes are snapshotted before queueing so the worker can
///          outlive both caller handles. NULL body is normalized to an empty
///          String; a non-NULL body must be a valid managed String and preserves
///          its exact length, including embedded NUL bytes. Setup and submission
///          failures settle the Future after freeing both snapshots.
/// @param url Non-empty managed URL String without embedded NUL bytes.
/// @param body Managed request-body String, or NULL for an empty body.
/// @return Caller-owned Future resolving to the response body String.
void *rt_async_http_post(rt_string url, rt_string body) {
    const char *u = NULL;
    size_t url_len = 0;
    if (!async_string_view_or_trap(url, "AsyncSocket: invalid URL String", &u, &url_len))
        return async_failed_future("AsyncSocket: invalid URL String");
    if (url_len == 0 || async_has_embedded_nul(u, url_len)) {
        rt_trap("AsyncSocket: invalid URL");
        return async_failed_future("AsyncSocket: invalid URL");
    }

    const char *b = NULL;
    size_t body_len = 0;
    if (body &&
        !async_string_view_or_trap(body, "AsyncSocket: invalid body String", &b, &body_len)) {
        return async_failed_future("AsyncSocket: invalid body String");
    }

    void *promise = NULL;
    void *future = NULL;
    if (!async_create_promise_pair(&promise, &future))
        return NULL;

    void *pool = get_default_pool();
    if (!pool) {
        async_fail_submit(promise, "AsyncSocket: worker pool initialization failed");
        return future;
    }

    http_args_t *args = (http_args_t *)malloc(sizeof(http_args_t));
    if (!args) {
        async_fail_submit(promise, "AsyncSocket: request allocation failed");
        return future;
    }
    args->url = async_copy_bytes(u, url_len);
    args->url_len = url_len;
    args->body = body ? async_copy_bytes(b ? b : "", body_len) : NULL;
    args->body_len = body_len;
    args->promise = promise;
    if (!args->url || (body && !args->body)) {
        free(args->url);
        free(args->body);
        free(args);
        async_fail_submit(promise, "AsyncSocket: request snapshot allocation failed");
        return future;
    }

    char submit_error[512];
    if (!async_submit_worker(
            pool, async_http_post_worker, args, submit_error, sizeof(submit_error))) {
        free(args->url);
        free(args->body);
        free(args);
        async_fail_submit(promise, submit_error);
    }
    return future;
}
