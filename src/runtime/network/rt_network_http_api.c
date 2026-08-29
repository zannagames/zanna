//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/network/rt_network_http_api.c
// Purpose: Zanna-facing HTTP classes — the Http static class plus the HttpReq
//          and HttpRes instance classes. Split out of rt_network_http.c; these
//          wrappers build rt_http_req_t requests and drive the HTTP client core
//          (do_http_request etc.) declared in rt_network_http_internal.h.
//
// Key invariants:
//   - Request/response data structures and the client core live in
//     rt_network_http.c; this layer only marshals Zanna types in and out.
//   - Header/body builders validate method tokens and reject embedded NULs.
//
// Ownership/Lifetime:
//   - HttpReq/HttpRes are GC objects; their backing rt_http_req_t/rt_http_res_t
//     storage is freed via the client core's free_* helpers.
//
// Links: src/runtime/network/rt_network_http.c (HTTP client core + structs),
//        src/runtime/network/rt_network_http_internal.h (shared types/decls)
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_network_http_api.c
 * @brief Exposes the managed Http, HttpReq, and HttpRes runtime APIs.
 * @details The wrappers in this file validate managed receivers and strings,
 *          assemble native request state, translate traps into Result values,
 *          and manage transactional download files. Network transport and
 *          response parsing remain in the shared HTTP client core.
 */

// rt_network_internal.h requires these feature-test macros before inclusion.
#define _DARWIN_C_SOURCE 1
#define _GNU_SOURCE 1

#include "rt_network_http_internal.h"
#include "rt_network_internal.h"

#include "rt_box.h"
#include "rt_bytes.h"
#include "rt_entropy_platform.h"
#include "rt_error.h"
#include "rt_heap.h"
#include "rt_internal.h"
#include "rt_map.h"
#include "rt_network.h"
#include "rt_platform.h"
#include "rt_result.h"
#include "rt_trap.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#if RT_PLATFORM_WINDOWS
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

static size_t g_http_download_temp_counter = 0;

#if RT_PLATFORM_WINDOWS
/// @brief Convert one runtime UTF-8 filesystem path to a caller-owned UTF-16 path.
/// @param path NUL-terminated UTF-8 path.
/// @return Newly allocated UTF-16 path, or NULL for invalid UTF-8, overflow, or
///         allocation failure.
static wchar_t *http_download_utf8_to_wide(const char *path) {
    if (!path)
        return NULL;
    int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    if (required <= 0 || (size_t)required > SIZE_MAX / sizeof(wchar_t))
        return NULL;
    wchar_t *wide = (wchar_t *)malloc((size_t)required * sizeof(wchar_t));
    if (!wide)
        return NULL;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, required) != required) {
        free(wide);
        return NULL;
    }
    return wide;
}
#endif

/// @brief Release a temporary object after a container has retained it.
/// @details `Result.Ok` retains the response payload; this helper drops the
///          local ownership reference returned by `rt_http_req_send`.
/// @param obj Runtime object to release, or NULL.
static void http_release_temp_object(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief Validate and cast a public HttpReq receiver.
/// @details Checks stable class identity and full payload size before native
///          ownership fields are read or mutated. This prevents zero-tagged or
///          unrelated managed objects from being reinterpreted as requests.
/// @param obj Candidate receiver.
/// @param context Diagnostic emitted for invalid input.
/// @return Request payload, or NULL after trapping.
static rt_http_req_t *http_require_request(void *obj, const char *context) {
    if (!rt_obj_is_instance(obj, RT_HTTP_REQ_CLASS_ID, sizeof(rt_http_req_t))) {
        rt_trap(context);
        return NULL;
    }
    return (rt_http_req_t *)obj;
}

/// @brief Validate and cast a public HttpRes receiver.
/// @details Stable class and payload-size validation guards all native response
///          fields, including pointers to status text, headers, and body bytes.
/// @param obj Candidate receiver.
/// @param context Diagnostic emitted for invalid input.
/// @return Response payload, or NULL after trapping.
static rt_http_res_t *http_require_response(void *obj, const char *context) {
    if (!rt_obj_is_instance(obj, RT_HTTP_RES_CLASS_ID, sizeof(rt_http_res_t))) {
        rt_trap(context);
        return NULL;
    }
    return (rt_http_res_t *)obj;
}

/// @brief Copy the current trap text before cleanup clears its recovery frame.
/// @param output Destination buffer.
/// @param capacity Destination capacity.
/// @param fallback Diagnostic used when the active text is empty.
static void http_save_trap(char *output, size_t capacity, const char *fallback) {
    if (!output || capacity == 0)
        return;
    const char *error = rt_trap_get_error();
    snprintf(output, capacity, "%s", error && error[0] ? error : fallback);
}

/// @brief Build `Result.ErrStr` from stable native diagnostic bytes.
/// @details String and Result creation run under a fresh recovery frame. Any
///          partial managed values are released before allocation failure is
///          propagated, and the String's initial reference is dropped after
///          the Result has retained it. This avoids retrying diagnostic OOM
///          against the recovery frame that caught the original HTTP failure.
/// @param message NUL-terminated diagnostic, with a fixed fallback for NULL.
/// @return Caller-owned error Result, or NULL after a returning trap hook.
static void *http_error_result(const char *message) {
    rt_string volatile error_string = NULL;
    void *volatile result = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        char saved_error[512];
        http_save_trap(saved_error, sizeof(saved_error), "HTTP: error Result allocation failed");
        rt_trap_clear_recovery();
        http_release_temp_object((void *)result);
        http_release_temp_object((void *)error_string);
        rt_trap(saved_error);
        return NULL;
    }
    const char *stable = message && message[0] ? message : "HTTP request failed";
    error_string = rt_string_from_bytes(stable, strlen(stable));
    if (!error_string) {
        rt_trap_clear_recovery();
        return NULL;
    }
    result = rt_result_err_str((rt_string)error_string);
    if (!result) {
        rt_trap_clear_recovery();
        http_release_temp_object((void *)error_string);
        return NULL;
    }
    rt_trap_clear_recovery();
    http_release_temp_object((void *)error_string);
    return (void *)result;
}

/// @brief Wrap and consume one caller-owned HttpRes in `Result.Ok`.
/// @details `rt_result_ok` retains the response. This helper releases the
///          transport's initial reference after success and also releases it
///          when Result allocation or payload retention traps.
/// @param response Caller-owned stable HttpRes reference.
/// @return Caller-owned success Result, or NULL after a returning trap hook.
static void *http_success_result_owned(void *response) {
    void *volatile result = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        char saved_error[512];
        http_save_trap(saved_error, sizeof(saved_error), "HTTP: success Result allocation failed");
        rt_trap_clear_recovery();
        http_release_temp_object((void *)result);
        http_release_temp_object(response);
        rt_trap(saved_error);
        return NULL;
    }
    result = rt_result_ok(response);
    if (!result) {
        rt_trap_clear_recovery();
        http_release_temp_object(response);
        return NULL;
    }
    rt_trap_clear_recovery();
    http_release_temp_object(response);
    return (void *)result;
}

/// @brief Build a sibling temporary path for an HTTP download target.
/// @details The returned path appends a hidden `.zanna-download-N.tmp` suffix to
///          the final filename, keeping the temp on the same filesystem so
///          rename-based replacement can be used after a successful transfer.
/// @param dest Final destination path.
/// @param kind Suffix kind such as `"tmp"` or `"bak"`.
/// @return Heap-allocated temp path, or NULL on allocation overflow/OOM.
static char *http_download_temp_path(const char *dest, const char *kind) {
    if (!dest || !kind)
        return NULL;
    size_t id = rt_atomic_fetch_add_size(&g_http_download_temp_counter, 1u, __ATOMIC_RELAXED) + 1u;
    uint64_t random_id = 0;
    if (rt_entropy_platform_random_u64(&random_id) != 0)
        return NULL;
    size_t dest_len = strlen(dest);
    size_t kind_len = strlen(kind);
    const char *marker = ".zanna-download-";
    size_t marker_len = strlen(marker);
    if (dest_len > SIZE_MAX - marker_len - kind_len - 64u)
        return NULL;
    size_t cap = dest_len + marker_len + kind_len + 64u;
    char *path = (char *)malloc(cap);
    if (!path)
        return NULL;
    int written = snprintf(
        path, cap, "%s%s%016llx-%zu.%s", dest, marker, (unsigned long long)random_id, id, kind);
    if (written < 0 || (size_t)written >= cap) {
        free(path);
        return NULL;
    }
    return path;
}

