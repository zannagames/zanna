//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/network/rt_http_server.c
// Purpose: Threaded HTTP/1.1 server with routing and request/response objects.
// Key invariants:
//   - Accept loop runs in a dedicated thread.
//   - Each request dispatched to thread pool for concurrent handling.
//   - Sequential HTTP/1.1 keep-alive requests are supported when framing permits.
//   - Routes matched via embedded HttpRouter.
// Ownership/Lifetime:
//   - Server object GC-managed. Stop() or finalizer stops accept loop.
//   - Handler-visible ServerReq/ServerRes snapshots are managed objects whose
//     producer references are released after dispatch; handlers may retain them.
// Links: rt_http_server.h (API), rt_http_router.h (routing)
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Implements the threaded HTTP/1.1 server and managed handler objects.
 * @details Owns listener and worker lifecycles, bounded request parsing and
 * keep-alive framing, synchronized route and handler bindings, active
 * connection interruption, and trap-safe managed ServerReq/ServerRes dispatch.
 */

#if !defined(_WIN32)
/** Enable Darwin extensions required by the POSIX server adapter. */
#define _DARWIN_C_SOURCE 1
/** Enable GNU/POSIX extensions required by the server implementation. */
#define _GNU_SOURCE 1
#endif

#include "rt_http_server.h"
#include "rt_http_router.h"

#include "rt_bytes.h"
#include "rt_internal.h"
#include "rt_map.h"
#include "rt_network_http_internal.h"
#include "rt_network_internal.h"
#include "rt_object.h"
#include "rt_seq.h"
#include "rt_string.h"
#include "rt_threadpool.h"
#include "rt_win32_wait.h"
#include "system/rt_machine.h"

#include <ctype.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
/** Restrict the Windows SDK surface to core declarations. */
#define WIN32_LEAN_AND_MEAN
#include <process.h>
#include <windows.h>
/** Windows compatibility alias for case-insensitive string comparison. */
#define strcasecmp _stricmp
/** Windows compatibility alias for bounded case-insensitive comparison. */
#define strncasecmp _strnicmp
typedef CRITICAL_SECTION http_server_mutex_t;
/** Initialize a Windows HttpServer mutex. */
#define HTTP_SERVER_MUTEX_INIT(m) (InitializeCriticalSectionEx((m), 0, 0) != FALSE)
/** Acquire a Windows HttpServer mutex. */
#define HTTP_SERVER_MUTEX_LOCK(m) EnterCriticalSection(m)
/** Release a Windows HttpServer mutex. */
#define HTTP_SERVER_MUTEX_UNLOCK(m) LeaveCriticalSection(m)
/** Destroy a Windows HttpServer mutex. */
#define HTTP_SERVER_MUTEX_DESTROY(m) DeleteCriticalSection(m)
#else
#include <pthread.h>
#include <strings.h>
#include <unistd.h>
typedef pthread_mutex_t http_server_mutex_t;
/** Initialize a POSIX HttpServer mutex and report success. */
#define HTTP_SERVER_MUTEX_INIT(m) (pthread_mutex_init(m, NULL) == 0)
/** Acquire a POSIX HttpServer mutex. */
#define HTTP_SERVER_MUTEX_LOCK(m) pthread_mutex_lock(m)
/** Release a POSIX HttpServer mutex. */
#define HTTP_SERVER_MUTEX_UNLOCK(m) pthread_mutex_unlock(m)
/** Destroy a POSIX HttpServer mutex. */
#define HTTP_SERVER_MUTEX_DESTROY(m) pthread_mutex_destroy(m)
#endif

#include "rt_trap.h"
/// @copydoc rt_trap_net()
extern void rt_trap_net(const char *msg, int err_code);
/// @copydoc rt_trap_get_net_code()
extern int rt_trap_get_net_code(void);

/// @brief Expose a runtime String's exact bytes while detecting embedded nulls.
/// @details Output receivers are cleared first. A valid handle with a
///          host-representable nonnegative length publishes its borrowed bytes
///          and length for additional caller validation.
/// @param s Runtime String to inspect.
/// @param data_out Optional receiver for the borrowed byte pointer.
/// @param len_out Optional receiver for the exact logical length.
/// @return Nonzero for a missing or wrong-kind handle or an embedded null byte; zero otherwise.
static int server_string_has_embedded_nul(rt_string s, const char **data_out, size_t *len_out) {
    if (data_out)
        *data_out = NULL;
    if (len_out)
        *len_out = 0;
    if (!s || !rt_string_is_handle(s))
        return 1;
    const char *data = rt_string_cstr(s);
    int64_t len = rt_str_len(s);
    if (!data || len < 0)
        return 0;
    if ((uint64_t)len > (uint64_t)SIZE_MAX)
        return 0;
    if (data_out)
        *data_out = data;
    if (len_out)
        *len_out = (size_t)len;
    return memchr(data, '\0', (size_t)len) != NULL;
}

//=============================================================================
// Internal Structures
//=============================================================================

/** Maximum request-line or individual header-line bytes. */
#define HTTP_REQ_MAX_LINE 8192
/** Maximum number of header fields accepted in one request. */
#define HTTP_REQ_MAX_HEADERS 100
/** Maximum decoded request body bytes. */
#define HTTP_REQ_MAX_BODY (16 * 1024 * 1024) // 16 MB
/** Maximum encoded body storage including chunk-framing overhead. */
#define HTTP_REQ_MAX_ENCODED_BODY (HTTP_REQ_MAX_BODY + 65536)
/** Maximum concurrently tracked connections for stop interruption. */
#define HTTP_SERVER_MAX_ACTIVE_CONNS 4096

/// @brief Compute the internal worker-pool size for HTTP server instances.
/// @details Uses the runtime machine adapter to query logical CPU count, then
///          clamps the result to a practical default so large machines do not
///          create hundreds of idle server workers by default.
///          The helper is deliberately local to the server implementation so
///          worker sizing can improve without expanding the runtime C ABI.
/// @return Worker count in the inclusive range 1..64.
static int64_t http_server_default_worker_count(void) {
    int64_t cores = rt_machine_cores();
    if (cores < 1)
        cores = 1;
    if (cores > 64)
        cores = 64;
    return cores;
}

/** Parsed request snapshot transferred into a managed ServerReq payload. */
typedef struct {
    char *method;
    char *path;
    char *query;
    char *body;
    size_t body_len;
    int http_major;
    int http_minor;
    void *headers; // Map
    void *params;  // Map (from router)
} server_req_t;

/** Mutable response builder state stored in a managed ServerRes payload. */
typedef struct {
    int status_code;
    void *headers; // Map
    char *body;
    size_t body_len;
    bool sent;
} server_res_t;

/** Route-index metadata connecting router matches to handler tags. */
typedef struct {
    char *tag;
} route_entry_t;

/** Native or VM handler dispatch tuple owned by the server. */
typedef struct {
    char *tag;
    rt_http_server_handler_dispatch_fn dispatch;
    void *ctx;
    rt_http_server_handler_cleanup_fn cleanup;
} handler_binding_t;

/** Complete private lifecycle, routing, worker, and connection state of a server. */
typedef struct {
    void *router;     // HttpRouter
    void *tcp_server; // TcpServer
    void *worker_pool;
    int64_t port;
    bool running;
    bool stopping;
    bool finalizing;
    size_t active_sync_requests;
    route_entry_t *routes;
    int route_count;
    int route_cap;
    handler_binding_t *bindings;
    int binding_count;
    int binding_cap;
#ifdef _WIN32
    HANDLE accept_thread;
#else
    pthread_t accept_thread;
#endif
    bool thread_started;
    void **active_conns;
    int active_conn_count;
    int active_conn_cap;
    http_server_mutex_t state_lock;
    bool state_lock_initialized;
    http_server_mutex_t lifecycle_lock;
    bool lifecycle_lock_initialized;
} rt_http_server_impl;

/// @copydoc free_server_req()
static void free_server_req(server_req_t *req);
/// @copydoc free_server_res()
static void free_server_res(server_res_t *res);
/// @copydoc build_route_response()
static void build_route_response(rt_http_server_impl *server, server_req_t *req, server_res_t *res);
/// @copydoc free_route_entries()
static void free_route_entries(rt_http_server_impl *server);
/// @copydoc free_handler_bindings()
static void free_handler_bindings(rt_http_server_impl *server);

/// @brief Acquire the mutex protecting running state and active-connection bookkeeping.
/// @param server Initialized server payload; null or partial payloads are no-ops.
static void server_state_lock(rt_http_server_impl *server) {
    if (server && server->state_lock_initialized)
        HTTP_SERVER_MUTEX_LOCK(&server->state_lock);
}

/// @brief Release the server state mutex.
/// @param server Server whose initialized state mutex is held by the caller.
static void server_state_unlock(rt_http_server_impl *server) {
    if (server && server->state_lock_initialized)
        HTTP_SERVER_MUTEX_UNLOCK(&server->state_lock);
}

/// @brief Acquire the mutex that serializes configuration and lifecycle changes.
/// @details The lifecycle mutex is always acquired before the state mutex when
///          both are needed. Accept and worker paths use only the state mutex,
///          preventing an inversion with Start, Stop, and registration calls.
/// @param server Fully initialized HttpServer payload.
static void server_lifecycle_lock(rt_http_server_impl *server) {
    if (server && server->lifecycle_lock_initialized)
        HTTP_SERVER_MUTEX_LOCK(&server->lifecycle_lock);
}

/// @brief Release the HttpServer lifecycle/configuration mutex.
/// @param server Fully initialized HttpServer payload.
static void server_lifecycle_unlock(rt_http_server_impl *server) {
    if (server && server->lifecycle_lock_initialized)
        HTTP_SERVER_MUTEX_UNLOCK(&server->lifecycle_lock);
}

/// @brief Validate and cast a public HttpServer receiver.
/// @details Stable class identity, complete payload size, and both initialized
///          mutexes are required before any public method accesses native state.
///          This rejects forged, stale, unrelated, and partially initialized
///          handles without dereferencing their payload as a server.
/// @param obj Candidate managed receiver.
/// @param message Diagnostic raised when validation fails.
/// @return Valid server payload, or NULL after trapping.
static rt_http_server_impl *http_server_checked(void *obj, const char *message) {
    if (!rt_obj_is_instance(obj, RT_HTTP_SERVER_CLASS_ID, sizeof(rt_http_server_impl))) {
        rt_trap(message);
        return NULL;
    }
    rt_http_server_impl *server = (rt_http_server_impl *)obj;
    if (!server->state_lock_initialized || !server->lifecycle_lock_initialized) {
        rt_trap(message);
        return NULL;
    }
    return server;
}

