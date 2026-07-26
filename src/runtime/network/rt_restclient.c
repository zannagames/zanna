//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
///
// File: src/runtime/network/rt_restclient.c
// Purpose: Implements the managed REST client facade over the HTTP transport,
//          including JSON request bodies and Result-based response handling.
// Key invariants:
//   - Native staging objects are not published until their complete managed
//     representation has been created successfully.
//   - A returning trap releases every retained request, response, and JSON
//     intermediate before propagating the original error.
// Ownership/Lifetime:
//   - Client configuration and response state live in managed objects; local
//     native buffers and retained values are released on every exit path.
// Links: rt_restclient.h, rt_network_http_internal.h, rt_http_client.c,
//        docs/adr/0127-session-http-client-identity-and-synchronized-snapshots.md
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_restclient.c
 * @brief Implements the synchronized managed REST-client facade.
 * @details RestClient combines copied base URLs, persistent headers,
 *          authentication helpers, JSON encoding, connection pooling, and
 *          last-response tracking over the native HTTP request core. Mutable
 *          configuration is snapshotted under a platform mutex so transport
 *          work and managed publication occur without exposing partial state.
 */

#include "rt_restclient.h"
#include "rt_codec.h"
#include "rt_heap.h"
#include "rt_internal.h"
#include "rt_json.h"
#include "rt_map.h"
#include "rt_network.h"
#include "rt_network_http_internal.h"
#include "rt_object.h"
#include "rt_platform.h"
#include "rt_result.h"
#include "rt_seq.h"
#include "rt_string.h"
#include "rt_string_builder.h"
#include "rt_trap.h"

#include <limits.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if RT_PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
typedef CRITICAL_SECTION rest_client_mutex_t;

/// @brief Initialize the native RestClient mutex on Windows.
/// @param mutex Zeroed mutex storage owned by a partially built client.
/// @return One after initialization. Windows reports rare initialization
///         failure through its structured-exception mechanism.
static int rest_client_mutex_init(rest_client_mutex_t *mutex) {
    InitializeCriticalSection(mutex);
    return 1;
}

/// @brief Acquire an initialized Windows RestClient mutex.
/// @param mutex Mutex owned by a live RestClient.
static void rest_client_mutex_lock(rest_client_mutex_t *mutex) {
    EnterCriticalSection(mutex);
}

/// @brief Release a Windows RestClient mutex held by the current thread.
/// @param mutex Locked mutex owned by a live RestClient.
static void rest_client_mutex_unlock(rest_client_mutex_t *mutex) {
    LeaveCriticalSection(mutex);
}

/// @brief Destroy an initialized Windows RestClient mutex during finalization.
/// @param mutex Quiescent mutex owned by the object being finalized.
static void rest_client_mutex_destroy(rest_client_mutex_t *mutex) {
    DeleteCriticalSection(mutex);
}
#else
#include <pthread.h>
typedef pthread_mutex_t rest_client_mutex_t;

/// @brief Initialize the native RestClient mutex on POSIX platforms.
/// @param mutex Zeroed mutex storage owned by a partially built client.
/// @return One on success; zero when pthread mutex initialization fails.
static int rest_client_mutex_init(rest_client_mutex_t *mutex) {
    return pthread_mutex_init(mutex, NULL) == 0 ? 1 : 0;
}

/// @brief Acquire an initialized POSIX RestClient mutex.
/// @param mutex Mutex owned by a live RestClient.
static void rest_client_mutex_lock(rest_client_mutex_t *mutex) {
    (void)pthread_mutex_lock(mutex);
}

/// @brief Release a POSIX RestClient mutex held by the current thread.
/// @param mutex Locked mutex owned by a live RestClient.
static void rest_client_mutex_unlock(rest_client_mutex_t *mutex) {
    (void)pthread_mutex_unlock(mutex);
}

/// @brief Destroy an initialized POSIX RestClient mutex during finalization.
/// @param mutex Quiescent mutex owned by the object being finalized.
static void rest_client_mutex_destroy(rest_client_mutex_t *mutex) {
    (void)pthread_mutex_destroy(mutex);
}
#endif

//=============================================================================
// Internal Structure
//=============================================================================

typedef struct {
    rt_string base_url;
    void *headers; // Map of default headers
    int64_t timeout_ms;
    void *last_response; // Last HttpRes
    int64_t last_status;
    void *connection_pool;
    int64_t pool_size;
    int8_t keep_alive;
    rest_client_mutex_t lock;
    int8_t lock_initialized;
} rest_client;

/// @brief One retained default-header entry captured for request construction.
typedef struct {
    rt_string key;
    rt_string value;
} rest_header_snapshot;

/// @brief Immutable request defaults copied while the RestClient mutex is held.
/// @details Every managed field owns a temporary reference. Native header
///          storage owns exactly @ref header_count initialized entries.
typedef struct {
    rt_string base_url;
    rest_header_snapshot *headers;
    int64_t header_count;
    int64_t timeout_ms;
    void *connection_pool;
    int8_t keep_alive;
} rest_request_snapshot;

/// @brief Release a temporary runtime object after another owner has retained it.
/// @details Result wrappers retain the response payload; this helper drops the
///          raw response reference returned by `execute_request_result`.
/// @param obj Runtime object to release, or NULL.
static void rest_release_temp_object(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief Copy the active trap diagnostic into stable native storage.
/// @details Recovery cleanup clears the current frame before Result creation or
///          rethrow. Copying first prevents the thread-local diagnostic pointer
///          from being invalidated or overwritten during that cleanup.
/// @param output Destination byte buffer.
/// @param capacity Destination capacity including its terminator.
/// @param fallback Message used when no trap text is active.
static void rest_save_trap(char *output, size_t capacity, const char *fallback) {
    if (!output || capacity == 0)
        return;
    const char *error = rt_trap_get_error();
    snprintf(output,
             capacity,
             "%s",
             error && error[0]
                 ? error
                 : (fallback && fallback[0] ? fallback : "RestClient request failed"));
}

/// @brief Build a caller-owned `Result.ErrStr` from stable native bytes.
/// @details Result construction uses a fresh recovery frame, balances the
///          diagnostic String's producer reference after Result retains it,
///          and releases every partial value before propagating allocation
///          failure. This prevents recursive jumps into the request handler.
/// @param message NUL-terminated diagnostic, with a fixed fallback for NULL.
/// @return Caller-owned error Result, or NULL after a returning trap hook.
static void *rest_error_result(const char *message) {
    rt_string volatile error_string = NULL;
    void *volatile result = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[512];
        rest_save_trap(
            saved_error, sizeof(saved_error), "RestClient: error Result allocation failed");
        rt_trap_clear_recovery();
        rest_release_temp_object((void *)result);
        rest_release_temp_object((void *)error_string);
        rt_trap(saved_error);
        return NULL;
    }
    const char *stable = message && message[0] ? message : "RestClient request failed";
    error_string = rt_string_from_bytes(stable, strlen(stable));
    if (!error_string) {
        rt_trap_clear_recovery();
        return NULL;
    }
    result = rt_result_err_str((rt_string)error_string);
    if (!result) {
        rt_trap_clear_recovery();
        rest_release_temp_object((void *)error_string);
        return NULL;
    }
    rt_trap_clear_recovery();
    rest_release_temp_object((void *)error_string);
    return (void *)result;
}

/// @brief Wrap and consume one caller-owned HttpRes in `Result.Ok`.
/// @details `rt_result_ok` retains its payload. The transport's initial
///          response reference is consumed on success and on every allocation
///          failure, while any partial Result is also released.
/// @param response Caller-owned stable HttpRes reference.
/// @return Caller-owned success Result, or NULL after a returning trap hook.
static void *rest_success_result_owned(void *response) {
    void *volatile result = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[512];
        rest_save_trap(
            saved_error, sizeof(saved_error), "RestClient: success Result allocation failed");
        rt_trap_clear_recovery();
        rest_release_temp_object((void *)result);
        rest_release_temp_object(response);
        rt_trap(saved_error);
        return NULL;
    }
    result = rt_result_ok(response);
    if (!result) {
        rt_trap_clear_recovery();
        rest_release_temp_object(response);
        return NULL;
    }
    rt_trap_clear_recovery();
    rest_release_temp_object(response);
    return (void *)result;
}

