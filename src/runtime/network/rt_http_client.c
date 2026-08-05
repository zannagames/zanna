//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/network/rt_http_client.c
// Purpose: Session-based HTTP client with cookie jar, auto-redirect, and
//          persistent default headers.
// Key invariants:
//   - Stable class identity is validated before private payload access.
//   - Cookies, defaults, redirect policy, and pool state are mutex protected.
//   - Requests and redirect hops share one recoverable ownership transaction.
//   - Response cookies stage completely before allocation-free jar publication.
// Ownership/Lifetime:
//   - Client objects and returned response/snapshot objects are runtime managed.
//   - The client owns its header Map, native cookie list, mutex, and pool reference.
// Links: rt_http_client.h (API), rt_network_http.c (underlying HTTP)
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Implements synchronized stateful HTTP client sessions.
 * @details Manages persistent default headers, scoped cookie storage,
 * configurable redirects and timeouts, transactional keep-alive pool
 * replacement, exact-length request inputs, and trap-safe ownership across
 * every request and redirect hop.
 */

#include "rt_http_client.h"
#include "rt_network_http_internal.h"
#include "rt_network_time.inc"

#include "rt_heap.h"
#include "rt_internal.h"
#include "rt_map.h"
#include "rt_network.h"
#include "rt_object.h"
#include "rt_seq.h"
#include "rt_string.h"

#include <limits.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
/** Restrict the Windows SDK surface to core declarations. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
/** Windows compatibility alias for case-insensitive string comparison. */
#define strcasecmp _stricmp
/** Windows compatibility alias for bounded case-insensitive comparison. */
#define strncasecmp _strnicmp
typedef CRITICAL_SECTION http_client_mutex_t;
/** Initialize a Windows HttpClient mutex. */
#define HTTP_CLIENT_MUTEX_INIT(m) (InitializeCriticalSectionEx((m), 0, 0) != FALSE)
/** Acquire a Windows HttpClient mutex. */
#define HTTP_CLIENT_MUTEX_LOCK(m) EnterCriticalSection(m)
/** Release a Windows HttpClient mutex. */
#define HTTP_CLIENT_MUTEX_UNLOCK(m) LeaveCriticalSection(m)
/** Destroy a Windows HttpClient mutex. */
#define HTTP_CLIENT_MUTEX_DESTROY(m) DeleteCriticalSection(m)
#else
#include <pthread.h>
#include <strings.h>
typedef pthread_mutex_t http_client_mutex_t;
/** Initialize a POSIX HttpClient mutex and report success. */
#define HTTP_CLIENT_MUTEX_INIT(m) (pthread_mutex_init(m, NULL) == 0)
/** Acquire a POSIX HttpClient mutex. */
#define HTTP_CLIENT_MUTEX_LOCK(m) pthread_mutex_lock(m)
/** Release a POSIX HttpClient mutex. */
#define HTTP_CLIENT_MUTEX_UNLOCK(m) pthread_mutex_unlock(m)
/** Destroy a POSIX HttpClient mutex. */
#define HTTP_CLIENT_MUTEX_DESTROY(m) pthread_mutex_destroy(m)
#endif

#include "rt_trap.h"

/// @brief Internal separator accepted when the lower-level HTTP parser joins Set-Cookie headers.
/// @details Kept in sync with rt_network_http.c. Newline is still accepted below for compatibility
///          with response objects created before the parser switched away from a newline separator.
#define HTTP_SET_COOKIE_JOIN_SEPARATOR '\037'

//=============================================================================
// Internal Structure
//=============================================================================

/** One native cookie-jar record with owned identity and attribute strings. */
typedef struct rt_http_cookie {
    char *name;
    char *value;
    char *domain;
    char *path;
    int8_t secure;
    int8_t host_only;
    int8_t http_only;
    int8_t same_site;
    int8_t persistent;
    int64_t expires_at;
    struct rt_http_cookie *next;
} rt_http_cookie;

/** Internal SameSite policy values stored on parsed cookies. */
enum {
    HTTP_COOKIE_SAMESITE_UNSPECIFIED = 0,
    HTTP_COOKIE_SAMESITE_LAX = 1,
    HTTP_COOKIE_SAMESITE_STRICT = 2,
    HTTP_COOKIE_SAMESITE_NONE = 3
};

/** Complete private state of one managed HttpClient session. */
typedef struct {
    void *default_headers; // Map<String, String>
    rt_http_cookie *cookies;
    int64_t timeout_ms;
    int64_t max_redirects;
    int8_t follow_redirects;
    void *connection_pool;
    int64_t pool_size;
    int8_t keep_alive;
    http_client_mutex_t lock;
    int8_t lock_initialized;
} rt_http_client_impl;

/// @brief Validate and cast a public HttpClient receiver.
/// @details Stable class identity, complete payload size, and mutex
///          initialization are checked before any native lock or cookie pointer
///          is accessed. Callers stop local control flow when NULL is returned,
///          supporting embedders whose trap hook resumes execution.
/// @param obj Candidate managed receiver.
/// @param context Diagnostic for NULL, stale, or wrong-class input.
/// @return Initialized HttpClient payload, or NULL after trapping.
static rt_http_client_impl *http_client_require(void *obj, const char *context) {
    if (!rt_obj_is_instance(obj, RT_HTTP_CLIENT_CLASS_ID, sizeof(rt_http_client_impl)) ||
        !((rt_http_client_impl *)obj)->lock_initialized) {
        rt_trap(context);
        return NULL;
    }
    return (rt_http_client_impl *)obj;
}

/// @brief Validate a public millisecond timeout and narrow it for socket APIs.
/// @param timeout_ms Candidate timeout in milliseconds.
/// @param out_timeout_ms Optional receiver for the validated `int` value.
/// @return 1 for values in the inclusive range 0 through `INT_MAX`; 0 otherwise.
static int http_client_timeout_ms_to_int(int64_t timeout_ms, int *out_timeout_ms) {
    if (timeout_ms < 0 || timeout_ms > INT_MAX)
        return 0;
    if (out_timeout_ms)
        *out_timeout_ms = (int)timeout_ms;
    return 1;
}

/// @brief Detect embedded null bytes across a runtime String's exact logical length.
/// @details Null Strings are considered free of embedded nulls, while malformed
///          or non-String handles are conservatively rejected as containing one.
/// @param value Runtime String to inspect.
/// @return Nonzero when an embedded null is present or the handle cannot be inspected safely.
static int http_client_string_has_embedded_nul(rt_string value) {
    if (!value)
        return 0;
    if (!rt_string_is_handle(value))
        return 1;
    const char *cstr = rt_string_cstr(value);
    int64_t len64 = rt_str_len(value);
    if (!cstr || len64 < 0 || (uint64_t)len64 > (uint64_t)SIZE_MAX)
        return 1;
    if (len64 == 0)
        return 0;
    return memchr(cstr, '\0', (size_t)len64) != NULL;
}

/// @brief Return true when a runtime string contains CR or LF bytes.
/// @details Header values must not contain raw line breaks because they would
///          split the HTTP header block. This helper scans the full runtime
///          string length instead of trusting C-string termination.
/// @param value Runtime string to inspect.
/// @return Nonzero when CR or LF is present.
static int http_client_string_has_crlf(rt_string value) {
    if (!value)
        return 0;
    if (!rt_string_is_handle(value))
        return 1;
    const char *cstr = rt_string_cstr(value);
    int64_t len64 = rt_str_len(value);
    if (!cstr || len64 < 0 || (uint64_t)len64 > (uint64_t)SIZE_MAX)
        return 1;
    if (len64 == 0)
        return 0;
    for (int64_t i = 0; i < len64; i++) {
        if (cstr[i] == '\r' || cstr[i] == '\n')
            return 1;
    }
    return 0;
}