/// @brief Validate and cast a managed ServerReq receiver.
/// @param obj Candidate request handle.
/// @return Valid request payload, or NULL after trapping.
static server_req_t *server_req_checked(void *obj) {
    if (!rt_obj_is_instance(obj, RT_SERVER_REQ_CLASS_ID, sizeof(server_req_t))) {
        rt_trap("HttpServer: invalid ServerReq handle");
        return NULL;
    }
    return (server_req_t *)obj;
}

/// @brief Validate and cast a managed ServerRes receiver.
/// @param obj Candidate response handle.
/// @return Valid response payload, or NULL after trapping.
static server_res_t *server_res_checked(void *obj) {
    if (!rt_obj_is_instance(obj, RT_SERVER_RES_CLASS_ID, sizeof(server_res_t))) {
        rt_trap("HttpServer: invalid ServerRes handle");
        return NULL;
    }
    return (server_res_t *)obj;
}

/// @brief Read the server's published running flag under its state mutex.
/// @param server Server to inspect.
/// @return One while the accept loop is running; zero for stopped or null servers.
static int server_is_running(rt_http_server_impl *server) {
    int running = 0;
    if (!server)
        return 0;
    server_state_lock(server);
    running = server->running ? 1 : 0;
    server_state_unlock(server);
    return running;
}

/// @brief Test whether route or binding mutation is currently forbidden.
/// @details The state snapshot is taken without the lifecycle mutex so a
///          handler attempting configuration during Stop can reject promptly
///          instead of blocking behind Stop while Stop drains that handler.
///          Callers must repeat the check after acquiring the lifecycle mutex.
/// @param server Valid HttpServer payload.
/// @return One while running or stopping; zero while configuration is allowed.
static int server_configuration_blocked(rt_http_server_impl *server) {
    int blocked = 1;
    if (!server)
        return 1;
    server_state_lock(server);
    blocked = server->running || server->stopping || server->finalizing ||
              server->active_sync_requests > 0;
    server_state_unlock(server);
    return blocked;
}

/// @brief Add a TCP handle to the synchronized active-connection registry.
/// @details Duplicate registration succeeds without adding a second entry.
///          Storage grows geometrically up to the implementation cap.
/// @param server Server that owns the registry.
/// @param conn Managed TCP connection handle to record.
/// @return 1 when already or newly registered; 0 for invalid input, capacity exhaustion, or
///         allocation failure.
static int server_register_active_conn(rt_http_server_impl *server, void *conn) {
    int ok = 1;
    if (!server || !conn)
        return 0;
    server_state_lock(server);
    for (int i = 0; i < server->active_conn_count; i++) {
        if (server->active_conns[i] == conn) {
            server_state_unlock(server);
            return 1;
        }
    }
    if (server->active_conn_count >= HTTP_SERVER_MAX_ACTIVE_CONNS) {
        server_state_unlock(server);
        return 0;
    }
    if (server->active_conn_count == server->active_conn_cap) {
        int new_cap = server->active_conn_cap > 0 ? server->active_conn_cap * 2 : 8;
        void **grown = (void **)realloc(server->active_conns, (size_t)new_cap * sizeof(void *));
        if (!grown) {
            ok = 0;
        } else {
            server->active_conns = grown;
            server->active_conn_cap = new_cap;
        }
    }
    if (ok)
        server->active_conns[server->active_conn_count++] = conn;
    server_state_unlock(server);
    return ok;
}

/// @brief Remove a TCP handle from the active registry using unordered compaction.
/// @param server Server that owns the registry.
/// @param conn Connection handle to remove.
static void server_unregister_active_conn(rt_http_server_impl *server, void *conn) {
    if (!server || !conn)
        return;
    server_state_lock(server);
    for (int i = 0; i < server->active_conn_count; i++) {
        if (server->active_conns[i] == conn) {
            server->active_conn_count--;
            server->active_conns[i] = server->active_conns[server->active_conn_count];
            server->active_conns[server->active_conn_count] = NULL;
            break;
        }
    }
    server_state_unlock(server);
}

/// @brief Capture retained references to all currently active TCP connections.
/// @param server Server whose registry is snapshotted.
/// @param count_out Optional receiver for the number of returned handles.
/// @return Malloc-owned array of retained connection references, or null when empty or allocation
///         fails.
static void **server_snapshot_active_conns(rt_http_server_impl *server, int *count_out) {
    void **snapshot = NULL;
    int count = 0;
    if (count_out)
        *count_out = 0;
    if (!server)
        return NULL;
    server_state_lock(server);
    count = server->active_conn_count;
    if (count > 0) {
        snapshot = (void **)malloc((size_t)count * sizeof(void *));
        if (snapshot) {
            memcpy(snapshot, server->active_conns, (size_t)count * sizeof(void *));
            for (int i = 0; i < count; i++)
                rt_obj_retain_maybe(snapshot[i]);
        } else {
            count = 0;
        }
    }
    server_state_unlock(server);
    if (count_out)
        *count_out = count;
    return snapshot;
}

/// @brief Interrupt blocking I/O on a TCP connection without releasing its handle.
/// @param conn Managed TCP connection whose native socket is shut down in both directions.
static void server_interrupt_tcp_conn(void *conn) {
    socket_t sock = rt_tcp_socket_fd(conn);
    if (sock == INVALID_SOCK)
        return;
#ifdef _WIN32
    shutdown(sock, SD_BOTH);
#else
    shutdown(sock, SHUT_RDWR);
#endif
}

typedef enum {
    CHUNK_PARSE_OK = 0,
    CHUNK_PARSE_INCOMPLETE = 1,
    CHUNK_PARSE_INVALID = 2,
} chunk_parse_status_t;

/// @brief Heap-allocate a NUL-terminated copy of a C string.
///
/// Uses plain `malloc` rather than the GC-managed string pool because
/// the resulting buffer is owned by an internal route / binding entry
/// whose lifetime is tied to the server (not to a script value). NULL
/// input returns NULL; allocation failure returns NULL.
///
/// @param text Source NUL-terminated string, or NULL.
/// @return Newly allocated copy that callers must `free()`, or NULL.
static char *dup_cstr(const char *text) {
    if (!text)
        return NULL;
    size_t len = strlen(text);
    char *copy = (char *)malloc(len + 1);
    if (!copy)
        return NULL;
    memcpy(copy, text, len + 1);
    return copy;
}

/// @brief Grow `server->routes` so it can hold at least `needed` entries.
///
/// Doubles the capacity geometrically (initial 8, then 16, 32, ...) up
/// to a hard cap of `INT32_MAX / 2` to avoid wraparound. New tail
/// entries are zero-initialized so callers can write into them
/// immediately. No-op when capacity already suffices.
///
/// @param server Server impl whose `routes`/`route_cap` is grown.
/// @param needed Minimum required capacity (`route_count + 1` at call sites).
/// @return `1` on success (capacity now satisfies `needed`); `0` on
///         allocation failure or overflow.
static int ensure_route_capacity(rt_http_server_impl *server, int needed) {
    if (needed <= server->route_cap)
        return 1;

    int new_cap = server->route_cap > 0 ? server->route_cap : 8;
    while (new_cap < needed) {
        if (new_cap > INT32_MAX / 2)
            return 0;
        new_cap *= 2;
    }

    route_entry_t *routes =
        (route_entry_t *)realloc(server->routes, (size_t)new_cap * sizeof(route_entry_t));
    if (!routes)
        return 0;
    memset(routes + server->route_cap, 0, (size_t)(new_cap - server->route_cap) * sizeof(*routes));
    server->routes = routes;
    server->route_cap = new_cap;
    return 1;
}

/// @brief Grow `server->bindings` so it can hold at least `needed` entries.
///
/// Mirror of `ensure_route_capacity` for the handler-binding array. Same
/// geometric-growth strategy with INT32_MAX/2 overflow guard and zeroed
/// tail.
///
/// @param server Server impl whose `bindings`/`binding_cap` is grown.
/// @param needed Minimum required capacity.
/// @return `1` on success, `0` on allocation failure or overflow.
static int ensure_binding_capacity(rt_http_server_impl *server, int needed) {
    if (needed <= server->binding_cap)
        return 1;

    int new_cap = server->binding_cap > 0 ? server->binding_cap : 8;
    while (new_cap < needed) {
        if (new_cap > INT32_MAX / 2)
            return 0;
        new_cap *= 2;
    }

    handler_binding_t *bindings =
        (handler_binding_t *)realloc(server->bindings, (size_t)new_cap * sizeof(handler_binding_t));
    if (!bindings)
        return 0;
    memset(bindings + server->binding_cap,
           0,
           (size_t)(new_cap - server->binding_cap) * sizeof(*bindings));
    server->bindings = bindings;
    server->binding_cap = new_cap;
    return 1;
}

/// @brief Linear-search the binding array for an entry with matching tag.
///
/// Bindings are typically few (one per handler), so a linear scan is
/// faster than maintaining a hash table for the cache-locality win.
/// NULL inputs return NULL.
///
/// @param server Server impl.
/// @param tag    Tag to match (case-sensitive `strcmp`).
/// @return Pointer to the matching entry, or NULL if not found.
static handler_binding_t *find_handler_binding(rt_http_server_impl *server, const char *tag) {
    if (!server || !tag)
        return NULL;

    for (int i = 0; i < server->binding_count; i++) {
        handler_binding_t *binding = &server->bindings[i];
        if (binding->tag && strcmp(binding->tag, tag) == 0)
            return binding;
    }
    return NULL;
}

/// @brief Release a binding's resources in place (without compacting
///        the array).
///
/// Calls the binding's `cleanup` (if present) on its `ctx` so the
/// handler implementation can release whatever it allocated, then frees
/// the heap-owned `tag` string and zeros the slot. Caller is responsible
/// for any array compaction or count adjustment.
///
/// @param binding Binding to release; NULL is a safe no-op.
static void release_handler_binding(handler_binding_t *binding) {
    if (!binding)
        return;
    if (binding->cleanup)
        binding->cleanup(binding->ctx);
    free(binding->tag);
    memset(binding, 0, sizeof(*binding));
}