/// @brief Open a path for binary writing with exclusive creation.
/// @details Uses file-descriptor APIs instead of C `fopen("xb")` because that
///          mode is not accepted by every supported C library. The returned
///          stream owns the descriptor and closes it through `fclose`.
/// @param path Filesystem path to create.
/// @return Writable binary stream, or NULL when the file exists or creation fails.
static FILE *http_download_fopen_exclusive(const char *path) {
#if RT_PLATFORM_WINDOWS
    wchar_t *wide = http_download_utf8_to_wide(path);
    if (!wide)
        return NULL;
    int fd = _wopen(
        wide, _O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY | _O_NOINHERIT, _S_IREAD | _S_IWRITE);
    free(wide);
#else
    int flags = O_CREAT | O_EXCL | O_WRONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    int fd = open(path, flags, 0600);
#endif
    if (fd < 0)
        return NULL;
#if RT_PLATFORM_WINDOWS
    FILE *f = _fdopen(fd, "wb");
#else
    FILE *f = fdopen(fd, "wb");
#endif
    if (!f) {
#if RT_PLATFORM_WINDOWS
        _close(fd);
#else
        close(fd);
#endif
        return NULL;
    }
    return f;
}

/// @brief Open a same-directory HTTP download temp file with exclusive creation.
/// @details Retries random sibling names so a pre-existing file or symlink cannot
///          be followed by `fopen("wb")`. The file remains beside the final
///          destination so the completed download can be installed with rename.
/// @param dest Final destination path.
/// @param temp_path_out Receives the allocated temp path on success.
/// @return Writable FILE on success; NULL on allocation, entropy, or create failure.
static FILE *http_download_open_unique_temp(const char *dest, char **temp_path_out) {
    if (temp_path_out)
        *temp_path_out = NULL;
    if (!dest || !temp_path_out)
        return NULL;
    for (int attempt = 0; attempt < 128; attempt++) {
        char *candidate = http_download_temp_path(dest, "tmp");
        if (!candidate)
            return NULL;
        FILE *f = http_download_fopen_exclusive(candidate);
        if (f) {
            *temp_path_out = candidate;
            return f;
        }
        int saved_errno = errno;
        free(candidate);
        if (saved_errno != EEXIST)
            return NULL;
    }
    return NULL;
}