/// @brief Release a retained runtime object and free it when the reference count reaches zero.
/// @details Used for short-lived snapshots and client-owned references so lock cleanup paths do
///          not repeat the release/free sequence or accidentally skip it on early returns.
/// @param obj Runtime object pointer to release, or NULL.
static void http_client_release_obj(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief Release retained default-header snapshot entries.
/// @details Header snapshots retain each key/value while the client mutex is
///          held, then apply those headers after unlocking. This helper is used
///          by both normal and trap-recovery paths so partially populated
///          snapshots do not leak.
/// @param headers Snapshot array, or NULL.
/// @param header_count Number of initialized entries in @p headers.
static void http_client_release_header_snapshot(void *headers, int64_t header_count) {
    typedef struct {
        rt_string key;
        rt_string value;
    } header_snapshot;

    header_snapshot *items = (header_snapshot *)headers;
    for (int64_t i = 0; i < header_count; i++) {
        if (items[i].value)
            rt_string_unref(items[i].value);
        if (items[i].key)
            rt_string_unref(items[i].key);
    }
}

/// @brief Copy the current thread's trap message into a fixed buffer.
/// @details Trap recovery stores the latest message in runtime TLS. Cleanup
///          paths copy it before clearing recovery and releasing resources so
///          they can re-raise the original error after the mutex is unlocked.
/// @param buffer Destination buffer.
/// @param buffer_size Size of @p buffer.
/// @param fallback Message used when the trap did not provide one.
static void http_client_save_trap_error(char *buffer, size_t buffer_size, const char *fallback) {
    const char *err = rt_trap_get_error();
    snprintf(buffer, buffer_size, "%s", err && err[0] ? err : fallback);
}

/// @brief Release a linked list of native cookie records and all owned strings.
/// @param cookie First cookie to release; may be null.
static void free_cookie_list(rt_http_cookie *cookie) {
    while (cookie) {
        rt_http_cookie *next = cookie->next;
        free(cookie->name);
        free(cookie->value);
        free(cookie->domain);
        free(cookie->path);
        free(cookie);
        cookie = next;
    }
}

/// @brief Finalize a managed HttpClient payload.
/// @details Releases the managed default-header Map and connection pool, frees
///          the native cookie jar, and destroys an initialized mutex.
/// @param obj HttpClient payload being finalized; may be null.
static void rt_http_client_finalize(void *obj) {
    if (!obj)
        return;
    rt_http_client_impl *c = (rt_http_client_impl *)obj;
    http_client_release_obj(c->default_headers);
    http_client_release_obj(c->connection_pool);
    free_cookie_list(c->cookies);
    if (c->lock_initialized)
        HTTP_CLIENT_MUTEX_DESTROY(&c->lock);
}

//=============================================================================
// Helpers
//=============================================================================

/// @brief Apply a synchronized snapshot of session defaults to one request object.
/// @details Copies and retains header entries under the client mutex, then
///          configures headers, timeout, redirect behavior, keep-alive, and the
///          retained pool after unlocking. Trap recovery releases every partial
///          snapshot and re-raises the original diagnostic.
/// @param c Initialized HttpClient payload.
/// @param req Managed HTTP request receiving the snapshot.
/// @param allow_sensitive_headers Nonzero to include credentials and other
///        sensitive defaults; zero filters them after a cross-origin redirect.
/// @return Nonzero after the complete snapshot is applied; zero after a
///         returning trap hook and local cleanup.
static int apply_defaults(rt_http_client_impl *c, void *req, int allow_sensitive_headers) {
    typedef struct {
        rt_string key;
        rt_string value;
    } header_snapshot;

    int64_t timeout_ms = 0;
    int8_t keep_alive = 0;
    void *volatile keys = NULL;
    void *volatile connection_pool = NULL;
    header_snapshot *volatile headers = NULL;
    volatile int64_t header_count = 0;
    int snapshot_failed = 0;
    volatile int mutex_locked = 0;

    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[256];
        http_client_save_trap_error(
            saved_error, sizeof(saved_error), "HttpClient: failed to apply defaults");
        rt_trap_clear_recovery();
        if (mutex_locked)
            HTTP_CLIENT_MUTEX_UNLOCK(&c->lock);
        http_client_release_obj((void *)keys);
        http_client_release_header_snapshot((void *)headers, (int64_t)header_count);
        free((void *)headers);
        http_client_release_obj((void *)connection_pool);
        rt_trap(saved_error);
        return 0;
    }

    HTTP_CLIENT_MUTEX_LOCK(&c->lock);
    mutex_locked = 1;
    keys = rt_map_keys(c->default_headers);
    if (!keys) {
        rt_trap("HttpClient: default header key snapshot failed");
        goto returning_trap;
    }
    int64_t count = rt_seq_len(keys);
    if (count > 0) {
        if ((uint64_t)count > (uint64_t)SIZE_MAX / sizeof(*headers)) {
            snapshot_failed = 1;
        } else {
            headers = (header_snapshot *)calloc((size_t)count, sizeof(*headers));
            if (!headers)
                snapshot_failed = 1;
        }
    }
    for (int64_t i = 0; i < count; i++) {
        rt_string key = (rt_string)rt_seq_get(keys, i);
        void *val = rt_map_get(c->default_headers, key);
        if (!snapshot_failed && val && key &&
            (allow_sensitive_headers ||
             !rt_http_header_is_sensitive_for_cross_origin_redirect(rt_string_cstr(key)))) {
            rt_string retained_key = rt_string_ref(key);
            if (!retained_key) {
                snapshot_failed = 1;
                continue;
            }
            rt_string retained_value = rt_string_ref((rt_string)val);
            if (!retained_value) {
                http_client_release_obj((void *)retained_key);
                snapshot_failed = 1;
                continue;
            }
            headers[(int64_t)header_count].key = retained_key;
            headers[(int64_t)header_count].value = retained_value;
            header_count++;
        }
    }
    http_client_release_obj((void *)keys);
    keys = NULL;

    timeout_ms = c->timeout_ms;
    keep_alive = c->keep_alive;
    connection_pool = keep_alive ? c->connection_pool : NULL;
    if (connection_pool)
        rt_obj_retain_maybe(connection_pool);
    HTTP_CLIENT_MUTEX_UNLOCK(&c->lock);
    mutex_locked = 0;

    if (snapshot_failed) {
        rt_trap("HttpClient: memory allocation failed");
        goto returning_trap;
    }

    for (int64_t i = 0; i < (int64_t)header_count; i++) {
        if (!rt_http_req_set_header(req, headers[i].key, headers[i].value)) {
            rt_trap("HttpClient: default header application failed");
            goto returning_trap;
        }
    }
    http_client_release_header_snapshot((void *)headers, (int64_t)header_count);
    free((void *)headers);
    headers = NULL;
    header_count = 0;

    if (!rt_http_req_set_timeout(req, timeout_ms)) {
        rt_trap("HttpClient: timeout application failed");
        goto returning_trap;
    }
    rt_http_req_set_follow_redirects(req, 0);
    rt_http_req_set_keep_alive(req, keep_alive);
    if (!rt_http_req_set_connection_pool(req, (void *)connection_pool)) {
        rt_trap("HttpClient: connection pool attachment failed");
        goto returning_trap;
    }
    http_client_release_obj((void *)connection_pool);
    connection_pool = NULL;
    rt_trap_clear_recovery();
    return 1;

returning_trap:
    rt_trap_clear_recovery();
    if (mutex_locked)
        HTTP_CLIENT_MUTEX_UNLOCK(&c->lock);
    http_client_release_obj((void *)keys);
    http_client_release_header_snapshot((void *)headers, (int64_t)header_count);
    free((void *)headers);
    http_client_release_obj((void *)connection_pool);
    return 0;
}

/// @brief Read the current wall-clock time for cookie expiration comparisons.
/// @return Seconds since the Unix epoch as reported by the C runtime.
static int64_t cookie_now_seconds(void) {
    return (int64_t)time(NULL);
}

/// @brief Copy a byte range after trimming surrounding spaces and horizontal tabs.
/// @param start First byte of the source range.
/// @param len Number of source bytes.
/// @return Newly allocated null-terminated trimmed copy, or null on allocation failure.
static char *cookie_strdup_range_trim(const char *start, size_t len) {
    while (len > 0 && (*start == ' ' || *start == '\t')) {
        start++;
        len--;
    }
    while (len > 0 && (start[len - 1] == ' ' || start[len - 1] == '\t'))
        len--;

    char *out = (char *)malloc(len + 1);
    if (!out)
        return NULL;
    memcpy(out, start, len);
    out[len] = '\0';
    return out;
}

/// @brief Allocate an ASCII-lowercase copy of cookie-related text.
/// @param text Null-terminated source; null is treated as an empty string.
/// @return Newly allocated lowercase copy, or null on allocation failure.
static char *cookie_strdup_lower(const char *text) {
    size_t len = text ? strlen(text) : 0;
    char *out = (char *)malloc(len + 1);
    if (!out)
        return NULL;
    for (size_t i = 0; i < len; i++) {
        char c = text[i];
        out[i] = (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
    }
    out[len] = '\0';
    return out;
}

/// @brief Normalize a manually supplied cookie domain into lowercase host form.
/// @details Removes leading dots and horizontal whitespace, trailing whitespace,
///          dots and slashes, and brackets around an IP literal.
/// @param text Domain text to normalize.
/// @return Newly allocated normalized domain, possibly empty, or null on allocation failure.
static char *cookie_strdup_manual_domain(const char *text) {
    char *out = cookie_strdup_lower(text);
    size_t len;
    if (!out)
        return NULL;
    char *trimmed = out;
    while (*trimmed == '.' || *trimmed == ' ' || *trimmed == '\t')
        trimmed++;
    if (trimmed != out)
        memmove(out, trimmed, strlen(trimmed) + 1);
    len = strlen(out);
    while (len > 0 && (out[len - 1] == ' ' || out[len - 1] == '\t' || out[len - 1] == '.' ||
                       out[len - 1] == '/')) {
        out[--len] = '\0';
    }
    if (len >= 2 && out[0] == '[' && out[len - 1] == ']') {
        memmove(out, out + 1, len - 2);
        out[len - 2] = '\0';
    }
    return out;
}

/// @brief Validate a cookie name against the HTTP token character set.
/// @details Rejects empty names, ASCII controls, DEL, whitespace, and RFC token separators so a
///          Set-Cookie header cannot smuggle additional cookie attributes or response syntax.
/// @param name NUL-terminated cookie name.
/// @return 1 if the name is syntactically valid; otherwise 0.
static int cookie_name_is_valid(const char *name) {
    static const char *const separators = "()<>@,;:\\\"/[]?={} \t";
    if (!name || !*name)
        return 0;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        if (*p < 0x21 || *p == 0x7f || strchr(separators, (int)*p))
            return 0;
    }
    return 1;
}

/// @brief Validate a cookie value against the cookie-octet byte ranges.
/// @details Allows the visible ASCII ranges permitted for unquoted cookie values while rejecting
///          quotes, semicolons, commas, backslashes, controls, and non-ASCII bytes that could
///          alter header parsing.
/// @param value NUL-terminated cookie value.
/// @return 1 if the value is safe to store and later emit in a Cookie header; otherwise 0.
static int cookie_value_is_valid(const char *value) {
    if (!value)
        return 0;
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        unsigned char c = *p;
        if (!(c == 0x21 || (c >= 0x23 && c <= 0x2b) || (c >= 0x2d && c <= 0x3a) ||
              (c >= 0x3c && c <= 0x5b) || (c >= 0x5d && c <= 0x7e))) {
            return 0;
        }
    }
    return 1;
}

/// @brief Validate a cookie Path attribute before storing it in the jar.
/// @details Requires an absolute path and rejects controls, DEL, and semicolons so path matching
///          cannot be confused with additional Set-Cookie attributes.
/// @param path NUL-terminated path attribute.
/// @return 1 when @p path is a safe cookie path; otherwise 0.
static int cookie_path_is_valid(const char *path) {
    if (!path || path[0] != '/')
        return 0;
    for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
        if (*p < 0x20 || *p == 0x7f || *p == ';')
            return 0;
    }
    return 1;
}

/// @brief Validate one DNS label from a cookie Domain attribute.
/// @details Labels must be 1..63 bytes, alphanumeric or hyphen, and may not begin or end with a
///          hyphen. The caller supplies lowercase text after leading/trailing dots are stripped.
/// @param start Pointer to the first byte of the label.
/// @param len Number of bytes in the label.
/// @return 1 if the label is valid for cookie domain matching; otherwise 0.
static int cookie_domain_label_is_valid(const char *start, size_t len) {
    if (len == 0 || len > 63 || start[0] == '-' || start[len - 1] == '-')
        return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)start[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
            return 0;
    }
    return 1;
}

/// @brief Validate the full normalized cookie Domain attribute.
/// @details Enforces DNS length and label syntax before the domain is compared with the request
///          host. IP literals and public-suffix-like domains are handled by separate checks.
/// @param domain Lowercase, NUL-terminated domain string.
/// @return 1 if the domain is syntactically valid; otherwise 0.
static int cookie_domain_is_valid(const char *domain) {
    const char *label = domain;
    size_t total_len;
    if (!domain || !*domain)
        return 0;
    total_len = strlen(domain);
    if (total_len > 253 || domain[0] == '.' || domain[total_len - 1] == '.')
        return 0;
    for (const char *p = domain;; p++) {
        if (*p == '.' || *p == '\0') {
            if (!cookie_domain_label_is_valid(label, (size_t)(p - label)))
                return 0;
            if (*p == '\0')
                break;
            label = p + 1;
        }
    }
    return 1;
}