/// @brief Validate and cast an opaque RestClient handle.
/// @details Public methods use this helper when a NULL receiver is a domain
///          error instead of a silent no-op. It traps with @p message and then
///          returns NULL so methods can safely stop local control flow when
///          trap recovery hooks return.
/// @param obj Opaque RestClient handle.
/// @param message Diagnostic used for the trap when @p obj is NULL.
/// @return Cast RestClient pointer, or NULL after trapping.
static rest_client *rest_client_checked(void *obj, const char *message) {
    if (!rt_obj_is_instance(obj, RT_RESTCLIENT_CLASS_ID, sizeof(rest_client))) {
        rt_trap(message);
        return NULL;
    }
    return (rest_client *)obj;
}

/// @brief Clear receiver-scoped last-response state under its mutex.
/// @details The previous cached response is detached while locked and released
///          after unlocking so its finalizer cannot run inside the critical
///          section.
/// @param client Valid RestClient receiver.
static void rest_client_clear_last_response(rest_client *client) {
    if (!client)
        return;
    void *old_response = NULL;
    rest_client_mutex_lock(&client->lock);
    old_response = client->last_response;
    client->last_response = NULL;
    client->last_status = 0;
    rest_client_mutex_unlock(&client->lock);
    rest_release_temp_object(old_response);
}

/// @brief Publish a response into thread-safe compatibility state.
/// @details The client retains an independent response reference before the
///          locked exchange. The detached old response is released after the
///          mutex is unlocked. The caller keeps its original response ownership.
/// @param client Valid RestClient receiver.
/// @param response Caller-owned HttpRes returned by the transport.
static void rest_client_publish_last_response(rest_client *client, void *response) {
    if (!client || !response)
        return;
    int64_t status = rt_http_res_status(response);
    rt_obj_retain_maybe(response);
    rest_client_mutex_lock(&client->lock);
    void *old_response = client->last_response;
    client->last_response = response;
    client->last_status = status;
    rest_client_mutex_unlock(&client->lock);
    rest_release_temp_object(old_response);
}

/// @brief Validate a public timeout and narrow it to the HTTP transport type.
/// @param timeout_ms Timeout in milliseconds; zero disables the timeout.
/// @param out_timeout_ms Optional destination receiving the validated value.
/// @return One when @p timeout_ms is in `[0, INT_MAX]`; otherwise zero.
static int rest_timeout_ms_to_int(int64_t timeout_ms, int *out_timeout_ms) {
    if (timeout_ms < 0 || timeout_ms > INT_MAX)
        return 0;
    if (out_timeout_ms)
        *out_timeout_ms = (int)timeout_ms;
    return 1;
}

/// @brief Obtain a host-sized borrowed byte view of a runtime String.
/// @details NULL is treated as an empty String. Invalid storage, an
///          unrepresentable length, or a forbidden embedded NUL traps with
///          @p context and returns the empty view if trap recovery returns.
/// @param text Runtime String to inspect, or NULL for empty input.
/// @param len_out Optional destination receiving the byte count.
/// @param context Diagnostic used when validation fails.
/// @param reject_embedded_nul Nonzero to reject NUL bytes within the span.
/// @return Borrowed bytes valid for the lifetime of @p text, or a static empty
///         string for NULL or after a returning trap hook.
static const char *rest_string_bytes(rt_string text,
                                     size_t *len_out,
                                     const char *context,
                                     int reject_embedded_nul) {
    if (len_out)
        *len_out = 0;
    if (!text)
        return "";
    const char *cstr = rt_string_cstr(text);
    int64_t len64 = rt_str_len(text);
    if (!cstr || len64 < 0 || (uint64_t)len64 > (uint64_t)SIZE_MAX) {
        rt_trap(context);
        return "";
    }
    size_t len = (size_t)len64;
    if (reject_embedded_nul && len > 0 && memchr(cstr, '\0', len)) {
        rt_trap(context);
        return "";
    }
    if (len_out)
        *len_out = len;
    return cstr;
}

/// @brief Validate a runtime String for safe inclusion in an HTTP field value.
/// @details Embedded NUL, carriage-return, and line-feed bytes are rejected to
///          prevent truncation and header-line injection.
/// @param text Header value to inspect, or NULL for an empty value.
/// @param len_out Optional destination receiving the validated byte count.
/// @param context Diagnostic used when validation fails.
/// @return Borrowed validated bytes, or a static empty string after a
///         returning trap hook.
static const char *rest_header_value_bytes(rt_string text, size_t *len_out, const char *context) {
    const char *cstr = rest_string_bytes(text, len_out, context, 1);
    size_t len = len_out ? *len_out : 0;
    if (len > 0 && (memchr(cstr, '\r', len) || memchr(cstr, '\n', len))) {
        rt_trap(context);
        if (len_out)
            *len_out = 0;
        return "";
    }
    return cstr;
}

/// @brief Check whether a String is a nonempty HTTP field-name token.
/// @param text Candidate field name.
/// @return One when every byte is visible ASCII and excludes HTTP separator
///         characters; otherwise zero.
static int rest_header_name_is_token(rt_string text) {
    if (!text)
        return 0;
    const char *cstr = rt_string_cstr(text);
    int64_t len64 = rt_str_len(text);
    if (!cstr || len64 <= 0 || (uint64_t)len64 > (uint64_t)SIZE_MAX)
        return 0;
    size_t len = (size_t)len64;
    if (memchr(cstr, '\0', len))
        return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)cstr[i];
        if (ch <= 32 || ch >= 127 || strchr("()<>@,;:\\\"/[]?={}", ch))
            return 0;
    }
    return 1;
}

/// @brief Check whether a String is safe for direct HTTP field-value storage.
/// @param text Candidate field value.
/// @return One for valid storage containing no NUL, CR, or LF bytes; otherwise
///         zero.
static int rest_header_value_is_safe(rt_string text) {
    if (!text)
        return 0;
    const char *cstr = rt_string_cstr(text);
    int64_t len64 = rt_str_len(text);
    if (!cstr || len64 < 0 || (uint64_t)len64 > (uint64_t)SIZE_MAX)
        return 0;
    size_t len = (size_t)len64;
    return !memchr(cstr, '\0', len) && !memchr(cstr, '\r', len) && !memchr(cstr, '\n', len);
}

//=============================================================================
// Finalizer
//=============================================================================

/// @brief Release every resource owned by a finalized RestClient.
/// @details Releases the base URL, default-header Map, cached response,
///          connection pool, and initialized native mutex. The runtime invokes
///          this only after the client is no longer reachable.
/// @param obj RestClient allocation being finalized, or NULL.
static void rest_client_finalize(void *obj) {
    if (!obj)
        return;
    rest_client *client = (rest_client *)obj;
    // Release base_url string
    if (client->base_url)
        rt_string_unref(client->base_url);
    // Release headers map
    if (client->headers && rt_obj_release_check0(client->headers))
        rt_obj_free(client->headers);
    // Release last response
    if (client->last_response && rt_obj_release_check0(client->last_response))
        rt_obj_free(client->last_response);
    if (client->connection_pool && rt_obj_release_check0(client->connection_pool))
        rt_obj_free(client->connection_pool);
    if (client->lock_initialized) {
        rest_client_mutex_destroy(&client->lock);
        client->lock_initialized = 0;
    }
}

//=============================================================================
// Helper Functions
//=============================================================================