/// @brief Install or replace the handler binding for `tag`.
///
/// If a binding already exists with `tag`, its prior cleanup/context pair is
/// detached through the output parameters when the context changes, and the
/// dispatch/ctx/cleanup tuple is overwritten in place. The caller invokes the
/// detached cleanup only after releasing lifecycle locks. Otherwise a new
/// binding is appended after growing capacity. Tag is duplicated so callers
/// can reuse their tag buffer afterwards.
///
/// @param server   Server impl.
/// @param tag      Identifier for the binding (matched by `strcmp`).
/// @param dispatch Callback that handles requests; required.
/// @param ctx      Opaque pointer passed to `dispatch` and `cleanup`.
/// @param cleanup  Optional callback invoked with `ctx` when the binding
///                 is released or replaced. NULL when `ctx` doesn't
///                 require cleanup.
/// @param old_cleanup_out Receives the detached prior cleanup callback.
/// @param old_ctx_out Receives the detached prior callback context.
/// @return `1` on success, `0` on allocation failure or NULL inputs.
static int set_handler_binding(rt_http_server_impl *server,
                               const char *tag,
                               rt_http_server_handler_dispatch_fn dispatch,
                               void *ctx,
                               rt_http_server_handler_cleanup_fn cleanup,
                               rt_http_server_handler_cleanup_fn *old_cleanup_out,
                               void **old_ctx_out) {
    if (!server || !tag || !dispatch)
        return 0;
    if (old_cleanup_out)
        *old_cleanup_out = NULL;
    if (old_ctx_out)
        *old_ctx_out = NULL;

    handler_binding_t *binding = find_handler_binding(server, tag);
    if (binding) {
        if (binding->cleanup && binding->ctx != ctx) {
            if (old_cleanup_out)
                *old_cleanup_out = binding->cleanup;
            if (old_ctx_out)
                *old_ctx_out = binding->ctx;
        }
        binding->dispatch = dispatch;
        binding->ctx = ctx;
        binding->cleanup = cleanup;
        return 1;
    }

    if (!ensure_binding_capacity(server, server->binding_count + 1))
        return 0;

    binding = &server->bindings[server->binding_count];
    binding->tag = dup_cstr(tag);
    if (!binding->tag)
        return 0;
    binding->dispatch = dispatch;
    binding->ctx = ctx;
    binding->cleanup = cleanup;
    server->binding_count++;
    return 1;
}

/// @brief Free every route entry and the surrounding array.
///
/// Walks `server->routes`, frees each entry's tag string, frees the
/// array itself, and zeros the count/capacity/pointer triplet so the
/// server can be safely re-initialized. Idempotent — safe to call when
/// no routes have been registered.
///
/// @param server Server impl.
static void free_route_entries(rt_http_server_impl *server) {
    if (!server || !server->routes)
        return;
    for (int i = 0; i < server->route_count; i++)
        free(server->routes[i].tag);
    free(server->routes);
    server->routes = NULL;
    server->route_count = 0;
    server->route_cap = 0;
}

/// @brief Free every handler binding and the surrounding array.
///
/// Mirror of `free_route_entries`: invokes per-binding `release_handler_
/// binding` (which runs `cleanup` callbacks), frees the array, and zeros
/// the count/capacity/pointer triplet.
///
/// @param server Server impl.
static void free_handler_bindings(rt_http_server_impl *server) {
    if (!server || !server->bindings)
        return;
    for (int i = 0; i < server->binding_count; i++)
        release_handler_binding(&server->bindings[i]);
    free(server->bindings);
    server->bindings = NULL;
    server->binding_count = 0;
    server->binding_cap = 0;
}

/// @brief Trampoline that adapts the C-API `rt_http_server_handler_fn`
///        to the dispatch callback signature.
///
/// Bindings can be installed in two flavors: a "native" handler taking
/// `(req, res)` directly, or a script-bridge dispatch taking
/// `(ctx, req, res)`. The native path stores its function pointer in
/// `ctx` and uses this trampoline as the dispatch slot so the call site
/// is uniform. NULL `ctx` is a safe no-op (skipped, no crash).
///
/// @param ctx Native handler function pointer (cast back to `rt_http_
///            server_handler_fn`).
/// @param req Request object handle, opaque to this layer.
/// @param res Response object handle, opaque to this layer.
static void native_handler_dispatch(void *ctx, void *req, void *res) {
    if (!ctx)
        return;
    ((rt_http_server_handler_fn)ctx)(req, res);
}

//=============================================================================
// Finalizer
//=============================================================================

/// @brief GC finalizer for `HttpServer` instances.
///
/// Called by the GC sweep when the last reference to an `HttpServer`
/// drops. Tears down every owned resource in the right order:
///   1. Stop the accept loop / worker pool so no thread can touch the
///      server's data while we free it.
///   2. Free the route + binding arrays (releases their cleanup
///      callbacks, then their tag strings).
///   3. Release the script-side router and worker_pool object refs (each
///      one is itself a GC handle; we take its refcount to zero before
///      calling `rt_obj_free`).
///
/// Safe to call with NULL `obj` (no-op) and idempotent if invoked twice.
///
/// @param obj `rt_http_server_impl *` cast to `void *`.
static void rt_http_server_finalize(void *obj) {
    if (!obj)
        return;
    rt_http_server_impl *server = http_server_checked(obj, "HttpServer: invalid server handle");
    if (!server)
        return;
    server_state_lock(server);
    server->finalizing = true;
    server_state_unlock(server);
    rt_http_server_stop(server);
    free_route_entries(server);
    free_handler_bindings(server);
    if (server->router && rt_obj_release_check0(server->router))
        rt_obj_free(server->router);
    server->router = NULL;
    if (server->worker_pool && rt_obj_release_check0(server->worker_pool))
        rt_obj_free(server->worker_pool);
    server->worker_pool = NULL;
    free(server->active_conns);
    server->active_conns = NULL;
    server->active_conn_count = 0;
    server->active_conn_cap = 0;
    if (server->lifecycle_lock_initialized) {
        HTTP_SERVER_MUTEX_DESTROY(&server->lifecycle_lock);
        server->lifecycle_lock_initialized = false;
    }
    if (server->state_lock_initialized) {
        HTTP_SERVER_MUTEX_DESTROY(&server->state_lock);
        server->state_lock_initialized = false;
    }
}

//=============================================================================
// Shared HTTP request/response helpers
//=============================================================================

#include "rt_http_server_shared.inc"

/// @brief Free every owned field of a parsed `server_req_t`.
///
/// Releases the heap-owned `method`, `path`, `query`, and `body`
/// strings (allocated during `parse_http_request`), then drops the
/// refcount on the GC-managed `headers` and `params` Map objects.
/// Caller is responsible for freeing the `req` storage itself if it
/// was heap-allocated.
///
/// @param req Request struct to release. Members may be NULL (skipped).
static void free_server_req(server_req_t *req) {
    if (!req)
        return;
    free(req->method);
    free(req->path);
    free(req->query);
    free(req->body);
    if (req->headers && rt_obj_release_check0(req->headers))
        rt_obj_free(req->headers);
    if (req->params && rt_obj_release_check0(req->params))
        rt_obj_free(req->params);
    memset(req, 0, sizeof(*req));
}

/// @brief Release every owned field of a ServerRes payload.
/// @details The helper is shared by managed finalization and stack-based parser
///          test adapters. It is idempotent because the payload is zeroed after
///          releasing the native body and managed header Map.
/// @param res Response payload whose fields are consumed.
static void free_server_res(server_res_t *res) {
    if (!res)
        return;
    free(res->body);
    if (res->headers && rt_obj_release_check0(res->headers))
        rt_obj_free(res->headers);
    memset(res, 0, sizeof(*res));
}

/// @brief Finalize one managed ServerReq payload.
/// @param obj Valid ServerReq payload supplied by the object runtime.
static void server_req_finalize(void *obj) {
    free_server_req((server_req_t *)obj);
}

/// @brief Finalize one managed ServerRes payload.
/// @param obj Valid ServerRes payload supplied by the object runtime.
static void server_res_finalize(void *obj) {
    free_server_res((server_res_t *)obj);
}

/// @brief Allocate an empty managed ServerReq with stable runtime identity.
/// @details The finalizer is installed before returning so any later parser or
///          route-dispatch failure can release the producer reference without
///          special-casing partially populated fields.
/// @return Caller-owned ServerReq handle, or NULL on allocation failure.
static server_req_t *server_req_new(void) {
    server_req_t *req =
        (server_req_t *)rt_obj_new_i64(RT_SERVER_REQ_CLASS_ID, (int64_t)sizeof(server_req_t));
    if (!req)
        return NULL;
    memset(req, 0, sizeof(*req));
    rt_obj_set_finalizer(req, server_req_finalize);
    return req;
}

/// @brief Allocate an empty managed ServerRes with stable runtime identity.
/// @details Body and header ownership begins empty and is released by the
///          installed finalizer after the server drops its producer reference.
/// @return Caller-owned ServerRes handle, or NULL on allocation failure.
static server_res_t *server_res_new(void) {
    server_res_t *res =
        (server_res_t *)rt_obj_new_i64(RT_SERVER_RES_CLASS_ID, (int64_t)sizeof(server_res_t));
    if (!res)
        return NULL;
    memset(res, 0, sizeof(*res));
    rt_obj_set_finalizer(res, server_res_finalize);
    return res;
}

