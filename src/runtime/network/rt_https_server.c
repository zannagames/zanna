//===----------------------------------------------------------------------===//

#if !defined(_WIN32)
#define _DARWIN_C_SOURCE 1
#define _GNU_SOURCE 1
#endif
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/network/rt_https_server.c
// Purpose: TLS-backed HTTP/1.1 and HTTP/2 server with routing and request/response objects.
// Key invariants:
//   - Accept loop runs in a dedicated thread.
//   - Each connection is dispatched to the worker pool for request handling.
//   - Sequential HTTP/1.1 keep-alive requests are supported when framing permits.
//   - Routes matched via embedded HttpRouter.
// Ownership/Lifetime:
//   - Server object GC-managed. Stop() or finalizer stops accept loop.
//   - Handler-visible ServerReq/ServerRes snapshots are managed objects whose
//     producer references are released after dispatch; handlers may retain them.
// Links: rt_https_server.h (API), rt_http_router.h (routing), rt_tls.c
//
//===----------------------------------------------------------------------===//

#include "rt_https_server.h"
#include "rt_http2.h"
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
#include "rt_tls_server_internal.h"
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
#define WIN32_LEAN_AND_MEAN
#include <process.h>
#include <windows.h>
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
typedef CRITICAL_SECTION https_server_mutex_t;
#define HTTPS_SERVER_MUTEX_INIT(m) (InitializeCriticalSection(m), 1)
#define HTTPS_SERVER_MUTEX_LOCK(m) EnterCriticalSection(m)
#define HTTPS_SERVER_MUTEX_UNLOCK(m) LeaveCriticalSection(m)
#define HTTPS_SERVER_MUTEX_DESTROY(m) DeleteCriticalSection(m)
#else
#include <pthread.h>
#include <strings.h>
#include <unistd.h>
typedef pthread_mutex_t https_server_mutex_t;
#define HTTPS_SERVER_MUTEX_INIT(m) (pthread_mutex_init(m, NULL) == 0)
#define HTTPS_SERVER_MUTEX_LOCK(m) pthread_mutex_lock(m)
#define HTTPS_SERVER_MUTEX_UNLOCK(m) pthread_mutex_unlock(m)
#define HTTPS_SERVER_MUTEX_DESTROY(m) pthread_mutex_destroy(m)
#endif

#include "rt_trap.h"
extern void rt_trap_net(const char *msg, int err_code);
extern int rt_trap_get_net_code(void);

#if defined(__GNUC__) || defined(__clang__)
#define HTTPS_MAYBE_UNUSED __attribute__((unused))
#else
#define HTTPS_MAYBE_UNUSED
#endif

//=============================================================================
// Internal Structures
//=============================================================================

#define HTTP_REQ_MAX_LINE 8192
#define HTTP_REQ_MAX_HEADERS 100
#define HTTP_REQ_MAX_BODY (16 * 1024 * 1024) // 16 MB
#define HTTP_REQ_MAX_ENCODED_BODY (HTTP_REQ_MAX_BODY + 65536)
#define HTTPS_SERVER_MAX_ACTIVE_CONNS 4096

/// @brief Compute the internal worker-pool size for HTTPS server instances.
/// @details Uses the runtime machine adapter to query logical CPU count, then
///          clamps the result to the range accepted by @ref rt_threadpool_new.
///          Keeping this local avoids exposing a new public runtime C ABI symbol
///          for a server implementation detail.
/// @return Worker count in the inclusive range 1..1024.
static int64_t https_server_default_worker_count(void) {
    int64_t cores = rt_machine_cores();
    if (cores < 1)
        cores = 1;
    if (cores > 1024)
        cores = 1024;
    return cores;
}

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

typedef struct {
    int status_code;
    void *headers; // Map
    char *body;
    size_t body_len;
    bool sent;
} server_res_t;

typedef struct {
    char *tag;
} route_entry_t;

typedef struct {
    char *tag;
    rt_http_server_handler_dispatch_fn dispatch;
    void *ctx;
    rt_http_server_handler_cleanup_fn cleanup;
} handler_binding_t;

typedef struct {
    void *router;     // HttpRouter
    void *tcp_server; // TcpServer
    void *worker_pool;
    rt_tls_server_ctx_t *tls_ctx;
    char *cert_file;
    char *key_file;
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
    https_server_mutex_t state_lock;
    bool state_lock_initialized;
    https_server_mutex_t lifecycle_lock;
    bool lifecycle_lock_initialized;
} rt_http_server_impl;

static void free_server_req(server_req_t *req);
static void free_server_res(server_res_t *res);
static void build_route_response(rt_http_server_impl *server, server_req_t *req, server_res_t *res);
static void free_route_entries(rt_http_server_impl *server);
static void free_handler_bindings(rt_http_server_impl *server);
static int contains_crlf(const char *text);
static int is_server_managed_header_name(const char *name);
static int response_forces_close(const server_res_t *res);

static void https_server_state_lock(rt_http_server_impl *server) {
    if (server && server->state_lock_initialized)
        HTTPS_SERVER_MUTEX_LOCK(&server->state_lock);
}

static void https_server_state_unlock(rt_http_server_impl *server) {
    if (server && server->state_lock_initialized)
        HTTPS_SERVER_MUTEX_UNLOCK(&server->state_lock);
}

/// @brief Acquire the HTTPS configuration/lifecycle mutex.
/// @details Lock order is lifecycle before state whenever both are required.
/// @param server Fully initialized HttpsServer payload.
static void https_server_lifecycle_lock(rt_http_server_impl *server) {
    if (server && server->lifecycle_lock_initialized)
        HTTPS_SERVER_MUTEX_LOCK(&server->lifecycle_lock);
}

/// @brief Release the HTTPS configuration/lifecycle mutex.
/// @param server Fully initialized HttpsServer payload.
static void https_server_lifecycle_unlock(rt_http_server_impl *server) {
    if (server && server->lifecycle_lock_initialized)
        HTTPS_SERVER_MUTEX_UNLOCK(&server->lifecycle_lock);
}