/// @brief Release every retained field in a request-default snapshot.
/// @details This helper accepts partially populated snapshots produced by a
///          trap-recovery path. Header entries, the native entry array, base
///          URL, and connection pool are each released exactly once and the
///          structure is reset for defensive reuse.
/// @param snapshot Snapshot to consume; NULL is a no-op.
static void rest_request_snapshot_release(rest_request_snapshot *snapshot) {
    if (!snapshot)
        return;
    for (int64_t i = 0; i < snapshot->header_count; i++) {
        if (snapshot->headers[i].value)
            rt_string_unref(snapshot->headers[i].value);
        if (snapshot->headers[i].key)
            rt_string_unref(snapshot->headers[i].key);
    }
    free(snapshot->headers);
    if (snapshot->base_url)
        rt_string_unref(snapshot->base_url);
    rest_release_temp_object(snapshot->connection_pool);
    memset(snapshot, 0, sizeof(*snapshot));
}

/// @brief Capture immutable RestClient request defaults under the client mutex.
/// @details The complete header key snapshot is obtained while mutations are
///          excluded, then each key/value, the base URL, and active pool receive
///          independent references. Managed traps unlock the mutex, release the
///          partial key Seq and snapshot, and propagate a stable diagnostic.
///          Native allocation failure follows the same cleanup path.
/// @param client Valid, initialized RestClient receiver.
/// @param snapshot Zeroed destination receiving owned references.
/// @return One on success; zero only after a returning trap hook.
static int rest_request_snapshot_capture(rest_client *client, rest_request_snapshot *snapshot) {
    void *volatile keys = NULL;
    volatile int mutex_locked = 0;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[512];
        rest_save_trap(
            saved_error, sizeof(saved_error), "RestClient: default snapshot allocation failed");
        rt_trap_clear_recovery();
        if (mutex_locked)
            rest_client_mutex_unlock(&client->lock);
        rest_release_temp_object((void *)keys);
        rest_request_snapshot_release(snapshot);
        rt_trap(saved_error);
        return 0;
    }

    rest_client_mutex_lock(&client->lock);
    mutex_locked = 1;
    snapshot->base_url = client->base_url;
    if (snapshot->base_url)
        rt_string_ref(snapshot->base_url);

    keys = rt_map_keys(client->headers);
    if (!keys)
        rt_trap("RestClient: default header snapshot allocation failed");
    int64_t count = rt_seq_len((void *)keys);
    if (count < 0 || (uint64_t)count > (uint64_t)SIZE_MAX / sizeof(*snapshot->headers))
        rt_trap("RestClient: default header snapshot is too large");
    if (count > 0) {
        snapshot->headers =
            (rest_header_snapshot *)calloc((size_t)count, sizeof(*snapshot->headers));
        if (!snapshot->headers)
            rt_trap("RestClient: default header snapshot allocation failed");
    }
    for (int64_t i = 0; i < count; i++) {
        rt_string key = (rt_string)rt_seq_get((void *)keys, i);
        rt_string value = (rt_string)rt_map_get(client->headers, key);
        if (!rt_string_is_handle(key) || !rt_string_is_handle(value))
            rt_trap("RestClient: corrupt default header storage");
        rt_string_ref(key);
        rt_string_ref(value);
        snapshot->headers[snapshot->header_count].key = key;
        snapshot->headers[snapshot->header_count].value = value;
        snapshot->header_count++;
    }

    snapshot->timeout_ms = client->timeout_ms;
    snapshot->keep_alive = client->keep_alive ? 1 : 0;
    snapshot->connection_pool = snapshot->keep_alive ? client->connection_pool : NULL;
    if (snapshot->connection_pool) {
        if (!rt_http_conn_pool_is_handle(snapshot->connection_pool))
            rt_trap("RestClient: corrupt connection pool storage");
        rt_obj_retain_maybe(snapshot->connection_pool);
    }

    rest_client_mutex_unlock(&client->lock);
    mutex_locked = 0;
    rt_trap_clear_recovery();
    rest_release_temp_object((void *)keys);
    return 1;
}

/// @brief Join a base URL and relative path with exactly one slash.
/// @details Trailing slashes are removed from @p base and leading slashes from
///          @p path before construction. Both inputs reject embedded NUL, and
///          length overflow or allocation failure traps.
/// @param base Base URL String.
/// @param path Relative request path.
/// @return Caller-owned joined String, or NULL after a returning trap hook.
static rt_string join_url(rt_string base, rt_string path) {
    size_t base_len = 0;
    size_t path_len = 0;
    const char *base_str = rest_string_bytes(base, &base_len, "RestClient: invalid base URL", 1);
    const char *path_str = rest_string_bytes(path, &path_len, "RestClient: invalid path", 1);

    // Remove trailing slash from base if present
    while (base_len > 0 && base_str[base_len - 1] == '/')
        base_len--;

    // Remove leading slash from path if present
    while (path_len > 0 && path_str[0] == '/') {
        path_str++;
        path_len--;
    }

    if (base_len > SIZE_MAX - path_len - 1) {
        rt_trap("RestClient: URL length overflow");
        return NULL;
    }
    size_t total = base_len + 1 + path_len;
    rt_string_builder sb;
    rt_sb_init(&sb);

    rt_sb_status_t status = rt_sb_reserve(&sb, total + 1);
    if (status == RT_SB_OK)
        status = rt_sb_append_bytes(&sb, base_str, base_len);
    if (status == RT_SB_OK)
        status = rt_sb_append_bytes(&sb, "/", 1);
    if (status == RT_SB_OK)
        status = rt_sb_append_bytes(&sb, path_str, path_len);
    if (status != RT_SB_OK) {
        rt_sb_free(&sb);
        rt_trap("RestClient: memory allocation failed");
        return NULL;
    }

    rt_string out = rt_string_from_bytes(sb.data, sb.len);
    rt_sb_free(&sb);
    if (!out) {
        rt_trap("RestClient: memory allocation failed");
        return NULL;
    }
    return out;
}

/// @brief Build one HttpReq from a synchronized snapshot of client defaults.
/// @details URL joining, request publication, default-header copies, timeout,
///          keep-alive pool attachment, and an optional copied body form one
///          recovery transaction. The request and every snapshot reference are
///          released before a setup trap is re-raised, so callers never inherit
///          a partially configured builder.
/// @param client Valid RestClient receiver.
/// @param method NUL-terminated HTTP method token copied for construction.
/// @param path Relative path joined to the snapshotted base URL.
/// @param body Optional request body String.
/// @param set_body Nonzero to configure @p body, including NULL as empty.
/// @return Caller-owned HttpReq, or NULL after a returning trap hook.
static void *create_request(
    rest_client *client, const char *method, rt_string path, rt_string body, int set_body) {
    if (!client) {
        rt_trap("RestClient: NULL client");
        return NULL;
    }
    rest_request_snapshot *const snapshot = (rest_request_snapshot *)calloc(1, sizeof(*snapshot));
    if (!snapshot) {
        rt_trap("RestClient: request snapshot allocation failed");
        return NULL;
    }

    rt_string volatile method_string = NULL;
    rt_string volatile url = NULL;
    void *volatile req = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[512];
        rest_save_trap(saved_error, sizeof(saved_error), "RestClient: request setup failed");
        rt_trap_clear_recovery();
        rest_release_temp_object((void *)req);
        rest_release_temp_object((void *)url);
        rest_release_temp_object((void *)method_string);
        rest_request_snapshot_release(snapshot);
        free(snapshot);
        rt_trap(saved_error);
        return NULL;
    }

    if (!rest_request_snapshot_capture(client, snapshot)) {
        rt_trap("RestClient: request snapshot failed");
        goto returned_trap;
    }
    url = join_url(snapshot->base_url, path);
    if (!url) {
        rt_trap("RestClient: URL construction failed");
        goto returned_trap;
    }
    if (!method || !method[0]) {
        rt_trap("RestClient: invalid HTTP method");
        goto returned_trap;
    }
    method_string = rt_string_from_bytes(method, strlen(method));
    if (!method_string) {
        rt_trap("RestClient: method allocation failed");
        goto returned_trap;
    }
    req = rt_http_req_new((rt_string)method_string, (rt_string)url);
    rest_release_temp_object((void *)method_string);
    method_string = NULL;
    rest_release_temp_object((void *)url);
    url = NULL;
    if (!req) {
        rt_trap("RestClient: request allocation failed");
        goto returned_trap;
    }

    for (int64_t i = 0; i < snapshot->header_count; i++) {
        if (!rt_http_req_set_header(
                (void *)req, snapshot->headers[i].key, snapshot->headers[i].value)) {
            rt_trap("RestClient: default header application failed");
            goto returned_trap;
        }
    }
    if (!rt_http_req_set_timeout((void *)req, snapshot->timeout_ms)) {
        rt_trap("RestClient: timeout application failed");
        goto returned_trap;
    }
    rt_http_req_set_keep_alive((void *)req, snapshot->keep_alive);
    rt_http_req_set_connection_pool((void *)req,
                                    snapshot->keep_alive ? snapshot->connection_pool : NULL);
    if (set_body)
        rt_http_req_set_body_str((void *)req, body);

    rt_trap_clear_recovery();
    rest_request_snapshot_release(snapshot);
    free(snapshot);
    return (void *)req;