/// @brief Reject domains that are too broad to be accepted into the local cookie jar.
/// @details This is a conservative built-in deny list for common registry-controlled suffixes,
///          plus a no-dot guard. It is not a complete Public Suffix List implementation, but it
///          prevents obvious super-cookie scopes when no PSL dependency is available.
/// @param domain Lowercase, normalized cookie domain.
/// @return 1 if the domain should be treated as public-suffix-like; otherwise 0.
static int cookie_domain_is_public_suffix_like(const char *domain) {
    static const char *const public_suffixes[] = {"ac.uk",
                                                  "appspot.com",
                                                  "blogspot.com",
                                                  "cloudfront.net",
                                                  "co.jp",
                                                  "co.uk",
                                                  "com",
                                                  "com.au",
                                                  "com.br",
                                                  "com.cn",
                                                  "com.mx",
                                                  "com.sg",
                                                  "de",
                                                  "edu",
                                                  "fr",
                                                  "github.io",
                                                  "gov",
                                                  "herokuapp.com",
                                                  "io",
                                                  "jp",
                                                  "net",
                                                  "org",
                                                  "pages.dev",
                                                  "ru",
                                                  "s3.amazonaws.com",
                                                  "uk",
                                                  "us",
                                                  "vercel.app"};
    if (!domain || !strchr(domain, '.'))
        return 1;
    for (size_t i = 0; i < sizeof(public_suffixes) / sizeof(public_suffixes[0]); i++) {
        if (strcasecmp(domain, public_suffixes[i]) == 0)
            return 1;
    }
    return 0;
}

/// @brief Return whether a civil year is a leap year in the Gregorian calendar.
/// @param year Four-digit year.
/// @return Nonzero for leap years.
static int cookie_is_leap_year(int year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

/// @brief Return the valid day count for a cookie expiry month.
/// @param year Four-digit year used for February leap-year handling.
/// @param month_index Zero-based month index, where 0 is January.
/// @return Days in the month, or 0 for an invalid index.
static int cookie_days_in_month(int year, int month_index) {
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month_index < 0 || month_index >= 12)
        return 0;
    if (month_index == 1 && cookie_is_leap_year(year))
        return 29;
    return days[month_index];
}

/// @brief Parse a SameSite attribute value into the cookie jar enum.
/// @details Unknown or empty values intentionally map to UNSPECIFIED so callers can ignore them
///          while still enforcing the Secure requirement for explicit SameSite=None.
/// @param value Attribute value following SameSite=, or NULL.
/// @return One of HTTP_COOKIE_SAMESITE_*.
static int cookie_parse_same_site(const char *value) {
    if (!value || !*value)
        return HTTP_COOKIE_SAMESITE_UNSPECIFIED;
    if (strcasecmp(value, "Lax") == 0)
        return HTTP_COOKIE_SAMESITE_LAX;
    if (strcasecmp(value, "Strict") == 0)
        return HTTP_COOKIE_SAMESITE_STRICT;
    if (strcasecmp(value, "None") == 0)
        return HTTP_COOKIE_SAMESITE_NONE;
    return HTTP_COOKIE_SAMESITE_UNSPECIFIED;
}

/// @brief Derive RFC-style default cookie scope from a request path.
/// @param request_path Request path, or null.
/// @return Newly allocated directory prefix, falling back to `/`, or null on allocation failure.
static char *cookie_default_path(const char *request_path) {
    if (!request_path || request_path[0] != '/')
        return strdup("/");
    const char *last_slash = strrchr(request_path, '/');
    if (!last_slash || last_slash == request_path)
        return strdup("/");
    size_t len = (size_t)(last_slash - request_path);
    char *out = (char *)malloc(len + 1);
    if (!out)
        return NULL;
    memcpy(out, request_path, len);
    out[len] = '\0';
    return out;
}

/// @brief Map a three-letter English month abbreviation to a zero-based index.
/// @param month Text whose first three bytes contain the month abbreviation.
/// @return Month index from 0 through 11, or -1 when no abbreviation matches.
static int cookie_month_index(const char *month) {
    static const char *const kMonths[] = {
        "Jan",
        "Feb",
        "Mar",
        "Apr",
        "May",
        "Jun",
        "Jul",
        "Aug",
        "Sep",
        "Oct",
        "Nov",
        "Dec",
    };
    for (int i = 0; i < 12; i++) {
        if (strncasecmp(month, kMonths[i], 3) == 0)
            return i;
    }
    return -1;
}

/// @brief Parse the supported IMF-fixdate-style cookie Expires value.
/// @details Validates civil date and time ranges, permits leap-second value 60,
///          and converts the result without depending on local timezone state.
/// @param text Null-terminated expiry text ending in `GMT`.
/// @param out_epoch Receives seconds since the Unix epoch.
/// @return 0 on success; -1 when the text or civil fields are invalid.
static int parse_cookie_expires(const char *text, int64_t *out_epoch) {
    char weekday[4] = {0};
    char month[4] = {0};
    int day = 0, year = 0, hour = 0, minute = 0, second = 0;

    if (sscanf(text,
               " %3[^,], %d %3s %d %d:%d:%d GMT",
               weekday,
               &day,
               month,
               &year,
               &hour,
               &minute,
               &second) != 7) {
        return -1;
    }

    int month_index = cookie_month_index(month);
    if (month_index < 0)
        return -1;

    if (day < 1 || day > cookie_days_in_month(year, month_index) || hour < 0 || hour > 23 ||
        minute < 0 || minute > 59 || second < 0 || second > 60 || year < 1601)
        return -1;

    *out_epoch =
        rt_network_days_from_civil(year, (unsigned)(month_index + 1), (unsigned)day) * 86400 +
        hour * 3600 + minute * 60 + second;
    return 0;
}

/// @brief Parse a decimal cookie Max-Age value with saturating overflow behavior.
/// @param text Signed decimal text; a leading minus is accepted but a leading plus is not.
/// @param out_delta Receives the signed lifetime in seconds, saturated to the `int64_t` limits.
/// @return 0 on success; -1 for null, empty, or syntactically invalid input.
static int parse_cookie_max_age(const char *text, int64_t *out_delta) {
    const char *p = text;
    int negative = 0;
    uint64_t value = 0;
    uint64_t limit = 0;

    if (!text || !*text || !out_delta)
        return -1;
    if (*p == '-') {
        negative = 1;
        p++;
        if (!*p)
            return -1;
    } else if (*p == '+') {
        return -1;
    }

    limit = negative ? ((uint64_t)INT64_MAX + 1u) : (uint64_t)INT64_MAX;
    for (; *p; p++) {
        uint64_t digit = 0;
        if (*p < '0' || *p > '9')
            return -1;
        digit = (uint64_t)(*p - '0');
        if (value > (limit - digit) / 10u) {
            *out_delta = negative ? INT64_MIN : INT64_MAX;
            return 0;
        }
        value = value * 10u + digit;
    }

    if (negative) {
        if (value == (uint64_t)INT64_MAX + 1u)
            *out_delta = INT64_MIN;
        else
            *out_delta = -(int64_t)value;
    } else {
        *out_delta = (int64_t)value;
    }
    return 0;
}

/// @brief Test a cookie's host-only or domain scope against a request host.
/// @param cookie Cookie containing normalized domain and host-only state.
/// @param host Request host to compare case-insensitively.
/// @return Nonzero for an exact host-only match or a label-boundary domain suffix match.
static int cookie_domain_matches(const rt_http_cookie *cookie, const char *host) {
    size_t host_len, domain_len;
    if (!cookie || !cookie->domain || !host)
        return 0;

    if (cookie->host_only)
        return strcasecmp(cookie->domain, host) == 0;

    if (strcasecmp(cookie->domain, host) == 0)
        return 1;

    host_len = strlen(host);
    domain_len = strlen(cookie->domain);
    if (host_len <= domain_len)
        return 0;
    return host[host_len - domain_len - 1] == '.' &&
           strcasecmp(host + host_len - domain_len, cookie->domain) == 0;
}

/// @brief Apply cookie path-prefix and segment-boundary matching to a request path.
/// @param cookie Cookie whose empty path defaults to `/`.
/// @param request_path Request path whose empty value defaults to `/`.
/// @return Nonzero when the request path is within the cookie's path scope.
static int cookie_path_matches(const rt_http_cookie *cookie, const char *request_path) {
    const char *cookie_path = (cookie && cookie->path && *cookie->path) ? cookie->path : "/";
    const char *path = (request_path && *request_path) ? request_path : "/";
    size_t cookie_len = strlen(cookie_path);

    if (strncmp(path, cookie_path, cookie_len) != 0)
        return 0;
    if (cookie_len == 1)
        return 1;
    return path[cookie_len] == '\0' || path[cookie_len] == '/' ||
           cookie_path[cookie_len - 1] == '/';
}

/// @brief Determine whether a persistent cookie has reached its expiry instant.
/// @param cookie Cookie to inspect.
/// @param now Current Unix time in seconds.
/// @return Nonzero only for a persistent cookie whose expiry is not later than @p now.
static int cookie_is_expired(const rt_http_cookie *cookie, int64_t now) {
    return cookie && cookie->persistent && cookie->expires_at <= now;
}

/// @brief Remove all expired cookies while the caller holds the client mutex.
/// @param c Client whose native cookie list is exclusively locked.
static void purge_expired_cookies_locked(rt_http_client_impl *c) {
    rt_http_cookie **link = &c->cookies;
    const int64_t now = cookie_now_seconds();
    while (*link) {
        if (cookie_is_expired(*link, now)) {
            rt_http_cookie *dead = *link;
            *link = dead->next;
            dead->next = NULL;
            free_cookie_list(dead);
        } else {
            link = &(*link)->next;
        }
    }
}

/// @brief Replace one cookie identity while the caller holds the client mutex.
/// @details Cookie identity combines case-sensitive name and path with a
///          case-insensitive domain. An already expired persistent cookie
///          performs deletion without inserting a replacement.
/// @param c Client whose native cookie list is exclusively locked.
/// @param incoming Owned cookie record consumed by this function.
static void replace_cookie_locked(rt_http_client_impl *c, rt_http_cookie *incoming) {
    rt_http_cookie **link = &c->cookies;
    while (*link) {
        rt_http_cookie *cookie = *link;
        // Cookie names are case-sensitive (RFC 6265): SID and sid coexist.
        // Domains are DNS names and stay case-insensitive.
        if (strcmp(cookie->name, incoming->name) == 0 &&
            strcasecmp(cookie->domain, incoming->domain) == 0 &&
            strcmp(cookie->path, incoming->path) == 0) {
            *link = cookie->next;
            cookie->next = NULL;
            free_cookie_list(cookie);
            break;
        }
        link = &cookie->next;
    }

    // Empty values are valid cookies (RFC 6265); only an already-expired
    // persistent cookie means "delete".
    if (incoming->persistent && incoming->expires_at <= cookie_now_seconds()) {
        free_cookie_list(incoming);
        return;
    }

    incoming->next = c->cookies;
    c->cookies = incoming;
}