/// @brief Drop one caller-owned managed object reference.
/// @param obj Managed object, or NULL.
static void server_release_object(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief Test-only entry point for the HTTP request parser.
///
/// Wraps `parse_http_request` so unit tests can exercise the parser
/// without needing a real socket. Out-parameters receive freshly
/// `strdup`'d copies of the parsed fields so the test can call the
/// req-free path independently. Caller owns each returned string.
///
/// @param raw           Raw request bytes (e.g. `"GET / HTTP/1.1\r\n..."`).
/// @param raw_len       Length of `raw`.
/// @param method_out    Receives method string (e.g. `"GET"`), or NULL.
/// @param path_out      Receives path string (e.g. `"/api/users"`), or NULL.
/// @param body_out      Receives body bytes (NUL-terminated copy), or NULL.
/// @param body_len_out  Receives body length in bytes, or NULL.
/// @return `1` on successful parse, `0` on malformed request.
int rt_http_server_test_parse_request(const char *raw,
                                      size_t raw_len,
                                      char **method_out,
                                      char **path_out,
                                      char **body_out,
                                      size_t *body_len_out) {
    server_req_t req;
    if (!parse_http_request(raw, raw_len, &req))
        return 0;

    if (method_out)
        *method_out = req.method ? strdup(req.method) : NULL;
    if (path_out)
        *path_out = req.path ? strdup(req.path) : NULL;
    if (body_out) {
        *body_out = NULL;
        if (req.body) {
            if (req.body_len == SIZE_MAX) {
                if (method_out && *method_out) {
                    free(*method_out);
                    *method_out = NULL;
                }
                if (path_out && *path_out) {
                    free(*path_out);
                    *path_out = NULL;
                }
                free_server_req(&req);
                return 0;
            }
            char *body_copy = (char *)malloc(req.body_len + 1);
            if (body_copy) {
                memcpy(body_copy, req.body, req.body_len);
                body_copy[req.body_len] = '\0';
                *body_out = body_copy;
            }
        }
    }
    if (body_len_out)
        *body_len_out = req.body_len;

    free_server_req(&req);
    return 1;
}

//=============================================================================
// HTTP Response Building
//=============================================================================

/// @brief Test-only entry point for the HTTP response builder.
///
/// Mirror of `rt_http_server_test_parse_request`: lets unit tests
/// construct a wire-format response from C input arrays without going
/// through the script-side `Response` object. Builds a temporary
/// `server_res_t`, populates it with the status code, body, and
/// headers, then delegates to the shared `build_response` formatter.
///
/// @param status_code   HTTP status code (e.g. 200, 404).
/// @param body          Body bytes (may be NULL when `body_len == 0`).
/// @param body_len      Body length in bytes.
/// @param header_names  Array of `header_count` header name strings.
/// @param header_values Array of `header_count` header value strings,
///                      paired with `header_names` by index.
/// @param header_count  Number of header pairs.
/// @param out_len       Out-param receiving the formatted response
///                      length in bytes.
/// @return Newly malloc'd response buffer (caller frees), or NULL on
///         allocation failure.
char *rt_http_server_test_build_response(int status_code,
                                         const char *body,
                                         size_t body_len,
                                         const char **header_names,
                                         const char **header_values,
                                         size_t header_count,
                                         size_t *out_len) {
    char *body_copy = NULL;
    server_res_t res;
    memset(&res, 0, sizeof(res));
    res.status_code = status_code;
    if (body && body_len > 0) {
        body_copy = (char *)malloc(body_len);
        if (!body_copy)
            return NULL;
        memcpy(body_copy, body, body_len);
    }
    res.body = body_copy;
    res.body_len = body_copy ? body_len : 0;
    res.sent = true;
    res.headers = rt_map_new();
    if (!res.headers) {
        free(body_copy);
        return NULL;
    }

    for (size_t i = 0; i < header_count; i++) {
        rt_string key = rt_string_from_bytes(header_names[i], strlen(header_names[i]));
        rt_string value = rt_string_from_bytes(header_values[i], strlen(header_values[i]));
        rt_map_set(res.headers, key, (void *)value);
        rt_string_unref(key);
        rt_string_unref(value);
    }

    char *resp = build_response(&res, 0, out_len);
    if (res.headers && rt_obj_release_check0(res.headers))
        rt_obj_free(res.headers);
    free(body_copy);
    return resp;
}

//=============================================================================
// Request Handler
//=============================================================================

typedef struct {
    char *buffer;
    void *received_bytes;
    server_req_t *request;
    server_res_t *response;
    char *wire_response;
    bool registered;
} http_connection_state_t;

/// @brief Release every resource owned by one HTTP connection transaction.
/// @details Worker-local trap recovery calls this helper after any managed
///          allocation, parser, router, handler, serializer, or socket trap.
///          Each field is detached before release and the active-connection
///          registration is cleared exactly once, making repeated cleanup safe.
/// @param server Server that owns the active-connection registry.
/// @param tcp Accepted Tcp handle; the caller retains and releases the object.
/// @param state Heap-resident transaction state.
static void http_connection_state_cleanup(rt_http_server_impl *server,
                                          void *tcp,
                                          http_connection_state_t *state) {
    if (!state)
        return;
    void *received_bytes = state->received_bytes;
    state->received_bytes = NULL;
    server_release_object(received_bytes);
    char *wire_response = state->wire_response;
    state->wire_response = NULL;
    free(wire_response);
    server_res_t *response = state->response;
    state->response = NULL;
    server_release_object(response);
    server_req_t *request = state->request;
    state->request = NULL;
    server_release_object(request);
    char *buffer = state->buffer;
    state->buffer = NULL;
    free(buffer);
    if (state->registered) {
        state->registered = false;
        server_unregister_active_conn(server, tcp);
    }
    rt_tcp_close(tcp);
}

/// @brief Handle a single accepted client connection end-to-end.
///
/// Reads the request (with a 30-second receive timeout to bound idle
/// keepalives), parses headers, validates the framing
/// (`Content-Length` or `Transfer-Encoding: chunked`; oversize bodies are
/// rejected at `HTTP_REQ_MAX_BODY`), parses the request, dispatches
/// through the route table via `build_route_response`, formats the
/// response via `build_response`, sends it on the socket, and closes
/// the connection. Caller (the accept loop or worker pool) owns the
/// `tcp` handle's lifetime up to entering this function; we close it
/// before returning.
///
/// On any allocation failure or malformed request, sends a minimal
/// `400 Bad Request` and closes the connection.
///
/// @param server Server impl owning the route table and bindings.
/// @param tcp    Accepted TCP connection handle (we close it).
/// @param state  Heap-resident per-connection cleanup and recovery state.
static void handle_connection(rt_http_server_impl *server,
                              void *tcp,
                              http_connection_state_t *state) {
    if (!server_register_active_conn(server, tcp)) {
        rt_tcp_close(tcp);
        return;
    }
    state->registered = true;
    rt_tcp_set_recv_timeout(tcp, 30000);
    rt_tcp_set_send_timeout(tcp, 30000);
    size_t buf_cap = 4096;
    size_t buf_len = 0;
    state->buffer = (char *)malloc(buf_cap);
    if (!state->buffer)
        goto done;

    while (rt_tcp_is_open(tcp) && server_is_running(server)) {
        size_t request_len = 0;
        bool bad_request = false;
        bool peer_closed = false;

        while (rt_tcp_is_open(tcp)) {
            size_t desired_cap = 0;
            http_request_frame_status_t frame_status = find_complete_http_request_frame(
                state->buffer, buf_len, &request_len, &desired_cap);
            if (frame_status == HTTP_REQUEST_FRAME_COMPLETE)
                break;
            if (frame_status == HTTP_REQUEST_FRAME_INVALID) {
                bad_request = true;
                break;
            }
            if (desired_cap > buf_cap) {
                char *new_buf = (char *)realloc(state->buffer, desired_cap);
                if (!new_buf) {
                    bad_request = true;
                    break;
                }
                state->buffer = new_buf;
                buf_cap = desired_cap;
            }
            if (buf_len == buf_cap) {
                bad_request = true;
                break;
            }

            state->received_bytes = rt_tcp_recv(tcp, (int64_t)(buf_cap - buf_len));
            int64_t data_len = rt_bytes_len(state->received_bytes);
            if (data_len <= 0) {
                server_release_object(state->received_bytes);
                state->received_bytes = NULL;
                peer_closed = true;
                break;
            }

            uint8_t *ptr = rt_bytes_data(state->received_bytes);
            if (!ptr) {
                server_release_object(state->received_bytes);
                state->received_bytes = NULL;
                peer_closed = true;
                break;
            }
            memcpy(state->buffer + buf_len, ptr, (size_t)data_len);
            buf_len += (size_t)data_len;
            server_release_object(state->received_bytes);
            state->received_bytes = NULL;
        }

        if (peer_closed && buf_len == 0)
            break;
        if (!bad_request && request_len == 0)
            break;

        if (buf_len < buf_cap)
            state->buffer[buf_len] = '\0';
        else
            state->buffer[buf_cap - 1] = '\0';

        state->request = server_req_new();
        if (!state->request || bad_request ||
            !parse_http_request(state->buffer, request_len, state->request)) {
            server_release_object(state->request);
            state->request = NULL;
            const char *bad =
                "HTTP/1.1 400 Bad Request\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
            rt_tcp_send_all_raw(tcp, bad, (int64_t)strlen(bad));
            break;
        }

        state->response = server_res_new();
        if (!state->response) {
            server_release_object(state->request);
            state->request = NULL;
            const char *failure =
                "HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\nContent-Length: "
                "0\r\n\r\n";
            rt_tcp_send_all_raw(tcp, failure, (int64_t)strlen(failure));
            break;
        }
        build_route_response(server, state->request, state->response);

        int keep_alive =
            request_allows_keep_alive(state->request) && !response_forces_close(state->response);
        size_t resp_len = 0;
        state->wire_response = build_response(state->response, keep_alive, &resp_len);
        if (state->wire_response) {
            rt_tcp_send_all_raw(tcp, state->wire_response, (int64_t)resp_len);
            if (!rt_tcp_is_open(tcp))
                keep_alive = 0;
            free(state->wire_response);
            state->wire_response = NULL;
        } else {
            keep_alive = 0;
        }

        server_release_object(state->response);
        state->response = NULL;
        server_release_object(state->request);
        state->request = NULL;

        if (!keep_alive)
            break;
        if (request_len < buf_len) {
            memmove(state->buffer, state->buffer + request_len, buf_len - request_len);
            buf_len -= request_len;
        } else {
            buf_len = 0;
        }
    }
done:
    http_connection_state_cleanup(server, tcp, state);
}

typedef struct {
    rt_http_server_impl *server;
    void *tcp;
} http_conn_task_t;

/// @brief Worker-pool task wrapper around `handle_connection`.
///
/// The accept loop allocates an `http_conn_task_t` (server + tcp pair)
/// and submits it to the worker pool; the worker calls this function,
/// which forwards to `handle_connection` and frees the task struct
/// afterwards. The TCP handle's lifetime is owned by `handle_connection`,
/// not the task.
///
/// @param arg `http_conn_task_t *` cast to `void *`.
static void handle_connection_task(void *arg) {
    http_conn_task_t *task = (http_conn_task_t *)arg;
    if (!task)
        return;
    http_connection_state_t *state =
        (http_connection_state_t *)calloc(1, sizeof(http_connection_state_t));
    if (!state) {
        rt_tcp_close(task->tcp);
        server_release_object(task->tcp);
        server_release_object(task->server);
        free(task);
        rt_trap("HttpServer: connection state allocation failed");
        return;
    }
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[256];
        const char *error = rt_trap_get_error();
        snprintf(saved_error,
                 sizeof(saved_error),
                 "%s",
                 error && error[0] ? error : "HttpServer: connection handler failed");
        rt_trap_clear_recovery();
        http_connection_state_cleanup(task->server, task->tcp, state);
        free(state);
        server_release_object(task->tcp);
        server_release_object(task->server);
        free(task);
        rt_trap(saved_error);
        return;
    }
    handle_connection(task->server, task->tcp, state);
    rt_trap_clear_recovery();
    free(state);
    server_release_object(task->tcp);
    server_release_object(task->server);
    free(task);
}

//=============================================================================
// Accept Loop
//=============================================================================