returned_trap:
    rt_trap_clear_recovery();
    rest_release_temp_object((void *)req);
    rest_release_temp_object((void *)url);
    rest_release_temp_object((void *)method_string);
    rest_request_snapshot_release(snapshot);
    free(snapshot);
    return NULL;
}

/// @brief Consume and send an HttpReq, then publish its response on the client.
/// @details The request is released on every path. A successful response is
///          returned with its transport-owned reference while the client
///          retains a separate synchronized cache reference. Failure clears
///          prior compatibility state and re-raises the original network trap
///          category after cleanup.
/// @param client Valid RestClient receiver.
/// @param req Caller-owned HttpReq whose ownership is consumed.
/// @return Caller-owned HttpRes, or NULL after a returning trap hook.
static void *execute_request(rest_client *client, void *req) {
    if (!client) {
        if (req && rt_obj_release_check0(req))
            rt_obj_free(req);
        rt_trap("RestClient: NULL client");
        return NULL;
    }
    if (!req) {
        rt_trap("RestClient: request allocation failed");
        return NULL;
    }
    void *volatile res = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[512];
        int saved_net_code = rt_trap_get_net_code();
        rest_save_trap(saved_error, sizeof(saved_error), "RestClient request failed");
        rt_trap_clear_recovery();
        rest_release_temp_object((void *)res);
        rest_release_temp_object(req);
        rest_client_clear_last_response(client);
        if (saved_net_code)
            rt_trap_net(saved_error, saved_net_code);
        else
            rt_trap(saved_error);
        return NULL;
    }
    res = rt_http_req_send(req);
    if (!res)
        rt_trap("RestClient request failed");
    rest_client_publish_last_response(client, (void *)res);
    rt_trap_clear_recovery();
    rest_release_temp_object(req);
    return (void *)res;
}

/// @brief Send an HttpReq and return `Result<HttpRes>`.
/// @details Converts request/transport traps into `Result.ErrStr`, releases
///          the request in all paths, and keeps the receiver-scoped
///          `LastResponse`/`LastStatus` compatibility state synchronized when
///          an HTTP response is received.
/// @param client RestClient receiver.
/// @param req Prepared HttpReq object; ownership is consumed.
/// @return Opaque `Zanna.Result` containing `Ok(HttpRes)` or `Err(String)`.
static void *execute_request_result(rest_client *client, void *req) {
    if (!client) {
        rest_release_temp_object(req);
        return rest_error_result("RestClient: NULL client");
    }
    if (!req) {
        rest_client_clear_last_response(client);
        return rest_error_result("RestClient: request allocation failed");
    }

    void *volatile res = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[512];
        rest_save_trap(saved_error, sizeof(saved_error), "RestClient request failed");
        rt_trap_clear_recovery();
        rest_release_temp_object((void *)res);
        rest_release_temp_object(req);
        rest_client_clear_last_response(client);
        return rest_error_result(saved_error);
    }
    res = rt_http_req_send(req);
    if (!res)
        rt_trap("RestClient request failed");
    rest_client_publish_last_response(client, (void *)res);
    rt_trap_clear_recovery();
    rest_release_temp_object(req);
    return rest_success_result_owned((void *)res);
}

//=============================================================================
// Creation and Configuration
//=============================================================================

/// @brief Construct a managed REST client for a base URL.
/// @details Copies @p base_url and initializes an empty default-header Map, a
///          30-second timeout, enabled keep-alive, and an eight-connection
///          pool. Construction is transactional: a trap finalizes any partially
///          initialized managed object before propagating the diagnostic.
/// @param base_url Base URL copied into client-owned storage.
/// @return Caller-owned managed RestClient, or NULL after a returning trap
///         hook.
void *rt_restclient_new(rt_string base_url) {
    rest_client *volatile client = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[512];
        rest_save_trap(saved_error, sizeof(saved_error), "RestClient: construction failed");
        rt_trap_clear_recovery();
        rest_release_temp_object((void *)client);
        rt_trap(saved_error);
        return NULL;
    }

    client = (rest_client *)rt_obj_new_i64(RT_RESTCLIENT_CLASS_ID, (int64_t)sizeof(rest_client));
    if (!client) {
        rt_trap_clear_recovery();
        return NULL;
    }
    memset((void *)client, 0, sizeof(rest_client));
    rt_obj_set_finalizer((void *)client, rest_client_finalize);
    if (!rest_client_mutex_init(&((rest_client *)client)->lock))
        rt_trap("RestClient: mutex initialization failed");
    ((rest_client *)client)->lock_initialized = 1;

    size_t url_len = 0;
    const char *url_str = rest_string_bytes(base_url, &url_len, "RestClient: invalid base URL", 1);
    ((rest_client *)client)->base_url = rt_string_from_bytes(url_str, url_len);
    if (!((rest_client *)client)->base_url)
        rt_trap("RestClient: base URL allocation failed");
    ((rest_client *)client)->headers = rt_map_new();
    if (!((rest_client *)client)->headers)
        rt_trap("RestClient: header Map allocation failed");
    ((rest_client *)client)->timeout_ms = 30000;
    ((rest_client *)client)->pool_size = 8;
    ((rest_client *)client)->keep_alive = 1;
    ((rest_client *)client)->connection_pool =
        rt_http_conn_pool_new(((rest_client *)client)->pool_size);
    if (!((rest_client *)client)->connection_pool)
        rt_trap("RestClient: connection pool allocation failed");

    rt_trap_clear_recovery();
    return (void *)client;
}

/// @brief Copy the base URL targeted by a RestClient.
/// @param obj RestClient receiver; NULL yields an empty String.
/// @return Caller-owned String copy, an empty String for NULL, or NULL after a
///         corrupt-storage trap.
rt_string rt_restclient_base_url(void *obj) {
    if (!obj)
        return rt_str_empty();
    rest_client *client = rest_client_checked(obj, "RestClient: invalid client");
    if (!client || !client->base_url)
        return rt_str_empty();
    const char *bytes = rt_string_cstr(client->base_url);
    int64_t length = rt_str_len(client->base_url);
    if (!bytes || length < 0 || (uint64_t)length > (uint64_t)SIZE_MAX) {
        rt_trap("RestClient: corrupt base URL storage");
        return NULL;
    }
    return rt_string_from_bytes(bytes, (size_t)length);
}