/// @brief Owned state for Cookie request-header construction.
/// @details Heap storage keeps cleanup fields defined across `longjmp`. URL
///          components and final header Strings are caller-owned; the native
///          header buffer is built while the jar mutex excludes mutation.
typedef struct {
    void *parsed_url;
    rt_string host;
    rt_string path;
    rt_string scheme;
    rt_string header_name;
    rt_string header_value;
    char *native_header;
    int mutex_locked;
} http_cookie_header_state;

/// @brief Release partial Cookie-header construction state.
/// @param state State to consume; NULL is a no-op.
static void http_cookie_header_state_release(http_cookie_header_state *state) {
    if (!state)
        return;
    free(state->native_header);
    http_client_release_obj((void *)state->header_value);
    http_client_release_obj((void *)state->header_name);
    http_client_release_obj((void *)state->scheme);
    http_client_release_obj((void *)state->path);
    http_client_release_obj((void *)state->host);
    http_client_release_obj(state->parsed_url);
    memset(state, 0, sizeof(*state));
}

/// @brief Release and destroy Cookie-header construction state.
/// @param state State to consume; NULL is a no-op.
static void http_cookie_header_state_destroy(http_cookie_header_state *state) {
    if (!state)
        return;
    http_cookie_header_state_release(state);
    free(state);
}

/// @brief Apply all matching session cookies to a newly built HttpReq.
/// @details Matching and exact sizing occur under the client mutex. Output is
///          written directly into one native allocation, avoiding repeated
///          formatting and intermediate Strings. Managed URL/header mutations
///          run under recovery; a trap unlocks the jar and releases every
///          partial resource before propagation.
/// @param c Valid HttpClient session.
/// @param req Newly allocated HttpReq to mutate.
/// @param url Current redirect-hop URL.
/// @return Nonzero after applying the header or finding no matching cookies;
///         zero after a returning trap hook and local cleanup.
static int apply_cookie_header(rt_http_client_impl *c, void *req, rt_string url) {
    http_cookie_header_state *const state = (http_cookie_header_state *)calloc(1, sizeof(*state));
    if (!state) {
        rt_trap("HttpClient: Cookie header state allocation failed");
        return 0;
    }

    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[256];
        http_client_save_trap_error(
            saved_error, sizeof(saved_error), "HttpClient: Cookie header construction failed");
        rt_trap_clear_recovery();
        if (state->mutex_locked)
            HTTP_CLIENT_MUTEX_UNLOCK(&c->lock);
        http_cookie_header_state_destroy(state);
        rt_trap(saved_error);
        return 0;
    }

    state->parsed_url = rt_url_parse(url);
    if (!state->parsed_url) {
        rt_trap("HttpClient: Cookie URL parsing failed");
        goto returning_trap;
    }
    state->host = rt_url_host(state->parsed_url);
    if (!state->host) {
        rt_trap("HttpClient: Cookie URL host allocation failed");
        goto returning_trap;
    }
    state->path = rt_url_path(state->parsed_url);
    if (!state->path) {
        rt_trap("HttpClient: Cookie URL path allocation failed");
        goto returning_trap;
    }
    state->scheme = rt_url_scheme(state->parsed_url);
    if (!state->scheme) {
        rt_trap("HttpClient: Cookie URL scheme allocation failed");
        goto returning_trap;
    }
    const char *host = rt_string_cstr(state->host);
    const char *path = rt_string_cstr(state->path);
    const char *scheme = rt_string_cstr(state->scheme);
    const int is_secure = scheme && strcasecmp(scheme, "https") == 0;
    if (!host || !host[0]) {
        rt_trap_clear_recovery();
        http_cookie_header_state_destroy(state);
        return 1;
    }

    size_t total_len = 0;
    size_t cookie_count = 0;
    HTTP_CLIENT_MUTEX_LOCK(&c->lock);
    state->mutex_locked = 1;
    purge_expired_cookies_locked(c);
    for (rt_http_cookie *cookie = c->cookies; cookie; cookie = cookie->next) {
        if ((cookie->secure && !is_secure) || !cookie_domain_matches(cookie, host) ||
            !cookie_path_matches(cookie, path)) {
            continue;
        }
        size_t name_len = strlen(cookie->name);
        size_t value_len = strlen(cookie->value);
        if (name_len > SIZE_MAX - value_len - 1u) {
            rt_trap("HttpClient: Cookie header size overflow");
            goto returning_trap;
        }
        size_t item_len = name_len + 1u + value_len;
        size_t separator_len = cookie_count > 0 ? 2u : 0u;
        if (total_len > SIZE_MAX - separator_len ||
            total_len + separator_len > SIZE_MAX - item_len) {
            rt_trap("HttpClient: Cookie header size overflow");
            goto returning_trap;
        }
        total_len += separator_len + item_len;
        cookie_count++;
    }

    if (cookie_count > 0) {
        if (total_len == SIZE_MAX) {
            rt_trap("HttpClient: Cookie header size overflow");
            goto returning_trap;
        }
        state->native_header = (char *)malloc(total_len + 1u);
        if (!state->native_header) {
            rt_trap("HttpClient: Cookie header allocation failed");
            goto returning_trap;
        }
        size_t offset = 0;
        size_t emitted = 0;
        for (rt_http_cookie *cookie = c->cookies; cookie; cookie = cookie->next) {
            if ((cookie->secure && !is_secure) || !cookie_domain_matches(cookie, host) ||
                !cookie_path_matches(cookie, path)) {
                continue;
            }
            if (emitted > 0) {
                memcpy(state->native_header + offset, "; ", 2u);
                offset += 2u;
            }
            size_t name_len = strlen(cookie->name);
            size_t value_len = strlen(cookie->value);
            memcpy(state->native_header + offset, cookie->name, name_len);
            offset += name_len;
            state->native_header[offset++] = '=';
            memcpy(state->native_header + offset, cookie->value, value_len);
            offset += value_len;
            emitted++;
        }
        state->native_header[offset] = '\0';
    }
    HTTP_CLIENT_MUTEX_UNLOCK(&c->lock);
    state->mutex_locked = 0;

    if (state->native_header) {
        state->header_name = rt_const_cstr("Cookie");
        state->header_value = rt_string_from_bytes(state->native_header, total_len);
        if (!state->header_name || !state->header_value ||
            !rt_http_req_set_header(req, state->header_name, state->header_value)) {
            rt_trap("HttpClient: Cookie request header allocation failed");
            goto returning_trap;
        }
    }

    rt_trap_clear_recovery();
    http_cookie_header_state_destroy(state);
    return 1;

returning_trap:
    rt_trap_clear_recovery();
    if (state->mutex_locked)
        HTTP_CLIENT_MUTEX_UNLOCK(&c->lock);
    http_cookie_header_state_destroy(state);
    return 0;
}

/// @brief Result of staging one response Set-Cookie field.
typedef enum {
    HTTP_COOKIE_STAGE_IGNORED = 0, ///< Invalid or policy-rejected field; no error.
    HTTP_COOKIE_STAGE_READY = 1,   ///< @c out_cookie owns a complete cookie.
    HTTP_COOKIE_STAGE_OOM = -1     ///< Native staging allocation failed.
} http_cookie_stage_result;