/// @brief Atomically replace a download destination with a completed temp file.
/// @details POSIX `rename` atomically replaces an existing non-directory path
///          without a race-prone backup name. Windows uses `MoveFileEx` with
///          replace-existing and write-through flags. On failure the temp file
///          remains caller-owned for removal and the prior destination remains.
/// @param temp_path Fully written temporary file.
/// @param dest_path Final destination path.
/// @return One on successful replacement; zero on replacement failure.
static int http_download_replace_file(const char *temp_path, const char *dest_path) {
    if (!temp_path || !dest_path)
        return 0;
#if RT_PLATFORM_WINDOWS
    wchar_t *wide_temp = http_download_utf8_to_wide(temp_path);
    wchar_t *wide_dest = http_download_utf8_to_wide(dest_path);
    int replaced =
        wide_temp && wide_dest &&
        MoveFileExW(wide_temp, wide_dest, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    free(wide_temp);
    free(wide_dest);
    return replaced ? 1 : 0;
#else
    return rename(temp_path, dest_path) == 0 ? 1 : 0;
#endif
}

/// @brief Flush C buffering and request durable storage for a download temp.
/// @details `fclose` alone reports buffered write failures but does not request
///          that the OS commit the completed file before atomic publication.
///          `_commit`/`fsync` provide the platform adapter for that durability
///          boundary while the descriptor is still owned by @p stream.
/// @param stream Open writable temp stream.
/// @return One only when flush and descriptor synchronization both succeed.
static int http_download_flush_and_sync(FILE *stream) {
    if (!stream || fflush(stream) != 0)
        return 0;
#if RT_PLATFORM_WINDOWS
    int fd = _fileno(stream);
    return fd >= 0 && _commit(fd) == 0 ? 1 : 0;
#else
    int fd = fileno(stream);
    return fd >= 0 && fsync(fd) == 0 ? 1 : 0;
#endif
}

/// @brief Preserve ordinary permission bits when replacing an existing file.
/// @details Special set-user/group/sticky bits are deliberately not copied to
///          newly downloaded content. A missing or unstatable destination keeps
///          the secure 0600 temp-file mode rather than turning a successful
///          transfer into a metadata failure.
/// @param temp_path Completed temp path.
/// @param dest_path Prospective destination path.
/// @return One when no change is needed or permissions were applied; zero only
///         when an existing file was statted but its ordinary mode could not be set.
static int http_download_preserve_mode(const char *temp_path, const char *dest_path) {
    if (!temp_path || !dest_path)
        return 1;
#if RT_PLATFORM_WINDOWS
    wchar_t *wide_temp = http_download_utf8_to_wide(temp_path);
    wchar_t *wide_dest = http_download_utf8_to_wide(dest_path);
    if (!wide_temp || !wide_dest) {
        free(wide_temp);
        free(wide_dest);
        return 0;
    }
    struct _stat64 existing;
    if (_wstat64(wide_dest, &existing) != 0) {
        free(wide_temp);
        free(wide_dest);
        return 1;
    }
    int mode = existing.st_mode & (_S_IREAD | _S_IWRITE);
    int changed = _wchmod(wide_temp, mode) == 0 ? 1 : 0;
    free(wide_temp);
    free(wide_dest);
    return changed;
#else
    struct stat existing;
    if (stat(dest_path, &existing) != 0)
        return 1;
    return chmod(temp_path, existing.st_mode & 0777) == 0 ? 1 : 0;
#endif
}

/// @brief Remove a staged download using the platform's native path encoding.
/// @param path NUL-terminated staged-file path.
/// @return One when the file is removed, otherwise zero.
static int http_download_remove_file(const char *path) {
    if (!path)
        return 0;
#if RT_PLATFORM_WINDOWS
    wchar_t *wide = http_download_utf8_to_wide(path);
    if (!wide)
        return 0;
    int removed = _wremove(wide) == 0 ? 1 : 0;
    free(wide);
    return removed;
#else
    return remove(path) == 0 ? 1 : 0;
#endif
}

/// @brief Copy an HTTP response body into a fresh Bytes object.
/// @details Centralizes the allocation check used by all `Http.*Bytes`
///          convenience APIs. Returning NULL after trapping prevents a
///          recoverable allocation failure from being followed by
///          `bytes_data(NULL)` and a secondary crash.
/// @param body Borrowed response body bytes; may be NULL when `body_len` is 0.
/// @param body_len Number of bytes to copy.
/// @param context Trap message for allocation failure.
/// @return Owned Bytes object, or NULL if allocation failed.
static void *http_copy_body_to_bytes(const uint8_t *body, size_t body_len, const char *context) {
    if (body_len > (size_t)INT64_MAX) {
        rt_trap(context ? context : "HTTP: response body too large");
        return NULL;
    }
    if (body_len > 0 && !body) {
        rt_trap("HTTP: invalid response body storage");
        return NULL;
    }
    void *result = rt_bytes_new((int64_t)body_len);
    if (!result)
        return NULL;
    uint8_t *result_ptr = bytes_data(result);
    if (body_len > 0 && result_ptr)
        memcpy(result_ptr, body, body_len);
    return result;
}

/// @brief GC finalizer for an HttpReq object. Safe on a partially-built request
///        because every helper either nulls or initialises its target.
/// @param obj HttpReq payload being finalized, or NULL.
static void rt_http_req_finalize(void *obj) {
    if (!obj)
        return;
    rt_http_req_t *req = (rt_http_req_t *)obj;
    free(req->method);
    req->method = NULL;
    free_parsed_url(&req->url);
    free_headers(req->headers);
    req->headers = NULL;
    free(req->body);
    req->body = NULL;
    req->body_len = 0;
    if (req->connection_pool && rt_obj_release_check0(req->connection_pool))
        rt_obj_free(req->connection_pool);
    req->connection_pool = NULL;
    req->timeout_ms = 0;
}

/// @brief Duplicate an HTTP method token into request-owned storage.
/// @details The one-shot and builder APIs store `method` in `rt_http_req_t` and later free it
///          through normal request cleanup. This helper centralizes the allocation check so an
///          out-of-memory trap cannot be followed by a request with a NULL method when a test or
///          embedding trap hook returns.
/// @param method Non-NULL HTTP method token to duplicate.
/// @return Heap-allocated method string, or NULL only if the active trap handler returned after
///         reporting allocation failure.
static char *http_dup_method_or_trap(const char *method) {
    char *copy = method ? strdup(method) : NULL;
    if (!copy) {
        rt_trap("HTTP: method allocation failed");
        return NULL;
    }
    return copy;
}

/// @brief Select the optional body representation for a one-shot HTTP request.
typedef enum http_one_shot_body_kind {
    HTTP_ONE_SHOT_BODY_NONE = 0,
    HTTP_ONE_SHOT_BODY_STRING = 1,
    HTTP_ONE_SHOT_BODY_BYTES = 2
} http_one_shot_body_kind_t;

/// @brief Release every native allocation owned by a temporary HTTP request.
/// @details Bytes bodies are borrowed for the synchronous transaction, while
///          String bodies are copied. @p owns_body distinguishes those cases so
///          cleanup cannot free caller-owned Bytes storage.
/// @param req Temporary request to reset. The record itself is not freed.
/// @param owns_body Nonzero when `req->body` is a native owned copy.
static void http_cleanup_stack_request(rt_http_req_t *req, int owns_body) {
    if (!req)
        return;
    free(req->method);
    req->method = NULL;
    if (owns_body)
        free(req->body);
    req->body = NULL;
    req->body_len = 0;
    free_parsed_url(&req->url);
    free_headers(req->headers);
    req->headers = NULL;
}

/// @brief Execute one public static HTTP verb with unified validation/cleanup.
/// @details Validates managed URL/body identities and length-delimited input
///          before native setup. One recovery frame owns method, parsed URL,
///          copied String body, headers, and any completed response. Allocation
///          or transport traps release all of them before the original category
///          and diagnostic are re-raised. Bytes bodies remain borrowed only for
///          this synchronous call.
/// @param method Constant valid HTTP method token.
/// @param url Non-empty managed URL without embedded NUL bytes.
/// @param body_kind Optional body representation.
/// @param body Managed String or Bytes matching @p body_kind; NULL means empty.
/// @param content_type Header value installed for a non-empty body, or NULL.
/// @return Caller-owned HttpRes, or NULL after a returning trap hook.
static rt_http_res_t *http_execute_one_shot(const char *method,
                                            rt_string url,
                                            http_one_shot_body_kind_t body_kind,
                                            void *body,
                                            const char *content_type) {
    const char *url_str = NULL;
    size_t url_len = 0;
    if (!rt_net_cstr_no_embedded_nul(url, &url_str, &url_len) || url_len == 0) {
        rt_trap_net("HTTP: invalid URL", Err_InvalidUrl);
        return NULL;
    }

    const char *string_body = NULL;
    int64_t body_len64 = 0;
    uint8_t *bytes_body = NULL;
    if (body_kind == HTTP_ONE_SHOT_BODY_STRING && body) {
        if (!rt_string_is_handle(body)) {
            rt_trap("HTTP: invalid String body");
            return NULL;
        }
        string_body = rt_string_cstr((rt_string)body);
        body_len64 = rt_str_len((rt_string)body);
        if (!string_body || body_len64 < 0 || (uint64_t)body_len64 > (uint64_t)SIZE_MAX) {
            rt_trap("HTTP: invalid body length");
            return NULL;
        }
    } else if (body_kind == HTTP_ONE_SHOT_BODY_BYTES && body) {
        if (!rt_bytes_is_bytes(body)) {
            rt_trap("HTTP: invalid Bytes body");
            return NULL;
        }
        body_len64 = rt_bytes_len(body);
        bytes_body = bytes_data(body);
        if (body_len64 < 0 || (uint64_t)body_len64 > (uint64_t)SIZE_MAX ||
            (body_len64 > 0 && !bytes_body)) {
            rt_trap("HTTP: invalid body length");
            return NULL;
        }
    }

    rt_http_req_t *const req = (rt_http_req_t *)calloc(1, sizeof(*req));
    if (!req) {
        rt_trap("HTTP: request allocation failed");
        return NULL;
    }
    rt_http_res_t *volatile response = NULL;
    volatile int owns_body = 0;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        char saved_error[512];
        int net_code = rt_trap_get_net_code();
        http_save_trap(saved_error, sizeof(saved_error), "HTTP: request failed");
        rt_trap_clear_recovery();
        http_cleanup_stack_request(req, owns_body);
        free(req);
        http_release_temp_object((void *)response);
        if (net_code)
            rt_trap_net(saved_error, net_code);
        else
            rt_trap(saved_error);
        return NULL;
    }

    req->method = http_dup_method_or_trap(method);
    if (!req->method)
        rt_trap("HTTP: method allocation failed");
    req->timeout_ms = HTTP_DEFAULT_TIMEOUT_MS;
    req->tls_verify = 1;
    req->follow_redirects = 1;
    req->accept_gzip = 1;
    req->decode_gzip = 1;
    if (parse_url(url_str, &req->url) < 0)
        rt_trap_net("HTTP: invalid URL format", Err_InvalidUrl);

    if (body_kind == HTTP_ONE_SHOT_BODY_STRING && body_len64 > 0) {
        owns_body = 1;
        set_request_body_from_string(req, (rt_string)body);
        if (!req->body || req->body_len != (size_t)body_len64)
            rt_trap("HTTP: body allocation failed");
    } else if (body_kind == HTTP_ONE_SHOT_BODY_BYTES && body_len64 > 0) {
        req->body = bytes_body;
        req->body_len = (size_t)body_len64;
    }
    if (req->body_len > 0 && content_type) {
        if (!add_header(req, "Content-Type", content_type))
            rt_trap("HTTP: header allocation failed");
    }

    response = do_http_request(req, HTTP_MAX_REDIRECTS);
    if (!response)
        rt_trap_net("HTTP: request failed", Err_NetworkError);

    http_cleanup_stack_request(req, owns_body);
    free(req);
    owns_body = 0;
    rt_trap_clear_recovery();
    return (rt_http_res_t *)response;
}

/// @brief Copy and consume an HttpRes body as a managed String.
/// @details The response remains owned until String allocation succeeds. A
///          recovery frame releases it before propagating allocation failure.
/// @param response Owned HttpRes to consume.
/// @return Caller-owned String.
static rt_string http_take_response_string(rt_http_res_t *response) {
    rt_string volatile result = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        char saved_error[512];
        http_save_trap(saved_error, sizeof(saved_error), "HTTP: response allocation failed");
        rt_trap_clear_recovery();
        http_release_temp_object((void *)result);
        http_release_temp_object(response);
        rt_trap(saved_error);
        return NULL;
    }
    result = rt_string_from_bytes((const char *)response->body, response->body_len);
    if (!result)
        rt_trap("HTTP: response allocation failed");
    rt_trap_clear_recovery();
    http_release_temp_object(response);
    return (rt_string)result;
}