/// @brief Set a default header applied to subsequent requests.
/// @details Field names are matched case-insensitively, so this replaces any
///          differently cased spelling. Invalid token names and values
///          containing NUL, CR, or LF are ignored; allocation failure traps.
///          Mutation is synchronized with request snapshot capture.
/// @param obj RestClient receiver; NULL is a no-op.
/// @param name Nonempty HTTP field-name token.
/// @param value HTTP field value without line-breaking bytes.
void rt_restclient_set_header(void *obj, rt_string name, rt_string value) {
    if (!obj)
        return;
    rest_client *client = rest_client_checked(obj, "RestClient: invalid client");
    if (!client)
        return;
    if (!rest_header_name_is_token(name) || !rest_header_value_is_safe(value))
        return;
    rest_client_mutex_lock(&client->lock);
    int updated = rt_http_header_map_set_ci(client->headers, name, (void *)value);
    rest_client_mutex_unlock(&client->lock);
    if (!updated)
        rt_trap("RestClient: default header allocation failed");
}

/// @brief Remove a case-insensitively matched default header.
/// @details Invalid field names and NULL receivers are ignored. Mutation is
///          synchronized with request snapshot capture.
/// @param obj RestClient receiver; NULL is a no-op.
/// @param name HTTP field-name token to remove.
void rt_restclient_del_header(void *obj, rt_string name) {
    if (!obj)
        return;
    rest_client *client = rest_client_checked(obj, "RestClient: invalid client");
    if (!client)
        return;
    if (!rest_header_name_is_token(name))
        return;
    rest_client_mutex_lock(&client->lock);
    int removed = rt_http_header_map_remove_ci(client->headers, rt_string_cstr(name));
    rest_client_mutex_unlock(&client->lock);
    if (!removed)
        rt_trap("RestClient: default header snapshot allocation failed");
}

/// @brief Owned staging shared by Bearer and Basic authentication updates.
/// @details Native construction uses one reusable buffer; managed credential,
///          encoding, final Authorization, and header-name Strings remain
///          individually owned until the header Map has retained its copy.
typedef struct {
    char *native_buffer;
    rt_string credential;
    rt_string encoded;
    rt_string authorization;
    rt_string header_name;
} rest_auth_transaction;

/// @brief Release all native and managed authentication-construction staging.
/// @details The helper accepts partial Bearer and Basic transactions, allowing
///          their recovery handlers to preserve the prior Authorization header
///          after validation, codec, String, or Map allocation failure.
/// @param transaction Transaction to consume; NULL is a no-op.
static void rest_auth_transaction_release(rest_auth_transaction *transaction) {
    if (!transaction)
        return;
    free(transaction->native_buffer);
    rest_release_temp_object((void *)transaction->credential);
    rest_release_temp_object((void *)transaction->encoded);
    rest_release_temp_object((void *)transaction->authorization);
    rest_release_temp_object((void *)transaction->header_name);
    memset(transaction, 0, sizeof(*transaction));
}

/// @brief Build and transactionally publish a Bearer Authorization value.
/// @details The full token byte span is validated against embedded NUL/CR/LF,
///          copied behind the `Bearer ` prefix, and converted to managed storage
///          before the synchronized header-map replacement. Recovery consumes
///          every staging allocation and re-raises one stable diagnostic.
/// @param obj RestClient receiver; NULL preserves the historical no-op.
/// @param token Bearer token, with NULL treated as an empty token.
void rt_restclient_set_auth_bearer(void *obj, rt_string token) {
    if (!obj)
        return;
    if (!rest_client_checked(obj, "RestClient: invalid client"))
        return;

    rest_auth_transaction *const transaction =
        (rest_auth_transaction *)calloc(1, sizeof(*transaction));
    if (!transaction) {
        rt_trap("RestClient: authentication allocation failed");
        return;
    }
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[512];
        rest_save_trap(
            saved_error, sizeof(saved_error), "RestClient: Bearer authentication failed");
        rt_trap_clear_recovery();
        rest_auth_transaction_release(transaction);
        free(transaction);
        rt_trap(saved_error);
        return;
    }

    size_t token_len = 0;
    const char *token_bytes =
        rest_header_value_bytes(token, &token_len, "RestClient: invalid bearer token");
    const size_t prefix_len = sizeof("Bearer ") - 1u;
    if (token_len > SIZE_MAX - prefix_len - 1u)
        rt_trap("RestClient: Bearer token is too large");
    transaction->native_buffer = (char *)malloc(prefix_len + token_len + 1u);
    if (!transaction->native_buffer)
        rt_trap("RestClient: Bearer authentication allocation failed");
    memcpy(transaction->native_buffer, "Bearer ", prefix_len);
    memcpy(transaction->native_buffer + prefix_len, token_bytes, token_len);
    transaction->native_buffer[prefix_len + token_len] = '\0';
    transaction->authorization =
        rt_string_from_bytes(transaction->native_buffer, prefix_len + token_len);
    if (!transaction->authorization)
        rt_trap("RestClient: Bearer authentication String allocation failed");
    transaction->header_name = rt_const_cstr("Authorization");
    if (!transaction->header_name)
        rt_trap("RestClient: Authorization header allocation failed");
    rt_restclient_set_header(obj, transaction->header_name, transaction->authorization);

    rt_trap_clear_recovery();
    rest_auth_transaction_release(transaction);
    free(transaction);
}

/// @brief Transactionally set HTTP Basic authentication.
/// @details Constructs the exact `username:password` byte sequence, Base64
///          encodes it, prefixes `Basic `, and publishes the Authorization
///          header only after all staging succeeds. Embedded NUL, CR, and LF
///          are rejected, and recovery preserves the previous header.
/// @param obj RestClient receiver; NULL is a no-op.
/// @param username Username bytes, with NULL treated as empty.
/// @param password Password bytes, with NULL treated as empty.
void rt_restclient_set_auth_basic(void *obj, rt_string username, rt_string password) {
    if (!obj)
        return;
    if (!rest_client_checked(obj, "RestClient: invalid client"))
        return;

    rest_auth_transaction *const transaction =
        (rest_auth_transaction *)calloc(1, sizeof(*transaction));
    if (!transaction) {
        rt_trap("RestClient: authentication allocation failed");
        return;
    }
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[512];
        rest_save_trap(saved_error, sizeof(saved_error), "RestClient: Basic authentication failed");
        rt_trap_clear_recovery();
        rest_auth_transaction_release(transaction);
        free(transaction);
        rt_trap(saved_error);
        return;
    }

    size_t user_len = 0;
    size_t password_len = 0;
    const char *user_bytes =
        rest_header_value_bytes(username, &user_len, "RestClient: invalid username");
    const char *password_bytes =
        rest_header_value_bytes(password, &password_len, "RestClient: invalid password");
    if (user_len > SIZE_MAX - password_len - 2u)
        rt_trap("RestClient: Basic credentials are too large");
    size_t credential_len = user_len + 1u + password_len;
    transaction->native_buffer = (char *)malloc(credential_len + 1u);
    if (!transaction->native_buffer)
        rt_trap("RestClient: Basic credential allocation failed");
    memcpy(transaction->native_buffer, user_bytes, user_len);
    transaction->native_buffer[user_len] = ':';
    memcpy(transaction->native_buffer + user_len + 1u, password_bytes, password_len);
    transaction->native_buffer[credential_len] = '\0';
    transaction->credential = rt_string_from_bytes(transaction->native_buffer, credential_len);
    if (!transaction->credential)
        rt_trap("RestClient: Basic credential String allocation failed");
    free(transaction->native_buffer);
    transaction->native_buffer = NULL;

    transaction->encoded = rt_codec_base64_enc(transaction->credential);
    if (!transaction->encoded)
        rt_trap("RestClient: Basic credential encoding failed");
    const char *encoded_bytes = rt_string_cstr(transaction->encoded);
    int64_t encoded_len64 = rt_str_len(transaction->encoded);
    if (!encoded_bytes || encoded_len64 < 0 || (uint64_t)encoded_len64 > (uint64_t)SIZE_MAX)
        rt_trap("RestClient: invalid Basic credential encoding");
    size_t encoded_len = (size_t)encoded_len64;
    const size_t prefix_len = sizeof("Basic ") - 1u;
    if (encoded_len > SIZE_MAX - prefix_len - 1u)
        rt_trap("RestClient: Basic authorization is too large");
    transaction->native_buffer = (char *)malloc(prefix_len + encoded_len + 1u);
    if (!transaction->native_buffer)
        rt_trap("RestClient: Basic authorization allocation failed");
    memcpy(transaction->native_buffer, "Basic ", prefix_len);
    memcpy(transaction->native_buffer + prefix_len, encoded_bytes, encoded_len);
    transaction->native_buffer[prefix_len + encoded_len] = '\0';
    transaction->authorization =
        rt_string_from_bytes(transaction->native_buffer, prefix_len + encoded_len);
    if (!transaction->authorization)
        rt_trap("RestClient: Basic authorization String allocation failed");
    transaction->header_name = rt_const_cstr("Authorization");
    if (!transaction->header_name)
        rt_trap("RestClient: Authorization header allocation failed");
    rt_restclient_set_header(obj, transaction->header_name, transaction->authorization);

    rt_trap_clear_recovery();
    rest_auth_transaction_release(transaction);
    free(transaction);
}