/// @brief Parse and validate one Set-Cookie field into detached native storage.
/// @details The function performs no managed allocation, takes no client lock,
///          and never mutates the cookie jar. Invalid syntax or cookie policy
///          rejection returns @ref HTTP_COOKIE_STAGE_IGNORED. Every native
///          allocation failure returns @ref HTTP_COOKIE_STAGE_OOM, allowing the
///          caller to abandon the complete response-cookie transaction instead
///          of silently publishing a partial jar update.
/// @param host Request host used for Domain validation.
/// @param request_path Request path used to derive the default cookie path.
/// @param is_secure Nonzero when the response arrived over HTTPS.
/// @param host_is_ip Nonzero when @p host is an IP literal.
/// @param line NUL-terminated Set-Cookie field value.
/// @param out_cookie Receives a detached cookie on READY; set to NULL otherwise.
/// @return A value from @ref http_cookie_stage_result.
static int stage_cookie_line(const char *host,
                             const char *request_path,
                             int is_secure,
                             int host_is_ip,
                             const char *line,
                             rt_http_cookie **out_cookie) {
    const char *cookie_end;
    const char *eq;
    size_t cookie_len;
    size_t name_len;
    size_t value_len;
    rt_http_cookie *cookie;
    int max_age_seen = 0;

    if (out_cookie)
        *out_cookie = NULL;
    if (!out_cookie || !host || !line || !*line)
        return HTTP_COOKIE_STAGE_IGNORED;

    cookie_end = strchr(line, ';');
    cookie_len = cookie_end ? (size_t)(cookie_end - line) : strlen(line);
    eq = memchr(line, '=', cookie_len);
    if (!eq || eq == line)
        return HTTP_COOKIE_STAGE_IGNORED;

    name_len = (size_t)(eq - line);
    value_len = cookie_len - name_len - 1;
    if (name_len == 0)
        return HTTP_COOKIE_STAGE_IGNORED;

    cookie = (rt_http_cookie *)calloc(1, sizeof(*cookie));
    if (!cookie)
        return HTTP_COOKIE_STAGE_OOM;

    cookie->name = cookie_strdup_range_trim(line, name_len);
    cookie->value = cookie_strdup_range_trim(eq + 1, value_len);
    cookie->domain = cookie_strdup_lower(host);
    cookie->path = cookie_default_path(request_path);
    cookie->host_only = 1;
    if (!cookie->name || !cookie->value || !cookie->domain || !cookie->path) {
        free_cookie_list(cookie);
        return HTTP_COOKIE_STAGE_OOM;
    }
    if (!cookie_name_is_valid(cookie->name) || !cookie_value_is_valid(cookie->value)) {
        free_cookie_list(cookie);
        return HTTP_COOKIE_STAGE_IGNORED;
    }

    const char *attr = cookie_end ? cookie_end + 1 : NULL;
    while (attr && *attr) {
        const char *attr_end;
        const char *attr_eq;
        size_t attr_len;
        char *attr_name;
        char *attr_value = NULL;

        while (*attr == ' ' || *attr == '\t' || *attr == ';')
            attr++;
        if (!*attr)
            break;

        attr_end = strchr(attr, ';');
        attr_len = attr_end ? (size_t)(attr_end - attr) : strlen(attr);
        attr_eq = memchr(attr, '=', attr_len);
        if (attr_eq) {
            attr_name = cookie_strdup_range_trim(attr, (size_t)(attr_eq - attr));
            attr_value =
                cookie_strdup_range_trim(attr_eq + 1, attr_len - (size_t)(attr_eq - attr) - 1);
        } else {
            attr_name = cookie_strdup_range_trim(attr, attr_len);
        }

        if (!attr_name || (attr_eq && !attr_value)) {
            free(attr_value);
            free(attr_name);
            free_cookie_list(cookie);
            return HTTP_COOKIE_STAGE_OOM;
        }

        if (strcasecmp(attr_name, "Domain") == 0 && attr_value && *attr_value) {
            char *domain = cookie_strdup_manual_domain(attr_value);
            if (!domain) {
                free(attr_value);
                free(attr_name);
                free_cookie_list(cookie);
                return HTTP_COOKIE_STAGE_OOM;
            }
            free(cookie->domain);
            cookie->domain = domain;
            cookie->host_only = 0;
        } else if (strcasecmp(attr_name, "Path") == 0 && attr_value && attr_value[0] == '/') {
            char *path = strdup(attr_value);
            if (!path) {
                free(attr_value);
                free(attr_name);
                free_cookie_list(cookie);
                return HTTP_COOKIE_STAGE_OOM;
            }
            free(cookie->path);
            cookie->path = path;
        } else if (strcasecmp(attr_name, "Secure") == 0) {
            cookie->secure = 1;
        } else if (strcasecmp(attr_name, "HttpOnly") == 0) {
            cookie->http_only = 1;
        } else if (strcasecmp(attr_name, "SameSite") == 0 && attr_value) {
            cookie->same_site = (int8_t)cookie_parse_same_site(attr_value);
        } else if (strcasecmp(attr_name, "Max-Age") == 0 && attr_value) {
            int64_t max_age = 0;
            if (parse_cookie_max_age(attr_value, &max_age) == 0) {
                int64_t now = cookie_now_seconds();
                cookie->persistent = 1;
                if (max_age <= 0) {
                    cookie->expires_at = now - 1;
                } else if (max_age > INT64_MAX - now) {
                    cookie->expires_at = INT64_MAX;
                } else {
                    cookie->expires_at = now + max_age;
                }
                max_age_seen = 1;
            }
        } else if (strcasecmp(attr_name, "Expires") == 0 && attr_value && !max_age_seen) {
            int64_t expires_at = 0;
            if (parse_cookie_expires(attr_value, &expires_at) == 0) {
                cookie->persistent = 1;
                cookie->expires_at = expires_at;
            }
        }

        free(attr_value);
        free(attr_name);
        attr = attr_end ? attr_end + 1 : NULL;
    }

    if (cookie->secure && !is_secure) {
        free_cookie_list(cookie);
        return HTTP_COOKIE_STAGE_IGNORED;
    }
    if ((cookie->same_site == HTTP_COOKIE_SAMESITE_NONE && !cookie->secure) ||
        !cookie_path_is_valid(cookie->path)) {
        free_cookie_list(cookie);
        return HTTP_COOKIE_STAGE_IGNORED;
    }

    if (!cookie->host_only) {
        if (!cookie_domain_is_valid(cookie->domain) ||
            cookie_domain_is_public_suffix_like(cookie->domain) ||
            !cookie_domain_matches(cookie, host) || host_is_ip == 1) {
            free_cookie_list(cookie);
            return HTTP_COOKIE_STAGE_IGNORED;
        }
    }

    *out_cookie = cookie;
    return HTTP_COOKIE_STAGE_READY;
}

/// @brief Owned staging for response-cookie extraction.
/// @details The current native cookie line and mutex flag live on the heap so a
///          managed accessor or validation trap can unlock and clean them after
///          `longjmp` without reading indeterminate automatic state.
typedef struct {
    void *parsed_url;
    rt_string host;
    rt_string path;
    rt_string scheme;
    rt_string header_name;
    rt_string cookie_header;
    char *entry;
    rt_http_cookie *staged_cookies;
    rt_http_cookie *staged_tail;
    int mutex_locked;
} http_response_cookie_state;

/// @brief Release partial response-cookie extraction state.
/// @param state State to consume; NULL is a no-op.
static void http_response_cookie_state_release(http_response_cookie_state *state) {
    if (!state)
        return;
    free(state->entry);
    free_cookie_list(state->staged_cookies);
    http_client_release_obj((void *)state->cookie_header);
    http_client_release_obj((void *)state->header_name);
    http_client_release_obj((void *)state->scheme);
    http_client_release_obj((void *)state->path);
    http_client_release_obj((void *)state->host);
    http_client_release_obj(state->parsed_url);
    memset(state, 0, sizeof(*state));
}

/// @brief Release and destroy response-cookie extraction state.
/// @param state State to consume; NULL is a no-op.
static void http_response_cookie_state_destroy(http_response_cookie_state *state) {
    if (!state)
        return;
    http_response_cookie_state_release(state);
    free(state);
}

/// @brief Parse and transactionally merge Set-Cookie response fields.
/// @details URL/accessor allocations occur first. Every joined Set-Cookie field
///          is copied, parsed, and validated into detached native staging
///          without holding the jar mutex. Only after every allocation succeeds
///          are staged cookies merged under one allocation-free critical
///          section. Thus native OOM preserves the pre-response jar, managed
///          operations cannot trap while locked, and recovery always releases
///          the complete unpublished staging list.
/// @param c Valid HttpClient session.
/// @param res Stable HttpRes for the completed hop.
/// @param url URL that produced @p res.
/// @return Nonzero after staging and committing every cookie; zero after a
///         returning trap hook and local cleanup.
static int store_response_cookies(rt_http_client_impl *c, void *res, rt_string url) {
    http_response_cookie_state *const state =
        (http_response_cookie_state *)calloc(1, sizeof(*state));
    if (!state) {
        rt_trap("HttpClient: response cookie state allocation failed");
        return 0;
    }

    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[256];
        http_client_save_trap_error(
            saved_error, sizeof(saved_error), "HttpClient: response cookie storage failed");
        rt_trap_clear_recovery();
        if (state->mutex_locked)
            HTTP_CLIENT_MUTEX_UNLOCK(&c->lock);
        http_response_cookie_state_destroy(state);
        rt_trap(saved_error);
        return 0;
    }

    state->parsed_url = rt_url_parse(url);
    if (!state->parsed_url) {
        rt_trap("HttpClient: response Cookie URL parsing failed");
        goto returning_trap;
    }
    state->host = rt_url_host(state->parsed_url);
    if (!state->host) {
        rt_trap("HttpClient: response Cookie host allocation failed");
        goto returning_trap;
    }
    state->path = rt_url_path(state->parsed_url);
    if (!state->path) {
        rt_trap("HttpClient: response Cookie path allocation failed");
        goto returning_trap;
    }
    state->scheme = rt_url_scheme(state->parsed_url);
    if (!state->scheme) {
        rt_trap("HttpClient: response Cookie scheme allocation failed");
        goto returning_trap;
    }
    const char *host = rt_string_cstr(state->host);
    const char *path = rt_string_cstr(state->path);
    const char *scheme = rt_string_cstr(state->scheme);
    if (!host || !host[0]) {
        rt_trap_clear_recovery();
        http_response_cookie_state_destroy(state);
        return 1;
    }
    int host_is_ip = rt_dns_is_ip(state->host);
    int is_secure = scheme && strcasecmp(scheme, "https") == 0;
    state->header_name = rt_const_cstr("set-cookie");
    if (!state->header_name) {
        rt_trap("HttpClient: response Cookie header allocation failed");
        goto returning_trap;
    }
    state->cookie_header = rt_http_res_header(res, state->header_name);
    if (!state->cookie_header) {
        rt_trap("HttpClient: response Cookie lookup failed");
        goto returning_trap;
    }
    const char *cookies = rt_string_cstr(state->cookie_header);
    if (cookies && cookies[0]) {
        const char *line = cookies;
        while (*line) {
            const char *end_lf = strchr(line, '\n');
            const char *end_separator = strchr(line, HTTP_SET_COOKIE_JOIN_SEPARATOR);
            const char *end = NULL;
            if (end_lf && end_separator)
                end = end_lf < end_separator ? end_lf : end_separator;
            else
                end = end_lf ? end_lf : end_separator;
            size_t length = end ? (size_t)(end - line) : strlen(line);
            if (length == SIZE_MAX) {
                rt_trap("HttpClient: Set-Cookie field is too large");
                goto returning_trap;
            }
            state->entry = (char *)malloc(length + 1u);
            if (!state->entry) {
                rt_trap("HttpClient: Set-Cookie staging allocation failed");
                goto returning_trap;
            }
            memcpy(state->entry, line, length);
            state->entry[length] = '\0';
            rt_http_cookie *candidate = NULL;
            int stage_result = stage_cookie_line(host,
                                                 path && path[0] ? path : "/",
                                                 is_secure,
                                                 host_is_ip,
                                                 state->entry,
                                                 &candidate);
            if (stage_result == HTTP_COOKIE_STAGE_OOM) {
                rt_trap("HttpClient: Set-Cookie allocation failed");
                goto returning_trap;
            }
            if (stage_result == HTTP_COOKIE_STAGE_READY) {
                if (state->staged_tail)
                    state->staged_tail->next = candidate;
                else
                    state->staged_cookies = candidate;
                state->staged_tail = candidate;
            }
            free(state->entry);
            state->entry = NULL;
            if (!end)
                break;
            line = end + 1;
        }

        HTTP_CLIENT_MUTEX_LOCK(&c->lock);
        state->mutex_locked = 1;
        while (state->staged_cookies) {
            rt_http_cookie *candidate = state->staged_cookies;
            state->staged_cookies = candidate->next;
            candidate->next = NULL;
            replace_cookie_locked(c, candidate);
        }
        state->staged_tail = NULL;
        HTTP_CLIENT_MUTEX_UNLOCK(&c->lock);
        state->mutex_locked = 0;
    }

    rt_trap_clear_recovery();
    http_response_cookie_state_destroy(state);
    return 1;

returning_trap:
    rt_trap_clear_recovery();
    if (state->mutex_locked)
        HTTP_CLIENT_MUTEX_UNLOCK(&c->lock);
    http_response_cookie_state_destroy(state);
    return 0;
}