/// @brief Background thread entry point: accept connections forever
///        and dispatch each to the worker pool.
///
/// Loops on `rt_tcp_server_accept_for` with a 1-second timeout so the
/// thread can wake periodically to check `server->running` (the stop
/// signal). For each accepted TCP connection, allocates a small
/// `http_conn_task_t` and submits `handle_connection_task` to the
/// worker pool. If allocation or submission fails, the connection is
/// closed immediately.
///
/// The platform-specific signature differs (`unsigned __stdcall` on Windows
/// vs `void *` on POSIX) but the body is identical.
///
/// @param arg `rt_http_server_impl *` cast to `void *`.
/// @return 0 / NULL on normal shutdown.
#ifdef _WIN32
static unsigned __stdcall accept_loop(void *arg)
#else
static void *accept_loop(void *arg)
#endif
{
    rt_http_server_impl *server = (rt_http_server_impl *)arg;

    while (server_is_running(server)) {
        void *tcp = rt_tcp_server_accept_for(server->tcp_server, 1000);
        if (!tcp)
            continue; // Timeout — check running flag

        if (!server_is_running(server)) {
            rt_tcp_close(tcp);
            server_release_object(tcp);
            break;
        }

        http_conn_task_t *task = (http_conn_task_t *)malloc(sizeof(http_conn_task_t));
        if (!task) {
            rt_tcp_close(tcp);
            server_release_object(tcp);
            continue;
        }

        task->server = server;
        task->tcp = tcp;
        rt_obj_retain_maybe(server);

        if (!server->worker_pool ||
            !rt_threadpool_submit(server->worker_pool, (void *)handle_connection_task, task)) {
            server_release_object(server);
            free(task);
            rt_tcp_close(tcp);
            server_release_object(tcp);
        }
    }

#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

//=============================================================================
// Public API
//=============================================================================

/// @brief `HttpServer.New(port)` — create a new HTTP server bound to
///        the given TCP port.
///
/// Allocates a GC-managed server impl, attaches the finalizer (which
/// will tear down routes / worker pool / accept loop on collection),
/// constructs the route trie, and stores the requested port for later use by
/// `Start`. The TCP listener and CPU-sized worker pool are created lazily by
/// `Start`, so stopped instances do not reserve threads or sockets.
///
/// Traps on invalid port (`<0` or `>65535`) or allocation failure.
///
/// @param port TCP port number, 0..65535. Port 0 asks the OS for an ephemeral port.
/// @return GC-managed `HttpServer` handle.
void *rt_http_server_new(int64_t port) {
    if (port < 0 || port > 65535) {
        rt_trap("HttpServer: invalid port");
        return NULL;
    }

    rt_http_server_impl *volatile server = NULL;
    volatile int finalizer_installed = 0;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[256];
        const char *error = rt_trap_get_error();
        snprintf(saved_error,
                 sizeof(saved_error),
                 "%s",
                 error && error[0] ? error : "HttpServer: construction failed");
        rt_trap_clear_recovery();
        if (server) {
            rt_http_server_impl *partial = (rt_http_server_impl *)server;
            if (finalizer_installed) {
                server_release_object(partial);
            } else {
                if (partial->lifecycle_lock_initialized) {
                    HTTP_SERVER_MUTEX_DESTROY(&partial->lifecycle_lock);
                    partial->lifecycle_lock_initialized = false;
                }
                if (partial->state_lock_initialized) {
                    HTTP_SERVER_MUTEX_DESTROY(&partial->state_lock);
                    partial->state_lock_initialized = false;
                }
                server_release_object(partial);
            }
        }
        rt_trap(saved_error);
        return NULL;
    }

    server = (rt_http_server_impl *)rt_obj_new_i64(RT_HTTP_SERVER_CLASS_ID,
                                                   (int64_t)sizeof(rt_http_server_impl));
    if (!server)
        rt_trap("HttpServer: memory allocation failed");
    memset((void *)server, 0, sizeof(rt_http_server_impl));
    if (!HTTP_SERVER_MUTEX_INIT(&((rt_http_server_impl *)server)->state_lock))
        rt_trap("HttpServer: state mutex initialization failed");
    ((rt_http_server_impl *)server)->state_lock_initialized = true;
    if (!HTTP_SERVER_MUTEX_INIT(&((rt_http_server_impl *)server)->lifecycle_lock))
        rt_trap("HttpServer: lifecycle mutex initialization failed");
    ((rt_http_server_impl *)server)->lifecycle_lock_initialized = true;
    rt_obj_set_finalizer((void *)server, rt_http_server_finalize);
    finalizer_installed = 1;

    ((rt_http_server_impl *)server)->port = port;
    ((rt_http_server_impl *)server)->router = rt_http_router_new();
    if (!((rt_http_server_impl *)server)->router)
        rt_trap("HttpServer: router allocation failed");
    rt_trap_clear_recovery();
    return (void *)server;
}

/// @brief Helper used by every `Get`/`Post`/`Put`/`Delete` registrar.
///
/// Forwards `pattern` to the router via the method-specific `adder`
/// (one of `rt_http_router_{get,post,put,delete}`) and records the
/// `handler_tag` in the server's route entry so subsequent
/// `bind_handler` calls can resolve the binding by tag.
///
/// Traps on invalid handler tag or route-add failure.
///
/// @param obj         Server impl, opaque.
/// @param pattern     Route URL pattern (e.g. `"/users/:id"`).
/// @param handler_tag Tag string used to associate the route with a
///                    handler bound later.
/// @param adder       Method-specific router-add function pointer.
static void add_route_binding(void *obj,
                              rt_string pattern,
                              rt_string handler_tag,
                              void *(*adder)(void *, rt_string)) {
    if (!obj || !adder)
        return;

    rt_http_server_impl *server = http_server_checked(obj, "HttpServer: invalid server handle");
    if (!server)
        return;
    if (server_configuration_blocked(server)) {
        rt_trap("HttpServer: cannot add routes while running");
        return;
    }

    // Transactional registration (VDOC-144): validate the tag and reserve
    // every allocation BEFORE touching the router, then commit the route
    // entry with steps that cannot fail. A failure at any point leaves the
    // router and the route-entry table consistent with each other.
    const char *tag = NULL;
    size_t tag_len = 0;
    if (server_string_has_embedded_nul(handler_tag, &tag, &tag_len) || !tag || tag_len == 0) {
        rt_trap("HttpServer: invalid route handler tag");
        return;
    }
    char *volatile tag_copy = dup_cstr(tag);
    if (!tag_copy) {
        rt_trap("HttpServer: failed to register route");
        return;
    }

    volatile int lifecycle_locked = 0;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[256];
        const char *error = rt_trap_get_error();
        snprintf(saved_error,
                 sizeof(saved_error),
                 "%s",
                 error && error[0] ? error : "HttpServer: failed to register route");
        rt_trap_clear_recovery();
        if (lifecycle_locked)
            server_lifecycle_unlock(server);
        free((void *)tag_copy);
        rt_trap(saved_error);
        return;
    }

    server_lifecycle_lock(server);
    lifecycle_locked = 1;
    if (server_configuration_blocked(server)) {
        server_lifecycle_unlock(server);
        lifecycle_locked = 0;
        rt_trap_clear_recovery();
        free((void *)tag_copy);
        rt_trap("HttpServer: cannot add routes while running");
        return;
    }
    if (!ensure_route_capacity(server, server->route_count + 1)) {
        server_lifecycle_unlock(server);
        lifecycle_locked = 0;
        rt_trap_clear_recovery();
        free((void *)tag_copy);
        rt_trap("HttpServer: failed to register route");
        return;
    }

    // Last fallible step: the router add commits atomically inside the
    // router (and traps on an invalid pattern before committing anything).
    if (!adder(server->router, pattern)) {
        server_lifecycle_unlock(server);
        lifecycle_locked = 0;
        rt_trap_clear_recovery();
        free((void *)tag_copy);
        rt_trap("HttpServer: failed to register route");
        return;
    }

    route_entry_t *entry = &server->routes[server->route_count];
    memset(entry, 0, sizeof(*entry));
    entry->tag = (char *)tag_copy;
    server->route_count++;
    tag_copy = NULL;
    server_lifecycle_unlock(server);
    lifecycle_locked = 0;
    rt_trap_clear_recovery();
}

/// @brief `HttpServer.Get(pattern, handler_tag)` — register a GET route.
/// @param obj         HttpServer handle.
/// @param pattern     URL pattern.
/// @param handler_tag Handler tag string (resolved by `BindHandler`).
void rt_http_server_get(void *obj, rt_string pattern, rt_string handler_tag) {
    add_route_binding(obj, pattern, handler_tag, rt_http_router_get);
}

/// @brief `HttpServer.Post(pattern, handler_tag)` — register a POST route.
/// @param obj HttpServer handle.
/// @param pattern URL pattern.
/// @param handler_tag Handler tag resolved by `BindHandler`.
void rt_http_server_post(void *obj, rt_string pattern, rt_string handler_tag) {
    add_route_binding(obj, pattern, handler_tag, rt_http_router_post);
}

/// @brief `HttpServer.Put(pattern, handler_tag)` — register a PUT route.
/// @param obj HttpServer handle.
/// @param pattern URL pattern.
/// @param handler_tag Handler tag resolved by `BindHandler`.
void rt_http_server_put(void *obj, rt_string pattern, rt_string handler_tag) {
    add_route_binding(obj, pattern, handler_tag, rt_http_router_put);
}

/// @brief `HttpServer.Delete(pattern, handler_tag)` — register a DELETE route.
/// @param obj HttpServer handle.
/// @param pattern URL pattern.
/// @param handler_tag Handler tag resolved by `BindHandler`.
void rt_http_server_del(void *obj, rt_string pattern, rt_string handler_tag) {
    add_route_binding(obj, pattern, handler_tag, rt_http_router_delete);
}

/// @brief `HttpServer.BindHandler(tag, native_fn)` — bind a native C
///        callback to a previously-registered route tag.
///
/// `entry` should be a function pointer with signature
/// `void(rt_http_server_handler_fn)(req, res)`. Stored via the shared
/// `set_handler_binding` infrastructure with `native_handler_dispatch`
/// as the trampoline so dispatch can route through the same code path
/// as script-bridged handlers.
///
/// @param obj         HttpServer handle.
/// @param handler_tag Tag string matching one previously passed to
///                    `Get`/`Post`/etc.
/// @param entry       Native handler function pointer cast to `void *`.
void rt_http_server_bind_handler(void *obj, rt_string handler_tag, void *entry) {
    if (!obj || !entry)
        return;

    rt_http_server_impl *server = http_server_checked(obj, "HttpServer: invalid server handle");
    if (!server)
        return;

    if (server_configuration_blocked(server)) {
        rt_trap("HttpServer: cannot bind handlers while running");
        return;
    }

    const char *tag = NULL;
    size_t tag_len = 0;
    if (server_string_has_embedded_nul(handler_tag, &tag, &tag_len) || !tag || tag_len == 0)
        return;

    rt_http_server_handler_cleanup_fn old_cleanup = NULL;
    void *old_ctx = NULL;
    server_lifecycle_lock(server);
    if (server_configuration_blocked(server)) {
        server_lifecycle_unlock(server);
        rt_trap("HttpServer: cannot bind handlers while running");
        return;
    }
    int bound = set_handler_binding(
        server, tag, native_handler_dispatch, entry, NULL, &old_cleanup, &old_ctx);
    server_lifecycle_unlock(server);
    if (!bound) {
        rt_trap("HttpServer: failed to bind handler");
        return;
    }
    if (old_cleanup)
        old_cleanup(old_ctx);
}