/// @brief Copy and consume an HttpRes body as Bytes.
/// @details Balances response ownership on both successful and trapped managed
///          result allocation.
/// @param response Owned HttpRes to consume.
/// @return Caller-owned Bytes.
static void *http_take_response_bytes(rt_http_res_t *response) {
    void *volatile result = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        char saved_error[512];
        http_save_trap(saved_error, sizeof(saved_error), "HTTP: response allocation failed");
        rt_trap_clear_recovery();
        http_release_temp_object((void *)result);
        http_release_temp_object(response);
        rt_trap(saved_error);
        return NULL;
    }
    result = http_copy_body_to_bytes(
        response->body, response->body_len, "HTTP: response body allocation failed");
    if (!result)
        rt_trap("HTTP: response allocation failed");
    rt_trap_clear_recovery();
    http_release_temp_object(response);
    return (void *)result;
}

/// @brief Copy headers and consume an HttpRes used by the HEAD convenience API.
/// @param response Owned HttpRes to consume.
/// @return Caller-owned Map copy.
static void *http_take_response_headers(rt_http_res_t *response) {
    void *volatile result = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        char saved_error[512];
        http_save_trap(saved_error, sizeof(saved_error), "HTTP: header copy failed");
        rt_trap_clear_recovery();
        http_release_temp_object((void *)result);
        http_release_temp_object(response);
        rt_trap(saved_error);
        return NULL;
    }
    result = rt_http_res_headers(response);
    if (!result)
        rt_trap("HTTP: header copy failed");
    rt_trap_clear_recovery();
    http_release_temp_object(response);
    return (void *)result;
}

//=============================================================================
// Http Static Class Implementation
//=============================================================================
//
// These one-shot helpers (rt_http_get, rt_http_post, …) build an
// `rt_http_req_t` on the stack, run a single transaction with the
// default timeout (`HTTP_DEFAULT_TIMEOUT_MS`) and redirect cap
// (`HTTP_MAX_REDIRECTS`), then return either the body as a string,
// the body as a Bytes object, or in the case of HEAD, just the
// response object. They `rt_trap_net` on malformed URLs and
// transport failures so callers don't need to error-check.
//=============================================================================

/// @brief HTTP GET that returns the body decoded as a UTF-8 string.
/// @param url Nonempty managed HTTP/HTTPS URL without embedded NUL.
/// @return Newly owned response-body String, or NULL after a returning trap.
/// @throws Err_InvalidUrl on empty / unparsable URL,
///         Err_NetworkError on transport failure.
rt_string rt_http_get(rt_string url) {
    rt_http_res_t *response =
        http_execute_one_shot("GET", url, HTTP_ONE_SHOT_BODY_NONE, NULL, NULL);
    return response ? http_take_response_string(response) : NULL;
}

/// @brief HTTP GET that returns the raw response body as a Bytes object.
/// @param url Nonempty managed HTTP/HTTPS URL without embedded NUL.
/// @return Newly owned response-body Bytes, or NULL after a returning trap.
/// @throws Err_InvalidUrl / Err_NetworkError on failure (see `rt_http_get`).
void *rt_http_get_bytes(rt_string url) {
    rt_http_res_t *response =
        http_execute_one_shot("GET", url, HTTP_ONE_SHOT_BODY_NONE, NULL, NULL);
    return response ? http_take_response_bytes(response) : NULL;
}

/// @brief HTTP POST with a string body; returns the response body as a string.
///
/// Adds `Content-Type: text/plain; charset=utf-8` automatically
/// when the body is non-empty. The body is copied into a request-
/// owned buffer so the caller's `rt_string` lifetime is independent.
/// @param url Nonempty managed HTTP/HTTPS URL without embedded NUL.
/// @param body Managed String whose exact bytes become the request body.
/// @return Newly owned response-body String, or NULL after a returning trap.
rt_string rt_http_post(rt_string url, rt_string body) {
    rt_http_res_t *response = http_execute_one_shot(
        "POST", url, HTTP_ONE_SHOT_BODY_STRING, body, "text/plain; charset=utf-8");
    return response ? http_take_response_string(response) : NULL;
}

/// @brief HTTP POST with a Bytes body; returns the response body as Bytes.
///
/// Adds `Content-Type: application/octet-stream` automatically when
/// the body is non-empty. The Bytes object is referenced directly
/// (not copied), so it must remain alive for the duration of the call.
/// @param url Nonempty managed HTTP/HTTPS URL without embedded NUL.
/// @param body Managed Bytes borrowed for this synchronous transaction.
/// @return Newly owned response-body Bytes, or NULL after a returning trap.
void *rt_http_post_bytes(rt_string url, void *body) {
    rt_http_res_t *response = http_execute_one_shot(
        "POST", url, HTTP_ONE_SHOT_BODY_BYTES, body, "application/octet-stream");
    return response ? http_take_response_bytes(response) : NULL;
}

/// @brief Native ownership state for one streaming HTTP download transaction.
/// @details Heap storage keeps every cleanup-relevant pointer well-defined
///          across the recovery frame that converts internal traps to Boolean
///          failure. No managed object is stored in this record.
typedef struct http_download_state {
    rt_http_req_t request; ///< Native request and parsed URL ownership.
    char *temp_path;       ///< Exclusively created sibling temp path.
    FILE *stream;          ///< Open temp stream until flush/close completes.
} http_download_state_t;

/// @brief Destroy one partially or fully initialized streaming-download state.
/// @details Safe after a non-local trap because the state itself is heap
///          allocated. The helper closes an open stream, optionally removes the
///          temp path, releases all request-native storage, and frees the state.
/// @param state Owned state, or NULL.
/// @param remove_temp Nonzero to unlink the staged file before freeing its path.
static void http_download_state_destroy(http_download_state_t *state, int remove_temp) {
    if (!state)
        return;
    if (state->stream) {
        (void)fclose(state->stream);
        state->stream = NULL;
    }
    if (remove_temp && state->temp_path)
        (void)http_download_remove_file(state->temp_path);
    http_cleanup_stack_request(&state->request, 1);
    free(state->temp_path);
    state->temp_path = NULL;
    free(state);
}

/// @brief HTTP GET that streams the response body to a file at `dest_path`.
/// @details Returns false on bad handles, embedded NULs, malformed URLs,
///          transport/protocol errors, non-2xx status, allocation failure,
///          short write, sync/close failure, or atomic replacement failure.
///          A same-directory exclusively created temp is durably flushed and
///          renamed only after the full response succeeds. Existing ordinary
///          permission bits are preserved. All internal traps are contained so
///          callers can branch on the Boolean contract.
/// @param url Non-empty HTTP/HTTPS URL without embedded NUL bytes.
/// @param dest_path Non-empty destination path without embedded NUL bytes.
/// @return One on complete atomic publication; zero on any failure.
int8_t rt_http_download(rt_string url, rt_string dest_path) {
    const char *url_str = NULL;
    const char *path_str = NULL;
    size_t url_len = 0;
    size_t path_len = 0;
    if (!rt_net_cstr_no_embedded_nul(url, &url_str, &url_len) || url_len == 0)
        return 0;
    if (!rt_net_cstr_no_embedded_nul(dest_path, &path_str, &path_len) || path_len == 0)
        return 0;

    http_download_state_t *const state = (http_download_state_t *)calloc(1, sizeof(*state));
    if (!state)
        return 0;
    state->request.method = strdup("GET");
    if (!state->request.method) {
        http_download_state_destroy(state, 0);
        return 0;
    }
    state->request.timeout_ms = HTTP_DEFAULT_TIMEOUT_MS;
    state->request.tls_verify = 1;
    state->request.follow_redirects = 1;
    state->request.accept_gzip = 0;
    state->request.decode_gzip = 0;
    state->request.force_http1 = 1;

    if (parse_url(url_str, &state->request.url) < 0) {
        http_download_state_destroy(state, 0);
        return 0;
    }

    state->stream = http_download_open_unique_temp(path_str, &state->temp_path);
    if (!state->stream) {
        http_download_state_destroy(state, 1);
        return 0;
    }

    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        rt_trap_clear_recovery();
        http_download_state_destroy(state, 1);
        return 0;
    }
    int ok = do_http_download_request(&state->request, HTTP_MAX_REDIRECTS, state->stream);
    rt_trap_clear_recovery();
    http_cleanup_stack_request(&state->request, 1);

    int sync_ok = ok ? http_download_flush_and_sync(state->stream) : 0;
    int close_ok = fclose(state->stream) == 0 ? 1 : 0;
    state->stream = NULL;

    // RC-14: if fwrite wrote fewer bytes (disk full, etc.) or fclose failed
    // (buffered data flush failure), remove the partial/corrupt file.
    if (!ok || !sync_ok || !close_ok) {
        http_download_state_destroy(state, 1);
        return 0;
    }
    if (!http_download_preserve_mode(state->temp_path, path_str) ||
        !http_download_replace_file(state->temp_path, path_str)) {
        http_download_state_destroy(state, 1);
        return 0;
    }
    http_download_state_destroy(state, 0);
    return 1;
}