/// @brief Heap-backed ownership state for one session request and its redirect chain.
/// @details A native heap allocation is used because C permits automatic values modified after
///          `setjmp` to become indeterminate after `longjmp`. Every managed reference acquired by
///          the request loop is recorded here before another trapping operation can occur. This
///          lets one outer recovery frame release the current request, intermediate response,
///          copied URL/body, and redirect temporaries exactly once.
typedef struct {
    rt_string current_url;
    rt_string current_body;
    rt_string method_string;
    rt_string location_name;
    rt_string location;
    rt_string next_url;
    void *request;
    void *response;
    const char *current_method;
    int64_t redirects_left;
    int8_t follow_redirects;
    int8_t allow_sensitive_defaults;
} http_client_request_transaction;

/// @brief Release every owned object in an HttpClient request transaction.
/// @details Fields are cleared after release, making the helper safe for both trap recovery and
///          normal completion after the final response has been detached. The method pointer and
///          scalar redirect policy are borrowed/value state and require no release.
/// @param transaction Transaction to empty; may be NULL.
static void http_client_request_transaction_release(http_client_request_transaction *transaction) {
    if (!transaction)
        return;
    http_client_release_obj(transaction->request);
    http_client_release_obj(transaction->response);
    http_client_release_obj((void *)transaction->next_url);
    http_client_release_obj((void *)transaction->location);
    http_client_release_obj((void *)transaction->location_name);
    http_client_release_obj((void *)transaction->method_string);
    http_client_release_obj((void *)transaction->current_body);
    http_client_release_obj((void *)transaction->current_url);
    memset(transaction, 0, sizeof(*transaction));
}

/// @brief Release and destroy one request transaction.
/// @param transaction Transaction to consume; NULL is a no-op.
static void http_client_request_transaction_destroy(http_client_request_transaction *transaction) {
    if (!transaction)
        return;
    http_client_request_transaction_release(transaction);
    free(transaction);
}

/// @brief Execute one session request, including client-managed redirect handling.
/// @details The input URL and optional body are copied by exact runtime-string length, then all
///          request construction, default/header/cookie application, transport, cookie capture,
///          and redirect processing run beneath one recovery frame. Cross-origin redirects stop
///          forwarding sensitive defaults. Status 303, and 301/302 following POST, rewrite the
///          next hop to GET and discard the body; 307/308 preserve both. A redirect response is
///          returned unchanged when following is disabled or the configured limit is exhausted.
///          On any trap, every transaction-owned object is released before the original network
///          category and code are re-raised. This remains correct when an embedding trap hook
///          returns instead of terminating execution.
/// @param c Valid initialized HttpClient session.
/// @param method Borrowed NUL-terminated method literal used for the first hop.
/// @param url Valid runtime URL string; copied before use.
/// @param body Optional runtime string body; copied before use and may contain embedded NUL bytes.
/// @return Caller-owned final HttpRes, or NULL after a returning trap hook.
static void *do_request(rt_http_client_impl *c, const char *method, rt_string url, rt_string body) {
    http_client_request_transaction *const transaction =
        (http_client_request_transaction *)calloc(1, sizeof(*transaction));
    if (!transaction) {
        rt_trap("HttpClient: request transaction allocation failed");
        return NULL;
    }
    transaction->current_method = method;
    transaction->allow_sensitive_defaults = 1;

    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[512];
        int saved_net_code = rt_trap_get_net_code();
        http_client_save_trap_error(saved_error, sizeof(saved_error), "HttpClient: request failed");
        rt_trap_clear_recovery();
        http_client_request_transaction_destroy(transaction);
        if (saved_net_code)
            rt_trap_net(saved_error, saved_net_code);
        else
            rt_trap(saved_error);
        return NULL;
    }

    if (!method || !method[0] || !rt_string_is_handle(url)) {
        rt_trap_net("HttpClient: invalid URL", Err_InvalidUrl);
        goto returning_trap;
    }
    const char *url_bytes = rt_string_cstr(url);
    int64_t url_len64 = rt_str_len(url);
    if (!url_bytes || url_len64 <= 0 || (uint64_t)url_len64 > (uint64_t)SIZE_MAX ||
        memchr(url_bytes, '\0', (size_t)url_len64) != NULL) {
        rt_trap_net("HttpClient: invalid URL", Err_InvalidUrl);
        goto returning_trap;
    }
    transaction->current_url = rt_string_from_bytes(url_bytes, (size_t)url_len64);
    if (!transaction->current_url) {
        rt_trap("HttpClient: URL copy allocation failed");
        goto returning_trap;
    }

    if (body) {
        if (!rt_string_is_handle(body)) {
            rt_trap("HttpClient: invalid body");
            goto returning_trap;
        }
        const char *body_bytes = rt_string_cstr(body);
        int64_t body_len64 = rt_str_len(body);
        if (!body_bytes || body_len64 < 0 || (uint64_t)body_len64 > (uint64_t)SIZE_MAX) {
            rt_trap("HttpClient: invalid body");
            goto returning_trap;
        }
        transaction->current_body = rt_string_from_bytes(body_bytes, (size_t)body_len64);
        if (!transaction->current_body) {
            rt_trap("HttpClient: body copy allocation failed");
            goto returning_trap;
        }
    }

    HTTP_CLIENT_MUTEX_LOCK(&c->lock);
    transaction->follow_redirects = c->follow_redirects ? 1 : 0;
    transaction->redirects_left = transaction->follow_redirects ? c->max_redirects : 0;
    HTTP_CLIENT_MUTEX_UNLOCK(&c->lock);

    for (;;) {
        transaction->method_string =
            rt_string_from_bytes(transaction->current_method, strlen(transaction->current_method));
        if (!transaction->method_string) {
            rt_trap("HttpClient: method allocation failed");
            goto returning_trap;
        }
        transaction->request =
            rt_http_req_new(transaction->method_string, transaction->current_url);
        http_client_release_obj((void *)transaction->method_string);
        transaction->method_string = NULL;
        if (!transaction->request) {
            rt_trap_net("HttpClient: request creation failed", Err_InvalidUrl);
            goto returning_trap;
        }

        if (!apply_defaults(c, transaction->request, transaction->allow_sensitive_defaults) ||
            !apply_cookie_header(c, transaction->request, transaction->current_url)) {
            goto returning_trap;
        }
        if (!rt_http_req_set_max_redirects(transaction->request, 0)) {
            rt_trap("HttpClient: redirect policy application failed");
            goto returning_trap;
        }

        if (transaction->current_body && strcmp(transaction->current_method, "GET") != 0 &&
            strcmp(transaction->current_method, "DELETE") != 0 &&
            !rt_http_req_set_body_str(transaction->request, transaction->current_body)) {
            rt_trap("HttpClient: request body application failed");
            goto returning_trap;
        }

        transaction->response = rt_http_req_send(transaction->request);
        http_client_release_obj(transaction->request);
        transaction->request = NULL;
        if (!transaction->response) {
            rt_trap_net("HttpClient: request failed", Err_NetworkError);
            goto returning_trap;
        }
        if (!store_response_cookies(c, transaction->response, transaction->current_url))
            goto returning_trap;

        int64_t status = rt_http_res_status(transaction->response);
        if (!transaction->follow_redirects || transaction->redirects_left <= 0 ||
            !(status == 301 || status == 302 || status == 303 || status == 307 || status == 308)) {
            break;
        }

        transaction->location_name = rt_const_cstr("location");
        if (!transaction->location_name) {
            rt_trap("HttpClient: redirect header allocation failed");
            goto returning_trap;
        }
        transaction->location =
            rt_http_res_header(transaction->response, transaction->location_name);
        http_client_release_obj((void *)transaction->location_name);
        transaction->location_name = NULL;
        if (!transaction->location) {
            rt_trap("HttpClient: redirect header lookup failed");
            goto returning_trap;
        }
        const char *location_bytes = rt_string_cstr(transaction->location);
        int64_t location_len64 = rt_str_len(transaction->location);
        if (!location_bytes || location_len64 <= 0)
            break;

        transaction->next_url =
            rt_http_resolve_redirect_url(transaction->current_url, transaction->location);
        http_client_release_obj((void *)transaction->location);
        transaction->location = NULL;
        if (!transaction->next_url) {
            rt_trap_net("HttpClient: invalid redirect URL", Err_InvalidUrl);
            goto returning_trap;
        }
        if (!rt_http_url_has_same_origin(transaction->current_url, transaction->next_url))
            transaction->allow_sensitive_defaults = 0;
        http_client_release_obj((void *)transaction->current_url);
        transaction->current_url = transaction->next_url;
        transaction->next_url = NULL;
        transaction->redirects_left--;

        if (status == 303 || ((status == 301 || status == 302) &&
                              strcmp(transaction->current_method, "POST") == 0)) {
            transaction->current_method = "GET";
            http_client_release_obj((void *)transaction->current_body);
            transaction->current_body = NULL;
        }

        http_client_release_obj(transaction->response);
        transaction->response = NULL;
    }

    void *final_response = transaction->response;
    transaction->response = NULL;
    rt_trap_clear_recovery();
    http_client_request_transaction_destroy(transaction);
    return final_response;

returning_trap:
    rt_trap_clear_recovery();
    http_client_request_transaction_destroy(transaction);
    return NULL;
}

//=============================================================================
// Public API
//=============================================================================

/// @brief Construct an HTTP client with sensible defaults: 30s timeout, 5 max redirects, redirect
/// following enabled, empty cookie jar, empty default-headers map. Built on top of `rt_http_req`
/// from `rt_network_http.c` — adds session state (cookies + defaults) and **per-request redirect
/// chasing** (rather than the underlying req's all-or-nothing redirect handling).
/// @return Caller-owned managed HttpClient, or null after construction failure and a returning
///         trap hook.
void *rt_http_client_new(void) {
    rt_http_client_impl *volatile c = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[256];
        http_client_save_trap_error(
            saved_error, sizeof(saved_error), "HttpClient: construction failed");
        rt_trap_clear_recovery();
        http_client_release_obj((void *)c);
        rt_trap(saved_error);
        return NULL;
    }

    c = (rt_http_client_impl *)rt_obj_new_i64(RT_HTTP_CLIENT_CLASS_ID,
                                              (int64_t)sizeof(rt_http_client_impl));
    if (!c) {
        rt_trap_clear_recovery();
        return NULL;
    }
    memset((void *)c, 0, sizeof(*c));
    rt_obj_set_finalizer((void *)c, rt_http_client_finalize);
    if (!HTTP_CLIENT_MUTEX_INIT(&((rt_http_client_impl *)c)->lock))
        rt_trap("HttpClient: mutex initialization failed");
    ((rt_http_client_impl *)c)->lock_initialized = 1;
    ((rt_http_client_impl *)c)->default_headers = rt_map_new();
    if (!((rt_http_client_impl *)c)->default_headers)
        rt_trap("HttpClient: default-header Map allocation failed");
    ((rt_http_client_impl *)c)->timeout_ms = 30000;
    ((rt_http_client_impl *)c)->max_redirects = 5;
    ((rt_http_client_impl *)c)->follow_redirects = 1;
    ((rt_http_client_impl *)c)->pool_size = 8;
    ((rt_http_client_impl *)c)->keep_alive = 1;
    ((rt_http_client_impl *)c)->connection_pool =
        rt_http_conn_pool_new(((rt_http_client_impl *)c)->pool_size);
    if (!((rt_http_client_impl *)c)->connection_pool)
        rt_trap("HttpClient: connection pool allocation failed");
    rt_trap_clear_recovery();
    return (void *)c;
}