/// @brief Validate and cast a public HttpsServer receiver.
/// @details Rejects forged, stale, unrelated, undersized, and partially
///          initialized objects before TLS or mutex state is accessed.
/// @param obj Candidate managed receiver.
/// @param message Diagnostic raised when validation fails.
/// @return Valid server payload, or NULL after trapping.
static rt_http_server_impl *https_server_checked(void *obj, const char *message) {
    if (!rt_obj_is_instance(obj, RT_HTTPS_SERVER_CLASS_ID, sizeof(rt_http_server_impl))) {
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

static int https_server_is_running(rt_http_server_impl *server) {
    int running = 0;
    if (!server)
        return 0;
    https_server_state_lock(server);
    running = server->running ? 1 : 0;
    https_server_state_unlock(server);
    return running;
}

/// @brief Test whether HTTPS route/binding mutation is currently forbidden.
/// @details The pre-lifecycle state snapshot prevents a worker handler from
///          blocking behind Stop while Stop waits for that worker to finish.
///          Callers repeat the check after acquiring the lifecycle mutex.
/// @param server Valid HttpsServer payload.
/// @return One while running, stopping, finalizing, or synchronously dispatching.
static int https_server_configuration_blocked(rt_http_server_impl *server) {
    int blocked = 1;
    if (!server)
        return 1;
    https_server_state_lock(server);
    blocked = server->running || server->stopping || server->finalizing ||
              server->active_sync_requests > 0;
    https_server_state_unlock(server);
    return blocked;
}

static int https_server_register_active_conn(rt_http_server_impl *server, void *conn) {
    int ok = 1;
    if (!server || !conn)
        return 0;
    https_server_state_lock(server);
    for (int i = 0; i < server->active_conn_count; i++) {
        if (server->active_conns[i] == conn) {
            https_server_state_unlock(server);
            return 1;
        }
    }
    if (server->active_conn_count >= HTTPS_SERVER_MAX_ACTIVE_CONNS) {
        https_server_state_unlock(server);
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
    https_server_state_unlock(server);
    return ok;
}

static void https_server_unregister_active_conn(rt_http_server_impl *server, void *conn) {
    if (!server || !conn)
        return;
    https_server_state_lock(server);
    for (int i = 0; i < server->active_conn_count; i++) {
        if (server->active_conns[i] == conn) {
            server->active_conn_count--;
            server->active_conns[i] = server->active_conns[server->active_conn_count];
            server->active_conns[server->active_conn_count] = NULL;
            break;
        }
    }
    https_server_state_unlock(server);
}

static void **https_server_snapshot_active_conns(rt_http_server_impl *server, int *count_out) {
    void **snapshot = NULL;
    int count = 0;
    if (count_out)
        *count_out = 0;
    if (!server)
        return NULL;
    https_server_state_lock(server);
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
    https_server_state_unlock(server);
    if (count_out)
        *count_out = count;
    return snapshot;
}

static void https_server_interrupt_conn(void *conn) {
    socket_t fd = (socket_t)rt_tls_get_socket((rt_tls_session_t *)conn);
    if (fd == INVALID_SOCK)
        return;
#ifdef _WIN32
    shutdown(fd, SD_BOTH);
#else
    shutdown(fd, SHUT_RDWR);
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

/// @brief Copy one managed certificate/key path into native owned storage.
/// @details Stable String identity and exact length are validated before payload
///          access. Empty paths, embedded NUL bytes, and size overflow trap with
///          the supplied diagnostic instead of reaching filesystem/TLS code.
/// @param path Managed path String.
/// @param message Diagnostic for invalid input or native allocation failure.
/// @return Newly allocated NUL-terminated path, or NULL after trapping.
static char *https_server_copy_path(rt_string path, const char *message) {
    if (!path || !rt_string_is_handle(path)) {
        rt_trap(message);
        return NULL;
    }
    const char *bytes = rt_string_cstr(path);
    int64_t len64 = rt_str_len(path);
    if (!bytes || len64 <= 0 || (uint64_t)len64 >= (uint64_t)SIZE_MAX ||
        memchr(bytes, '\0', (size_t)len64)) {
        rt_trap(message);
        return NULL;
    }
    size_t len = (size_t)len64;
    char *copy = (char *)malloc(len + 1);
    if (!copy) {
        rt_trap(message);
        return NULL;
    }
    memcpy(copy, bytes, len);
    copy[len] = '\0';
    return copy;
}

/// @brief Validate a non-empty managed String for native tag use.
/// @details Returns length-bounded bytes only after stable identity, size, and
///          embedded-NUL checks. No trap is raised so callers can unlock before
///          publishing their context-specific diagnostic.
/// @param text Candidate managed String.
/// @param bytes_out Receives borrowed bytes on success.
/// @param len_out Receives exact byte length on success.
/// @return One for valid non-empty text; zero otherwise.
static int https_server_tag_bytes(rt_string text, const char **bytes_out, size_t *len_out) {
    if (bytes_out)
        *bytes_out = NULL;
    if (len_out)
        *len_out = 0;
    if (!text || !rt_string_is_handle(text))
        return 0;
    const char *bytes = rt_string_cstr(text);
    int64_t len64 = rt_str_len(text);
    if (!bytes || len64 <= 0 || (uint64_t)len64 > (uint64_t)SIZE_MAX)
        return 0;
    size_t len = (size_t)len64;
    if (memchr(bytes, '\0', len))
        return 0;
    if (bytes_out)
        *bytes_out = bytes;
    if (len_out)
        *len_out = len;
    return 1;
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
    rt_http_server_impl *server = (rt_http_server_impl *)obj;
    https_server_state_lock(server);
    server->finalizing = true;
    https_server_state_unlock(server);
    rt_https_server_stop(server);
    free_route_entries(server);
    free_handler_bindings(server);
    if (server->router && rt_obj_release_check0(server->router))
        rt_obj_free(server->router);
    server->router = NULL;
    if (server->worker_pool && rt_obj_release_check0(server->worker_pool))
        rt_obj_free(server->worker_pool);
    server->worker_pool = NULL;
    rt_tls_server_ctx_free(server->tls_ctx);
    server->tls_ctx = NULL;
    free(server->cert_file);
    free(server->key_file);
    server->cert_file = NULL;
    server->key_file = NULL;
    free(server->active_conns);
    server->active_conns = NULL;
    server->active_conn_count = 0;
    server->active_conn_cap = 0;
    if (server->lifecycle_lock_initialized) {
        HTTPS_SERVER_MUTEX_DESTROY(&server->lifecycle_lock);
        server->lifecycle_lock_initialized = false;
    }
    if (server->state_lock_initialized) {
        HTTPS_SERVER_MUTEX_DESTROY(&server->state_lock);
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

/// @brief Release every owned ServerRes field and zero the payload.
/// @param res Response payload; NULL is a safe no-op.
static void free_server_res(server_res_t *res) {
    if (!res)
        return;
    free(res->body);
    if (res->headers && rt_obj_release_check0(res->headers))
        rt_obj_free(res->headers);
    memset(res, 0, sizeof(*res));
}

/// @brief Finalize a managed ServerReq created by the HTTPS server.
/// @param obj ServerReq payload supplied by the object runtime.
static void https_server_req_finalize(void *obj) {
    free_server_req((server_req_t *)obj);
}

/// @brief Finalize a managed ServerRes created by the HTTPS server.
/// @param obj ServerRes payload supplied by the object runtime.
static void https_server_res_finalize(void *obj) {
    free_server_res((server_res_t *)obj);
}

/// @brief Allocate an empty managed ServerReq with shared stable identity.
/// @return Caller-owned request handle, or NULL on allocation failure.
static server_req_t *https_server_req_new(void) {
    server_req_t *req =
        (server_req_t *)rt_obj_new_i64(RT_SERVER_REQ_CLASS_ID, (int64_t)sizeof(server_req_t));
    if (!req)
        return NULL;
    memset(req, 0, sizeof(*req));
    rt_obj_set_finalizer(req, https_server_req_finalize);
    return req;
}

/// @brief Allocate an empty managed ServerRes with shared stable identity.
/// @return Caller-owned response handle, or NULL on allocation failure.
static server_res_t *https_server_res_new(void) {
    server_res_t *res =
        (server_res_t *)rt_obj_new_i64(RT_SERVER_RES_CLASS_ID, (int64_t)sizeof(server_res_t));
    if (!res)
        return NULL;
    memset(res, 0, sizeof(*res));
    rt_obj_set_finalizer(res, https_server_res_finalize);
    return res;
}

/// @brief Drop one caller-owned managed object reference.
/// @param obj Managed object, or NULL.
static void https_server_release_object(void *obj) {
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
static HTTPS_MAYBE_UNUSED int rt_https_server_test_parse_request(const char *raw,
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
static HTTPS_MAYBE_UNUSED char *rt_https_server_test_build_response(int status_code,
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

static int https_conn_is_open(rt_tls_session_t *tls) {
    return tls && rt_tls_get_socket(tls) >= 0;
}

static void https_conn_set_recv_timeout(rt_tls_session_t *tls, int timeout_ms) {
    socket_t fd = tls ? (socket_t)rt_tls_get_socket(tls) : INVALID_SOCK;
    if (fd != INVALID_SOCK)
        set_socket_timeout(fd, timeout_ms, true);
}

static void https_conn_set_send_timeout(rt_tls_session_t *tls, int timeout_ms) {
    socket_t fd = tls ? (socket_t)rt_tls_get_socket(tls) : INVALID_SOCK;
    if (fd != INVALID_SOCK)
        set_socket_timeout(fd, timeout_ms, false);
}

static long https_conn_recv(rt_tls_session_t *tls, uint8_t *buf, size_t len) {
    return rt_tls_recv(tls, buf, len);
}

static int https_conn_send_all(rt_tls_session_t *tls, const void *buf, size_t len) {
    return rt_tls_send(tls, buf, len) == (long)len;
}

static long https_http2_read(void *ctx, uint8_t *buf, size_t len) {
    return https_conn_recv((rt_tls_session_t *)ctx, buf, len);
}

static int https_http2_write(void *ctx, const uint8_t *buf, size_t len) {
    return https_conn_send_all((rt_tls_session_t *)ctx, buf, len);
}

static int http2_request_to_server_req(const rt_http2_request_t *src, server_req_t *dst) {
    char *path_copy = NULL;
    char *qmark = NULL;
    if (!src || !dst || !src->method || !src->path || src->path[0] != '/')
        return 0;

    memset(dst, 0, sizeof(*dst));
    dst->method = strdup(src->method);
    path_copy = strdup(src->path);
    if (!dst->method || !path_copy)
        goto fail;

    qmark = strchr(path_copy, '?');
    if (qmark) {
        *qmark = '\0';
        dst->query = strdup(qmark + 1);
        if (!dst->query)
            goto fail;
    }

    dst->path = strdup(path_copy);
    if (!dst->path)
        goto fail;
    dst->http_major = 2;
    dst->http_minor = 0;
    dst->headers = rt_map_new();
    if (!dst->headers)
        goto fail;
    for (const rt_http2_header_t *it = src->headers; it; it = it->next) {
        rt_string key = NULL;
        rt_string value = NULL;
        if (!it->name || !it->value)
            continue;
        key = rt_string_from_bytes(it->name, strlen(it->name));
        value = rt_string_from_bytes(it->value, strlen(it->value));
        if (!key || !value) {
            if (key)
                rt_string_unref(key);
            if (value)
                rt_string_unref(value);
            goto fail;
        }
        rt_map_set(dst->headers, key, value);
        rt_string_unref(key);
        rt_string_unref(value);
    }
    if (src->body_len > 0) {
        dst->body = (char *)malloc(src->body_len + 1);
        if (!dst->body)
            goto fail;
        memcpy(dst->body, src->body, src->body_len);
        dst->body[src->body_len] = '\0';
        dst->body_len = src->body_len;
    }
    free(path_copy);
    return 1;

fail:
    free(path_copy);
    free_server_req(dst);
    memset(dst, 0, sizeof(*dst));
    return 0;
}

/// @brief Snapshot and normalize managed response headers for HTTP/2.
/// @details A single managed key snapshot is held across traversal. Every key
///          and value is identity-checked and validated by exact byte length;
///          server-managed fields are omitted, and accepted field names are
///          copied in lowercase as required by HTTP/2. A local trap boundary
///          releases partial native and managed state before returning failure.
/// @param src Managed response payload to serialize.
/// @param headers_out Receives an owned native HTTP/2 header chain.
/// @return One on complete conversion; zero on validation/allocation failure.
static int server_headers_to_http2(const server_res_t *src, rt_http2_header_t **headers_out) {
    rt_http2_header_t *volatile headers = NULL;
    void *volatile keys = NULL;
    char *volatile lowercase_name = NULL;
    if (!headers_out)
        return 0;
    *headers_out = NULL;

    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        rt_trap_clear_recovery();
        free((void *)lowercase_name);
        https_server_release_object((void *)keys);
        rt_http2_headers_free((rt_http2_header_t *)headers);
        return 0;
    }

    if (src && src->headers) {
        keys = rt_map_keys(src->headers);
        if (!keys)
            goto fail;
        int64_t count = rt_seq_len((void *)keys);
        if (count < 0)
            goto fail;
        for (int64_t i = 0; i < count; i++) {
            rt_string key = (rt_string)rt_seq_get((void *)keys, i);
            void *val = rt_map_get(src->headers, key);
            const char *key_bytes = NULL;
            const char *value_bytes = NULL;
            size_t key_len = 0;
            size_t value_len = 0;
            if (!response_header_string_bytes(key, &key_bytes, &key_len) ||
                !response_header_string_bytes((rt_string)val, &value_bytes, &value_len) ||
                !server_header_name_valid(key_bytes, key_len)) {
                continue;
            }
            if (is_server_managed_header_name(key_bytes))
                continue;
            if (key_len == SIZE_MAX)
                goto fail;
            lowercase_name = (char *)malloc(key_len + 1);
            if (!lowercase_name)
                goto fail;
            for (size_t j = 0; j < key_len; j++) {
                unsigned char c = (unsigned char)key_bytes[j];
                ((char *)lowercase_name)[j] =
                    c >= 'A' && c <= 'Z' ? (char)(c + ('a' - 'A')) : (char)c;
            }
            ((char *)lowercase_name)[key_len] = '\0';
            rt_http2_header_t *working = (rt_http2_header_t *)headers;
            if (!rt_http2_header_append_copy(&working, (char *)lowercase_name, value_bytes))
                goto fail;
            headers = working;
            free((void *)lowercase_name);
            lowercase_name = NULL;
        }
        https_server_release_object((void *)keys);
        keys = NULL;
    }

    size_t body_len = response_wire_body_len(src);
    if (body_len > 0 && (!src || !src->body))
        goto fail;
    char len_buf[32];
    int len_chars = snprintf(len_buf, sizeof(len_buf), "%zu", body_len);
    if (len_chars < 0 || (size_t)len_chars >= sizeof(len_buf))
        goto fail;
    rt_http2_header_t *working = (rt_http2_header_t *)headers;
    if (!rt_http2_header_append_copy(&working, "content-length", len_buf))
        goto fail;
    headers = working;

    rt_trap_clear_recovery();
    *headers_out = (rt_http2_header_t *)headers;
    headers = NULL;
    return 1;

fail:
    rt_trap_clear_recovery();
    free((void *)lowercase_name);
    https_server_release_object((void *)keys);
    rt_http2_headers_free((rt_http2_header_t *)headers);
    return 0;
}

typedef struct {
    void *accepted_tcp;
    rt_tls_session_t *tls;
    bool registered;
    char *buffer;
    server_req_t *request;
    server_res_t *response;
    char *wire_response;
    rt_http2_conn_t *http2;
    rt_http2_request_t http2_request;
    bool http2_request_active;
    rt_http2_header_t *http2_response_headers;
} https_connection_state_t;

/// @brief Release every resource owned by one HTTPS worker transaction.
/// @details The heap-resident state survives `longjmp`, allowing the task-level
///          recovery boundary to clean TLS handshake, HTTP/1, and HTTP/2
///          failures uniformly. TLS ownership is detached before Close because
///          Close consumes the session's producer reference.
/// @param server Server that owns the active TLS-session registry.
/// @param state Heap-resident connection transaction.
static void https_connection_state_cleanup(rt_http_server_impl *server,
                                           https_connection_state_t *state) {
    if (!state)
        return;
    rt_http2_header_t *response_headers = state->http2_response_headers;
    state->http2_response_headers = NULL;
    rt_http2_headers_free(response_headers);
    if (state->http2_request_active) {
        state->http2_request_active = false;
        rt_http2_request_free(&state->http2_request);
    }
    if (state->http2) {
        rt_http2_conn_t *http2 = state->http2;
        state->http2 = NULL;
        rt_http2_conn_free(http2);
    }
    char *wire_response = state->wire_response;
    state->wire_response = NULL;
    free(wire_response);
    server_res_t *response = state->response;
    state->response = NULL;
    https_server_release_object(response);
    server_req_t *request = state->request;
    state->request = NULL;
    https_server_release_object(request);
    char *buffer = state->buffer;
    state->buffer = NULL;
    free(buffer);

    rt_tls_session_t *tls = state->tls;
    state->tls = NULL;
    if (state->registered) {
        state->registered = false;
        https_server_unregister_active_conn(server, tls);
    }
    if (tls)
        rt_tls_close(tls);

    void *accepted_tcp = state->accepted_tcp;
    state->accepted_tcp = NULL;
    if (accepted_tcp) {
        rt_tcp_close(accepted_tcp);
        https_server_release_object(accepted_tcp);
    }
}

static void handle_connection_http2(rt_http_server_impl *server, https_connection_state_t *state) {
    rt_tls_session_t *tls = state->tls;
    rt_http2_io_t io;

    io.ctx = tls;
    io.read = https_http2_read;
    io.write = https_http2_write;
    state->http2 = rt_http2_server_new(&io);
    if (!state->http2) {
        https_connection_state_cleanup(server, state);
        return;
    }

    if (!https_server_register_active_conn(server, tls)) {
        https_connection_state_cleanup(server, state);
        return;
    }
    state->registered = true;
    https_conn_set_recv_timeout(tls, 30000);
    https_conn_set_send_timeout(tls, 30000);

    while (https_conn_is_open(tls) && https_server_is_running(server) &&
           rt_http2_conn_is_usable(state->http2)) {
        int close_after = 0;

        memset(&state->http2_request, 0, sizeof(state->http2_request));
        state->http2_request_active = true;

        if (!rt_http2_server_receive_request(
                state->http2, HTTP_REQ_MAX_BODY, &state->http2_request))
            break;
        state->request = https_server_req_new();
        if (!state->request ||
            !http2_request_to_server_req(&state->http2_request, state->request)) {
            https_server_release_object(state->request);
            state->request = NULL;
            (void)rt_http2_server_send_response(
                state->http2, state->http2_request.stream_id, 400, NULL, NULL, 0);
            rt_http2_request_free(&state->http2_request);
            state->http2_request_active = false;
            break;
        }

        state->response = https_server_res_new();
        if (!state->response) {
            https_server_release_object(state->request);
            state->request = NULL;
            rt_http2_request_free(&state->http2_request);
            state->http2_request_active = false;
            break;
        }
        build_route_response(server, state->request, state->response);
        size_t response_body_len = response_wire_body_len(state->response);
        const uint8_t *response_body =
            response_body_len > 0 ? (const uint8_t *)state->response->body : NULL;
        if (!server_headers_to_http2(state->response, &state->http2_response_headers) ||
            !rt_http2_server_send_response(state->http2,
                                           state->http2_request.stream_id,
                                           state->response->status_code,
                                           state->http2_response_headers,
                                           response_body,
                                           response_body_len)) {
            rt_http2_headers_free(state->http2_response_headers);
            state->http2_response_headers = NULL;
            https_server_release_object(state->response);
            state->response = NULL;
            https_server_release_object(state->request);
            state->request = NULL;
            rt_http2_request_free(&state->http2_request);
            state->http2_request_active = false;
            break;
        }

        close_after = response_forces_close(state->response);
        rt_http2_headers_free(state->http2_response_headers);
        state->http2_response_headers = NULL;
        https_server_release_object(state->response);
        state->response = NULL;
        https_server_release_object(state->request);
        state->request = NULL;
        rt_http2_request_free(&state->http2_request);
        state->http2_request_active = false;
        if (close_after)
            break;
    }

    https_connection_state_cleanup(server, state);
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
/// @param state  Heap-resident TLS/HTTP transaction state.
static void handle_connection(rt_http_server_impl *server, https_connection_state_t *state) {
    rt_tls_session_t *tls = state->tls;
    if (strcmp(rt_tls_get_negotiated_alpn(tls), "h2") == 0) {
        handle_connection_http2(server, state);
        return;
    }
    if (!https_server_register_active_conn(server, tls)) {
        https_connection_state_cleanup(server, state);
        return;
    }
    state->registered = true;
    https_conn_set_recv_timeout(tls, 30000);
    https_conn_set_send_timeout(tls, 30000);
    size_t buf_cap = 4096;
    size_t buf_len = 0;
    state->buffer = (char *)malloc(buf_cap);
    if (!state->buffer)
        goto done;

    while (https_conn_is_open(tls) && https_server_is_running(server)) {
        size_t request_len = 0;
        bool bad_request = false;
        bool peer_closed = false;

        while (https_conn_is_open(tls)) {
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

            long data_len =
                https_conn_recv(tls, (uint8_t *)state->buffer + buf_len, buf_cap - buf_len);
            if (data_len <= 0) {
                peer_closed = true;
                break;
            }
            buf_len += (size_t)data_len;
        }

        if (peer_closed && buf_len == 0)
            break;
        if (!bad_request && request_len == 0)
            break;

        if (buf_len < buf_cap)
            state->buffer[buf_len] = '\0';
        else
            state->buffer[buf_cap - 1] = '\0';

        state->request = https_server_req_new();
        if (!state->request || bad_request ||
            !parse_http_request(state->buffer, request_len, state->request)) {
            https_server_release_object(state->request);
            state->request = NULL;
            const char *bad =
                "HTTP/1.1 400 Bad Request\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
            https_conn_send_all(tls, bad, strlen(bad));
            break;
        }

        state->response = https_server_res_new();
        if (!state->response) {
            https_server_release_object(state->request);
            state->request = NULL;
            break;
        }
        build_route_response(server, state->request, state->response);

        int keep_alive =
            request_allows_keep_alive(state->request) && !response_forces_close(state->response);
        size_t resp_len = 0;
        state->wire_response = build_response(state->response, keep_alive, &resp_len);
        if (state->wire_response) {
            if (!https_conn_send_all(tls, state->wire_response, resp_len))
                keep_alive = 0;
            free(state->wire_response);
            state->wire_response = NULL;
        } else {
            keep_alive = 0;
        }

        https_server_release_object(state->response);
        state->response = NULL;
        https_server_release_object(state->request);
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
    https_connection_state_cleanup(server, state);
}

typedef struct {
    rt_http_server_impl *server;
    void *tcp;
} http_conn_task_t;

static void close_accepted_tcp_handle(void *tcp) {
    if (!tcp)
        return;
    rt_tcp_close(tcp);
    if (rt_obj_release_check0(tcp))
        rt_obj_free(tcp);
}

/// @brief Worker-pool task wrapper around `handle_connection`.
///
/// The accept loop allocates an `http_conn_task_t` (server + accepted TCP
/// handle) and submits it to the worker pool. Performing the TLS handshake in
/// the worker keeps a slow or rejected handshake from blocking the accept loop
/// and delaying later clients.
///
/// @param arg `http_conn_task_t *` cast to `void *`.
static void handle_connection_task(void *arg) {
    http_conn_task_t *task = (http_conn_task_t *)arg;
    rt_http_server_impl *server = task ? task->server : NULL;
    https_connection_state_t *state =
        (https_connection_state_t *)calloc(1, sizeof(https_connection_state_t));
    if (!state) {
        close_accepted_tcp_handle(task ? task->tcp : NULL);
        https_server_release_object(server);
        free(task);
        rt_trap("HttpsServer: connection state allocation failed");
        return;
    }
    state->accepted_tcp = task ? task->tcp : NULL;

    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[256];
        const char *error = rt_trap_get_error();
        snprintf(saved_error,
                 sizeof(saved_error),
                 "%s",
                 error && error[0] ? error : "HttpsServer: connection handler failed");
        rt_trap_clear_recovery();
        https_connection_state_cleanup(server, state);
        free(state);
        https_server_release_object(server);
        free(task);
        rt_trap(saved_error);
        return;
    }

    if (server && state->accepted_tcp && https_server_is_running(server)) {
        state->tls = rt_tls_server_accept_socket((intptr_t)rt_tcp_socket_fd(state->accepted_tcp),
                                                 server->tls_ctx);
        rt_tcp_detach_socket(state->accepted_tcp);
        https_server_release_object(state->accepted_tcp);
        state->accepted_tcp = NULL;
    }

    if (state->tls)
        handle_connection(server, state);
    else
        https_connection_state_cleanup(server, state);

    rt_trap_clear_recovery();
    free(state);
    https_server_release_object(server);
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

    while (https_server_is_running(server)) {
        void *tcp = rt_tcp_server_accept_for(server->tcp_server, 1000);
        if (!tcp)
            continue; // Timeout — check running flag

        if (!https_server_is_running(server)) {
            close_accepted_tcp_handle(tcp);
            break;
        }

        http_conn_task_t *task = (http_conn_task_t *)malloc(sizeof(http_conn_task_t));
        if (!task) {
            close_accepted_tcp_handle(tcp);
            continue;
        }

        task->server = server;
        task->tcp = tcp;
        rt_obj_retain_maybe(server);

        if (!server->worker_pool ||
            !rt_threadpool_submit(server->worker_pool, (void *)handle_connection_task, task)) {
            https_server_release_object(server);
            free(task);
            close_accepted_tcp_handle(tcp);
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

/// @brief `HttpsServer.New(port, certFile, keyFile)` — create a new HTTPS server bound to
///        the given TCP port.
///
/// Allocates a GC-managed server impl, attaches the finalizer (which
/// will tear down routes / worker pool / accept loop on collection),
/// constructs the route trie and TLS context, and stores the requested port for
/// later use by `Start`. The TCP listener and CPU-sized worker pool are created
/// lazily by `Start`, so stopped instances reserve neither threads nor sockets.
///
/// Traps on invalid port (`<0` or `>65535`) or allocation failure.
///
/// @param port TCP port number, 0..65535; zero requests an ephemeral port at Start().
/// @return GC-managed `HttpsServer` handle.
void *rt_https_server_new(int64_t port, rt_string cert_file, rt_string key_file) {
    if (port < 0 || port > 65535) {
        rt_trap("HttpsServer: invalid port");
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
                 error && error[0] ? error : "HttpsServer: construction failed");
        rt_trap_clear_recovery();
        if (server) {
            rt_http_server_impl *partial = (rt_http_server_impl *)server;
            if (finalizer_installed) {
                https_server_release_object(partial);
            } else {
                if (partial->lifecycle_lock_initialized) {
                    HTTPS_SERVER_MUTEX_DESTROY(&partial->lifecycle_lock);
                    partial->lifecycle_lock_initialized = false;
                }
                if (partial->state_lock_initialized) {
                    HTTPS_SERVER_MUTEX_DESTROY(&partial->state_lock);
                    partial->state_lock_initialized = false;
                }
                https_server_release_object(partial);
            }
        }
        rt_trap(saved_error);
        return NULL;
    }

    server = (rt_http_server_impl *)rt_obj_new_i64(RT_HTTPS_SERVER_CLASS_ID,
                                                   (int64_t)sizeof(rt_http_server_impl));
    if (!server)
        rt_trap("HttpsServer: memory allocation failed");
    memset((void *)server, 0, sizeof(rt_http_server_impl));
    if (!HTTPS_SERVER_MUTEX_INIT(&((rt_http_server_impl *)server)->state_lock))
        rt_trap("HttpsServer: state mutex initialization failed");
    ((rt_http_server_impl *)server)->state_lock_initialized = true;
    if (!HTTPS_SERVER_MUTEX_INIT(&((rt_http_server_impl *)server)->lifecycle_lock))
        rt_trap("HttpsServer: lifecycle mutex initialization failed");
    ((rt_http_server_impl *)server)->lifecycle_lock_initialized = true;
    rt_obj_set_finalizer((void *)server, rt_http_server_finalize);
    finalizer_installed = 1;

    ((rt_http_server_impl *)server)->port = port;
    ((rt_http_server_impl *)server)->cert_file =
        https_server_copy_path(cert_file, "HttpsServer: invalid certificate path");
    ((rt_http_server_impl *)server)->key_file =
        https_server_copy_path(key_file, "HttpsServer: invalid private-key path");
    ((rt_http_server_impl *)server)->router = rt_http_router_new();
    if (!((rt_http_server_impl *)server)->router)
        rt_trap("HttpsServer: router allocation failed");
    rt_tls_server_config_t tls_cfg;
    rt_tls_server_config_init(&tls_cfg);
    tls_cfg.cert_file = ((rt_http_server_impl *)server)->cert_file;
    tls_cfg.key_file = ((rt_http_server_impl *)server)->key_file;
    tls_cfg.alpn_protocol = "h2,http/1.1";
    ((rt_http_server_impl *)server)->tls_ctx = rt_tls_server_ctx_new(&tls_cfg);
    if (!((rt_http_server_impl *)server)->tls_ctx)
        rt_trap(rt_tls_server_last_error());

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

    rt_http_server_impl *server = https_server_checked(obj, "HttpsServer: invalid server handle");
    if (!server)
        return;
    if (https_server_configuration_blocked(server)) {
        rt_trap("HttpsServer: cannot add routes while running");
        return;
    }

    // Transactional registration (VDOC-144): validate the tag (rejecting
    // empty and embedded-NUL tags, matching the HTTP server) and reserve
    // every allocation BEFORE touching the router, then commit the route
    // entry with steps that cannot fail. A failure at any point leaves the
    // router and the route-entry table consistent with each other.
    const char *tag = NULL;
    if (!https_server_tag_bytes(handler_tag, &tag, NULL)) {
        rt_trap("HttpsServer: invalid route handler tag");
        return;
    }
    char *volatile tag_copy = dup_cstr(tag);
    if (!tag_copy) {
        rt_trap("HttpsServer: failed to register route");
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
                 error && error[0] ? error : "HttpsServer: failed to register route");
        rt_trap_clear_recovery();
        if (lifecycle_locked)
            https_server_lifecycle_unlock(server);
        free((void *)tag_copy);
        rt_trap(saved_error);
        return;
    }

    https_server_lifecycle_lock(server);
    lifecycle_locked = 1;
    if (https_server_configuration_blocked(server)) {
        https_server_lifecycle_unlock(server);
        lifecycle_locked = 0;
        rt_trap_clear_recovery();
        free((void *)tag_copy);
        rt_trap("HttpsServer: cannot add routes while running");
        return;
    }
    if (!ensure_route_capacity(server, server->route_count + 1)) {
        https_server_lifecycle_unlock(server);
        lifecycle_locked = 0;
        rt_trap_clear_recovery();
        free((void *)tag_copy);
        rt_trap("HttpsServer: failed to register route");
        return;
    }
    if (!adder(server->router, pattern)) {
        https_server_lifecycle_unlock(server);
        lifecycle_locked = 0;
        rt_trap_clear_recovery();
        free((void *)tag_copy);
        rt_trap("HttpsServer: failed to register route");
        return;
    }

    route_entry_t *entry = &server->routes[server->route_count];
    memset(entry, 0, sizeof(*entry));
    entry->tag = (char *)tag_copy;
    server->route_count++;
    tag_copy = NULL;
    https_server_lifecycle_unlock(server);
    lifecycle_locked = 0;
    rt_trap_clear_recovery();
}

/// @brief `HttpsServer.Get(pattern, handler_tag)` — register a GET route.
/// @param obj         HttpsServer handle.
/// @param pattern     URL pattern.
/// @param handler_tag Handler tag string (resolved by `BindHandler`).
void rt_https_server_get(void *obj, rt_string pattern, rt_string handler_tag) {
    add_route_binding(obj, pattern, handler_tag, rt_http_router_get);
}

/// @brief `HttpsServer.Post(pattern, handler_tag)` — register a POST route.
void rt_https_server_post(void *obj, rt_string pattern, rt_string handler_tag) {
    add_route_binding(obj, pattern, handler_tag, rt_http_router_post);
}

/// @brief `HttpsServer.Put(pattern, handler_tag)` — register a PUT route.
void rt_https_server_put(void *obj, rt_string pattern, rt_string handler_tag) {
    add_route_binding(obj, pattern, handler_tag, rt_http_router_put);
}

/// @brief `HttpsServer.Delete(pattern, handler_tag)` — register a DELETE route.
void rt_https_server_del(void *obj, rt_string pattern, rt_string handler_tag) {
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
void rt_https_server_bind_handler(void *obj, rt_string handler_tag, void *entry) {
    if (!obj || !entry)
        return;

    rt_http_server_impl *server = https_server_checked(obj, "HttpsServer: invalid server handle");
    if (!server)
        return;
    if (https_server_configuration_blocked(server)) {
        rt_trap("HttpsServer: cannot bind handlers while running");
        return;
    }

    const char *tag = NULL;
    if (!https_server_tag_bytes(handler_tag, &tag, NULL))
        return;

    rt_http_server_handler_cleanup_fn old_cleanup = NULL;
    void *old_ctx = NULL;
    https_server_lifecycle_lock(server);
    if (https_server_configuration_blocked(server)) {
        https_server_lifecycle_unlock(server);
        rt_trap("HttpsServer: cannot bind handlers while running");
        return;
    }
    int bound = set_handler_binding(
        server, tag, native_handler_dispatch, entry, NULL, &old_cleanup, &old_ctx);
    https_server_lifecycle_unlock(server);
    if (!bound) {
        rt_trap("HttpsServer: failed to bind handler");
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
void rt_https_server_bind_handler_dispatch(
    void *obj, rt_string handler_tag, void *dispatch, void *ctx, void *cleanup) {
    if (!obj || !dispatch)
        return;

    rt_http_server_impl *server = https_server_checked(obj, "HttpsServer: invalid server handle");
    if (!server)
        return;
    if (https_server_configuration_blocked(server)) {
        rt_trap("HttpsServer: cannot bind handlers while running");
        return;
    }

    const char *tag = NULL;
    if (!https_server_tag_bytes(handler_tag, &tag, NULL))
        return;

    rt_http_server_handler_cleanup_fn old_cleanup = NULL;
    void *old_ctx = NULL;
    https_server_lifecycle_lock(server);
    if (https_server_configuration_blocked(server)) {
        https_server_lifecycle_unlock(server);
        rt_trap("HttpsServer: cannot bind handlers while running");
        return;
    }
    int bound = set_handler_binding(server,
                                    tag,
                                    (rt_http_server_handler_dispatch_fn)dispatch,
                                    ctx,
                                    (rt_http_server_handler_cleanup_fn)cleanup,
                                    &old_cleanup,
                                    &old_ctx);
    https_server_lifecycle_unlock(server);
    if (!bound) {
        rt_trap("HttpsServer: failed to bind handler");
        return;
    }
    if (old_cleanup)
        old_cleanup(old_ctx);
}

/// @brief `HttpsServer.Start()` — open the TLS listener and spin up
///        the accept loop on a background thread.
///
/// Idempotent: re-running on a server that's already running is a silent no-op.
/// Lazily creates the reusable worker pool on first start. Traps on NULL
/// receiver. The accept thread terminates when `running` is cleared by `Stop`.
///
/// @param obj HttpServer handle.
void rt_https_server_start(void *obj) {
    if (!obj) {
        rt_trap("HttpsServer: NULL server");
        return;
    }

    rt_http_server_impl *server = https_server_checked(obj, "HttpsServer: invalid server handle");
    if (!server)
        return;
    if (https_server_configuration_blocked(server)) {
        if (https_server_is_running(server))
            return;
        rt_trap("HttpsServer: stop is in progress");
        return;
    }
    if (!server->tls_ctx) {
        rt_trap("HttpsServer: missing TLS context");
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
                 error && error[0] ? error : "HttpsServer: start failed");
        rt_trap_clear_recovery();
        if (published) {
            https_server_state_lock(server);
            server->running = false;
            if (server->tcp_server == listener)
                server->tcp_server = NULL;
            if (new_pool && server->worker_pool == new_pool)
                server->worker_pool = NULL;
            https_server_state_unlock(server);
        }
        if (listener)
            rt_tcp_server_close((void *)listener);
        https_server_release_object((void *)listener);
        https_server_release_object((void *)new_pool);
        if (lifecycle_locked)
            https_server_lifecycle_unlock(server);
        if (saved_net_code)
            rt_trap_net(saved_error, saved_net_code);
        else
            rt_trap(saved_error);
        return;
    }

    https_server_lifecycle_lock(server);
    lifecycle_locked = 1;
    https_server_state_lock(server);
    int already_running = server->running ? 1 : 0;
    int stopping = server->stopping ? 1 : 0;
    int64_t requested_port = server->port;
    https_server_state_unlock(server);
    if (already_running) {
        rt_trap_clear_recovery();
        https_server_lifecycle_unlock(server);
        return;
    }
    if (stopping)
        rt_trap("HttpsServer: stop is in progress");

    listener = rt_tcp_server_listen(requested_port);
    if (!listener)
        rt_trap("HttpsServer: failed to bind listener");
    int64_t bound_port = rt_tcp_server_port((void *)listener);
    if (!server->worker_pool) {
        new_pool = rt_threadpool_new(https_server_default_worker_count());
        if (!new_pool)
            rt_trap("HttpsServer: worker pool allocation failed");
    }

    https_server_state_lock(server);
    server->tcp_server = (void *)listener;
    if (new_pool)
        server->worker_pool = (void *)new_pool;
    server->port = bound_port;
    server->running = true;
    server->thread_started = false;
    https_server_state_unlock(server);
    published = 1;

#ifdef _WIN32
    uintptr_t thread_handle = _beginthreadex(NULL, 0, accept_loop, server, 0, NULL);
    server->accept_thread = thread_handle ? (HANDLE)thread_handle : NULL;
    int thread_started = server->accept_thread != NULL;
#else
    int thread_started = pthread_create(&server->accept_thread, NULL, accept_loop, server) == 0;
#endif
    if (!thread_started)
        rt_trap("HttpsServer: failed to start accept thread");

    https_server_state_lock(server);
    server->thread_started = true;
    https_server_state_unlock(server);
    listener = NULL;
    new_pool = NULL;
    published = 0;
    rt_trap_clear_recovery();
    https_server_lifecycle_unlock(server);
}

/// @brief `HttpsServer.Stop()` — tear down the listener and join the
///        accept thread.
///
/// Clears the running flag, closes the underlying TCP server (which
/// unblocks any in-progress `accept()` call), then waits for the accept
/// thread to exit. Drains
/// any in-flight worker tasks before returning. NULL receiver is a
/// silent no-op. Safe to call repeatedly.
///
/// @param obj HttpsServer handle.
void rt_https_server_stop(void *obj) {
    if (!obj)
        return;
    rt_http_server_impl *server = https_server_checked(obj, "HttpsServer: invalid server handle");
    if (!server)
        return;
    void *listener = NULL;
    int had_thread = 0;
#ifdef _WIN32
    HANDLE accept_thread = NULL;
#else
    pthread_t accept_thread;
#endif

    https_server_lifecycle_lock(server);
    https_server_state_lock(server);
    if (!server->running && !server->thread_started && !server->tcp_server) {
        https_server_state_unlock(server);
        https_server_lifecycle_unlock(server);
        return;
    }
    server->stopping = true;
    server->running = false;
    listener = server->tcp_server;
    had_thread = server->thread_started ? 1 : 0;
    accept_thread = server->accept_thread;
    https_server_state_unlock(server);

    if (listener)
        rt_tcp_server_close(listener);

    {
        int conn_count = 0;
        void **active_conns = https_server_snapshot_active_conns(server, &conn_count);
        for (int i = 0; i < conn_count; i++) {
            https_server_interrupt_conn(active_conns[i]);
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

    https_server_state_lock(server);
    if (server->tcp_server == listener)
        server->tcp_server = NULL;
    if (had_thread)
        server->thread_started = false;
    https_server_state_unlock(server);

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
                     error && error[0] ? error : "HttpsServer: worker drain failed");
            rt_trap_clear_recovery();
            https_server_state_lock(server);
            server->stopping = false;
            https_server_state_unlock(server);
            https_server_lifecycle_unlock(server);
            rt_trap(saved_error);
            return;
        }
        rt_threadpool_wait(worker_pool);
        rt_trap_clear_recovery();
    }

    https_server_state_lock(server);
    server->stopping = false;
    https_server_state_unlock(server);
    https_server_lifecycle_unlock(server);
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
int64_t rt_https_server_port(void *obj) {
    if (!obj)
        return 0;
    rt_http_server_impl *server = https_server_checked(obj, "HttpsServer: invalid server handle");
    if (!server)
        return 0;
    https_server_state_lock(server);
    int64_t port = server->port;
    https_server_state_unlock(server);
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
int8_t rt_https_server_is_running(void *obj) {
    if (!obj)
        return 0;
    rt_http_server_impl *server = https_server_checked(obj, "HttpsServer: invalid server handle");
    return server && https_server_is_running(server) ? 1 : 0;
}

//=============================================================================
// ServerReq Accessors
//=============================================================================

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
} https_sync_request_state_t;

/// @brief Detach managed HTTPS request state and release its activity slot.
/// @param server Valid HttpsServer payload.
/// @param state Heap-resident synchronous request transaction.
static void https_sync_request_release_managed(rt_http_server_impl *server,
                                               https_sync_request_state_t *state) {
    if (!state)
        return;
    server_res_t *response = state->response;
    state->response = NULL;
    https_server_release_object(response);
    server_req_t *request = state->request;
    state->request = NULL;
    https_server_release_object(request);
    if (state->registered) {
        state->registered = false;
        https_server_lifecycle_lock(server);
        https_server_state_lock(server);
        if (server->active_sync_requests > 0)
            server->active_sync_requests--;
        https_server_state_unlock(server);
        https_server_lifecycle_unlock(server);
    }
}

/// @brief Release every owned HTTPS synchronous request resource.
/// @param server Valid HttpsServer payload.
/// @param state Heap-resident synchronous request transaction.
static void https_sync_request_cleanup(rt_http_server_impl *server,
                                       https_sync_request_state_t *state) {
    if (!state)
        return;
    free(state->wire_response);
    state->wire_response = NULL;
    https_sync_request_release_managed(server, state);
}

HTTPS_MAYBE_UNUSED void *rt_https_server_process_request(void *obj, rt_string raw_request) {
    if (!obj)
        return rt_string_from_bytes("", 0);
    rt_http_server_impl *server = https_server_checked(obj, "HttpsServer: invalid server handle");
    if (!server)
        return NULL;
    if (raw_request && !rt_string_is_handle(raw_request)) {
        rt_trap("HttpsServer: invalid request String");
        return NULL;
    }

    https_sync_request_state_t *state =
        (https_sync_request_state_t *)calloc(1, sizeof(https_sync_request_state_t));
    if (!state) {
        rt_trap("HttpsServer: synchronous request state allocation failed");
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
                 error && error[0] ? error : "HttpsServer: synchronous request failed");
        rt_trap_clear_recovery();
        https_sync_request_cleanup(server, state);
        free(state);
        rt_trap(saved_error);
        return NULL;
    }

    https_server_lifecycle_lock(server);
    https_server_state_lock(server);
    if (server->finalizing) {
        https_server_state_unlock(server);
        https_server_lifecycle_unlock(server);
        free(state);
        rt_trap_clear_recovery();
        rt_trap("HttpsServer: server is finalizing");
        return NULL;
    }
    if (server->active_sync_requests == SIZE_MAX) {
        https_server_state_unlock(server);
        https_server_lifecycle_unlock(server);
        free(state);
        rt_trap_clear_recovery();
        rt_trap("HttpsServer: too many synchronous requests");
        return NULL;
    }
    server->active_sync_requests++;
    state->registered = true;
    https_server_state_unlock(server);
    https_server_lifecycle_unlock(server);

    const char *raw = raw_request ? rt_string_cstr(raw_request) : NULL;
    int64_t raw_len = raw_request ? rt_str_len(raw_request) : 0;
    state->request = https_server_req_new();
    if (!state->request)
        rt_trap("HttpsServer: request allocation failed");
    if (!raw || raw_len < 0 || (uint64_t)raw_len > (uint64_t)SIZE_MAX ||
        !parse_http_request(raw, (size_t)raw_len, state->request)) {
        https_sync_request_release_managed(server, state);
        rt_string result = rt_string_from_bytes(
            "HTTP/1.1 400 Bad Request\r\nConnection: close\r\nContent-Length: 0\r\n\r\n",
            sizeof("HTTP/1.1 400 Bad Request\r\nConnection: close\r\nContent-Length: 0\r\n\r\n") -
                1);
        free(state);
        rt_trap_clear_recovery();
        return result;
    }

    state->response = https_server_res_new();
    if (!state->response)
        rt_trap("HttpsServer: response allocation failed");
    build_route_response(server, state->request, state->response);
    int keep_alive =
        request_allows_keep_alive(state->request) && !response_forces_close(state->response);
    size_t response_len = 0;
    state->wire_response = build_response(state->response, keep_alive, &response_len);
    https_sync_request_release_managed(server, state);
    rt_string result = state->wire_response
                           ? rt_string_from_bytes(state->wire_response, response_len)
                           : rt_string_from_bytes("", 0);
    free(state->wire_response);
    state->wire_response = NULL;
    free(state);
    rt_trap_clear_recovery();
    return result;
}