/// @brief HTTP HEAD request; returns just the response headers map.
///
/// Useful for size/type probes (Content-Length, Content-Type) and
/// existence checks without paying for the body. Throws on
/// transport failure.
/// @param url Nonempty managed HTTP/HTTPS URL without embedded NUL.
/// @return Newly owned independent response-header Map.
void *rt_http_head(rt_string url) {
    rt_http_res_t *response =
        http_execute_one_shot("HEAD", url, HTTP_ONE_SHOT_BODY_NONE, NULL, NULL);
    return response ? http_take_response_headers(response) : NULL;
}

/// @brief HTTP PATCH with a string body; returns the response body as a string.
///
/// Mirrors `rt_http_post` but sends the `PATCH` method
/// (RFC 5789), used to apply partial updates to a resource.
/// @param url Nonempty managed HTTP/HTTPS URL without embedded NUL.
/// @param body Managed String copied into request-owned storage.
/// @return Newly owned response-body String, or NULL after a returning trap.
rt_string rt_http_patch(rt_string url, rt_string body) {
    rt_http_res_t *response = http_execute_one_shot(
        "PATCH", url, HTTP_ONE_SHOT_BODY_STRING, body, "text/plain; charset=utf-8");
    return response ? http_take_response_string(response) : NULL;
}

/// @brief HTTP OPTIONS request; returns the response body as a string.
///
/// Typically used for CORS preflight discovery — the meaningful
/// data lives in the response headers (`Allow`, `Access-Control-*`).
/// @param url Nonempty managed HTTP/HTTPS URL without embedded NUL.
/// @return Newly owned response-body String, or NULL after a returning trap.
rt_string rt_http_options(rt_string url) {
    rt_http_res_t *response =
        http_execute_one_shot("OPTIONS", url, HTTP_ONE_SHOT_BODY_NONE, NULL, NULL);
    return response ? http_take_response_string(response) : NULL;
}

/// @brief HTTP PUT with a string body; returns the response body as a string.
///
/// PUT replaces the target resource (RFC 7231 §4.3.4). Body is
/// copied and tagged `Content-Type: text/plain; charset=utf-8`.
/// @param url Nonempty managed HTTP/HTTPS URL without embedded NUL.
/// @param body Managed String copied into request-owned storage.
/// @return Newly owned response-body String, or NULL after a returning trap.
rt_string rt_http_put(rt_string url, rt_string body) {
    rt_http_res_t *response = http_execute_one_shot(
        "PUT", url, HTTP_ONE_SHOT_BODY_STRING, body, "text/plain; charset=utf-8");
    return response ? http_take_response_string(response) : NULL;
}

/// @brief HTTP PUT with a Bytes body; returns the response body as Bytes.
///
/// As `rt_http_put` but for binary payloads — sets
/// `Content-Type: application/octet-stream` when the body is non-empty.
/// @param url Nonempty managed HTTP/HTTPS URL without embedded NUL.
/// @param body Managed Bytes borrowed for this synchronous transaction.
/// @return Newly owned response-body Bytes, or NULL after a returning trap.
void *rt_http_put_bytes(rt_string url, void *body) {
    rt_http_res_t *response = http_execute_one_shot(
        "PUT", url, HTTP_ONE_SHOT_BODY_BYTES, body, "application/octet-stream");
    return response ? http_take_response_bytes(response) : NULL;
}

/// @brief HTTP DELETE; returns the response body as a string.
/// @param url Nonempty managed HTTP/HTTPS URL without embedded NUL.
/// @return Newly owned response-body String, or NULL after a returning trap.
rt_string rt_http_delete(rt_string url) {
    rt_http_res_t *response =
        http_execute_one_shot("DELETE", url, HTTP_ONE_SHOT_BODY_NONE, NULL, NULL);
    return response ? http_take_response_string(response) : NULL;
}

/// @brief HTTP DELETE; returns the response body as a Bytes object.
/// @param url Nonempty managed HTTP/HTTPS URL without embedded NUL.
/// @return Newly owned response-body Bytes, or NULL after a returning trap.
void *rt_http_delete_bytes(rt_string url) {
    rt_http_res_t *response =
        http_execute_one_shot("DELETE", url, HTTP_ONE_SHOT_BODY_NONE, NULL, NULL);
    return response ? http_take_response_bytes(response) : NULL;
}

//=============================================================================
// HttpReq Instance Class Implementation
//
// The builder-style HttpReq class lets callers configure a request
// (headers, body, timeout, redirect policy) before sending. Setters
// return `obj` so they can be chained. All instances are GC-managed
// and finalized via `rt_http_req_finalize`.
//=============================================================================

/// @brief Construct a new HTTP request with the given method and URL.
///
/// The URL is parsed eagerly so callers see invalid-URL traps at
/// construction time rather than when sending. Defaults: 30s
/// timeout, follow redirects with cap `HTTP_MAX_REDIRECTS`.
/// @param method Managed nonempty HTTP method token without embedded NUL.
/// @param url Managed nonempty HTTP/HTTPS URL without embedded NUL.
/// @return Newly owned managed HttpReq, or NULL after a returning trap.
/// @throws Err_InvalidUrl on bad URL, generic trap on null method.
void *rt_http_req_new(rt_string method, rt_string url) {
    const char *method_str = method && rt_string_is_handle(method) ? rt_string_cstr(method) : NULL;
    const char *url_str = url && rt_string_is_handle(url) ? rt_string_cstr(url) : NULL;

    if (!method_str || http_rt_string_has_embedded_nul(method) ||
        !http_method_is_token(method_str)) {
        rt_trap("HTTP: invalid method");
        return NULL;
    }
    if (!url_str || *url_str == '\0' || http_rt_string_has_embedded_nul(url)) {
        rt_trap_net("HTTP: invalid URL", Err_InvalidUrl);
        return NULL;
    }

    rt_http_req_t *volatile req = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        char saved_error[512];
        int net_code = rt_trap_get_net_code();
        http_save_trap(saved_error, sizeof(saved_error), "HTTP: request construction failed");
        rt_trap_clear_recovery();
        http_release_temp_object((void *)req);
        if (net_code)
            rt_trap_net(saved_error, net_code);
        else
            rt_trap(saved_error);
        return NULL;
    }

    req = (rt_http_req_t *)rt_obj_new_i64(RT_HTTP_REQ_CLASS_ID, (int64_t)sizeof(rt_http_req_t));
    if (!req)
        rt_trap("HTTP: memory allocation failed");
    memset((void *)req, 0, sizeof(*req));
    rt_obj_set_finalizer((void *)req, rt_http_req_finalize);
    req->method = http_dup_method_or_trap(method_str);
    if (!req->method)
        rt_trap("HTTP: method allocation failed");
    req->timeout_ms = HTTP_DEFAULT_TIMEOUT_MS;
    req->tls_verify = 1;
    req->follow_redirects = 1;
    req->max_redirects = HTTP_MAX_REDIRECTS;
    req->accept_gzip = 1;
    req->decode_gzip = 1;
    req->keep_alive = 0;
    req->connection_pool = NULL;

    if (parse_url(url_str, &req->url) < 0) {
        rt_trap_net("HTTP: invalid URL format", Err_InvalidUrl);
    }

    rt_trap_clear_recovery();
    return (void *)req;
}