/// @brief Remove any default Authorization header.
/// @param obj RestClient receiver; NULL is a no-op.
void rt_restclient_clear_auth(void *obj) {
    if (!obj)
        return;
    if (!rest_client_checked(obj, "RestClient: invalid client"))
        return;
    rt_string name = rt_const_cstr("Authorization");
    if (!name)
        return;
    rt_restclient_del_header(obj, name);
    rt_string_unref(name);
}

/// @brief Set the timeout copied into subsequent requests.
/// @param obj RestClient receiver; NULL is a no-op.
/// @param timeout_ms Milliseconds in `[0, INT_MAX]`; zero disables timeout.
void rt_restclient_set_timeout(void *obj, int64_t timeout_ms) {
    if (!obj)
        return;
    rest_client *client = rest_client_checked(obj, "RestClient: invalid client");
    if (!client)
        return;
    int timeout_int = 0;
    if (!rest_timeout_ms_to_int(timeout_ms, &timeout_int)) {
        rt_trap("RestClient: invalid timeout");
        return;
    }
    rest_client_mutex_lock(&client->lock);
    client->timeout_ms = timeout_int;
    rest_client_mutex_unlock(&client->lock);
}

/// @brief Read whether keep-alive connection reuse is enabled.
/// @param obj RestClient receiver; NULL yields zero.
/// @return One when enabled; otherwise zero.
int8_t rt_restclient_get_keep_alive(void *obj) {
    if (!obj)
        return 0;
    rest_client *client = rest_client_checked(obj, "RestClient: invalid client");
    if (!client)
        return 0;
    rest_client_mutex_lock(&client->lock);
    int8_t keep_alive = client->keep_alive;
    rest_client_mutex_unlock(&client->lock);
    return keep_alive;
}

/// @brief Enable or disable keep-alive connection reuse.
/// @details Disabling atomically detaches the pool, clears its idle
///          connections, and releases it. Enabling creates a pool outside the
///          mutex and retries if a concurrent pool-size update makes it stale.
/// @param obj RestClient receiver; NULL is a no-op.
/// @param keep_alive Nonzero to enable reuse; zero to disable it.
void rt_restclient_set_keep_alive(void *obj, int8_t keep_alive) {
    if (!obj)
        return;
    rest_client *client = rest_client_checked(obj, "RestClient: invalid client");
    if (!client)
        return;

    int8_t enable = keep_alive ? 1 : 0;
    if (!enable) {
        rest_client_mutex_lock(&client->lock);
        void *old_pool = client->connection_pool;
        client->connection_pool = NULL;
        client->keep_alive = 0;
        rest_client_mutex_unlock(&client->lock);
        if (old_pool)
            rt_http_conn_pool_clear(old_pool);
        rest_release_temp_object(old_pool);
        return;
    }

    for (;;) {
        rest_client_mutex_lock(&client->lock);
        if (client->keep_alive && client->connection_pool) {
            rest_client_mutex_unlock(&client->lock);
            return;
        }
        int64_t pool_size = client->pool_size > 0 ? client->pool_size : 8;
        rest_client_mutex_unlock(&client->lock);

        void *new_pool = rt_http_conn_pool_new(pool_size);
        if (!new_pool)
            return;

        rest_client_mutex_lock(&client->lock);
        if (client->pool_size != pool_size) {
            rest_client_mutex_unlock(&client->lock);
            rest_release_temp_object(new_pool);
            continue;
        }
        if (client->keep_alive && client->connection_pool) {
            rest_client_mutex_unlock(&client->lock);
            rest_release_temp_object(new_pool);
            return;
        }
        void *old_pool = client->connection_pool;
        client->connection_pool = new_pool;
        client->keep_alive = 1;
        rest_client_mutex_unlock(&client->lock);
        rest_release_temp_object(old_pool);
        return;
    }
}

/// @brief Replace the configured keep-alive connection pool.
/// @details Values below one are normalized to one. The replacement is
///          installed only when keep-alive is enabled; otherwise its size is
///          remembered for the next enable operation. The prior pool is
///          released after the synchronized exchange.
/// @param obj RestClient receiver; NULL is a no-op.
/// @param max_size Maximum retained connections, normalized to at least one.
void rt_restclient_set_pool_size(void *obj, int64_t max_size) {
    if (!obj)
        return;
    rest_client *client = rest_client_checked(obj, "RestClient: invalid client");
    if (!client)
        return;
    if (max_size <= 0)
        max_size = 1;

    void *new_pool = rt_http_conn_pool_new(max_size);
    if (!new_pool)
        return;
    rest_client_mutex_lock(&client->lock);
    client->pool_size = max_size;
    void *old_pool = client->connection_pool;
    if (client->keep_alive) {
        client->connection_pool = new_pool;
        new_pool = NULL;
    } else {
        client->connection_pool = NULL;
    }
    rest_client_mutex_unlock(&client->lock);
    rest_release_temp_object(old_pool);
    rest_release_temp_object(new_pool);
}

//=============================================================================
// HTTP Methods - Raw
//=============================================================================

/// @brief Build and execute one Result-returning RestClient method.
/// @details Receiver identity, request snapshot/setup, and optional body copy
///          share one implementation for every verb. Setup traps are converted
///          only after the recovery frame has been cleared and the partial
///          request released; transport/response ownership is then delegated to
///          @ref execute_request_result. HTTP status remains response data.
/// @param obj Candidate RestClient receiver.
/// @param method NUL-terminated HTTP method token.
/// @param path Relative request path.
/// @param body Optional String body.
/// @param set_body Nonzero to copy @p body into the request.
/// @return Caller-owned `Result<HttpRes>` or NULL if Result allocation traps.
static void *rest_execute_method_result(
    void *obj, const char *method, rt_string path, rt_string body, int set_body) {
    if (!rt_obj_is_instance(obj, RT_RESTCLIENT_CLASS_ID, sizeof(rest_client)))
        return rest_error_result("RestClient: invalid client");
    rest_client *client = (rest_client *)obj;
    void *volatile req = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[512];
        rest_save_trap(saved_error, sizeof(saved_error), "RestClient request setup failed");
        rt_trap_clear_recovery();
        rest_release_temp_object((void *)req);
        rest_client_clear_last_response(client);
        return rest_error_result(saved_error);
    }
    req = create_request(client, method, path, body, set_body);
    if (!req)
        rt_trap("RestClient request setup failed");
    rt_trap_clear_recovery();
    return execute_request_result(client, (void *)req);
}