/// @brief Send a `GET` to `url`. Applies default headers + cookies; auto-follows redirects up to
/// `max_redirects`. Returns an HttpRes for the FINAL response after any redirects.
/// @param obj HttpClient receiver.
/// @param url Absolute HTTP or HTTPS URL.
/// @return Caller-owned final HttpRes, or null after validation or request failure.
void *rt_http_client_get(void *obj, rt_string url) {
    rt_http_client_impl *c = http_client_require(obj, "HttpClient: invalid client");
    return c ? do_request(c, "GET", url, NULL) : NULL;
}

/// @brief Send a `POST` with a string body. **Redirect semantics:** 301/302 with POST switch to
/// GET (per common browser behavior); 303 always switches to GET; 307/308 preserve method+body.
/// @param obj HttpClient receiver.
/// @param url Absolute HTTP or HTTPS URL.
/// @param body Request body String; embedded null bytes are preserved by exact length.
/// @return Caller-owned final HttpRes, or null after validation or request failure.
void *rt_http_client_post(void *obj, rt_string url, rt_string body) {
    rt_http_client_impl *c = http_client_require(obj, "HttpClient: invalid client");
    return c ? do_request(c, "POST", url, body) : NULL;
}

/// @brief Send a `PUT` with a string body. Redirects preserve method+body for 307/308.
/// @param obj HttpClient receiver.
/// @param url Absolute HTTP or HTTPS URL.
/// @param body Request body String; embedded null bytes are preserved by exact length.
/// @return Caller-owned final HttpRes, or null after validation or request failure.
void *rt_http_client_put(void *obj, rt_string url, rt_string body) {
    rt_http_client_impl *c = http_client_require(obj, "HttpClient: invalid client");
    return c ? do_request(c, "PUT", url, body) : NULL;
}

/// @brief Send a `DELETE` (no body). Same redirect handling as `_get`.
/// @param obj HttpClient receiver.
/// @param url Absolute HTTP or HTTPS URL.
/// @return Caller-owned final HttpRes, or null after validation or request failure.
void *rt_http_client_delete(void *obj, rt_string url) {
    rt_http_client_impl *c = http_client_require(obj, "HttpClient: invalid client");
    return c ? do_request(c, "DELETE", url, NULL) : NULL;
}

/// @brief Add or replace a default header that is sent with every request.
/// @details Replacement is case-insensitive: setting `authorization` supersedes
///          a stored `Authorization`. Invalid names, embedded nulls, and CR/LF
///          values trap without publishing a partial replacement.
/// @param obj HttpClient receiver; null is a no-op.
/// @param name Header field name.
/// @param value Header field value.
void rt_http_client_set_header(void *obj, rt_string name, rt_string value) {
    if (!obj)
        return;
    rt_http_client_impl *c = http_client_require(obj, "HttpClient: invalid client");
    if (!c)
        return;
    if (!rt_string_is_handle(name) || !rt_string_is_handle(value)) {
        rt_trap("HttpClient: invalid default header");
        return;
    }
    const char *name_cstr = rt_string_cstr(name);
    if (!name_cstr || !http_method_is_token(name_cstr) ||
        http_client_string_has_embedded_nul(name) || http_client_string_has_embedded_nul(value) ||
        http_client_string_has_crlf(value)) {
        rt_trap("HttpClient: invalid default header");
        return;
    }
    HTTP_CLIENT_MUTEX_LOCK(&c->lock);
    int updated = rt_http_header_map_set_ci(c->default_headers, name, (void *)value);
    HTTP_CLIENT_MUTEX_UNLOCK(&c->lock);
    if (!updated)
        rt_trap("HttpClient: default header allocation failed");
}

/// @brief Set the request timeout in milliseconds for all subsequent requests.
/// @param obj HttpClient receiver; null is a no-op.
/// @param timeout_ms Value in the inclusive range 0 through `INT_MAX`; zero disables deadlines.
void rt_http_client_set_timeout(void *obj, int64_t timeout_ms) {
    if (!obj)
        return;
    rt_http_client_impl *c = http_client_require(obj, "HttpClient: invalid client");
    if (!c)
        return;
    int timeout_int = 0;
    if (!http_client_timeout_ms_to_int(timeout_ms, &timeout_int)) {
        rt_trap("HttpClient: invalid timeout");
        return;
    }
    HTTP_CLIENT_MUTEX_LOCK(&c->lock);
    c->timeout_ms = timeout_int;
    HTTP_CLIENT_MUTEX_UNLOCK(&c->lock);
}

/// @brief Check whether this client keeps HTTP connections open for reuse.
/// @param obj HttpClient receiver; null returns zero.
/// @return One when pooled reuse is enabled; zero otherwise.
int8_t rt_http_client_get_keep_alive(void *obj) {
    if (!obj)
        return 0;
    rt_http_client_impl *c = http_client_require(obj, "HttpClient: invalid client");
    if (!c)
        return 0;
    int8_t keep_alive;
    HTTP_CLIENT_MUTEX_LOCK(&c->lock);
    keep_alive = c->keep_alive;
    HTTP_CLIENT_MUTEX_UNLOCK(&c->lock);
    return keep_alive;
}

/// @brief Enable or disable pooled keep-alive transport for this client.
/// @details Enabling allocates a pool outside the mutex and rechecks the
///          configured capacity before publication. Disabling detaches the
///          current pool under lock and clears it afterward.
/// @param obj HttpClient receiver; null is a no-op.
/// @param keep_alive Nonzero to enable pooled reuse.
void rt_http_client_set_keep_alive(void *obj, int8_t keep_alive) {
    if (!obj)
        return;
    rt_http_client_impl *c = http_client_require(obj, "HttpClient: invalid client");
    if (!c)
        return;
    const int8_t enable = keep_alive ? 1 : 0;
    if (!enable) {
        HTTP_CLIENT_MUTEX_LOCK(&c->lock);
        void *old_pool = c->connection_pool;
        c->connection_pool = NULL;
        c->keep_alive = 0;
        HTTP_CLIENT_MUTEX_UNLOCK(&c->lock);
        if (old_pool)
            rt_http_conn_pool_clear(old_pool);
        http_client_release_obj(old_pool);
        return;
    }

    for (;;) {
        HTTP_CLIENT_MUTEX_LOCK(&c->lock);
        if (c->keep_alive && c->connection_pool) {
            HTTP_CLIENT_MUTEX_UNLOCK(&c->lock);
            return;
        }
        int64_t pool_size = c->pool_size > 0 ? c->pool_size : 8;
        HTTP_CLIENT_MUTEX_UNLOCK(&c->lock);

        void *new_pool = rt_http_conn_pool_new(pool_size);
        if (!new_pool)
            return;

        HTTP_CLIENT_MUTEX_LOCK(&c->lock);
        if (c->pool_size != pool_size) {
            HTTP_CLIENT_MUTEX_UNLOCK(&c->lock);
            http_client_release_obj(new_pool);
            continue;
        }
        if (c->keep_alive && c->connection_pool) {
            HTTP_CLIENT_MUTEX_UNLOCK(&c->lock);
            http_client_release_obj(new_pool);
            return;
        }
        void *old_pool = c->connection_pool;
        c->connection_pool = new_pool;
        c->keep_alive = 1;
        HTTP_CLIENT_MUTEX_UNLOCK(&c->lock);
        http_client_release_obj(old_pool);
        return;
    }
}

/// @brief Resize the keep-alive pool. Existing idle connections are dropped.
/// @details Allocates the replacement before taking the client mutex. If
///          keep-alive is disabled, only the future capacity is retained and
///          no inactive pool remains attached.
/// @param obj HttpClient receiver; null is a no-op.
/// @param max_size Requested capacity; non-positive values clamp to one.
void rt_http_client_set_pool_size(void *obj, int64_t max_size) {
    if (!obj)
        return;
    rt_http_client_impl *c = http_client_require(obj, "HttpClient: invalid client");
    if (!c)
        return;
    if (max_size <= 0)
        max_size = 1;
    void *new_pool = rt_http_conn_pool_new(max_size);
    if (!new_pool)
        return;
    HTTP_CLIENT_MUTEX_LOCK(&c->lock);
    c->pool_size = max_size;
    void *old_pool = c->connection_pool;
    if (c->keep_alive) {
        c->connection_pool = new_pool;
        new_pool = NULL;
    } else {
        c->connection_pool = NULL;
    }
    HTTP_CLIENT_MUTEX_UNLOCK(&c->lock);
    http_client_release_obj(old_pool);
    http_client_release_obj(new_pool);
}

/// @brief Set the maximum number of HTTP redirects to follow (0 = no redirects).
/// @param obj HttpClient receiver; null is a no-op.
/// @param max Maximum redirect hops; negative values clamp to zero.
void rt_http_client_set_max_redirects(void *obj, int64_t max) {
    if (!obj)
        return;
    rt_http_client_impl *c = http_client_require(obj, "HttpClient: invalid client");
    if (!c)
        return;
    HTTP_CLIENT_MUTEX_LOCK(&c->lock);
    c->max_redirects = max < 0 ? 0 : max;
    HTTP_CLIENT_MUTEX_UNLOCK(&c->lock);
}

/// @brief Check whether the client automatically follows HTTP redirects.
/// @param obj HttpClient receiver; null returns zero.
/// @return One when automatic redirects are enabled; zero otherwise.
int8_t rt_http_client_get_follow_redirects(void *obj) {
    if (!obj)
        return 0;
    rt_http_client_impl *c = http_client_require(obj, "HttpClient: invalid client");
    if (!c)
        return 0;
    int8_t follow;
    HTTP_CLIENT_MUTEX_LOCK(&c->lock);
    follow = c->follow_redirects;
    HTTP_CLIENT_MUTEX_UNLOCK(&c->lock);
    return follow;
}