/// @brief Script-bridge variant of `BindHandler` that takes an explicit
///        dispatch function, opaque context, and optional cleanup.
///
/// Used by Zia/BASIC frontends to install a closure-style handler:
/// `dispatch` is the trampoline that invokes the script handler;
/// `ctx` carries the script's environment (closure captures, this-
/// pointer, etc.); `cleanup` is invoked when the binding is replaced
/// or the server is finalized.
///
/// @param obj         HttpServer handle.
/// @param handler_tag Handler tag string.
/// @param dispatch    Dispatch function `void(*)(ctx, req, res)`.
/// @param ctx         Opaque context passed to dispatch.
/// @param cleanup     Optional cleanup `void(*)(ctx)`, or NULL.
void rt_http_server_bind_handler_dispatch(
    void *obj, rt_string handler_tag, void *dispatch, void *ctx, void *cleanup) {
    if (!obj || !dispatch)
        return;

    rt_http_server_impl *server = http_server_checked(obj, "HttpServer: invalid server handle");
    if (!server)
        return;

    if (server_configuration_blocked(server)) {
        rt_trap("HttpServer: cannot bind handlers while running");
        return;
    }

    const char *tag = NULL;
    size_t tag_len = 0;
    if (server_string_has_embedded_nul(handler_tag, &tag, &tag_len) || !tag || tag_len == 0)
        return;

    rt_http_server_handler_cleanup_fn old_cleanup = NULL;
    void *old_ctx = NULL;
    server_lifecycle_lock(server);
    if (server_configuration_blocked(server)) {
        server_lifecycle_unlock(server);
        rt_trap("HttpServer: cannot bind handlers while running");
        return;
    }
    int bound = set_handler_binding(server,
                                    tag,
                                    (rt_http_server_handler_dispatch_fn)dispatch,
                                    ctx,
                                    (rt_http_server_handler_cleanup_fn)cleanup,
                                    &old_cleanup,
                                    &old_ctx);
    server_lifecycle_unlock(server);
    if (!bound) {
        rt_trap("HttpServer: failed to bind handler");
        return;
    }
    if (old_cleanup)
        old_cleanup(old_ctx);
}

/// @brief `HttpServer.Start()` — open the listening socket and spin up
///        the accept loop on a background thread.
///
/// Idempotent: re-running on a server that's already running is a silent no-op.
/// Lazily creates the reusable worker pool on first start. Traps on NULL
/// receiver. The accept thread terminates when `running` is cleared by `Stop`.
///
/// @param obj HttpServer handle.
void rt_http_server_start(void *obj) {
    if (!obj) {
        rt_trap("HttpServer: NULL server");
        return;
    }

    rt_http_server_impl *server = http_server_checked(obj, "HttpServer: invalid server handle");
    if (!server)
        return;
    if (server_configuration_blocked(server)) {
        if (server_is_running(server))
            return;
        rt_trap("HttpServer: stop is in progress");
        return;
    }

    void *volatile listener = NULL;
    void *volatile new_pool = NULL;
    volatile int lifecycle_locked = 0;
    volatile int published = 0;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[256];
        int saved_net_code = rt_trap_get_net_code();
        const char *error = rt_trap_get_error();
        snprintf(saved_error,
                 sizeof(saved_error),
                 "%s",
                 error && error[0] ? error : "HttpServer: start failed");
        rt_trap_clear_recovery();
        if (published) {
            server_state_lock(server);
            server->running = false;
            if (server->tcp_server == listener)
                server->tcp_server = NULL;
            if (new_pool && server->worker_pool == new_pool)
                server->worker_pool = NULL;
            server_state_unlock(server);
        }
        if (listener)
            rt_tcp_server_close((void *)listener);
        server_release_object((void *)listener);
        server_release_object((void *)new_pool);
        if (lifecycle_locked)
            server_lifecycle_unlock(server);
        if (saved_net_code)
            rt_trap_net(saved_error, saved_net_code);
        else
            rt_trap(saved_error);
        return;
    }

    server_lifecycle_lock(server);
    lifecycle_locked = 1;
    server_state_lock(server);
    int already_running = server->running ? 1 : 0;
    int stopping = server->stopping ? 1 : 0;
    int64_t requested_port = server->port;
    server_state_unlock(server);
    if (already_running) {
        rt_trap_clear_recovery();
        server_lifecycle_unlock(server);
        return;
    }
    if (stopping)
        rt_trap("HttpServer: stop is in progress");

    listener = rt_tcp_server_listen(requested_port);
    if (!listener)
        rt_trap("HttpServer: failed to bind listener");
    int64_t bound_port = rt_tcp_server_port((void *)listener);
    if (!server->worker_pool) {
        new_pool = rt_threadpool_new(http_server_default_worker_count());
        if (!new_pool)
            rt_trap("HttpServer: worker pool allocation failed");
    }

    server_state_lock(server);
    server->tcp_server = (void *)listener;
    if (new_pool)
        server->worker_pool = (void *)new_pool;
    server->port = bound_port;
    server->running = true;
    server->thread_started = false;
    server_state_unlock(server);
    published = 1;

#ifdef _WIN32
    uintptr_t thread_handle = _beginthreadex(NULL, 0, accept_loop, server, 0, NULL);
    server->accept_thread = thread_handle ? (HANDLE)thread_handle : NULL;
    int thread_started = server->accept_thread != NULL;
#else
    int thread_started = pthread_create(&server->accept_thread, NULL, accept_loop, server) == 0;
#endif
    if (!thread_started)
        rt_trap("HttpServer: failed to start accept thread");

    server_state_lock(server);
    server->thread_started = true;
    server_state_unlock(server);
    listener = NULL;
    new_pool = NULL;
    published = 0;
    rt_trap_clear_recovery();
    server_lifecycle_unlock(server);
}

/// @brief `HttpServer.Stop()` — tear down the listener and join the
///        accept thread.
///
/// Clears the running flag, closes the underlying TCP server (which
/// unblocks any in-progress `accept()` call), then waits for the accept
/// thread to exit. Drains
/// any in-flight worker tasks before returning. NULL receiver is a
/// silent no-op. Safe to call repeatedly.
///
/// @param obj HttpServer handle.
void rt_http_server_stop(void *obj) {
    if (!obj)
        return;
    rt_http_server_impl *server = http_server_checked(obj, "HttpServer: invalid server handle");
    if (!server)
        return;
    void *listener = NULL;
    int had_thread = 0;
#ifdef _WIN32
    HANDLE accept_thread = NULL;
#else
    pthread_t accept_thread;
#endif

    server_lifecycle_lock(server);
    server_state_lock(server);
    if (!server->running && !server->thread_started && !server->tcp_server) {
        server_state_unlock(server);
        server_lifecycle_unlock(server);
        return;
    }
    server->stopping = true;
    server->running = false;
    listener = server->tcp_server;
    had_thread = server->thread_started ? 1 : 0;
    accept_thread = server->accept_thread;
    server_state_unlock(server);

    if (listener)
        rt_tcp_server_close(listener);

    {
        int conn_count = 0;
        void **active_conns = server_snapshot_active_conns(server, &conn_count);
        for (int i = 0; i < conn_count; i++) {
            server_interrupt_tcp_conn(active_conns[i]);
            if (active_conns[i] && rt_obj_release_check0(active_conns[i]))
                rt_obj_free(active_conns[i]);
        }
        free(active_conns);
    }

    if (had_thread) {
#ifdef _WIN32
        if (rt_win32_join_thread_handle(accept_thread) == RT_WIN32_THREAD_JOIN_FAILED)
            abort();
#else
        if (pthread_equal(pthread_self(), accept_thread) != 0)
            pthread_detach(accept_thread);
        else
            pthread_join(accept_thread, NULL);
#endif
    }

    server_state_lock(server);
    if (server->tcp_server == listener)
        server->tcp_server = NULL;
    if (had_thread)
        server->thread_started = false;
    server_state_unlock(server);

    if (listener && rt_obj_release_check0(listener))
        rt_obj_free(listener);

    void *worker_pool = server->worker_pool;
    if (worker_pool && rt_threadpool_current_worker_pool() != worker_pool) {
        jmp_buf recovery;
        rt_trap_set_recovery(&recovery);
        if (setjmp(recovery) != 0) {
            char saved_error[256];
            const char *error = rt_trap_get_error();
            snprintf(saved_error,
                     sizeof(saved_error),
                     "%s",
                     error && error[0] ? error : "HttpServer: worker drain failed");
            rt_trap_clear_recovery();
            server_state_lock(server);
            server->stopping = false;
            server_state_unlock(server);
            server_lifecycle_unlock(server);
            rt_trap(saved_error);
            return;
        }
        rt_threadpool_wait(worker_pool);
        rt_trap_clear_recovery();
    }

    server_state_lock(server);
    server->stopping = false;
    server_state_unlock(server);
    server_lifecycle_unlock(server);
}

/// @brief `HttpServer.Port` — return the TCP port the server is bound to.
///
/// Returns 0 if the receiver is NULL or the server was constructed with
/// no explicit port and never started. After a successful `Start`, this
/// reflects the actual kernel-assigned port (useful when the server was
/// constructed with port 0 to request an ephemeral port).
///
/// @param obj HttpServer handle.
/// @return Bound TCP port, or 0 if not bound / NULL receiver.
int64_t rt_http_server_port(void *obj) {
    if (!obj)
        return 0;
    rt_http_server_impl *server = http_server_checked(obj, "HttpServer: invalid server handle");
    if (!server)
        return 0;
    server_state_lock(server);
    int64_t port = server->port;
    server_state_unlock(server);
    return port;
}