/// @brief Send a GET request and return its raw response.
/// @param obj RestClient receiver.
/// @param path Relative path joined to the snapshotted base URL.
/// @return Caller-owned HttpRes, or NULL after a returning trap hook.
void *rt_restclient_get(void *obj, rt_string path) {
    rest_client *client = rest_client_checked(obj, "RestClient: null client");
    if (!client)
        return NULL;

    void *req = create_request(client, "GET", path, NULL, 0);
    if (!req)
        return NULL;
    return execute_request(client, req);
}

/// @brief Send a GET request with traps represented as a Result.
/// @param obj RestClient receiver.
/// @param path Relative path joined to the snapshotted base URL.
/// @return Caller-owned `Result<HttpRes>` containing the response or an error
///         String.
void *rt_restclient_get_result(void *obj, rt_string path) {
    return rest_execute_method_result(obj, "GET", path, NULL, 0);
}

/// @brief Send a POST request with a String body.
/// @details No Content-Type is implied; configure one with the default-header
///          API when required.
/// @param obj RestClient receiver.
/// @param path Relative path joined to the snapshotted base URL.
/// @param body String body copied into the request.
/// @return Caller-owned HttpRes, or NULL after a returning trap hook.
void *rt_restclient_post(void *obj, rt_string path, rt_string body) {
    rest_client *client = rest_client_checked(obj, "RestClient: null client");
    if (!client)
        return NULL;

    void *req = create_request(client, "POST", path, body, 1);
    if (!req)
        return NULL;
    return execute_request(client, req);
}

/// @brief Send a POST request with traps represented as a Result.
/// @param obj RestClient receiver.
/// @param path Relative path joined to the snapshotted base URL.
/// @param body String body copied into the request.
/// @return Caller-owned `Result<HttpRes>` containing the response or an error
///         String.
void *rt_restclient_post_result(void *obj, rt_string path, rt_string body) {
    return rest_execute_method_result(obj, "POST", path, body, 1);
}

/// @brief Send a PUT request with a String body.
/// @param obj RestClient receiver.
/// @param path Relative path joined to the snapshotted base URL.
/// @param body String body copied into the request.
/// @return Caller-owned HttpRes, or NULL after a returning trap hook.
void *rt_restclient_put(void *obj, rt_string path, rt_string body) {
    rest_client *client = rest_client_checked(obj, "RestClient: null client");
    if (!client)
        return NULL;

    void *req = create_request(client, "PUT", path, body, 1);
    if (!req)
        return NULL;
    return execute_request(client, req);
}

/// @brief Send a PUT request with traps represented as a Result.
/// @param obj RestClient receiver.
/// @param path Relative path joined to the snapshotted base URL.
/// @param body String body copied into the request.
/// @return Caller-owned `Result<HttpRes>` containing the response or an error
///         String.
void *rt_restclient_put_result(void *obj, rt_string path, rt_string body) {
    return rest_execute_method_result(obj, "PUT", path, body, 1);
}

/// @brief Send a PATCH request with a String body.
/// @param obj RestClient receiver.
/// @param path Relative path joined to the snapshotted base URL.
/// @param body String body copied into the request.
/// @return Caller-owned HttpRes, or NULL after a returning trap hook.
void *rt_restclient_patch(void *obj, rt_string path, rt_string body) {
    rest_client *client = rest_client_checked(obj, "RestClient: null client");
    if (!client)
        return NULL;

    void *req = create_request(client, "PATCH", path, body, 1);
    if (!req)
        return NULL;
    return execute_request(client, req);
}

/// @brief Send a PATCH request with traps represented as a Result.
/// @param obj RestClient receiver.
/// @param path Relative path joined to the snapshotted base URL.
/// @param body String body copied into the request.
/// @return Caller-owned `Result<HttpRes>` containing the response or an error
///         String.
void *rt_restclient_patch_result(void *obj, rt_string path, rt_string body) {
    return rest_execute_method_result(obj, "PATCH", path, body, 1);
}

/// @brief Send a bodyless DELETE request.
/// @param obj RestClient receiver.
/// @param path Relative path joined to the snapshotted base URL.
/// @return Caller-owned HttpRes, or NULL after a returning trap hook.
void *rt_restclient_delete(void *obj, rt_string path) {
    rest_client *client = rest_client_checked(obj, "RestClient: null client");
    if (!client)
        return NULL;

    void *req = create_request(client, "DELETE", path, NULL, 0);
    if (!req)
        return NULL;
    return execute_request(client, req);
}

/// @brief Send a bodyless DELETE request with traps represented as a Result.
/// @param obj RestClient receiver.
/// @param path Relative path joined to the snapshotted base URL.
/// @return Caller-owned `Result<HttpRes>` containing the response or an error
///         String.
void *rt_restclient_delete_result(void *obj, rt_string path) {
    return rest_execute_method_result(obj, "DELETE", path, NULL, 0);
}

/// @brief Send a HEAD request for response metadata without a body transfer.
/// @param obj RestClient receiver.
/// @param path Relative path joined to the snapshotted base URL.
/// @return Caller-owned HttpRes, or NULL after a returning trap hook.
void *rt_restclient_head(void *obj, rt_string path) {
    rest_client *client = rest_client_checked(obj, "RestClient: null client");
    if (!client)
        return NULL;

    void *req = create_request(client, "HEAD", path, NULL, 0);
    if (!req)
        return NULL;
    return execute_request(client, req);
}

/// @brief Send a HEAD request with traps represented as a Result.
/// @param obj RestClient receiver.
/// @param path Relative path joined to the snapshotted base URL.
/// @return Caller-owned `Result<HttpRes>` containing the response or an error
///         String.
void *rt_restclient_head_result(void *obj, rt_string path) {
    return rest_execute_method_result(obj, "HEAD", path, NULL, 0);
}

//=============================================================================
// HTTP Methods - JSON Convenience
//=============================================================================

/// @brief Managed values owned during one RestClient JSON transaction.
typedef struct {
    void *request;
    void *response;
    rt_string request_body;
    rt_string response_body;
    rt_string accept_name;
    rt_string accept_value;
    rt_string content_type_name;
    rt_string content_type_value;
} rest_json_transaction;

/// @brief Release every temporary owned by a JSON convenience request.
/// @details The response reference released here is the transport's caller
///          reference; RestClient's synchronized last-response cache retains a
///          separate reference. The helper accepts any partially initialized
///          transaction created before a managed trap.
/// @param transaction Transaction to consume; NULL is a no-op.
static void rest_json_transaction_release(rest_json_transaction *transaction) {
    if (!transaction)
        return;
    rest_release_temp_object(transaction->request);
    rest_release_temp_object(transaction->response);
    rest_release_temp_object((void *)transaction->request_body);
    rest_release_temp_object((void *)transaction->response_body);
    rest_release_temp_object((void *)transaction->accept_name);
    rest_release_temp_object((void *)transaction->accept_value);
    rest_release_temp_object((void *)transaction->content_type_name);
    rest_release_temp_object((void *)transaction->content_type_value);
    memset(transaction, 0, sizeof(*transaction));
}