/// @brief Enable or disable automatic following of HTTP redirects.
/// @param obj HttpClient receiver; null is a no-op.
/// @param follow Nonzero to enable redirect following up to the configured limit.
void rt_http_client_set_follow_redirects(void *obj, int8_t follow) {
    if (!obj)
        return;
    rt_http_client_impl *c = http_client_require(obj, "HttpClient: invalid client");
    if (!c)
        return;
    HTTP_CLIENT_MUTEX_LOCK(&c->lock);
    c->follow_redirects = follow ? 1 : 0;
    HTTP_CLIENT_MUTEX_UNLOCK(&c->lock);
}

/// @brief Store a cookie for exactly the given host; sent automatically on matching requests.
/// @details Manual cookies require token-valid names, cookie-octet values, and
///          syntactically valid domains. They are stored HOST-ONLY, so a cookie
///          set for `example.com` is not sent to `sub.example.com`; this exact
///          matching also makes public-suffix rejection unnecessary here.
///          Empty values are stored (they are valid cookies); use `DeleteCookie` to remove one.
///          Invalid inputs trap instead of being silently dropped or stored unsafely.
/// @param obj HttpClient receiver; null is a no-op.
/// @param domain Exact host or address scope.
/// @param name Case-sensitive cookie name.
/// @param value Cookie value; an empty String is valid.
void rt_http_client_set_cookie(void *obj, rt_string domain, rt_string name, rt_string value) {
    const char *domain_cstr;
    const char *name_cstr;
    const char *value_cstr;
    rt_http_cookie *cookie;
    if (!obj)
        return;
    rt_http_client_impl *c = http_client_require(obj, "HttpClient: invalid client");
    if (!c)
        return;
    if (!rt_string_is_handle(domain) || !rt_string_is_handle(name) || !rt_string_is_handle(value)) {
        rt_trap("HttpClient.SetCookie: invalid cookie");
        return;
    }
    domain_cstr = rt_string_cstr(domain);
    name_cstr = rt_string_cstr(name);
    value_cstr = rt_string_cstr(value);
    if (!domain_cstr || !*domain_cstr || !name_cstr || !*name_cstr || !value_cstr ||
        http_client_string_has_embedded_nul(domain) || http_client_string_has_embedded_nul(name) ||
        http_client_string_has_embedded_nul(value)) {
        rt_trap("HttpClient.SetCookie: invalid cookie");
        return;
    }
    if (!cookie_name_is_valid(name_cstr) || !cookie_value_is_valid(value_cstr)) {
        rt_trap("HttpClient.SetCookie: invalid cookie name or value");
        return;
    }

    cookie = (rt_http_cookie *)calloc(1, sizeof(*cookie));
    if (!cookie) {
        rt_trap("HttpClient.SetCookie: memory allocation failed");
        return;
    }
    cookie->name = strdup(name_cstr);
    cookie->value = strdup(value_cstr);
    cookie->domain = cookie_strdup_manual_domain(domain_cstr);
    cookie->path = strdup("/");
    cookie->host_only = 1;
    if (!cookie->name || !cookie->value || !cookie->domain || !cookie->path || !*cookie->domain) {
        free_cookie_list(cookie);
        rt_trap("HttpClient.SetCookie: memory allocation failed");
        return;
    }
    // Host-only storage already prevents supercookies (an exact-host cookie
    // for "com" is only ever sent to the literal host "com"), so only the
    // domain SYNTAX is validated here — single-label hosts like localhost
    // remain usable.
    if (!cookie_domain_is_valid(cookie->domain)) {
        free_cookie_list(cookie);
        rt_trap("HttpClient.SetCookie: invalid cookie domain");
        return;
    }

    HTTP_CLIENT_MUTEX_LOCK(&c->lock);
    replace_cookie_locked(c, cookie);
    HTTP_CLIENT_MUTEX_UNLOCK(&c->lock);
}

/// @brief Remove a manually or automatically stored cookie by exact domain and
///        case-sensitive name. No-op when nothing matches.
/// @param obj HttpClient receiver; null is a no-op.
/// @param domain Exact cookie domain.
/// @param name Case-sensitive cookie name.
void rt_http_client_delete_cookie(void *obj, rt_string domain, rt_string name) {
    const char *domain_cstr;
    const char *name_cstr;
    if (!obj)
        return;
    rt_http_client_impl *c = http_client_require(obj, "HttpClient: invalid client");
    if (!c)
        return;
    if ((domain && !rt_string_is_handle(domain)) || (name && !rt_string_is_handle(name))) {
        rt_trap("HttpClient.DeleteCookie: invalid cookie identity");
        return;
    }
    domain_cstr = domain ? rt_string_cstr(domain) : NULL;
    name_cstr = name ? rt_string_cstr(name) : NULL;
    if (!domain_cstr || !*domain_cstr || !name_cstr || !*name_cstr ||
        http_client_string_has_embedded_nul(domain) || http_client_string_has_embedded_nul(name))
        return;

    char *normalized = cookie_strdup_manual_domain(domain_cstr);
    if (!normalized) {
        rt_trap("HttpClient.DeleteCookie: memory allocation failed");
        return;
    }
    if (!*normalized) {
        free(normalized);
        return;
    }

    HTTP_CLIENT_MUTEX_LOCK(&c->lock);
    rt_http_cookie **link = &c->cookies;
    while (*link) {
        rt_http_cookie *cookie = *link;
        if (strcmp(cookie->name, name_cstr) == 0 && strcasecmp(cookie->domain, normalized) == 0) {
            *link = cookie->next;
            cookie->next = NULL;
            free_cookie_list(cookie);
            continue;
        }
        link = &cookie->next;
    }
    HTTP_CLIENT_MUTEX_UNLOCK(&c->lock);
    free(normalized);
}

/// @brief One native cookie key/value copied out of the synchronized jar.
typedef struct {
    char *name;
    char *value;
} http_cookie_snapshot_entry;

/// @brief Release a native cookie snapshot captured under the client mutex.
/// @param entries Snapshot array, or NULL.
/// @param count Number of initialized entries.
static void http_client_cookie_snapshot_release(http_cookie_snapshot_entry *entries, size_t count) {
    if (!entries)
        return;
    for (size_t i = 0; i < count; i++) {
        free(entries[i].name);
        free(entries[i].value);
    }
    free(entries);
}

/// @brief Return a cloned snapshot of cookies matching one domain.
/// @details Cookie name/value bytes are copied into native staging while the
///          jar mutex is held, then converted to a managed Map after unlocking.
///          Managed allocation traps release the partial Map, current Strings,
///          and full native snapshot, so no longjmp can strand the mutex.
/// @param obj HttpClient receiver; NULL returns an empty Map.
/// @param domain Hostname used for cookie domain matching.
/// @return Caller-owned Map of String names to String values.
void *rt_http_client_get_cookies(void *obj, rt_string domain) {
    const char *domain_cstr;
    if (!obj)
        return rt_map_new();
    rt_http_client_impl *c = http_client_require(obj, "HttpClient: invalid client");
    if (!c)
        return NULL;
    if (domain && !rt_string_is_handle(domain)) {
        rt_trap("HttpClient.GetCookies: invalid domain");
        return NULL;
    }
    domain_cstr = domain ? rt_string_cstr(domain) : NULL;
    if (!domain_cstr || !*domain_cstr || http_client_string_has_embedded_nul(domain))
        return rt_map_new();

    http_cookie_snapshot_entry *entries = NULL;
    size_t entry_count = 0;
    int snapshot_failed = 0;
    HTTP_CLIENT_MUTEX_LOCK(&c->lock);
    purge_expired_cookies_locked(c);
    for (rt_http_cookie *cookie = c->cookies; cookie; cookie = cookie->next) {
        if (cookie_domain_matches(cookie, domain_cstr)) {
            if (entry_count == SIZE_MAX / sizeof(*entries)) {
                snapshot_failed = 1;
                break;
            }
            entry_count++;
        }
    }
    if (!snapshot_failed && entry_count > 0) {
        entries = (http_cookie_snapshot_entry *)calloc(entry_count, sizeof(*entries));
        if (!entries)
            snapshot_failed = 1;
    }
    size_t initialized = 0;
    if (!snapshot_failed && entries) {
        for (rt_http_cookie *cookie = c->cookies; cookie; cookie = cookie->next) {
            if (!cookie_domain_matches(cookie, domain_cstr))
                continue;
            if (initialized >= entry_count) {
                snapshot_failed = 1;
                break;
            }
            entries[initialized].name = strdup(cookie->name);
            entries[initialized].value = strdup(cookie->value);
            if (!entries[initialized].name || !entries[initialized].value) {
                snapshot_failed = 1;
                break;
            }
            initialized++;
        }
        if (initialized != entry_count)
            snapshot_failed = 1;
    }
    HTTP_CLIENT_MUTEX_UNLOCK(&c->lock);
    if (snapshot_failed) {
        http_client_cookie_snapshot_release(entries, entry_count);
        rt_trap("HttpClient.GetCookies: snapshot allocation failed");
        return NULL;
    }

    void *volatile snapshot = NULL;
    rt_string volatile key = NULL;
    rt_string volatile value = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[256];
        http_client_save_trap_error(
            saved_error, sizeof(saved_error), "HttpClient.GetCookies: allocation failed");
        rt_trap_clear_recovery();
        http_client_release_obj((void *)value);
        http_client_release_obj((void *)key);
        http_client_release_obj((void *)snapshot);
        http_client_cookie_snapshot_release(entries, entry_count);
        rt_trap(saved_error);
        return NULL;
    }
    snapshot = rt_map_new();
    if (!snapshot) {
        rt_trap("HttpClient.GetCookies: Map allocation failed");
        goto returning_trap;
    }
    for (size_t i = 0; i < entry_count; i++) {
        key = rt_string_from_bytes(entries[i].name, strlen(entries[i].name));
        value = rt_string_from_bytes(entries[i].value, strlen(entries[i].value));
        if (!key || !value) {
            rt_trap("HttpClient.GetCookies: String allocation failed");
            goto returning_trap;
        }
        rt_map_set((void *)snapshot, (rt_string)key, (void *)value);
        http_client_release_obj((void *)value);
        value = NULL;
        http_client_release_obj((void *)key);
        key = NULL;
    }
    rt_trap_clear_recovery();
    http_client_cookie_snapshot_release(entries, entry_count);
    return (void *)snapshot;

returning_trap:
    rt_trap_clear_recovery();
    http_client_release_obj((void *)value);
    http_client_release_obj((void *)key);
    http_client_release_obj((void *)snapshot);
    http_client_cookie_snapshot_release(entries, entry_count);
    return NULL;
}