/// @brief Validate header name/value for a request. Traps on NULL request,
///        embedded NUL, non-token names, or CR/LF in the value.
/// @param obj Candidate HttpReq receiver.
/// @param name Managed header field name, or NULL for an intentional no-op.
/// @param value Managed field value, or NULL for an intentional no-op.
/// @param name_out Receives the borrowed validated field-name bytes.
/// @param value_out Receives the borrowed validated field-value bytes.
/// @return One when valid, zero when NULL name/value is intentionally ignored,
///         and negative one after trapping for an invalid receiver or value.
static int http_req_header_args_valid(
    void *obj, rt_string name, rt_string value, const char **name_out, const char **value_out) {
    if (!http_require_request(obj, "HTTP: invalid request"))
        return -1;

    if (!name || !value)
        return 0;
    if (!rt_string_is_handle(name) || !rt_string_is_handle(value)) {
        rt_trap("HTTP: invalid header");
        return -1;
    }
    const char *name_str = rt_string_cstr(name);
    const char *value_str = rt_string_cstr(value);
    if (http_rt_string_has_embedded_nul(name) || http_rt_string_has_embedded_nul(value) ||
        !http_method_is_token(name_str)) {
        rt_trap("HTTP: invalid header");
        return -1;
    }
    for (const char *p = value_str; *p; ++p) {
        if (*p == '\r' || *p == '\n') {
            rt_trap("HTTP: invalid header");
            return -1;
        }
    }
    *name_out = name_str;
    *value_out = value_str;
    return 1;
}

/// @brief Set a request header, replacing any existing field with the same
///        case-insensitive name. Silently ignores NULL name/value.
/// @param obj Required HttpReq receiver.
/// @param name Managed valid HTTP field name.
/// @param value Managed value without CR, LF, or embedded NUL.
/// @return `obj` (for fluent chaining).
void *rt_http_req_set_header(void *obj, rt_string name, rt_string value) {
    const char *name_str = NULL;
    const char *value_str = NULL;
    int valid = http_req_header_args_valid(obj, name, value, &name_str, &value_str);
    if (valid <= 0)
        return valid < 0 ? NULL : obj;

    rt_http_req_t *req = (rt_http_req_t *)obj;
    if (!set_header(req, name_str, value_str)) {
        rt_trap("HTTP: header allocation failed");
        return obj;
    }
    return obj;
}

/// @brief Append a request header without replacing existing same-name
///        fields (for legitimately repeatable fields). Silently ignores
///        NULL name/value.
/// @param obj Required HttpReq receiver.
/// @param name Managed valid HTTP field name.
/// @param value Managed value without CR, LF, or embedded NUL.
/// @return `obj` (for fluent chaining).
void *rt_http_req_add_header(void *obj, rt_string name, rt_string value) {
    const char *name_str = NULL;
    const char *value_str = NULL;
    int valid = http_req_header_args_valid(obj, name, value, &name_str, &value_str);
    if (valid <= 0)
        return valid < 0 ? NULL : obj;

    if (!add_header((rt_http_req_t *)obj, name_str, value_str)) {
        rt_trap("HTTP: header allocation failed");
        return obj;
    }
    return obj;
}

/// @brief Replace the request body with a Bytes object (copied).
///
/// Frees any previously-set body. The bytes are duplicated into a
/// request-owned buffer so the caller's Bytes lifetime is independent.
/// @param obj Required HttpReq receiver.
/// @param data Managed Bytes to copy, or NULL to clear the body.
/// @return `obj` (for fluent chaining).
void *rt_http_req_set_body(void *obj, void *data) {
    rt_http_req_t *req = http_require_request(obj, "HTTP: invalid request");
    if (!req)
        return NULL;

    if (!data) {
        free(req->body);
        req->body = NULL;
        req->body_len = 0;
        return obj;
    }
    if (!rt_bytes_is_bytes(data)) {
        rt_trap("HTTP: invalid Bytes body");
        return obj;
    }
    int64_t len = rt_bytes_len(data);
    uint8_t *ptr = bytes_data(data);
    if (len < 0 || (uint64_t)len > (uint64_t)SIZE_MAX || (len > 0 && !ptr)) {
        rt_trap("HTTP: invalid body length");
        return obj;
    }
    uint8_t *replacement = NULL;
    if (len > 0) {
        replacement = (uint8_t *)malloc((size_t)len);
        if (!replacement) {
            rt_trap("HTTP: memory allocation failed");
            return obj;
        }
        memcpy(replacement, ptr, (size_t)len);
    }
    free(req->body);
    req->body = replacement;
    req->body_len = (size_t)len;

    return obj;
}

/// @brief Replace the request body with a String's exact bytes.
/// @details Frees the old body after the replacement copy is complete. NULL
///          clears the body; embedded NUL bytes are preserved as content.
/// @param obj Required HttpReq receiver.
/// @param text Managed String to copy, or NULL to clear the body.
/// @return `obj` (for fluent chaining).
void *rt_http_req_set_body_str(void *obj, rt_string text) {
    rt_http_req_t *req = http_require_request(obj, "HTTP: invalid request");
    if (!req)
        return NULL;
    if (!text) {
        free(req->body);
        req->body = NULL;
        req->body_len = 0;
        return obj;
    }
    if (!rt_string_is_handle(text)) {
        rt_trap("HTTP: invalid String body");
        return obj;
    }
    const char *text_str = rt_string_cstr(text);
    int64_t len64 = rt_str_len(text);
    if (!text_str || len64 < 0 || (uint64_t)len64 > (uint64_t)SIZE_MAX) {
        rt_trap("HTTP: invalid body length");
        return obj;
    }
    size_t len = (size_t)len64;
    uint8_t *replacement = NULL;
    if (len > 0) {
        replacement = (uint8_t *)malloc(len);
        if (!replacement) {
            rt_trap("HTTP: memory allocation failed");
            return obj;
        }
        memcpy(replacement, text_str, len);
    }
    free(req->body);
    req->body = replacement;
    req->body_len = len;

    return obj;
}

/// @brief Set per-request I/O timeout in milliseconds.
/// @param obj Required HttpReq receiver.
/// @param timeout_ms Timeout in [0, @c INT_MAX] milliseconds.
/// @return `obj` (for fluent chaining).
void *rt_http_req_set_timeout(void *obj, int64_t timeout_ms) {
    rt_http_req_t *req = http_require_request(obj, "HTTP: invalid request");
    if (!req)
        return NULL;
    int timeout_int = 0;
    if (!rt_net_timeout_ms_to_int(timeout_ms, &timeout_int)) {
        rt_trap("HTTP: invalid timeout");
        return obj;
    }
    req->timeout_ms = timeout_int;

    return obj;
}

/// @brief Toggle TLS certificate verification for HTTPS requests.
/// @param obj Required HttpReq receiver.
/// @param verify Nonzero to verify peer certificate and hostname, zero to skip.
/// @return `obj` (for fluent chaining).
void *rt_http_req_set_tls_verify(void *obj, int8_t verify) {
    rt_http_req_t *req = http_require_request(obj, "HTTP: invalid request");
    if (!req)
        return NULL;
    req->tls_verify = verify ? 1 : 0;
    return obj;
}