/// @brief Execute one JSON convenience verb as a single cleanup transaction.
/// @details All verbs apply `Accept: application/json`; body-bearing verbs also
///          serialize @p json_body and apply `Content-Type: application/json`.
///          Request setup, send, body snapshot, and JSON parse are protected by
///          one outer recovery frame. The consumed HttpReq and raw HttpRes
///          caller reference are released on success, non-2xx/empty responses,
///          and every nested trap while the client's cached response remains
///          available for diagnostics after a completed exchange.
/// @param client Valid RestClient receiver.
/// @param method NUL-terminated HTTP method.
/// @param path Relative request path.
/// @param json_body Managed JSON value serialized for body-bearing methods.
/// @param send_json_body Nonzero to serialize and attach @p json_body.
/// @return Caller-owned parsed JSON value; NULL for non-2xx or empty response,
///         or after a returning trap hook.
static void *rest_execute_json(
    rest_client *client, const char *method, rt_string path, void *json_body, int send_json_body) {
    rest_json_transaction *const transaction =
        (rest_json_transaction *)calloc(1, sizeof(*transaction));
    if (!transaction) {
        rt_trap("RestClient: JSON transaction allocation failed");
        return NULL;
    }

    void *parsed = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[512];
        int saved_net_code = rt_trap_get_net_code();
        rest_save_trap(saved_error, sizeof(saved_error), "RestClient: JSON request failed");
        rt_trap_clear_recovery();
        rest_json_transaction_release(transaction);
        free(transaction);
        if (saved_net_code)
            rt_trap_net(saved_error, saved_net_code);
        else
            rt_trap(saved_error);
        return NULL;
    }

    transaction->request = create_request(client, method, path, NULL, 0);
    if (!transaction->request)
        rt_trap("RestClient: JSON request setup failed");
    transaction->accept_name = rt_const_cstr("Accept");
    transaction->accept_value = rt_const_cstr("application/json");
    if (!transaction->accept_name || !transaction->accept_value ||
        !rt_http_req_set_header(
            transaction->request, transaction->accept_name, transaction->accept_value)) {
        rt_trap("RestClient: JSON Accept header allocation failed");
    }

    if (send_json_body) {
        transaction->content_type_name = rt_const_cstr("Content-Type");
        transaction->content_type_value = rt_const_cstr("application/json");
        if (!transaction->content_type_name || !transaction->content_type_value ||
            !rt_http_req_set_header(transaction->request,
                                    transaction->content_type_name,
                                    transaction->content_type_value)) {
            rt_trap("RestClient: JSON Content-Type header allocation failed");
        }
        transaction->request_body = rt_json_format(json_body);
        if (!transaction->request_body)
            rt_trap("RestClient: JSON serialization failed");
        if (!rt_http_req_set_body_str(transaction->request, transaction->request_body))
            rt_trap("RestClient: JSON body allocation failed");
    }

    void *request = transaction->request;
    transaction->request = NULL;
    transaction->response = execute_request(client, request);
    if (!transaction->response)
        rt_trap("RestClient: JSON request failed");
    if (!rt_http_res_is_ok(transaction->response)) {
        rt_trap_clear_recovery();
        rest_json_transaction_release(transaction);
        free(transaction);
        return NULL;
    }

    transaction->response_body = rt_http_res_body_str(transaction->response);
    if (!transaction->response_body)
        rt_trap("RestClient: JSON response body allocation failed");
    if (rt_str_len(transaction->response_body) == 0) {
        rt_trap_clear_recovery();
        rest_json_transaction_release(transaction);
        free(transaction);
        return NULL;
    }
    parsed = rt_json_parse(transaction->response_body);
    rt_trap_clear_recovery();
    rest_json_transaction_release(transaction);
    free(transaction);
    return parsed;
}

/// @brief Send a GET accepting JSON and parse a successful response body.
/// @param obj RestClient receiver.
/// @param path Relative path joined to the snapshotted base URL.
/// @return Caller-owned parsed JSON value, or NULL for a non-2xx response,
///         empty body, or after a returning trap hook.
void *rt_restclient_get_json(void *obj, rt_string path) {
    rest_client *client = rest_client_checked(obj, "RestClient: null client");
    if (!client)
        return NULL;
    return rest_execute_json(client, "GET", path, NULL, 0);
}

/// @brief Serialize JSON into a POST and parse a successful JSON response.
/// @param obj RestClient receiver.
/// @param path Relative path joined to the snapshotted base URL.
/// @param json_body Managed JSON value serialized as the request body.
/// @return Caller-owned parsed JSON value, or NULL for a non-2xx response,
///         empty body, or after a returning trap hook.
void *rt_restclient_post_json(void *obj, rt_string path, void *json_body) {
    rest_client *client = rest_client_checked(obj, "RestClient: null client");
    if (!client)
        return NULL;

    return rest_execute_json(client, "POST", path, json_body, 1);
}

/// @brief Serialize JSON into a PUT and parse a successful JSON response.
/// @param obj RestClient receiver.
/// @param path Relative path joined to the snapshotted base URL.
/// @param json_body Managed JSON value serialized as the request body.
/// @return Caller-owned parsed JSON value, or NULL for a non-2xx response,
///         empty body, or after a returning trap hook.
void *rt_restclient_put_json(void *obj, rt_string path, void *json_body) {
    rest_client *client = rest_client_checked(obj, "RestClient: null client");
    if (!client)
        return NULL;

    return rest_execute_json(client, "PUT", path, json_body, 1);
}

/// @brief Serialize JSON into a PATCH and parse a successful JSON response.
/// @param obj RestClient receiver.
/// @param path Relative path joined to the snapshotted base URL.
/// @param json_body Managed JSON value serialized as the request body.
/// @return Caller-owned parsed JSON value, or NULL for a non-2xx response,
///         empty body, or after a returning trap hook.
void *rt_restclient_patch_json(void *obj, rt_string path, void *json_body) {
    rest_client *client = rest_client_checked(obj, "RestClient: null client");
    if (!client)
        return NULL;

    return rest_execute_json(client, "PATCH", path, json_body, 1);
}

/// @brief Send a bodyless DELETE and parse a successful JSON response.
/// @param obj RestClient receiver.
/// @param path Relative path joined to the snapshotted base URL.
/// @return Caller-owned parsed JSON value, or NULL for a non-2xx response,
///         empty body, or after a returning trap hook.
void *rt_restclient_delete_json(void *obj, rt_string path) {
    rest_client *client = rest_client_checked(obj, "RestClient: null client");
    if (!client)
        return NULL;

    return rest_execute_json(client, "DELETE", path, NULL, 0);
}

//=============================================================================
// Error Handling
//=============================================================================

/// @brief Read the synchronized status code of the last response.
/// @param obj RestClient receiver; NULL yields zero.
/// @return Last HTTP status, or zero before a response or after failure clears
///         compatibility state.
int64_t rt_restclient_last_status(void *obj) {
    if (!obj)
        return 0;
    rest_client *client = rest_client_checked(obj, "RestClient: invalid client");
    if (!client)
        return 0;
    rest_client_mutex_lock(&client->lock);
    int64_t status = client->last_status;
    rest_client_mutex_unlock(&client->lock);
    return status;
}

/// @brief Retain and return the client's last response.
/// @details The live-retain operation occurs while the client mutex protects
///          the cached pointer. A corrupt or overflowing reference count traps.
/// @param obj RestClient receiver; NULL yields NULL.
/// @return Caller-owned retained HttpRes, or NULL when no response is cached or
///         after a returning trap hook.
void *rt_restclient_last_response(void *obj) {
    if (!obj)
        return NULL;
    rest_client *client = rest_client_checked(obj, "RestClient: invalid client");
    if (!client)
        return NULL;
    rest_client_mutex_lock(&client->lock);
    void *response = client->last_response;
    int32_t retained = response ? rt_heap_try_retain_live(response) : 0;
    rest_client_mutex_unlock(&client->lock);
    if (response && retained != 1 && retained != 2) {
        rt_trap(retained < 0 ? "RestClient: last response reference count overflow"
                             : "RestClient: corrupt last response storage");
        return NULL;
    }
    return response;
}

/// @brief Check whether the synchronized last status is successful.
/// @param obj RestClient receiver; NULL yields zero.
/// @return One for a status in `[200, 299]`; otherwise zero.
int8_t rt_restclient_last_ok(void *obj) {
    if (!obj)
        return 0;
    rest_client *client = rest_client_checked(obj, "RestClient: invalid client");
    if (!client)
        return 0;
    rest_client_mutex_lock(&client->lock);
    int64_t status = client->last_status;
    rest_client_mutex_unlock(&client->lock);
    return (status >= 200 && status < 300) ? 1 : 0;
}