/// @brief `HttpServer.IsRunning` — whether the accept loop is active.
///
/// Returns the latched `running` flag set by `Start` and cleared by
/// `Stop`. This is a snapshot — there is a brief window during teardown
/// where the flag is false but the accept thread is still draining.
///
/// @param obj HttpServer handle.
/// @return 1 if running, 0 if stopped or NULL receiver.
int8_t rt_http_server_is_running(void *obj) {
    if (!obj)
        return 0;
    rt_http_server_impl *server = http_server_checked(obj, "HttpServer: invalid server handle");
    return server && server_is_running(server) ? 1 : 0;
}

//=============================================================================
// ServerReq Accessors
//=============================================================================

/// @brief `ServerReq.Method` — return the HTTP method as a string.
///
/// Returns "GET", "POST", "PUT", "DELETE", etc. as parsed from the
/// request line. Returns "" for NULL receiver or a malformed request.
/// The result is a fresh string the caller owns.
///
/// @param obj ServerReq handle.
/// @return Owned `rt_string` containing the method, never NULL.
rt_string rt_server_req_method(void *obj) {
    if (!obj)
        return rt_string_from_bytes("", 0);
    server_req_t *req = server_req_checked(obj);
    if (!req)
        return NULL;
    return req->method ? rt_string_from_bytes(req->method, strlen(req->method))
                       : rt_string_from_bytes("", 0);
}

/// @brief `ServerReq.Path` — return the request path (without query string).
///
/// The query string is stripped during request parsing and stored
/// separately (accessible via `Query`). Returns "" for NULL receiver
/// or a request with no path (defensive only — the parser rejects
/// such inputs upstream).
///
/// @param obj ServerReq handle.
/// @return Owned `rt_string` containing the path, never NULL.
rt_string rt_server_req_path(void *obj) {
    if (!obj)
        return rt_string_from_bytes("", 0);
    server_req_t *req = server_req_checked(obj);
    if (!req)
        return NULL;
    return req->path ? rt_string_from_bytes(req->path, strlen(req->path))
                     : rt_string_from_bytes("", 0);
}

/// @brief `ServerReq.Body` — return the raw request body.
///
/// The body is exactly what came over the wire after the headers — no
/// content-decoding, no charset normalization. Length is taken from the
/// parsed Content-Length, so binary bodies with embedded NULs are
/// preserved. Returns "" if there is no body or the receiver is NULL.
///
/// @param obj ServerReq handle.
/// @return Owned `rt_string` containing the body bytes, never NULL.
rt_string rt_server_req_body(void *obj) {
    if (!obj)
        return rt_string_from_bytes("", 0);
    server_req_t *req = server_req_checked(obj);
    if (!req)
        return NULL;
    return req->body ? rt_string_from_bytes(req->body, req->body_len) : rt_string_from_bytes("", 0);
}

/// @brief `ServerReq.Header(name)` — case-insensitive header lookup.
///
/// HTTP header names are case-insensitive per RFC 7230 §3.2, so the
/// lookup name is lowercased before consulting the parsed header map
/// (which stores keys in lowercase form). Allocates a transient lower
/// buffer for the lookup; returns "" for missing headers, NULL receiver,
/// NULL name, or allocation failure. The returned string is a fresh
/// copy so the caller can outlive the request object.
///
/// @param obj  ServerReq handle.
/// @param name Header name (any case).
/// @return Owned `rt_string` with header value, or "" if not present.
rt_string rt_server_req_header(void *obj, rt_string name) {
    if (!obj)
        return rt_string_from_bytes("", 0);
    server_req_t *req = server_req_checked(obj);
    if (!req)
        return NULL;
    if (!req->headers)
        return rt_string_from_bytes("", 0);

    const char *name_cstr = NULL;
    size_t len = 0;
    if (server_string_has_embedded_nul(name, &name_cstr, &len) || !name_cstr)
        return rt_string_from_bytes("", 0);

    char *lower = (char *)malloc(len + 1);
    if (!lower)
        return rt_string_from_bytes("", 0);
    for (size_t i = 0; i <= len; i++) {
        char c = name_cstr[i];
        lower[i] = (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
    }

    rt_string header = map_get_lower_header_string(req->headers, lower);
    free(lower);
    if (!header)
        return rt_string_from_bytes("", 0);
    const char *header_cstr = rt_string_cstr(header);
    int64_t header_len = rt_str_len(header);
    return header_cstr && header_len >= 0 ? rt_string_from_bytes(header_cstr, (size_t)header_len)
                                          : rt_string_from_bytes("", 0);
}

/// @brief `ServerReq.Param(name)` — read a captured route parameter.
///
/// Route parameters are the placeholders in route patterns like
/// `/users/:id` — at match time the segment is captured and stored on
/// the request's `params` table. Returns "" if the parameter wasn't
/// captured for this request, the receiver is NULL, or the params
/// table is missing.
///
/// @param obj  ServerReq handle.
/// @param name Parameter name (without the leading `:`).
/// @return Owned `rt_string` with the captured value, or "" if absent.
rt_string rt_server_req_param(void *obj, rt_string name) {
    if (!obj)
        return rt_string_from_bytes("", 0);
    server_req_t *req = server_req_checked(obj);
    if (!req)
        return NULL;
    if (!req->params)
        return rt_string_from_bytes("", 0);
    rt_string value = rt_route_match_param(req->params, name);
    const char *value_cstr = rt_string_cstr(value);
    int64_t value_len = value ? rt_str_len(value) : 0;
    rt_string result = value_cstr && value_len >= 0
                           ? rt_string_from_bytes(value_cstr, (size_t)value_len)
                           : rt_string_from_bytes("", 0);
    rt_string_unref(value);
    return result;
}

/// @brief `ServerReq.Query(name)` — read a query-string value.
///
/// Linear-scans the raw query string for `name=value` pairs separated
/// by `&`. The matched value is URL-decoded (percent-escapes and `+`
/// → space) before being returned. Only the first occurrence of a
/// duplicated name is returned. Returns "" if the parameter is absent,
/// the receiver/name is NULL, or no query string was present.
///
/// @param obj  ServerReq handle.
/// @param name Query-parameter name.
/// @return Owned `rt_string` with the URL-decoded value, or "" if absent.
rt_string rt_server_req_query(void *obj, rt_string name) {
    if (!obj)
        return rt_string_from_bytes("", 0);
    server_req_t *req = server_req_checked(obj);
    if (!req)
        return NULL;
    if (!req->query)
        return rt_string_from_bytes("", 0);

    const char *n = NULL;
    size_t nlen = 0;
    if (server_string_has_embedded_nul(name, &n, &nlen) || !n)
        return rt_string_from_bytes("", 0);

    const char *p = req->query;
    while (p && *p) {
        const char *amp = strchr(p, '&');
        const char *param_end = amp ? amp : p + strlen(p);
        const char *eq = find_char_bounded(p, (size_t)(param_end - p), '=');
        size_t key_len = eq ? (size_t)(eq - p) : (size_t)(param_end - p);
        rt_string raw_key = rt_string_from_bytes(p, key_len);
        rt_string decoded_key = rt_url_decode(raw_key);
        const char *decoded_key_cstr = rt_string_cstr(decoded_key);
        int64_t decoded_key_len = decoded_key ? rt_str_len(decoded_key) : -1;
        int match = decoded_key_cstr && decoded_key_len == (int64_t)nlen &&
                    memcmp(decoded_key_cstr, n, nlen) == 0;
        rt_string_unref(decoded_key);
        rt_string_unref(raw_key);
        if (match) {
            const char *val = eq ? eq + 1 : param_end;
            size_t vlen = (size_t)(param_end - val);
            rt_string raw = rt_string_from_bytes(val, vlen);
            rt_string decoded = rt_url_decode(raw);
            if (!decoded)
                decoded = rt_string_from_bytes("", 0);
            rt_string_unref(raw);
            return decoded;
        }
        p = amp ? amp + 1 : NULL;
    }

    return rt_string_from_bytes("", 0);
}

//=============================================================================
// ServerRes Accessors
//=============================================================================

/// @brief `ServerRes.Status(code)` — set the HTTP status code.
///
/// Stores `code` on the response builder. Only the HTTP status range
/// 100-599 is accepted. Returns the receiver to
/// support chained builder syntax: `res.Status(404).Send("Not Found")`.
/// NULL receiver is a no-op that returns NULL.
///
/// @param obj  ServerRes handle.
/// @param code HTTP status code (e.g., 200, 404, 500).
/// @return The same `obj` for chaining.
void *rt_server_res_status(void *obj, int64_t code) {
    if (!obj)
        return obj;
    if (code < 100 || code > 599) {
        rt_trap("HttpServer: invalid status");
        return obj;
    }
    server_res_t *res = server_res_checked(obj);
    if (!res)
        return NULL;
    res->status_code = (int)code;
    return obj;
}

/// @brief `ServerRes.Header(name, value)` — set a custom response header.
///
/// Adds an entry to the response header map (creating it lazily on
/// first call). Silently rejects:
///   - NULL or non-c-string name/value (defensive),
///   - any name or value containing CR or LF (header injection guard),
///   - "server-managed" header names like Content-Length,
///     Transfer-Encoding, and Connection that the framework owns.
/// Returns the receiver for chaining. NULL receiver is a no-op.
///
/// @param obj   ServerRes handle.
/// @param name  Header name.
/// @param value Header value.
/// @return The same `obj` for chaining.
void *rt_server_res_header(void *obj, rt_string name, rt_string value) {
    if (!obj)
        return obj;
    server_res_t *res = server_res_checked(obj);
    if (!res)
        return NULL;
    const char *name_cstr = NULL;
    const char *value_cstr = NULL;
    size_t name_len = 0;
    size_t value_len = 0;
    if (server_string_has_embedded_nul(name, &name_cstr, &name_len) ||
        server_string_has_embedded_nul(value, &value_cstr, &value_len) || !name_cstr ||
        !value_cstr || !server_header_name_valid(name_cstr, name_len) ||
        contains_crlf(value_cstr) || is_server_managed_header_name(name_cstr))
        return obj;
    if (!res->headers) {
        res->headers = rt_map_new();
        if (!res->headers) {
            rt_trap("HttpServer: response header allocation failed");
            return obj;
        }
    }
    if (!rt_http_header_map_set_ci(res->headers, name, (void *)value)) {
        rt_trap("HttpServer: response header allocation failed");
        return obj;
    }
    return obj;
}

/// @brief `ServerRes.Send(body)` — finalize the response with a text body.
///
/// Replaces any previously buffered body, captures the new body via
/// `strdup` (so the response outlives the source string), and marks
/// the response as `sent` so the dispatch loop knows to flush rather
/// than fall through to the default 404. NULL receiver is a no-op;
/// NULL body collapses to a zero-length response.
///
/// @param obj  ServerRes handle.
/// @param body Body bytes to send (may be NULL → empty body).
void rt_server_res_send(void *obj, rt_string body) {
    if (!obj)
        return;
    server_res_t *res = server_res_checked(obj);
    if (!res)
        return;
    if (body && !rt_string_is_handle(body)) {
        rt_trap("HttpServer: invalid response body");
        return;
    }
    const char *b = body ? rt_string_cstr(body) : NULL;
    int64_t body_len64 = body ? rt_str_len(body) : 0;
    if ((body && !b) || body_len64 < 0 || (uint64_t)body_len64 > (uint64_t)SIZE_MAX) {
        rt_trap("HttpServer: invalid response body");
        return;
    }
    size_t body_len = (size_t)body_len64;
    char *replacement = (b && body_len > 0) ? (char *)malloc(body_len) : NULL;
    if (b && body_len > 0 && !replacement) {
        rt_trap("HttpServer: response body allocation failed");
        return;
    }
    if (replacement)
        memcpy(replacement, b, body_len);
    char *old_body = res->body;
    res->body = replacement;
    res->body_len = replacement ? body_len : 0;
    res->sent = true;
    free(old_body);
}

/// @brief `ServerRes.Json(jsonStr)` — finalize as a JSON response.
///
/// Stages an exact body copy and a cloned header map containing one
/// case-insensitive `Content-Type: application/json` entry. Both replacements
/// publish together only after every allocation succeeds; a recovered trap
/// therefore observes the complete prior response rather than a mixed old-body
/// or new-header state. This function does *not* validate JSON syntax. NULL
/// receiver is a no-op.
///
/// @param obj      ServerRes handle.
/// @param json_str Pre-serialized JSON body.
void rt_server_res_json(void *obj, rt_string json_str) {
    if (!obj)
        return;
    server_res_t *res = server_res_checked(obj);
    if (!res)
        return;
    if (json_str && !rt_string_is_handle(json_str)) {
        rt_trap("HttpServer: invalid JSON response body");
        return;
    }

    const char *json_bytes = json_str ? rt_string_cstr(json_str) : NULL;
    int64_t json_len64 = json_str ? rt_str_len(json_str) : 0;
    if ((json_str && !json_bytes) || json_len64 < 0 || (uint64_t)json_len64 > (uint64_t)SIZE_MAX) {
        rt_trap("HttpServer: invalid JSON response body");
        return;
    }

    char *volatile replacement_body = NULL;
    void *volatile replacement_headers = NULL;
    void *volatile keys = NULL;
    rt_string volatile ct_key = NULL;
    rt_string volatile ct_val = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[256];
        const char *error = rt_trap_get_error();
        snprintf(saved_error,
                 sizeof(saved_error),
                 "%s",
                 error && error[0] ? error : "HttpServer: JSON response allocation failed");
        rt_trap_clear_recovery();
        rt_string_unref((rt_string)ct_val);
        rt_string_unref((rt_string)ct_key);
        server_release_object((void *)keys);
        server_release_object((void *)replacement_headers);
        free((void *)replacement_body);
        rt_trap(saved_error);
        return;
    }

    size_t json_len = (size_t)json_len64;
    if (json_len > 0) {
        replacement_body = (char *)malloc(json_len);
        if (!replacement_body)
            rt_trap("HttpServer: JSON response body allocation failed");
        memcpy((void *)replacement_body, json_bytes, json_len);
    }

    replacement_headers = rt_map_new();
    if (!replacement_headers)
        rt_trap("HttpServer: response header allocation failed");
    if (res->headers) {
        keys = rt_map_keys(res->headers);
        if (!keys)
            rt_trap("HttpServer: response header snapshot failed");
        int64_t count = rt_seq_len((void *)keys);
        if (count < 0)
            rt_trap("HttpServer: invalid response header snapshot");
        for (int64_t i = 0; i < count; i++) {
            rt_string key = (rt_string)rt_seq_get((void *)keys, i);
            void *value = rt_map_get(res->headers, key);
            rt_map_set((void *)replacement_headers, key, value);
        }
        server_release_object((void *)keys);
        keys = NULL;
    }

    ct_key = rt_const_cstr("Content-Type");
    ct_val = rt_const_cstr("application/json");
    if (!ct_key || !ct_val ||
        !rt_http_header_map_set_ci(
            (void *)replacement_headers, (rt_string)ct_key, (void *)ct_val)) {
        rt_trap("HttpServer: response header allocation failed");
    }
    rt_string_unref((rt_string)ct_val);
    rt_string_unref((rt_string)ct_key);
    ct_val = NULL;
    ct_key = NULL;

    void *old_headers = res->headers;
    char *old_body = res->body;
    res->headers = (void *)replacement_headers;
    res->body = (char *)replacement_body;
    res->body_len = json_len;
    res->sent = true;
    replacement_headers = NULL;
    replacement_body = NULL;
    rt_trap_clear_recovery();
    server_release_object(old_headers);
    free(old_body);
}