/// @brief Disable TLS certificate verification for explicitly local/test HTTP requests.
/// @details This wrapper is equivalent to `rt_http_req_set_tls_verify(obj, 0)`, but
///          keeps insecure certificate handling visible in source and runtime API
///          dumps. Do not use it for production HTTPS clients.
/// @param obj Required HttpReq receiver.
/// @return `obj` (for fluent chaining).
void *rt_http_req_allow_insecure_certificates_for_testing(void *obj) {
    rt_http_req_t *req = http_require_request(obj, "HTTP: invalid request");
    if (!req)
        return NULL;
    req->tls_verify = 0;
    return obj;
}

/// @brief Toggle automatic redirect following (3xx Location handling).
/// @param obj Required HttpReq receiver.
/// @param follow Nonzero to follow redirects, zero to return them.
/// @return `obj` (for fluent chaining).
void *rt_http_req_set_follow_redirects(void *obj, int8_t follow) {
    rt_http_req_t *req = http_require_request(obj, "HTTP: invalid request");
    if (!req)
        return NULL;
    req->follow_redirects = follow ? 1 : 0;
    return obj;
}

/// @brief Override the per-request redirect cap (negative values clamp to 0).
/// @param obj Required HttpReq receiver.
/// @param max_redirects Maximum hops; values above @c INT_MAX trap.
/// @return `obj` (for fluent chaining).
void *rt_http_req_set_max_redirects(void *obj, int64_t max_redirects) {
    rt_http_req_t *req = http_require_request(obj, "HTTP: invalid request");
    if (!req)
        return NULL;
    if (max_redirects < 0)
        max_redirects = 0;
    if (max_redirects > INT_MAX) {
        rt_trap("HTTP: invalid redirect limit");
        return obj;
    }
    req->max_redirects = (int)max_redirects;
    return obj;
}

/// @brief Toggle keep-alive / pooled transport for this request.
/// @details Standalone requests (no HttpClient/RestClient pool attached) get
///          the process-wide default connection pool at send time, so the
///          flag enables real socket reuse on the public `HttpReq` surface.
/// @param obj Required HttpReq receiver.
/// @param keep_alive Nonzero to permit pooled reuse, zero to close afterward.
/// @return `obj` (for fluent chaining).
void *rt_http_req_set_keep_alive(void *obj, int8_t keep_alive) {
    rt_http_req_t *req = http_require_request(obj, "HTTP: invalid request");
    if (!req)
        return NULL;
    req->keep_alive = keep_alive ? 1 : 0;
    return obj;
}

/// @brief Restrict this request to HTTP/1.1 transport even over TLS.
/// @details When set, the TLS client advertises only `http/1.1` via
///          ALPN instead of the default `h2,http/1.1`, forcing the
///          server to select HTTP/1.1. Useful for code that depends
///          on HTTP/1.1 framing semantics (e.g. the `Connection:
///          keep-alive` response header, which HTTP/2 omits because
///          persistence is implicit). Default is `0` (allow HTTP/2).
/// @param obj Required HttpReq receiver.
/// @param force Nonzero to advertise only HTTP/1.1, zero for default ALPN.
/// @return `obj` (for fluent chaining).
void *rt_http_req_set_force_http1(void *obj, int8_t force) {
    rt_http_req_t *req = http_require_request(obj, "HTTP: invalid request");
    if (!req)
        return NULL;
    req->force_http1 = force ? 1 : 0;
    return obj;
}

/// @brief Attach an internal connection pool to this request.
/// @details A registry-backed non-throwing retain closes the validation/share
///          race. Stable pool identity is checked again while that reference
///          prevents finalization from destroying the native mutex. Invalid or
///          stale pools leave the request's previous attachment unchanged.
/// @param obj Required HttpReq receiver.
/// @param pool Managed connection pool to retain, or NULL to detach.
/// @return `obj` (for fluent chaining).
void *rt_http_req_set_connection_pool(void *obj, void *pool) {
    rt_http_req_t *req = http_require_request(obj, "HTTP: invalid request");
    if (!req)
        return NULL;
    if (req->connection_pool == pool)
        return obj;

    int retained = 0;
    if (pool) {
        retained = rt_heap_try_retain_live(pool);
        if (retained != 1 && retained != 2) {
            rt_trap(retained < 0 ? "HTTP: connection pool reference count overflow"
                                 : "HTTP: invalid connection pool");
            return obj;
        }
        if (!rt_http_conn_pool_is_handle(pool)) {
            if (retained == 1)
                http_release_temp_object(pool);
            rt_trap("HTTP: invalid connection pool");
            return obj;
        }
    }

    void *old_pool = req->connection_pool;
    req->connection_pool = pool;
    http_release_temp_object(old_pool);
    return obj;
}

/// @brief Execute a configured HttpReq and return the resulting HttpRes.
///
/// Auto-tags a non-empty body with
/// `Content-Type: application/octet-stream` if the caller didn't
/// already set one. Honours the request's redirect cap (which may
/// be 0 to disable following).
/// @param obj Required HttpReq receiver.
/// @return Newly-allocated `rt_http_res_t*` (GC-managed) or NULL on
///         transport failure (`do_http_request` returns NULL).
void *rt_http_req_send(void *obj) {
    rt_http_req_t *req = http_require_request(obj, "HTTP: invalid request");
    if (!req)
        return NULL;

    // Standalone keep-alive requests attach the process-wide default pool so
    // SetKeepAlive(true) actually reuses connections; HttpClient/RestClient
    // inject their own per-client pools before this point.
    if (req->keep_alive && !req->connection_pool) {
        void *pool = rt_http_default_connection_pool();
        if (!pool)
            return NULL;
        if (!rt_http_req_set_connection_pool(obj, pool))
            return NULL;
    }

    // Add Content-Type for POST with body if not set
    if (req->body && req->body_len > 0 && !has_header(req, "Content-Type")) {
        if (!add_header(req, "Content-Type", "application/octet-stream")) {
            rt_trap("HTTP: header allocation failed");
            return NULL;
        }
    }

    rt_http_res_t *res = do_http_request(req, req->max_redirects);
    return res;
}

/// @brief Execute a configured HttpReq and return `Result<HttpRes>`.
/// @details This is the production-friendly companion to `rt_http_req_send`:
///          transport/setup traps and NULL transport results are converted to
///          `Result.ErrStr`, while received HTTP responses are returned as
///          `Result.Ok(HttpRes)` regardless of status code.
/// @param obj HttpReq receiver to execute.
/// @return Newly owned Result containing Ok(HttpRes) or Err(String).
void *rt_http_req_send_result(void *obj) {
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        char saved_error[512];
        http_save_trap(saved_error, sizeof(saved_error), "HTTP request failed");
        rt_trap_clear_recovery();
        return http_error_result(saved_error);
    }
    void *response = rt_http_req_send(obj);
    char null_error[512] = "HTTP request failed";
    if (!response)
        http_save_trap(null_error, sizeof(null_error), "HTTP request failed");
    rt_trap_clear_recovery();
    return response ? http_success_result_owned(response) : http_error_result(null_error);
}

//=============================================================================
// HttpRes Instance Class Implementation
//
// Read-only accessors over the response. Header lookups are
// case-insensitive: the parser stores keys lower-cased so the
// lookup helper just down-cases the request name.
//=============================================================================

/// @brief Status code (e.g. 200, 404). Returns 0 if `obj` is NULL.
/// @param obj HttpRes receiver, or NULL.
/// @return Numeric status, or zero for NULL or after a returning invalid-handle trap.
int64_t rt_http_res_status(void *obj) {
    if (!obj)
        return 0;
    rt_http_res_t *res = http_require_response(obj, "HTTP: invalid response");
    return res ? res->status : 0;
}

/// @brief Reason phrase from the status line (e.g. "Not Found"). Empty if unset.
/// @param obj HttpRes receiver, or NULL.
/// @return Newly owned reason-phrase String, or empty when absent.
rt_string rt_http_res_status_text(void *obj) {
    if (!obj)
        return rt_str_empty();

    rt_http_res_t *res = http_require_response(obj, "HTTP: invalid response");
    if (!res)
        return rt_str_empty();
    if (!res->status_text)
        return rt_str_empty();

    return rt_string_from_bytes(res->status_text, strlen(res->status_text));
}

/// @brief Return a *copy* of the response headers as a fresh Map(String→String).
///
/// Copying defends the response's internal map against caller
/// mutation. Header keys are lower-case (already canonicalised by
/// the parser). Empty map is returned if `obj` is NULL or the
/// response has no headers.
/// @param obj HttpRes receiver, or NULL.
/// @return Newly owned independent Map, or NULL after a returning trap.
void *rt_http_res_headers(void *obj) {
    if (!obj)
        return rt_map_new();

    rt_http_res_t *res = http_require_response(obj, "HTTP: invalid response");
    if (!res)
        return NULL;
    void *volatile copy = NULL;
    void *volatile keys = NULL;
    rt_string volatile value = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        char saved_error[512];
        http_save_trap(saved_error, sizeof(saved_error), "HTTP: header copy failed");
        rt_trap_clear_recovery();
        http_release_temp_object((void *)value);
        http_release_temp_object((void *)keys);
        http_release_temp_object((void *)copy);
        rt_trap(saved_error);
        return NULL;
    }

    copy = rt_map_new();
    if (!copy) {
        rt_trap_clear_recovery();
        return NULL;
    }
    if (!res->headers) {
        rt_trap_clear_recovery();
        return (void *)copy;
    }

    keys = rt_map_keys(res->headers);
    if (!keys) {
        rt_trap_clear_recovery();
        http_release_temp_object((void *)copy);
        return NULL;
    }
    int64_t count = rt_seq_len((void *)keys);
    for (int64_t i = 0; i < count; i++) {
        rt_string key = (rt_string)rt_seq_get((void *)keys, i);
        void *boxed = rt_map_get(res->headers, key);
        if (!boxed || rt_box_type(boxed) != RT_BOX_STR)
            continue;
        value = rt_unbox_str(boxed);
        if (!value)
            continue;
        rt_map_set((void *)copy, key, (void *)value);
        if (!rt_map_has((void *)copy, key)) {
            http_release_temp_object((void *)value);
            value = NULL;
            rt_trap_clear_recovery();
            http_release_temp_object((void *)keys);
            http_release_temp_object((void *)copy);
            return NULL;
        }
        http_release_temp_object((void *)value);
        value = NULL;
    }
    rt_trap_clear_recovery();
    http_release_temp_object((void *)keys);
    return (void *)copy;
}

/// @brief Return a copy of the raw response body as a Bytes object.
/// @param obj HttpRes receiver, or NULL.
/// @return Newly owned body Bytes; NULL receivers produce empty Bytes.
void *rt_http_res_body(void *obj) {
    if (!obj)
        return rt_bytes_new(0);

    rt_http_res_t *res = http_require_response(obj, "HTTP: invalid response");
    if (!res)
        return NULL;
    return http_copy_body_to_bytes(
        res->body, res->body_len, "HTTP: response body allocation failed");
}

/// @brief Return the response body decoded as a UTF-8 string.
///
/// No charset detection — bytes are passed through as if UTF-8.
/// Empty string for null/empty bodies.
/// @param obj HttpRes receiver, or NULL.
/// @return Newly owned body String, or empty for NULL/empty bodies.
rt_string rt_http_res_body_str(void *obj) {
    if (!obj)
        return rt_str_empty();

    rt_http_res_t *res = http_require_response(obj, "HTTP: invalid response");
    if (!res)
        return rt_str_empty();
    if (res->body_len == 0)
        return rt_str_empty();
    if (!res->body) {
        rt_trap("HTTP: invalid response body storage");
        return NULL;
    }

    return rt_string_from_bytes((const char *)res->body, res->body_len);
}

/// @brief Look up a single response header by name (case-insensitive).
///
/// Down-cases `name` and probes the parser-canonicalised header
/// map. Returns an empty string on miss or null arguments.
/// @param obj HttpRes receiver, or NULL.
/// @param name Managed header name without embedded NUL.
/// @return Newly owned independent value String, or empty on a miss.
rt_string rt_http_res_header(void *obj, rt_string name) {
    if (!obj)
        return rt_str_empty();

    rt_http_res_t *res = http_require_response(obj, "HTTP: invalid response");
    if (!res)
        return rt_str_empty();
    if (!res->headers)
        return rt_str_empty();

    if (!name || !rt_string_is_handle(name)) {
        if (name)
            rt_trap("HTTP: invalid header name");
        return rt_str_empty();
    }
    const char *name_str = rt_string_cstr(name);
    int64_t len64 = rt_str_len(name);
    if (!name_str || len64 < 0 || (uint64_t)len64 > (uint64_t)SIZE_MAX ||
        (len64 > 0 && memchr(name_str, '\0', (size_t)len64) != NULL)) {
        rt_trap("HTTP: invalid header name");
        return rt_str_empty();
    }

    size_t len = (size_t)len64;
    char stack_lower[256];
    char *volatile heap_lower = NULL;
    rt_string volatile lower_key = NULL;
    rt_string volatile value = NULL;
    rt_string volatile copy = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        char saved_error[512];
        http_save_trap(saved_error, sizeof(saved_error), "HTTP: header lookup failed");
        rt_trap_clear_recovery();
        http_release_temp_object((void *)copy);
        http_release_temp_object((void *)value);
        http_release_temp_object((void *)lower_key);
        free((void *)heap_lower);
        rt_trap(saved_error);
        return NULL;
    }

    char *lower_name = stack_lower;
    if (len >= sizeof(stack_lower)) {
        if (len == SIZE_MAX) {
            rt_trap("HTTP: header name is too large");
        }
        heap_lower = (char *)malloc(len + 1);
        if (!heap_lower)
            rt_trap("HTTP: header lookup allocation failed");
        lower_name = (char *)heap_lower;
    }

    for (size_t i = 0; i < len; i++) {
        char c = name_str[i];
        if (c >= 'A' && c <= 'Z')
            lower_name[i] = c + ('a' - 'A');
        else
            lower_name[i] = c;
    }
    lower_name[len] = '\0';

    lower_key = rt_string_from_bytes(lower_name, len);
    if (!lower_key) {
        rt_trap_clear_recovery();
        free((void *)heap_lower);
        return NULL;
    }

    void *boxed = rt_map_get(res->headers, (rt_string)lower_key);
    if (!boxed || rt_box_type(boxed) != RT_BOX_STR) {
        rt_trap_clear_recovery();
        http_release_temp_object((void *)lower_key);
        free((void *)heap_lower);
        return rt_str_empty();
    }

    value = rt_unbox_str(boxed);
    if (!value) {
        rt_trap_clear_recovery();
        http_release_temp_object((void *)lower_key);
        free((void *)heap_lower);
        return rt_str_empty();
    }
    const char *value_cstr = rt_string_cstr((rt_string)value);
    int64_t value_len64 = rt_str_len((rt_string)value);
    if (!value_cstr || value_len64 < 0 || (uint64_t)value_len64 > (uint64_t)SIZE_MAX)
        rt_trap("HTTP: invalid stored header value");
    copy = rt_string_from_bytes(value_cstr, (size_t)value_len64);
    if (!copy) {
        rt_trap_clear_recovery();
        http_release_temp_object((void *)value);
        http_release_temp_object((void *)lower_key);
        free((void *)heap_lower);
        return NULL;
    }
    rt_trap_clear_recovery();
    http_release_temp_object((void *)value);
    http_release_temp_object((void *)lower_key);
    free((void *)heap_lower);
    return (rt_string)copy;
}

/// @brief Convenience predicate: 2xx success status?
/// @param obj HttpRes receiver, or NULL.
/// @return 1 if `200 <= status < 300`, 0 otherwise (and on NULL).
int8_t rt_http_res_is_ok(void *obj) {
    if (!obj)
        return 0;
    rt_http_res_t *res = http_require_response(obj, "HTTP: invalid response");
    if (!res)
        return 0;

    int status = res->status;
    return (status >= 200 && status < 300) ? 1 : 0;
}