/// @brief Synchronous router entry-point — parse a raw request string
///        and return the wire-format response.
///
/// Bypasses the socket, accept loop, and worker pool — useful for unit
/// tests and embedding scenarios where the host owns the I/O. The
/// pipeline is identical to the live path: parse → route → execute →
/// build wire response. Returns a 400 response if parsing fails.
/// Releases all transient header/body/req state before returning.
///
/// @param obj         HttpServer handle.
/// @param raw_request Raw HTTP request (request line + headers + body).
/// @return Owned `rt_string` containing the full HTTP/1.1 response.
typedef struct {
    server_req_t *request;
    server_res_t *response;
    char *wire_response;
    bool registered;
} http_sync_request_state_t;

/// @brief Detach managed request state and release its activity reservation.
/// @details Fields are nulled before their release operations so an unusual
///          finalizer trap can re-enter full transaction cleanup safely.
/// @param server Valid HttpServer payload.
/// @param state Heap-resident synchronous transaction state.
static void http_sync_request_release_managed(rt_http_server_impl *server,
                                              http_sync_request_state_t *state) {
    if (!state)
        return;
    server_res_t *response = state->response;
    state->response = NULL;
    server_release_object(response);
    server_req_t *request = state->request;
    state->request = NULL;
    server_release_object(request);
    if (state->registered) {
        state->registered = false;
        server_lifecycle_lock(server);
        server_state_lock(server);
        if (server->active_sync_requests > 0)
            server->active_sync_requests--;
        server_state_unlock(server);
        server_lifecycle_unlock(server);
    }
}

/// @brief Release one synchronous request transaction and its activity slot.
/// @details Request/response handles and the native wire buffer are detached
///          first. The activity counter is then decremented under lifecycle ->
///          state lock order so route/binding mutation cannot overlap a handler.
/// @param server Valid HttpServer payload.
/// @param state Heap-resident synchronous transaction state.
static void http_sync_request_cleanup(rt_http_server_impl *server,
                                      http_sync_request_state_t *state) {
    if (!state)
        return;
    free(state->wire_response);
    state->wire_response = NULL;
    http_sync_request_release_managed(server, state);
}

/// @brief Process one complete HTTP/1 request through the live parser, router, and serializer.
/// @details Registers a synchronous activity slot so route or binding mutation
///          cannot overlap handler dispatch. Malformed requests produce a
///          complete 400 wire response; trap recovery releases all managed and
///          native transaction state.
/// @param obj HttpServer handle; null returns an empty String.
/// @param raw_request Complete request bytes in a managed String.
/// @return Caller-owned wire-response String, or null after a returning trap hook.
void *rt_http_server_process_request(void *obj, rt_string raw_request) {
    if (!obj)
        return rt_string_from_bytes("", 0);

    rt_http_server_impl *server = http_server_checked(obj, "HttpServer: invalid server handle");
    if (!server)
        return NULL;
    if (raw_request && !rt_string_is_handle(raw_request)) {
        rt_trap("HttpServer: invalid request String");
        return NULL;
    }

    http_sync_request_state_t *state =
        (http_sync_request_state_t *)calloc(1, sizeof(http_sync_request_state_t));
    if (!state) {
        rt_trap("HttpServer: synchronous request state allocation failed");
        return NULL;
    }

    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[256];
        const char *error = rt_trap_get_error();
        snprintf(saved_error,
                 sizeof(saved_error),
                 "%s",
                 error && error[0] ? error : "HttpServer: synchronous request failed");
        rt_trap_clear_recovery();
        http_sync_request_cleanup(server, state);
        free(state);
        rt_trap(saved_error);
        return NULL;
    }

    server_lifecycle_lock(server);
    server_state_lock(server);
    if (server->finalizing) {
        server_state_unlock(server);
        server_lifecycle_unlock(server);
        free(state);
        rt_trap_clear_recovery();
        rt_trap("HttpServer: server is finalizing");
        return NULL;
    }
    if (server->active_sync_requests == SIZE_MAX) {
        server_state_unlock(server);
        server_lifecycle_unlock(server);
        free(state);
        rt_trap_clear_recovery();
        rt_trap("HttpServer: too many synchronous requests");
        return NULL;
    }
    server->active_sync_requests++;
    state->registered = true;
    server_state_unlock(server);
    server_lifecycle_unlock(server);

    const char *raw = raw_request ? rt_string_cstr(raw_request) : NULL;
    int64_t raw_len = raw_request ? rt_str_len(raw_request) : 0;
    state->request = server_req_new();
    if (!state->request)
        rt_trap("HttpServer: request allocation failed");
    if (!raw || raw_len < 0 || (uint64_t)raw_len > (uint64_t)SIZE_MAX ||
        !parse_http_request(raw, (size_t)raw_len, state->request)) {
        http_sync_request_release_managed(server, state);
        rt_string result = rt_string_from_bytes(
            "HTTP/1.1 400 Bad Request\r\nConnection: close\r\nContent-Length: 0\r\n\r\n",
            sizeof("HTTP/1.1 400 Bad Request\r\nConnection: close\r\nContent-Length: 0\r\n\r\n") -
                1);
        free(state);
        rt_trap_clear_recovery();
        return result;
    }

    state->response = server_res_new();
    if (!state->response)
        rt_trap("HttpServer: response allocation failed");
    build_route_response(server, state->request, state->response);

    int keep_alive =
        request_allows_keep_alive(state->request) && !response_forces_close(state->response);
    size_t resp_len = 0;
    state->wire_response = build_response(state->response, keep_alive, &resp_len);
    http_sync_request_release_managed(server, state);
    rt_string result = state->wire_response ? rt_string_from_bytes(state->wire_response, resp_len)
                                            : rt_string_from_bytes("", 0);
    free(state->wire_response);
    state->wire_response = NULL;
    free(state);
    rt_trap_clear_recovery();
    return result;
}
