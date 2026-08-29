//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/network/rt_network_http.c
// Purpose: Implements HTTP request/response transport, URL parsing, redirects,
//          response framing, and managed publication for Zanna.Network.
// Key invariants:
//   - Request and response handles are validated against stable private class
//     identities before native fields are accessed.
//   - Response bodies and headers are staged transactionally and are published
//     only after the complete HTTP exchange succeeds.
// Ownership/Lifetime:
//   - Native request/response buffers are owned by their managed wrapper until
//     finalization; socket and TLS resources are released on every exit path.
//   - Returned managed strings, maps, and byte arrays follow the runtime
//     registry's owning-return convention.
// Links: rt_network_http_internal.h, rt_network_internal.h, rt_http_client.c,
//        rt_tls_internal.h,
//        docs/adr/0126-http-client-stable-identity-and-transactional-ownership.md,
//        docs/adr/0228-http-end-to-end-request-deadlines.md,
//        docs/adr/0229-bounded-native-gzip-decoding.md
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_network_http.c
 * @brief Implements native HTTP transport, framing, redirects, and response publication.
 * @details This translation unit owns the HTTP/1.x and negotiated HTTP/2
 *          exchange core used by the public runtime wrappers. It validates
 *          request syntax, manages plain or TLS connections and keep-alive
 *          leases, parses bounded response metadata and bodies, and publishes
 *          managed responses only after an exchange completes successfully.
 */

#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE 1
#endif
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include "rt_http2.h"
#include "rt_network_http_internal.h"
#include "rt_network_internal.h"
#include "rt_platform.h"
#include "rt_tls.h"
#include "rt_tls_internal.h"

#include "rt_box.h"
#include "rt_compress.h"
#include "rt_error.h"
#include "rt_heap.h"
#include "rt_map.h"
#include "rt_threads.h"
#include "rt_time.h"

#include <ctype.h>
#include <errno.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if RT_PLATFORM_WINDOWS
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
typedef CRITICAL_SECTION http_pool_mutex_t;
#define HTTP_POOL_MUTEX_LOCK(m) EnterCriticalSection(m)
#define HTTP_POOL_MUTEX_UNLOCK(m) LeaveCriticalSection(m)
#define HTTP_POOL_MUTEX_DESTROY(m) DeleteCriticalSection(m)
#else
#include <pthread.h>
#include <strings.h>
typedef pthread_mutex_t http_pool_mutex_t;
#define HTTP_POOL_MUTEX_LOCK(m) pthread_mutex_lock(m)
#define HTTP_POOL_MUTEX_UNLOCK(m) pthread_mutex_unlock(m)
#define HTTP_POOL_MUTEX_DESTROY(m) pthread_mutex_destroy(m)
#endif

#include "rt_trap.h"

/// @brief Initialize one HTTP pool mutex through the active platform adapter.
/// @details Both adapters report resource exhaustion explicitly. The caller
///          publishes `lock_initialized` only after this helper succeeds.
/// @param mutex Uninitialized native mutex storage.
/// @return One when initialized; zero on a reported platform failure.
static int http_pool_mutex_init(http_pool_mutex_t *mutex) {
    if (!mutex)
        return 0;
#if RT_PLATFORM_WINDOWS
    return InitializeCriticalSectionEx(mutex, 0, 0) != FALSE ? 1 : 0;
#else
    return pthread_mutex_init(mutex, NULL) == 0 ? 1 : 0;
#endif
}

/// @brief Internal response-map separator for repeated Set-Cookie headers.
/// @details The public response header map stores string values, but Set-Cookie cannot be safely
///          folded with comma or semicolon separators. Use an HTTP-invalid control byte instead of
///          CR/LF so cookie storage can recover individual fields without exposing header-splitting
///          syntax through the parsed response value.
#define HTTP_SET_COOKIE_JOIN_SEPARATOR "\037"

/// @brief Test whether host text needs IPv6 brackets in an authority.
/// @details A colon-containing host that does not already begin with `[` is
///          treated as a bare IPv6 literal requiring RFC 3986 brackets.
/// @param host Optional NUL-terminated host text.
/// @return true when brackets must be added, otherwise false.
static bool host_needs_brackets(const char *host) {
    return host && strchr(host, ':') != NULL && host[0] != '[';
}

/// @brief Detect HTTP-forbidden control or whitespace bytes.
/// @param text NUL-terminated text to inspect; NULL is invalid.
/// @return One when @p text is NULL or contains a byte at or below space or
///         DEL, otherwise zero.
static int http_contains_ctl_or_space(const char *text) {
    if (!text)
        return 1;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        if (*p <= 0x20 || *p == 0x7Fu)
            return 1;
    }
    return 0;
}

/// @brief Validate an HTTP method or field name as an RFC token.
/// @details Rejects empty input, controls, whitespace, DEL, and every HTTP
///          separator byte.
/// @param method NUL-terminated candidate token.
/// @return One for a nonempty valid token, otherwise zero.
int http_method_is_token(const char *method) {
    static const char *kSeparators = "()<>@,;:\\\"/[]?={} \t";
    if (!method || !*method)
        return 0;
    for (const unsigned char *p = (const unsigned char *)method; *p; ++p) {
        if (*p <= 0x20 || *p == 0x7Fu || strchr(kSeparators, (int)*p) != NULL)
            return 0;
    }
    return 1;
}

/// @brief Validate an HTTP header field name as a token.
/// @param name NUL-terminated candidate field name.
/// @return One when @p name satisfies the shared token grammar, otherwise zero.
static int http_header_field_name_is_token(const char *name) {
    return http_method_is_token(name);
}

/// @brief Expose header-name token validation to focused runtime tests.
/// @param name NUL-terminated candidate field name.
/// @return One for a valid HTTP field name, otherwise zero.
int rt_http_header_name_valid_for_test(const char *name) {
    return http_header_field_name_is_token(name);
}

/// @brief Test whether text contains any byte from a delimiter set.
/// @param text NUL-terminated text to scan.
/// @param chars NUL-terminated set of bytes to match.
/// @return One on the first match, otherwise zero; NULL input yields zero.
static int http_contains_any_char(const char *text, const char *chars) {
    if (!text || !chars)
        return 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        for (const unsigned char *c = (const unsigned char *)chars; *c; ++c) {
            if (*p == *c)
                return 1;
        }
    }
    return 0;
}

/// @brief Detect a NUL byte inside a managed String's logical payload.
/// @details NULL, unavailable storage, and nonpositive lengths report no
///          embedded byte; callers validate the handle separately.
/// @param text Managed runtime String to inspect.
/// @return One when a NUL occurs before the logical end, otherwise zero.
int http_rt_string_has_embedded_nul(rt_string text) {
    if (!text)
        return 0;
    const char *cstr = rt_string_cstr(text);
    int64_t len64 = rt_str_len(text);
    if (!cstr || len64 <= 0)
        return 0;
    return memchr(cstr, '\0', (size_t)len64) != NULL;
}

/// @brief Validate host text for safe use in an HTTP authority.
/// @param host Nonempty NUL-terminated hostname or numeric address.
/// @return One when the host contains no controls, whitespace, URL authority
///         delimiters, or backslash, otherwise zero.
static int http_host_is_valid(const char *host) {
    if (!host || !*host || http_contains_ctl_or_space(host))
        return 0;
    return !http_contains_any_char(host, "/?#@\\");
}

/// @brief Validate an origin-form HTTP request target.
/// @param target NUL-terminated target expected to begin with `/`.
/// @return One when the target has origin form and contains no controls,
///         whitespace, DEL, or backslash, otherwise zero.
static int http_request_target_is_valid(const char *target) {
    if (!target || target[0] != '/')
        return 0;
    for (const unsigned char *p = (const unsigned char *)target; *p; ++p) {
        if (*p <= 0x20 || *p == 0x7Fu || *p == '\\')
            return 0;
    }
    return 1;
}

//=============================================================================
// HTTP Client Implementation
//=============================================================================

/// @brief Maximum number of redirects to follow.

/// @brief Default timeout for HTTP requests (30 seconds).

/// @brief Initial buffer size for reading responses.
#define HTTP_BUFFER_SIZE 4096

/// @brief Maximum response body size (256 MB) — prevents decompression/server DoS (S-09 fix).
#define HTTP_MAX_BODY_SIZE (256u * 1024u * 1024u)

/// @brief Maximum decoded-to-encoded ratio for buffered gzip responses.
#define HTTP_MAX_GZIP_EXPANSION_RATIO 128u

/// @brief Ratio-policy slack retained for small, highly compressible responses.
#define HTTP_GZIP_EXPANSION_SLACK (1u * 1024u * 1024u)

/// @brief Maximum aggregate bytes accepted in a response header block.
#define HTTP_MAX_HEADER_BYTES (256u * 1024u)

/// @brief Maximum aggregate bytes accepted in chunked response trailers.
#define HTTP_MAX_TRAILER_BYTES (64u * 1024u)

/// @brief Maximum chunked response trailer fields.
#define HTTP_MAX_TRAILER_LINES 64u

/// @brief HTTP connection context (TCP or TLS).
typedef struct http_conn {
    socket_t socket_fd;       // Connected socket (owned directly for HTTP, by TLS for HTTPS)
    rt_tls_session_t *tls;    // TLS session (for HTTPS)
    rt_http2_conn_t *http2;   // HTTP/2 transport state (for ALPN h2)
    int use_tls;              // 1 if using TLS
    uint8_t read_buf[4096];   // Read buffer
    size_t read_buf_len;      // Bytes in buffer
    size_t read_buf_pos;      // Current position in buffer
    void *pool;               // Owning connection pool, if this lease came from / returns to one
    int pool_slot;            // Slot reserved inside @p pool while the lease is checked out
    int tls_verify;           // Verification mode used to establish this connection
    int reused_from_pool;     // 1 when this request is reusing an already-open pooled connection
    int timeout_ms;           // Request I/O timeout used for retry readiness waits
    int64_t deadline_us;      // Absolute monotonic end-to-end request deadline
    int timed_out;            // Sticky classification for deadline-driven I/O failure
    uint64_t pool_generation; // Pool-clear generation captured for this lease
    char pool_key[320];       // Stable host/port/TLS key for reuse
} http_conn_t;

/// @brief Construct a saturating monotonic deadline from a timeout.
/// @param timeout_ms Positive timeout budget; nonpositive disables the deadline.
/// @return Absolute monotonic microseconds, or zero for no deadline.
static int64_t http_deadline_from_timeout_ms(int timeout_ms) {
    if (timeout_ms <= 0)
        return 0;
    int64_t now_us = rt_clock_ticks_us();
    int64_t budget_us = (int64_t)timeout_ms * 1000;
    return now_us > INT64_MAX - budget_us ? INT64_MAX : now_us + budget_us;
}

/// @brief Convert an absolute deadline to a positive rounded-up wait budget.
/// @param deadline_us Absolute monotonic microseconds; nonpositive is unbounded.
/// @return Zero for unbounded, -1 for expired, or a value in [1, INT_MAX].
static int http_deadline_remaining_ms(int64_t deadline_us) {
    if (deadline_us <= 0)
        return 0;
    int64_t remaining_us = deadline_us - rt_clock_ticks_us();
    if (remaining_us <= 0)
        return -1;
    int64_t remaining_ms = remaining_us / 1000 + (remaining_us % 1000 != 0 ? 1 : 0);
    return remaining_ms > INT_MAX ? INT_MAX : (int)remaining_ms;
}

/// @brief Mark and report whether one connection exhausted its request budget.
/// @param conn Connection carrying the shared request deadline.
/// @return One when expired, otherwise zero.
static int http_conn_deadline_expired(http_conn_t *conn) {
    if (!conn || conn->deadline_us <= 0)
        return 0;
    if (rt_clock_ticks_us() < conn->deadline_us)
        return 0;
    conn->timed_out = 1;
    return 1;
}

/// @brief Prepare one transport operation to observe the remaining deadline.
/// @details TLS receives the same absolute deadline at record-layer granularity.
///          Plain sockets use @ref http_conn_wait_ready before their blocking
///          operation. The rounded remaining time is retained for legacy
///          would-block waits.
/// @param conn Active HTTP connection.
/// @return One while budget remains, otherwise zero after marking timeout.
static int http_conn_prepare_io(http_conn_t *conn) {
    if (!conn)
        return 0;
    int remaining_ms = http_deadline_remaining_ms(conn->deadline_us);
    if (remaining_ms < 0) {
        conn->timed_out = 1;
        return 0;
    }
    if (remaining_ms > 0)
        conn->timeout_ms = remaining_ms;
    if (conn->tls)
        rt_tls_set_internal_io_deadline(conn->tls, conn->deadline_us);
    return 1;
}

/// @brief Wait for plain-socket readiness within the shared request budget.
/// @param conn Active plain HTTP connection.
/// @param for_write Nonzero for write readiness; zero for read readiness.
/// @return One when ready or unbounded, otherwise zero.
static int http_conn_wait_ready(http_conn_t *conn, int for_write) {
    if (!http_conn_prepare_io(conn))
        return 0;
    if (!conn || conn->deadline_us <= 0)
        return 1;
    int ready = wait_socket(conn->socket_fd, conn->timeout_ms, for_write != 0);
    if (ready > 0)
        return 1;
    if (ready == 0 || http_conn_deadline_expired(conn))
        conn->timed_out = 1;
    return 0;
}

/// @brief Classify an HTTP transport/protocol failure against its deadline.
/// @param conn Connection that observed the failure.
/// @param fallback Existing non-timeout error category.
/// @return @ref Err_Timeout after deadline exhaustion, otherwise @p fallback.
static int http_conn_failure_code(http_conn_t *conn, int fallback) {
    return conn && (conn->timed_out || http_conn_deadline_expired(conn)) ? Err_Timeout : fallback;
}

/// @brief Remove request-local deadline state before close or pooling.
/// @param conn Connection leaving the active request.
static void http_conn_clear_deadline(http_conn_t *conn) {
    if (!conn)
        return;
    if (conn->tls)
        rt_tls_set_internal_io_deadline(conn->tls, 0);
    conn->deadline_us = 0;
}

/// @brief Initialize an HTTP connection context around a plain TCP socket.
/// @details Clears all buffered and pool state and records that @p socket_fd is
///          owned directly by the context.
/// @param conn Uninitialized connection context to populate.
/// @param socket_fd Owned connected native socket.
static void http_conn_init_tcp(http_conn_t *conn, socket_t socket_fd) {
    memset(conn, 0, sizeof(*conn));
    conn->socket_fd = socket_fd;
    conn->tls = NULL;
    conn->http2 = NULL;
    conn->use_tls = 0;
    conn->pool_slot = -1;
    conn->timeout_ms = 0;
}

/// @brief Initialize an HTTP connection context around a TLS session.
/// @details Clears all buffered and pool state and borrows the session's native
///          descriptor for readiness operations. The context owns @p tls.
/// @param conn Uninitialized connection context to populate.
/// @param tls Owned TLS session, or NULL to initialize an invalid descriptor.
static void http_conn_init_tls(http_conn_t *conn, rt_tls_session_t *tls) {
    memset(conn, 0, sizeof(*conn));
    conn->socket_fd = tls ? (socket_t)rt_tls_get_socket(tls) : INVALID_SOCK;
    conn->tls = tls;
    conn->http2 = NULL;
    conn->use_tls = 1;
    conn->pool_slot = -1;
    conn->timeout_ms = 0;
}

/// @brief Adapt TLS receive to the HTTP/2 transport read callback.
/// @param ctx TLS session supplied as an opaque callback context.
/// @param buf Writable destination buffer.
/// @param len Maximum bytes to receive.
/// @return TLS receive result, or -1 for invalid callback arguments.
static long http2_tls_read(void *ctx, uint8_t *buf, size_t len) {
    rt_tls_session_t *tls = (rt_tls_session_t *)ctx;
    if (!tls || !buf)
        return -1;
    return rt_tls_recv(tls, buf, len);
}

/// @brief Adapt TLS send to the HTTP/2 all-or-failure write callback.
/// @param ctx TLS session supplied as an opaque callback context.
/// @param buf Source buffer, or NULL only for a zero length.
/// @param len Exact bytes required by the HTTP/2 transport.
/// @return One only when TLS writes exactly @p len bytes, otherwise zero.
static int http2_tls_write(void *ctx, const uint8_t *buf, size_t len) {
    rt_tls_session_t *tls = (rt_tls_session_t *)ctx;
    long sent = 0;
    if (!tls || (!buf && len > 0))
        return 0;
    sent = rt_tls_send(tls, buf, len);
    return sent == (long)len;
}

/// @brief Send an entire buffer over a plain or TLS HTTP connection.
/// @details Loops over partial writes, chunks native plain-socket calls at
///          @c INT_MAX, retries interrupted calls, and waits for writability
///          after would-block using the request timeout.
/// @param conn Initialized connection context.
/// @param data Readable buffer containing @p len bytes.
/// @param len Exact byte count to send.
/// @return Zero after complete delivery, or -1 on transport failure.
static int http_conn_send(http_conn_t *conn, const uint8_t *data, size_t len) {
    size_t total_sent = 0;

    if (conn->use_tls) {
        while (total_sent < len) {
            if (!http_conn_prepare_io(conn))
                return -1;
            long sent = rt_tls_send(conn->tls, data + total_sent, len - total_sent);
            if (sent <= 0) {
                (void)http_conn_deadline_expired(conn);
                return -1;
            }
            total_sent += (size_t)sent;
        }
    } else {
        while (total_sent < len) {
            if (!http_conn_wait_ready(conn, 1))
                return -1;
            int sent = send(conn->socket_fd,
                            (const char *)(data + total_sent),
                            (int)(len - total_sent > INT_MAX ? INT_MAX : len - total_sent),
                            SEND_FLAGS);
            if (sent < 0) {
                int err = rt_socket_last_error();
                if (rt_socket_error_is_interrupted(err))
                    continue;
                if (rt_socket_error_is_would_block(err)) {
                    if (conn->deadline_us > 0)
                        continue;
                    int ready = wait_socket(conn->socket_fd, conn->timeout_ms, true);
                    if (ready > 0)
                        continue;
                }
                return -1;
            }
            if (sent == 0)
                return -1;
            total_sent += (size_t)sent;
        }
    }
    return 0;
}

/// @brief Receive up to a requested count from an HTTP connection.
/// @details Drains bytes previously buffered by single-byte parsing before one
///          TLS or native socket read. Interrupted plain reads are retried.
/// @param conn Initialized connection context.
/// @param buf Writable destination buffer.
/// @param len Maximum bytes to return.
/// @return Nonnegative bytes copied, including zero for EOF, or a negative
///         transport result when no buffered bytes preceded the failure.
static long http_conn_recv(http_conn_t *conn, uint8_t *buf, size_t len) {
    size_t total = 0;

    if (!http_conn_prepare_io(conn))
        return -1;

    // First, drain any buffered data
    while (total < len && conn->read_buf_pos < conn->read_buf_len) {
        buf[total++] = conn->read_buf[conn->read_buf_pos++];
    }

    if (total == len)
        return (long)total;

    // Need more data from network
    if (conn->use_tls) {
        long n = rt_tls_recv(conn->tls, buf + total, len - total);
        if (n > 0)
            total += n;
        else if (total == 0 && n < 0) {
            (void)http_conn_deadline_expired(conn);
            return n;
        }
    } else {
        int n = -1;
        for (;;) {
            if (!http_conn_wait_ready(conn, 0))
                break;
            n = recv(conn->socket_fd,
                     (char *)(buf + total),
                     (int)(len - total > INT_MAX ? INT_MAX : len - total),
                     0);
            if (n >= 0)
                break;
            int err = rt_socket_last_error();
            if (rt_socket_error_is_interrupted(err))
                continue;
            if (rt_socket_error_is_would_block(err)) {
                int ready = wait_socket(conn->socket_fd, conn->timeout_ms, false);
                if (ready > 0)
                    continue;
            }
            break;
        }
        if (n > 0)
            total += (size_t)n;
        else if (total == 0 && n < 0)
            return -1;
    }

    return (long)total;
}

/// @brief Receive one byte through the connection's parsing buffer.
/// @details Returns a buffered byte when available; otherwise refills the
///          internal buffer. Plain sockets retry interruptions and wait after
///          would-block using the request timeout.
/// @param conn Initialized connection context.
/// @param byte Receives the next byte on success.
/// @return One when a byte is produced, otherwise zero for EOF or failure.
static int http_conn_recv_byte(http_conn_t *conn, uint8_t *byte) {
    if (http_conn_deadline_expired(conn))
        return 0;

    // Check buffer first
    if (conn->read_buf_pos < conn->read_buf_len) {
        *byte = conn->read_buf[conn->read_buf_pos++];
        return 1;
    }

    // Refill buffer
    if (conn->use_tls) {
        if (!http_conn_prepare_io(conn))
            return 0;
        long n = rt_tls_recv(conn->tls, conn->read_buf, sizeof(conn->read_buf));
        if (n <= 0) {
            (void)http_conn_deadline_expired(conn);
            return 0;
        }
        conn->read_buf_len = (size_t)n;
        conn->read_buf_pos = 0;
    } else {
        int n;
        while (1) {
            if (!http_conn_wait_ready(conn, 0))
                return 0;
            n = recv(conn->socket_fd, (char *)conn->read_buf, (int)sizeof(conn->read_buf), 0);
            if (n >= 0)
                break;
            int err = rt_socket_last_error();
            if (rt_socket_error_is_interrupted(err))
                continue;
            if (rt_socket_error_is_would_block(err)) {
                if (conn->deadline_us > 0)
                    continue;
                int ready = wait_socket(conn->socket_fd, conn->timeout_ms, false);
                if (ready > 0)
                    continue;
            }
            return 0;
        }
        if (n <= 0)
            return 0;
        conn->read_buf_len = (size_t)n;
        conn->read_buf_pos = 0;
    }

    *byte = conn->read_buf[conn->read_buf_pos++];
    return 1;
}

/// @brief Close and reset every transport owned by an HTTP connection.
/// @details Releases HTTP/2 state first, then closes TLS or the directly owned
///          socket, and clears buffered and pool-lease metadata.
/// @param conn Initialized connection context to consume.
static void http_conn_close(http_conn_t *conn) {
    http_conn_clear_deadline(conn);
    if (conn->http2) {
        rt_http2_conn_free(conn->http2);
        conn->http2 = NULL;
    }
    if (conn->use_tls && conn->tls) {
        rt_tls_close(conn->tls);
        conn->tls = NULL;
        conn->socket_fd = INVALID_SOCK;
    } else if (conn->socket_fd != INVALID_SOCK) {
        CLOSE_SOCKET(conn->socket_fd);
        conn->socket_fd = INVALID_SOCK;
    }
    conn->read_buf_len = 0;
    conn->read_buf_pos = 0;
    conn->pool = NULL;
    conn->pool_slot = -1;
    conn->tls_verify = 0;
    conn->timed_out = 0;
    conn->pool_generation = 0;
    conn->pool_key[0] = '\0';
}

#define HTTP_CONN_POOL_MAX_ENTRIES 64
#define HTTP_CONN_POOL_IDLE_TIMEOUT_SEC 30

typedef struct http_conn_pool_entry {
    http_conn_t conn;
    char *key;
    int64_t last_used_ms;
    int in_use;
} http_conn_pool_entry_t;

/**
 * Compact ownership record used to defer transport destruction until after
 * the pool bookkeeping mutex is released.
 */
typedef struct http_conn_pool_discard {
    socket_t socket_fd;
    rt_tls_session_t *tls;
    rt_http2_conn_t *http2;
    char *key;
    int use_tls;
} http_conn_pool_discard_t;

typedef struct http_conn_pool {
    http_conn_pool_entry_t entries[HTTP_CONN_POOL_MAX_ENTRIES];
    int count;
    int max_size;
    uint64_t generation;
    http_pool_mutex_t lock;
    int lock_initialized;
} http_conn_pool_t;

/// @brief Test the stable managed identity and initialized payload of a pool.
/// @details Payload-size validation precedes the native-state read, preventing
///          unrelated or undersized objects from being reinterpreted as a
///          mutex-bearing HTTP pool. The caller must separately own or retain
///          the object for the duration of any subsequent operation.
/// @param obj Candidate managed object.
/// @return Nonzero only for a fully initialized HTTP connection pool.
int rt_http_conn_pool_is_handle(void *obj) {
    return rt_obj_is_instance(obj, RT_HTTP_CONN_POOL_CLASS_ID, sizeof(http_conn_pool_t)) &&
           ((http_conn_pool_t *)obj)->lock_initialized;
}

/// @brief Drop one temporary managed pool reference.
/// @param obj Owned mortal reference, or NULL.
static void http_conn_pool_release_ref(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief Read the monotonic millisecond clock used by HTTP connection pools.
/// @details Keep-alive idle eviction is based on elapsed time, not civil time,
///          so clock corrections cannot prematurely evict or indefinitely retain
///          sockets.
/// @return Monotonic milliseconds from the runtime clock's unspecified epoch.
static int64_t http_pool_now_ms(void) {
    return rt_clock_ticks_us() / 1000;
}

/// @brief Format the stable origin and security identity of a pooled connection.
/// @details Encodes plain/TLS mode, TLS verification policy, bracketed host,
///          and numeric port so connections with different security contracts
///          cannot share a slot.
/// @param host Origin host text.
/// @param port Origin port.
/// @param use_tls Nonzero for a TLS transport.
/// @param tls_verify Nonzero when peer verification was enabled.
/// @param buf Destination key buffer.
/// @param buf_len Capacity of @p buf in bytes.
/// @return One when the complete key fits, otherwise zero after clearing the
///         destination when possible.
static int http_make_pool_key(
    const char *host, int port, int use_tls, int tls_verify, char *buf, size_t buf_len) {
    if (!buf || buf_len == 0)
        return 0;
    int written = snprintf(buf,
                           buf_len,
                           "%c%c|%s%s%s|%d",
                           use_tls ? 's' : 'p',
                           tls_verify ? 'v' : 'i',
                           host_needs_brackets(host) ? "[" : "",
                           host ? host : "",
                           host_needs_brackets(host) ? "]" : "",
                           port);
    if (written < 0 || (size_t)written >= buf_len) {
        buf[0] = '\0';
        return 0;
    }
    return 1;
}

/// @brief Probe whether an idle connection remains reusable.
/// @details HTTP/2 delegates to its transport state. TLS buffered data counts
///          as healthy, while unexpected readable TLS state is conservatively
///          rejected. Plain sockets use zero-time readiness plus @c MSG_PEEK to
///          distinguish live data/would-block from orderly close.
/// @param conn Candidate idle connection.
/// @return One when the connection can be reused, otherwise zero.
static int http_conn_is_healthy(http_conn_t *conn) {
    if (!conn || conn->socket_fd == INVALID_SOCK)
        return 0;
    if (conn->http2)
        return rt_http2_conn_is_usable(conn->http2);
    if (conn->use_tls && conn->tls && rt_tls_has_buffered_data(conn->tls))
        return 1;

    {
        int ready = wait_socket(conn->socket_fd, 0, false);
        if (ready <= 0)
            return 1;
    }

    if (conn->use_tls)
        return 0;

    {
        uint8_t byte = 0;
        int peeked = recv(conn->socket_fd, (char *)&byte, 1, MSG_PEEK);
        if (peeked > 0)
            return 1;
        if (peeked == 0)
            return 0;
    }

    {
        int err = rt_socket_last_error();
        return rt_socket_error_is_interrupted(err) || rt_socket_error_is_would_block(err);
    }
}

/// @brief Detach one pool entry and restore its vacant sentinel state.
/// @details Only compact owning fields are moved so a complete 4 KiB HTTP read
///          buffer is not copied into each deferred-cleanup record.
/// @param entry Entry whose transport and key ownership are consumed.
/// @param discard Destination cleanup record.
static void http_conn_pool_entry_detach(http_conn_pool_entry_t *entry,
                                        http_conn_pool_discard_t *discard) {
    if (!entry || !discard)
        return;
    discard->socket_fd = entry->conn.socket_fd;
    discard->tls = entry->conn.tls;
    discard->http2 = entry->conn.http2;
    discard->key = entry->key;
    discard->use_tls = entry->conn.use_tls;
    memset(entry, 0, sizeof(*entry));
    entry->conn.socket_fd = INVALID_SOCK;
    entry->conn.pool_slot = -1;
}

/// @brief Close and free ownership previously detached from a pool entry.
/// @details HTTP/2 teardown and graceful TLS shutdown may execute callbacks or
///          perform network I/O, so callers must not hold the pool mutex.
/// @param discard Detached ownership record to consume.
static void http_conn_pool_discard_release(http_conn_pool_discard_t *discard) {
    if (!discard)
        return;
    if (discard->http2)
        rt_http2_conn_free(discard->http2);
    if (discard->use_tls && discard->tls)
        rt_tls_close(discard->tls);
    else if (discard->socket_fd != INVALID_SOCK)
        CLOSE_SOCKET(discard->socket_fd);
    free(discard->key);
    memset(discard, 0, sizeof(*discard));
    discard->socket_fd = INVALID_SOCK;
}

/// @brief Destroy one pool entry and restore its vacant sentinel state.
/// @param entry Entry whose transport and key ownership are consumed, or NULL.
static void http_conn_pool_entry_reset(http_conn_pool_entry_t *entry) {
    if (!entry)
        return;
    http_conn_pool_discard_t discard;
    http_conn_pool_entry_detach(entry, &discard);
    http_conn_pool_discard_release(&discard);
}

/// @brief Remove vacant trailing slots from a locked pool's logical extent.
/// @param pool Pool whose mutex is held by the caller.
static void http_conn_pool_trim_locked(http_conn_pool_t *pool) {
    while (pool->count > 0) {
        http_conn_pool_entry_t *tail = &pool->entries[pool->count - 1];
        if (tail->in_use || tail->key)
            break;
        memset(tail, 0, sizeof(*tail));
        tail->conn.socket_fd = INVALID_SOCK;
        tail->conn.pool_slot = -1;
        pool->count--;
    }
}

/// @brief Finalize an HTTP connection pool.
/// @details Closes every pooled transport, frees all keys, and destroys the
///          platform mutex when initialization completed.
/// @param obj Pool payload being finalized, or NULL.
static void http_conn_pool_finalize(void *obj) {
    if (!obj)
        return;
    http_conn_pool_t *pool = (http_conn_pool_t *)obj;
    for (int i = 0; i < pool->count; i++)
        http_conn_pool_entry_reset(&pool->entries[i]);
    if (pool->lock_initialized)
        HTTP_POOL_MUTEX_DESTROY(&pool->lock);
}

/// @brief Allocate an initialized managed HTTP connection pool.
/// @details Positive sizes below the hard 64-entry limit are honored; zero,
///          negative, and larger values select the hard limit. Every slot is
///          initialized with invalid transport sentinels before the mutex is
///          published.
/// @param max_size Requested maximum idle/in-use entry count.
/// @return Newly owned pool, or NULL after allocation or mutex initialization
///         failure.
void *rt_http_conn_pool_new(int64_t max_size) {
    http_conn_pool_t *pool = (http_conn_pool_t *)rt_obj_new_i64(RT_HTTP_CONN_POOL_CLASS_ID,
                                                                (int64_t)sizeof(http_conn_pool_t));
    if (!pool)
        return NULL;
    memset(pool, 0, sizeof(*pool));
    rt_obj_set_finalizer(pool, http_conn_pool_finalize);
    pool->max_size =
        (int)(max_size > 0 && max_size < HTTP_CONN_POOL_MAX_ENTRIES ? max_size
                                                                    : HTTP_CONN_POOL_MAX_ENTRIES);
    for (int i = 0; i < HTTP_CONN_POOL_MAX_ENTRIES; i++) {
        pool->entries[i].conn.socket_fd = INVALID_SOCK;
        pool->entries[i].conn.pool_slot = -1;
    }
    if (!http_pool_mutex_init(&pool->lock)) {
        http_conn_pool_release_ref(pool);
        rt_trap("HTTP: connection pool mutex initialization failed");
        return NULL;
    }
    pool->lock_initialized = 1;
    return pool;
}

/// @brief Close and remove every entry from an HTTP connection pool.
/// @details Takes a safe live retain before stable-identity validation, then
///          atomically detaches all entries under the pool mutex. Potentially
///          blocking transport shutdown and key release run after unlocking.
///          NULL is a no-op.
/// @param obj Managed pool receiver, or NULL.
void rt_http_conn_pool_clear(void *obj) {
    if (!obj)
        return;
    int retained = rt_heap_try_retain_live(obj);
    if (retained != 1 && retained != 2) {
        rt_trap(retained < 0 ? "HTTP: connection pool reference count overflow"
                             : "HTTP: invalid connection pool");
        return;
    }
    if (!rt_http_conn_pool_is_handle(obj)) {
        if (retained == 1)
            http_conn_pool_release_ref(obj);
        rt_trap("HTTP: invalid connection pool");
        return;
    }
    http_conn_pool_t *pool = (http_conn_pool_t *)obj;
    http_conn_pool_discard_t discards[HTTP_CONN_POOL_MAX_ENTRIES];
    size_t discard_count = 0;
    HTTP_POOL_MUTEX_LOCK(&pool->lock);
    pool->generation++;
    for (int i = 0; i < pool->count; i++) {
        http_conn_pool_entry_detach(&pool->entries[i], &discards[discard_count]);
        discard_count++;
    }
    pool->count = 0;
    HTTP_POOL_MUTEX_UNLOCK(&pool->lock);
    for (size_t i = 0; i < discard_count; i++)
        http_conn_pool_discard_release(&discards[i]);
    if (retained == 1)
        http_conn_pool_release_ref(obj);
}

/// Process-wide connection pool backing standalone `HttpReq.SetKeepAlive(true)`
/// requests. HttpClient/RestClient attach their own per-client pools; requests
/// built through the public `HttpReq` surface previously had no way to obtain
/// one, so the keep-alive flag could never reuse a socket. The global holds one
/// permanent reference (process-lifetime singleton; the pool itself is
/// mutex-guarded and safe to share across threads).
static void *g_http_default_conn_pool = NULL;
static volatile int g_http_default_pool_state = 0;

enum {
    HTTP_DEFAULT_POOL_UNINITIALIZED = 0,
    HTTP_DEFAULT_POOL_INITIALIZING = 1,
    HTTP_DEFAULT_POOL_READY = 2,
};

/// @brief Return the retryable process-wide standalone-request pool.
/// @details A portable atomic state gate permits exactly one initializer while
///          contenders yield. Construction traps reset the gate before being
///          re-raised, so transient OOM never permanently publishes NULL as a
///          successful once-only result. Release/acquire publication makes the
///          initialized managed object visible without a data race.
/// @return Process-lifetime pool, or NULL after a returning trap hook.
void *rt_http_default_connection_pool(void) {
    for (;;) {
        int state = rt_atomic_load_i32(&g_http_default_pool_state, __ATOMIC_ACQUIRE);
        if (state == HTTP_DEFAULT_POOL_READY)
            return g_http_default_conn_pool;
        if (state == HTTP_DEFAULT_POOL_INITIALIZING) {
            rt_thread_yield();
            continue;
        }

        int expected = HTTP_DEFAULT_POOL_UNINITIALIZED;
        if (!rt_atomic_compare_exchange_i32(&g_http_default_pool_state,
                                            &expected,
                                            HTTP_DEFAULT_POOL_INITIALIZING,
                                            __ATOMIC_ACQ_REL,
                                            __ATOMIC_ACQUIRE)) {
            continue;
        }

        void *volatile candidate = NULL;
        jmp_buf recovery;
        rt_trap_set_recovery(&recovery);
        if (RT_SETJMP(recovery) != 0) {
            char saved_error[256];
            const char *error = rt_trap_get_error();
            snprintf(saved_error,
                     sizeof(saved_error),
                     "%s",
                     error && error[0] ? error : "HTTP: default connection pool failed");
            rt_trap_clear_recovery();
            rt_atomic_store_i32(
                &g_http_default_pool_state, HTTP_DEFAULT_POOL_UNINITIALIZED, __ATOMIC_RELEASE);
            rt_trap(saved_error);
            return NULL;
        }
        candidate = rt_http_conn_pool_new(0);
        rt_trap_clear_recovery();
        if (!candidate) {
            rt_atomic_store_i32(
                &g_http_default_pool_state, HTTP_DEFAULT_POOL_UNINITIALIZED, __ATOMIC_RELEASE);
            return NULL;
        }
        g_http_default_conn_pool = (void *)candidate;
        rt_atomic_store_i32(&g_http_default_pool_state, HTTP_DEFAULT_POOL_READY, __ATOMIC_RELEASE);
        return (void *)candidate;
    }
}

/// @brief Evict idle HTTP keep-alive entries whose elapsed monotonic age exceeds the limit.
/// @param pool Locked pool to sweep.
/// @param now_ms Current monotonic milliseconds.
/// @param discards Fixed-capacity destination for detached ownership.
/// @param discard_capacity Number of available cleanup records.
/// @return Number of cleanup records populated.
static size_t http_conn_pool_evict_idle_locked(http_conn_pool_t *pool,
                                               int64_t now_ms,
                                               http_conn_pool_discard_t *discards,
                                               size_t discard_capacity) {
    size_t discard_count = 0;
    for (int i = 0; i < pool->count; i++) {
        http_conn_pool_entry_t *entry = &pool->entries[i];
        if (entry->in_use)
            continue;
        int64_t age_ms = now_ms >= entry->last_used_ms ? now_ms - entry->last_used_ms : 0;
        if (age_ms <= (int64_t)HTTP_CONN_POOL_IDLE_TIMEOUT_SEC * 1000)
            continue;
        if (discard_count >= discard_capacity)
            rt_abort("HTTP: connection pool discard capacity exhausted");
        http_conn_pool_entry_detach(entry, &discards[discard_count]);
        discard_count++;
    }
    http_conn_pool_trim_locked(pool);
    return discard_count;
}

/// @brief Check out a healthy idle connection matching an origin key.
/// @details Evicts expired entries while holding the mutex, then reserves and
///          detaches one matching transport. Socket health is probed only after
///          unlocking, preventing readiness races from blocking every pool
///          user. An unhealthy lease is closed outside the mutex and its slot
///          is cleared only when no concurrent pool clear changed generation.
/// @param obj Valid managed pool.
/// @param host Origin host.
/// @param port Origin port.
/// @param use_tls Nonzero for TLS.
/// @param tls_verify Nonzero for verified TLS.
/// @param out_conn Receives the checked-out connection and pool lease metadata.
/// @return One when a reusable match is acquired, otherwise zero.
static int http_conn_pool_acquire(
    void *obj, const char *host, int port, int use_tls, int tls_verify, http_conn_t *out_conn) {
    if (!host || !out_conn || !rt_http_conn_pool_is_handle(obj))
        return 0;

    http_conn_pool_t *pool = (http_conn_pool_t *)obj;
    char key[sizeof(out_conn->pool_key)];
    if (!http_make_pool_key(host, port, use_tls, tls_verify, key, sizeof(key)))
        return 0;

    for (;;) {
        http_conn_t candidate;
        http_conn_pool_discard_t discards[HTTP_CONN_POOL_MAX_ENTRIES];
        size_t discard_count = 0;
        int slot = -1;
        uint64_t generation = 0;
        memset(&candidate, 0, sizeof(candidate));
        candidate.socket_fd = INVALID_SOCK;
        candidate.pool_slot = -1;

        HTTP_POOL_MUTEX_LOCK(&pool->lock);
        discard_count = http_conn_pool_evict_idle_locked(
            pool, http_pool_now_ms(), discards, HTTP_CONN_POOL_MAX_ENTRIES);
        for (int i = 0; i < pool->count; i++) {
            http_conn_pool_entry_t *entry = &pool->entries[i];
            if (entry->in_use || !entry->key || strcmp(entry->key, key) != 0)
                continue;
            entry->in_use = 1;
            candidate = entry->conn;
            memset(&entry->conn, 0, sizeof(entry->conn));
            entry->conn.socket_fd = INVALID_SOCK;
            entry->conn.pool_slot = -1;
            slot = i;
            generation = pool->generation;
            break;
        }
        HTTP_POOL_MUTEX_UNLOCK(&pool->lock);
        for (size_t i = 0; i < discard_count; i++)
            http_conn_pool_discard_release(&discards[i]);

        if (slot < 0)
            return 0;

        if (http_conn_is_healthy(&candidate)) {
            candidate.pool = pool;
            candidate.pool_slot = slot;
            candidate.pool_generation = generation;
            candidate.reused_from_pool = 1;
            snprintf(candidate.pool_key, sizeof(candidate.pool_key), "%s", key);
            *out_conn = candidate;
            return 1;
        }

        HTTP_POOL_MUTEX_LOCK(&pool->lock);
        if (pool->generation == generation && slot < pool->count) {
            http_conn_pool_entry_t *entry = &pool->entries[slot];
            if (entry->in_use) {
                free(entry->key);
                entry->key = NULL;
                entry->last_used_ms = 0;
                entry->in_use = 0;
                memset(&entry->conn, 0, sizeof(entry->conn));
                entry->conn.socket_fd = INVALID_SOCK;
                entry->conn.pool_slot = -1;
            }
            http_conn_pool_trim_locked(pool);
        }
        HTTP_POOL_MUTEX_UNLOCK(&pool->lock);
        http_conn_close(&candidate);
    }
}

/// @brief Snapshot the clear-generation of a retained connection pool.
/// @param obj Valid managed pool retained by a request.
/// @return Current generation, or zero for an invalid pool.
static uint64_t http_conn_pool_generation_snapshot(void *obj) {
    if (!rt_http_conn_pool_is_handle(obj))
        return 0;
    http_conn_pool_t *pool = (http_conn_pool_t *)obj;
    HTTP_POOL_MUTEX_LOCK(&pool->lock);
    uint64_t generation = pool->generation;
    HTTP_POOL_MUTEX_UNLOCK(&pool->lock);
    return generation;
}

/// @brief Return or discard a checked-out pooled connection.
/// @details Invalid pool metadata, an empty key, an explicit nonreusable
///          result, a failed health probe, or a generation changed by
///          `ConnectionPool.Clear` closes the transport. Health checks and
///          transport closure run outside the mutex. Otherwise the connection
///          returns to its reserved slot, or occupies a new idle slot, with a
///          fresh monotonic timestamp.
/// @param conn Checked-out connection, consumed and reset by this call.
/// @param reusable Nonzero when HTTP framing permits another request.
static void http_conn_pool_release(http_conn_t *conn, int reusable) {
    if (!conn)
        return;

    http_conn_clear_deadline(conn);
    http_conn_pool_t *pool = (http_conn_pool_t *)conn->pool;
    if (!rt_http_conn_pool_is_handle(pool)) {
        http_conn_close(conn);
        memset(conn, 0, sizeof(*conn));
        conn->socket_fd = INVALID_SOCK;
        conn->pool_slot = -1;
        return;
    }
    if (conn->pool_key[0] == '\0') {
        http_conn_close(conn);
        memset(conn, 0, sizeof(*conn));
        conn->socket_fd = INVALID_SOCK;
        conn->pool_slot = -1;
        return;
    }

    int healthy = reusable && http_conn_is_healthy(conn);
    char *new_key = NULL;
    if (healthy && conn->pool_slot < 0) {
        new_key = strdup(conn->pool_key);
        if (!new_key)
            healthy = 0;
    }

    if (!healthy) {
        int slot = conn->pool_slot;
        HTTP_POOL_MUTEX_LOCK(&pool->lock);
        if (pool->generation == conn->pool_generation && slot >= 0 && slot < pool->count) {
            http_conn_pool_entry_t *entry = &pool->entries[slot];
            if (entry->in_use) {
                free(entry->key);
                entry->key = NULL;
                entry->last_used_ms = 0;
                entry->in_use = 0;
                memset(&entry->conn, 0, sizeof(entry->conn));
                entry->conn.socket_fd = INVALID_SOCK;
                entry->conn.pool_slot = -1;
            }
        }
        http_conn_pool_trim_locked(pool);
        HTTP_POOL_MUTEX_UNLOCK(&pool->lock);
        free(new_key);
        http_conn_close(conn);
        memset(conn, 0, sizeof(*conn));
        conn->socket_fd = INVALID_SOCK;
        conn->pool_slot = -1;
        return;
    }

    int stored = 0;
    HTTP_POOL_MUTEX_LOCK(&pool->lock);
    if (pool->generation == conn->pool_generation) {
        if (conn->pool_slot >= 0 && conn->pool_slot < pool->count) {
            http_conn_pool_entry_t *entry = &pool->entries[conn->pool_slot];
            if (entry->in_use && entry->key) {
                entry->conn = *conn;
                entry->last_used_ms = http_pool_now_ms();
                entry->in_use = 0;
                entry->conn.pool = NULL;
                entry->conn.pool_slot = -1;
                entry->conn.pool_generation = 0;
                stored = 1;
            }
        } else if (conn->pool_slot < 0 && new_key) {
            int slot = -1;
            for (int i = 0; i < pool->count; i++) {
                if (!pool->entries[i].in_use && !pool->entries[i].key) {
                    slot = i;
                    break;
                }
            }
            if (slot < 0 && pool->count < pool->max_size)
                slot = pool->count++;
            if (slot >= 0) {
                http_conn_pool_entry_t *entry = &pool->entries[slot];
                memset(entry, 0, sizeof(*entry));
                entry->conn = *conn;
                entry->key = new_key;
                new_key = NULL;
                entry->last_used_ms = http_pool_now_ms();
                entry->conn.pool = NULL;
                entry->conn.pool_slot = -1;
                entry->conn.pool_generation = 0;
                stored = 1;
            }
        }
    }
    HTTP_POOL_MUTEX_UNLOCK(&pool->lock);

    free(new_key);
    if (!stored)
        http_conn_close(conn);
    memset(conn, 0, sizeof(*conn));
    conn->socket_fd = INVALID_SOCK;
    conn->pool_slot = -1;
}

static RT_THREAD_LOCAL char g_http_tls_open_error[256];

/// @brief Store the current thread's TLS connection-opening diagnostic.
/// @param msg Diagnostic to copy, or NULL/empty to clear the buffer.
static void http_set_tls_open_error(const char *msg) {
    if (!msg || !*msg) {
        g_http_tls_open_error[0] = '\0';
        return;
    }
    snprintf(g_http_tls_open_error, sizeof(g_http_tls_open_error), "%s", msg);
}

static socket_t http_create_tcp_socket(const char *host,
                                       int port,
                                       int64_t deadline_us,
                                       int *err_code);
static void rt_http_res_finalize(void *obj);
rt_http_res_t *do_http_request(rt_http_req_t *req, int redirects_remaining);
static rt_http_res_t *do_http_request_deadline(rt_http_req_t *req,
                                               int redirects_remaining,
                                               int64_t deadline_us);
static int do_http_download_request_deadline(rt_http_req_t *req,
                                             int redirects_remaining,
                                             FILE *out,
                                             int64_t deadline_us);

/// @brief Test whether a request is eligible to use its attached pool.
/// @param req Candidate native request.
/// @return One when keep-alive is enabled and the attached pool is valid,
///         otherwise zero.
static int http_request_wants_pool(const rt_http_req_t *req) {
    return req && req->keep_alive && rt_http_conn_pool_is_handle(req->connection_pool);
}

/// @brief Test whether a method may be retried after stale pooled reuse.
/// @details Restricts automatic retry to the idempotent GET, HEAD, OPTIONS, and
///          DELETE methods so a stale connection cannot duplicate a body
///          mutation.
/// @param method NUL-terminated method token.
/// @return One for a retryable method, otherwise zero.
static int http_method_retryable_on_stale_reuse(const char *method) {
    return method && (strcmp(method, "GET") == 0 || strcmp(method, "HEAD") == 0 ||
                      strcmp(method, "OPTIONS") == 0 || strcmp(method, "DELETE") == 0);
}

/// @brief Acquire or establish the transport for one HTTP request.
/// @details First attempts a healthy origin/security-matched pool lease. New
///          HTTPS transports configure verification, timeout, and ALPN, perform
///          the TLS handshake, and create HTTP/2 state when `h2` is selected;
///          plain HTTP adopts the connected TCP socket directly. Successful new
///          connections are associated with the request pool when eligible.
/// @param req Fully parsed native request configuration.
/// @param conn Receives the initialized owned or pooled connection.
/// @param deadline_us Shared absolute monotonic request deadline, or zero.
/// @param err_out Optional classified runtime error code on failure.
/// @return One on success, otherwise zero after releasing partial transports.
static int http_open_connection(rt_http_req_t *req,
                                http_conn_t *conn,
                                int64_t deadline_us,
                                int *err_out) {
    if (!req || !conn) {
        if (err_out)
            *err_out = Err_NetworkError;
        return 0;
    }

    http_set_tls_open_error(NULL);

    memset(conn, 0, sizeof(*conn));
    conn->socket_fd = INVALID_SOCK;
    conn->pool_slot = -1;
    conn->tls_verify = req->tls_verify ? 1 : 0;
    conn->reused_from_pool = 0;
    conn->timeout_ms = req->timeout_ms;
    conn->deadline_us = deadline_us;
    conn->timed_out = 0;
    if (!http_make_pool_key(req->url.host,
                            req->url.port,
                            req->url.use_tls,
                            conn->tls_verify,
                            conn->pool_key,
                            sizeof(conn->pool_key)))
        conn->pool_key[0] = '\0';

    if (http_request_wants_pool(req) && http_conn_pool_acquire(req->connection_pool,
                                                               req->url.host,
                                                               req->url.port,
                                                               req->url.use_tls,
                                                               conn->tls_verify,
                                                               conn)) {
        conn->deadline_us = deadline_us;
        conn->timed_out = 0;
        if (!http_conn_prepare_io(conn)) {
            if (err_out)
                *err_out = Err_Timeout;
            http_conn_pool_release(conn, 0);
            return 0;
        }
        if (req->timeout_ms > 0 && conn->socket_fd != INVALID_SOCK) {
            set_socket_timeout(conn->socket_fd, conn->timeout_ms, true);
            set_socket_timeout(conn->socket_fd, conn->timeout_ms, false);
        }
        return 1;
    }

    if (req->url.use_tls) {
        int connect_err = Err_NetworkError;
        socket_t sock =
            http_create_tcp_socket(req->url.host, req->url.port, deadline_us, &connect_err);
        if (sock == INVALID_SOCK) {
            if (err_out)
                *err_out = connect_err;
            return 0;
        }
        int remaining_ms = http_deadline_remaining_ms(deadline_us);
        if (remaining_ms < 0) {
            CLOSE_SOCKET(sock);
            if (err_out)
                *err_out = Err_Timeout;
            return 0;
        }
        if (remaining_ms > 0) {
            set_socket_timeout(sock, remaining_ms, true);
            set_socket_timeout(sock, remaining_ms, false);
        }

        rt_tls_config_t tls_config;
        rt_tls_config_init(&tls_config);
        tls_config.hostname = req->url.host;
        tls_config.alpn_protocol = req->force_http1 ? "http/1.1" : "h2,http/1.1";
        tls_config.verify_cert = conn->tls_verify;
        if (remaining_ms > 0)
            tls_config.timeout_ms = remaining_ms;

        rt_tls_session_t *tls = rt_tls_new((intptr_t)sock, &tls_config);
        if (!tls) {
            http_set_tls_open_error(rt_tls_last_error());
            CLOSE_SOCKET(sock);
            if (err_out)
                *err_out = Err_TlsError;
            return 0;
        }
        rt_tls_set_internal_io_deadline(tls, deadline_us);
        if (rt_tls_handshake(tls) != RT_TLS_OK) {
            http_set_tls_open_error(rt_tls_get_error(tls));
            rt_tls_close(tls);
            if (err_out)
                *err_out = http_deadline_remaining_ms(deadline_us) < 0 ? Err_Timeout : Err_TlsError;
            return 0;
        }
        remaining_ms = http_deadline_remaining_ms(deadline_us);
        if (remaining_ms < 0) {
            rt_tls_close(tls);
            if (err_out)
                *err_out = Err_Timeout;
            return 0;
        }
        http_conn_init_tls(conn, tls);
        conn->deadline_us = deadline_us;
        conn->timeout_ms = remaining_ms > 0 ? remaining_ms : req->timeout_ms;
        if (strcmp(rt_tls_get_negotiated_alpn(tls), "h2") == 0) {
            rt_http2_io_t io;
            io.ctx = tls;
            io.read = http2_tls_read;
            io.write = http2_tls_write;
            conn->http2 = rt_http2_client_new(&io);
            if (!conn->http2) {
                http_set_tls_open_error("HTTP/2: transport allocation failed");
                rt_tls_close(tls);
                conn->tls = NULL;
                conn->socket_fd = INVALID_SOCK;
                if (err_out)
                    *err_out = Err_TlsError;
                return 0;
            }
        }
        conn->tls_verify = tls_config.verify_cert;
    } else {
        int connect_err = Err_NetworkError;
        socket_t sock =
            http_create_tcp_socket(req->url.host, req->url.port, deadline_us, &connect_err);
        if (sock == INVALID_SOCK) {
            if (err_out)
                *err_out = connect_err;
            return 0;
        }
        int remaining_ms = http_deadline_remaining_ms(deadline_us);
        if (remaining_ms < 0) {
            CLOSE_SOCKET(sock);
            if (err_out)
                *err_out = Err_Timeout;
            return 0;
        }
        if (remaining_ms > 0) {
            set_socket_timeout(sock, remaining_ms, true);
            set_socket_timeout(sock, remaining_ms, false);
        }
        http_conn_init_tcp(conn, sock);
        conn->deadline_us = deadline_us;
        conn->timeout_ms = remaining_ms > 0 ? remaining_ms : req->timeout_ms;
        conn->tls_verify = req->tls_verify ? 1 : 0;
    }

    if (!http_make_pool_key(req->url.host,
                            req->url.port,
                            req->url.use_tls,
                            conn->tls_verify,
                            conn->pool_key,
                            sizeof(conn->pool_key)))
        conn->pool_key[0] = '\0';
    if (http_request_wants_pool(req) && conn->pool_key[0] != '\0') {
        conn->pool = req->connection_pool;
        conn->pool_generation = http_conn_pool_generation_snapshot(req->connection_pool);
    }
    return 1;
}

/// @brief Release all heap fields owned by a parsed HTTP URL.
/// @param url Parsed URL whose host and path are freed and cleared.
void free_parsed_url(parsed_url_t *url) {
    if (url->host)
        free(url->host);
    if (url->path)
        free(url->path);
    url->host = NULL;
    url->path = NULL;
}

/// @brief Parse URL into components.
/// @details Accepts explicit `http://` and `https://` URLs. For compatibility
///          with earlier Zanna programs, a URL without any `://` scheme is
///          treated as an HTTP URL on port 80; unknown explicit schemes are
///          rejected. IPv6 authorities may be bracketed, fragments are removed
///          from the request target, and an omitted path becomes `/`.
/// @param url_str Nonempty NUL-terminated URL without CR or LF.
/// @param result Receives owned host/path fields and normalized port/TLS state;
///        it is cleared before parsing.
/// @return 0 on success, -1 on error.
int parse_url(const char *url_str, parsed_url_t *result) {
    memset(result, 0, sizeof(*result));
    result->port = 80;
    result->use_tls = 0;

    if (!url_str || !*url_str)
        return -1;
    for (const unsigned char *p = (const unsigned char *)url_str; *p; ++p) {
        if (*p == '\r' || *p == '\n')
            return -1;
    }

    // Check for http:// or https:// prefix; reject any other scheme
    if (strncasecmp(url_str, "http://", 7) == 0) {
        url_str += 7;
        result->use_tls = 0;
        result->port = 80;
    } else if (strncasecmp(url_str, "https://", 8) == 0) {
        url_str += 8;
        result->use_tls = 1;
        result->port = 443;
    } else if (strstr(url_str, "://") != NULL) {
        // An unrecognized scheme (e.g. ftp://, ws://) — reject rather than
        // silently defaulting to HTTP on port 80.
        return -1;
    }

    const char *p = url_str;
    if (*p == '[') {
        const char *bracket_end = strchr(p + 1, ']');
        if (!bracket_end)
            return -1;
        size_t host_len = (size_t)(bracket_end - (p + 1));
        if (host_len == 0)
            return -1;
        result->host = (char *)malloc(host_len + 1);
        if (!result->host)
            return -1;
        memcpy(result->host, p + 1, host_len);
        result->host[host_len] = '\0';
        p = bracket_end + 1;
    } else {
        const char *host_end = p;
        while (*host_end && *host_end != ':' && *host_end != '/' && *host_end != '?' &&
               *host_end != '#')
            host_end++;

        size_t host_len = (size_t)(host_end - p);
        if (host_len == 0)
            return -1;

        result->host = (char *)malloc(host_len + 1);
        if (!result->host)
            return -1;
        memcpy(result->host, p, host_len);
        result->host[host_len] = '\0';
        p = host_end;
    }

    if (!http_host_is_valid(result->host)) {
        free_parsed_url(result);
        return -1;
    }

    if (*p != '\0' && *p != ':' && *p != '/' && *p != '?' && *p != '#') {
        free_parsed_url(result);
        return -1;
    }

    // Parse port if present
    if (*p == ':') {
        p++;
        uint64_t parsed_port = 0;
        if (*p < '0' || *p > '9') {
            free_parsed_url(result);
            return -1;
        }
        while (*p >= '0' && *p <= '9') {
            parsed_port = parsed_port * 10u + (uint64_t)(*p - '0');
            if (parsed_port > 65535u) {
                free_parsed_url(result);
                return -1;
            }
            p++;
        }
        if (parsed_port == 0u || (*p != '\0' && *p != '/' && *p != '?' && *p != '#')) {
            free_parsed_url(result);
            return -1;
        }
        result->port = (int)parsed_port;
    }

    // Parse request-target path + query (fragments are never sent on the wire)
    if (*p == '/') {
        const char *path_end = p;
        while (*path_end && *path_end != '#')
            path_end++;
        size_t path_len = (size_t)(path_end - p);
        result->path = (char *)malloc(path_len + 1);
        if (!result->path) {
            free_parsed_url(result);
            return -1;
        }
        memcpy(result->path, p, path_len);
        result->path[path_len] = '\0';
    } else if (*p == '?') {
        const char *query_end = p;
        while (*query_end && *query_end != '#')
            query_end++;
        size_t query_len = (size_t)(query_end - p);
        result->path = (char *)malloc(query_len + 2);
        if (!result->path) {
            free_parsed_url(result);
            return -1;
        }
        result->path[0] = '/';
        memcpy(result->path + 1, p, query_len);
        result->path[query_len + 1] = '\0';
    } else {
        result->path = (char *)malloc(2);
        if (!result->path) {
            free_parsed_url(result);
            return -1;
        }
        result->path[0] = '/';
        result->path[1] = '\0';
    }

    if (!http_request_target_is_valid(result->path)) {
        free_parsed_url(result);
        return -1;
    }

    return 0;
}

/// @brief Release a complete native HTTP header list.
/// @param headers Owned list head; NULL is a no-op.
void free_headers(http_header_t *headers) {
    while (headers) {
        http_header_t *next = headers->next;
        free(headers->name);
        free(headers->value);
        free(headers);
        headers = next;
    }
}

/// @brief Best-effort enable TCP_NODELAY on a connected HTTP socket.
/// @param sock Connected native TCP socket.
static void http_set_nodelay(socket_t sock) {
    int flag = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&flag, sizeof(flag));
}

/// @brief Set or clear native non-blocking mode for a connect operation.
/// @param sock Native socket to configure.
/// @param nonblocking true to enable non-blocking mode, false to restore it.
/// @return true when the platform adapter succeeds, otherwise false.
static bool http_set_nonblocking(socket_t sock, bool nonblocking) {
    return rt_socket_set_nonblocking(sock, nonblocking);
}

/// @brief Connect a socket with an optional timeout.
/// @details Positive timeouts use non-blocking connect, readiness waiting, and
///          @c SO_ERROR before restoring blocking mode. Zero performs a normal
///          blocking connect. The caller owns and closes the socket on failure.
/// @param sock Unconnected native socket.
/// @param addr Destination socket address.
/// @param addrlen Size of @p addr.
/// @param timeout_ms Nonnegative timeout in milliseconds; zero blocks.
/// @param err_out Optional output for the native error, including
///        @c ETIMEDOUT on readiness expiry.
/// @return true when connected in blocking mode, otherwise false.
static bool http_connect_socket_with_timeout(
    socket_t sock, const struct sockaddr *addr, socklen_t addrlen, int timeout_ms, int *err_out) {
    if (err_out)
        *err_out = 0;

    if (timeout_ms > 0) {
        if (!http_set_nonblocking(sock, true)) {
            if (err_out)
                *err_out = GET_LAST_ERROR();
            return false;
        }

        int connect_result = connect(sock, addr, addrlen);
        if (connect_result == SOCK_ERROR) {
            int err = GET_LAST_ERROR();
            if (rt_socket_error_is_in_progress(err)) {
                int ready = wait_socket(sock, timeout_ms, true);
                if (ready <= 0) {
                    if (err_out)
                        *err_out = ready == 0 ? ETIMEDOUT : GET_LAST_ERROR();
                    http_set_nonblocking(sock, false);
                    return false;
                }

                int so_error = 0;
                socklen_t len = sizeof(so_error);
                getsockopt(sock, SOL_SOCKET, SO_ERROR, (char *)&so_error, &len);
                if (so_error != 0) {
                    if (err_out)
                        *err_out = so_error;
                    http_set_nonblocking(sock, false);
                    return false;
                }
            } else {
                if (err_out)
                    *err_out = err;
                http_set_nonblocking(sock, false);
                return false;
            }
        }

        if (!http_set_nonblocking(sock, false)) {
            if (err_out)
                *err_out = GET_LAST_ERROR();
            return false;
        }
        return true;
    }

    if (connect(sock, addr, addrlen) == SOCK_ERROR) {
        if (err_out)
            *err_out = GET_LAST_ERROR();
        return false;
    }

    return true;
}

/// @brief Open a raw TCP socket to an HTTP origin without trapping.
/// @details Resolves IPv4 and IPv6 candidates, shares one absolute timeout
///          budget across every attempt, suppresses SIGPIPE, and best-effort
///          enables TCP_NODELAY on the first successful connection.
/// @param host Origin hostname or numeric address.
/// @param port Origin port.
/// @param deadline_us Absolute monotonic connect deadline; zero blocks.
/// @param err_code Optional output for a classified runtime network error.
/// @return Connected owned socket, or @c INVALID_SOCK on failure.
static socket_t http_create_tcp_socket(const char *host,
                                       int port,
                                       int64_t deadline_us,
                                       int *err_code) {
    if (err_code)
        *err_code = Err_NetworkError;

    struct addrinfo hints, *res = NULL, *rp = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) {
        if (err_code)
            *err_code = Err_HostNotFound;
        return INVALID_SOCK;
    }

    socket_t sock = INVALID_SOCK;
    int last_err = 0;
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        int remaining_ms = http_deadline_remaining_ms(deadline_us);
        if (remaining_ms < 0) {
            last_err = ETIMEDOUT;
            break;
        }

        socket_t candidate = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (candidate == INVALID_SOCK)
            continue;

        remaining_ms = http_deadline_remaining_ms(deadline_us);
        if (remaining_ms < 0) {
            last_err = ETIMEDOUT;
            CLOSE_SOCKET(candidate);
            break;
        }
        suppress_sigpipe(candidate);
        if (http_connect_socket_with_timeout(
                candidate, rp->ai_addr, (socklen_t)rp->ai_addrlen, remaining_ms, &last_err)) {
            sock = candidate;
            break;
        }

        CLOSE_SOCKET(candidate);
    }
    freeaddrinfo(res);

    if (sock == INVALID_SOCK) {
        if (http_deadline_remaining_ms(deadline_us) < 0)
            last_err = ETIMEDOUT;
        if (err_code) {
            if (last_err == CONN_REFUSED)
                *err_code = Err_ConnectionRefused;
            else if (rt_socket_error_is_timeout(last_err))
                *err_code = Err_Timeout;
            else
                *err_code = Err_NetworkError;
        }
        return INVALID_SOCK;
    }

    http_set_nodelay(sock);
    return sock;
}

/// @brief Format and raise a classified TLS failure.
/// @details Appends an explicit detail when supplied, otherwise uses the
///          thread's latest TLS diagnostic, then raises @c Err_TlsError.
/// @param prefix Stable high-level failure description.
/// @param detail Optional operation-specific TLS diagnostic.
static void http_trap_tls_error(const char *prefix, const char *detail) {
    const char *tls_err = detail && *detail ? detail : rt_tls_last_error();
    if (tls_err && *tls_err) {
        char msg[512];
        snprintf(msg, sizeof(msg), "%s: %s", prefix, tls_err);
        rt_trap_net(msg, Err_TlsError);
    }
    rt_trap_net(prefix, Err_TlsError);
}

/// @brief Parse a strict decimal Content-Length value.
/// @details Permits surrounding spaces or tabs but rejects signs, missing
///          digits, trailing syntax, and @c size_t overflow.
/// @param text NUL-terminated header value.
/// @param out_len Receives the parsed length on success.
/// @return Zero on success, otherwise -1.
static int parse_content_length_strict(const char *text, size_t *out_len) {
    const unsigned char *p = (const unsigned char *)text;
    size_t value = 0;
    int saw_digit = 0;

    while (*p == ' ' || *p == '\t')
        p++;
    if (*p == '\0' || *p == '-')
        return -1;

    while (*p) {
        if (isdigit(*p)) {
            unsigned digit = (unsigned)(*p - '0');
            if (value > (SIZE_MAX - digit) / 10)
                return -1;
            value = value * 10 + digit;
            saw_digit = 1;
            p++;
            continue;
        }
        break;
    }

    while (*p == ' ' || *p == '\t')
        p++;
    if (*p != '\0' || !saw_digit)
        return -1;

    *out_len = value;
    return 0;
}

/// @brief Determine whether HTTP semantics forbid a response body.
/// @param req Request whose method is examined.
/// @param status Numeric response status.
/// @return One for HEAD responses, informational responses, 204, or 304;
///         otherwise zero.
static int response_has_no_body(const rt_http_req_t *req, int status) {
    if (strcmp(req->method, "HEAD") == 0)
        return 1;
    if ((status >= 100 && status < 200) || status == 204 || status == 304)
        return 1;
    return 0;
}

/// @brief Serialize a parsed request URL as an absolute URL.
/// @details Selects HTTP/HTTPS, brackets bare IPv6 hosts, omits the scheme's
///          default port, includes the request target, and checks every size
///          accumulation for overflow.
/// @param url Fully initialized parsed request URL.
/// @return Newly allocated NUL-terminated URL, or NULL on overflow or
///         allocation failure.
static char *build_absolute_url(const parsed_url_t *url) {
    const char *scheme = url->use_tls ? "https" : "http";
    int default_port = url->use_tls ? 443 : 80;
    size_t scheme_len = strlen(scheme);
    size_t host_len = strlen(url->host);
    size_t path_len = strlen(url->path);
    int include_port = url->port != default_port;
    size_t out_len = 0;

    if (scheme_len > SIZE_MAX - 3 || host_len > SIZE_MAX - scheme_len - 3)
        return NULL;
    out_len = scheme_len + 3 + host_len;
    if (host_needs_brackets(url->host)) {
        if (out_len > SIZE_MAX - 2)
            return NULL;
        out_len += 2;
    }
    if (include_port) {
        if (out_len > SIZE_MAX - 16)
            return NULL;
        out_len += 16;
    }
    if (path_len > SIZE_MAX - out_len)
        return NULL;
    out_len += path_len;
    if (out_len == SIZE_MAX)
        return NULL;

    char *full = (char *)malloc(out_len + 1);
    if (!full)
        return NULL;

    if (include_port) {
        snprintf(full,
                 out_len + 1,
                 host_needs_brackets(url->host) ? "%s://[%s]:%d%s" : "%s://%s:%d%s",
                 scheme,
                 url->host,
                 url->port,
                 url->path);
    } else {
        snprintf(full,
                 out_len + 1,
                 host_needs_brackets(url->host) ? "%s://[%s]%s" : "%s://%s%s",
                 scheme,
                 url->host,
                 url->path);
    }

    return full;
}

/// @brief Identify credentials that must not cross redirect origin boundaries.
/// @param name Header field name, compared case-insensitively.
/// @return One for authorization, cookie, API-key, or access-token headers;
///         otherwise zero.
int8_t rt_http_header_is_sensitive_for_cross_origin_redirect(const char *name) {
    if (!name)
        return 0;
    return strcasecmp(name, "Authorization") == 0 || strcasecmp(name, "Proxy-Authorization") == 0 ||
           strcasecmp(name, "Cookie") == 0 || strcasecmp(name, "Cookie2") == 0 ||
           strcasecmp(name, "X-API-Key") == 0 || strcasecmp(name, "Api-Key") == 0 ||
           strcasecmp(name, "ApiKey") == 0 || strcasecmp(name, "X-Auth-Token") == 0 ||
           strcasecmp(name, "X-Access-Token") == 0;
}

/// @brief Compare two URL Strings by normalized HTTP origin.
/// @details Parses both URLs and compares scheme and host case-insensitively
///          plus the parsed numeric port. All temporary managed objects are
///          released before return.
/// @param lhs First URL String.
/// @param rhs Second URL String.
/// @return One when scheme, host, and port match, otherwise zero.
int8_t rt_http_url_has_same_origin(rt_string lhs, rt_string rhs) {
    int same_origin = 0;
    void *lhs_parsed = NULL;
    void *rhs_parsed = NULL;
    rt_string lhs_scheme = NULL;
    rt_string rhs_scheme = NULL;
    rt_string lhs_host = NULL;
    rt_string rhs_host = NULL;
    const char *lhs_scheme_cstr = NULL;
    const char *rhs_scheme_cstr = NULL;
    const char *lhs_host_cstr = NULL;
    const char *rhs_host_cstr = NULL;
    int64_t lhs_port = 0;
    int64_t rhs_port = 0;

    if (!lhs || !rhs)
        return 0;

    lhs_parsed = rt_url_parse(lhs);
    rhs_parsed = rt_url_parse(rhs);
    if (!lhs_parsed || !rhs_parsed)
        goto cleanup;

    lhs_scheme = rt_url_scheme(lhs_parsed);
    rhs_scheme = rt_url_scheme(rhs_parsed);
    lhs_host = rt_url_host(lhs_parsed);
    rhs_host = rt_url_host(rhs_parsed);
    lhs_scheme_cstr = rt_string_cstr(lhs_scheme);
    rhs_scheme_cstr = rt_string_cstr(rhs_scheme);
    lhs_host_cstr = rt_string_cstr(lhs_host);
    rhs_host_cstr = rt_string_cstr(rhs_host);
    lhs_port = rt_url_port(lhs_parsed);
    rhs_port = rt_url_port(rhs_parsed);

    if (lhs_scheme_cstr && rhs_scheme_cstr && lhs_host_cstr && rhs_host_cstr &&
        strcasecmp(lhs_scheme_cstr, rhs_scheme_cstr) == 0 &&
        strcasecmp(lhs_host_cstr, rhs_host_cstr) == 0 && lhs_port == rhs_port) {
        same_origin = 1;
    }

cleanup:
    rt_string_unref(rhs_host);
    rt_string_unref(lhs_host);
    rt_string_unref(rhs_scheme);
    rt_string_unref(lhs_scheme);
    if (rhs_parsed && rt_obj_release_check0(rhs_parsed))
        rt_obj_free(rhs_parsed);
    if (lhs_parsed && rt_obj_release_check0(lhs_parsed))
        rt_obj_free(lhs_parsed);
    return same_origin ? 1 : 0;
}

/// @brief Determine whether a redirect target must be treated as cross-origin.
/// @details Builds the current absolute URL, resolves the Location value, and
///          compares origins. Any allocation, parsing, or resolution failure is
///          treated as cross-origin so sensitive headers are stripped before
///          following the redirect.
/// @param current Parsed URL for the request being redirected.
/// @param location Raw Location header value.
/// @return Nonzero when the target is cross-origin or origin comparison failed.
static int redirect_cross_origin_or_unknown(const parsed_url_t *current, const char *location) {
    char *current_full = NULL;
    rt_string current_url = NULL;
    rt_string location_url = NULL;
    rt_string next_url = NULL;
    int cross_origin = 1;

    if (!current || !location)
        return 1;
    current_full = build_absolute_url(current);
    if (!current_full)
        goto done;
    current_url = rt_string_from_bytes(current_full, strlen(current_full));
    location_url = rt_string_from_bytes(location, strlen(location));
    if (!current_url || !location_url)
        goto done;
    next_url = rt_http_resolve_redirect_url(current_url, location_url);
    if (!next_url)
        goto done;
    cross_origin = !rt_http_url_has_same_origin(current_url, next_url);

done:
    if (next_url)
        rt_string_unref(next_url);
    if (location_url)
        rt_string_unref(location_url);
    if (current_url)
        rt_string_unref(current_url);
    free(current_full);
    return cross_origin;
}

/// @brief Resolve an HTTP Location value against the current absolute URL.
/// @details Absolute locations pass through, scheme-relative locations inherit
///          the current scheme, and other references use the Url resolver.
///          Empty Location returns an owned empty String.
/// @param current_url Current absolute request URL.
/// @param location Redirect Location reference.
/// @return Newly owned absolute URL String.
rt_string rt_http_resolve_redirect_url(rt_string current_url, rt_string location) {
    const char *location_cstr = rt_string_cstr(location);
    if (!location_cstr || !*location_cstr)
        return rt_string_from_bytes("", 0);

    if (strstr(location_cstr, "://"))
        return rt_string_from_bytes(location_cstr, strlen(location_cstr));

    if (strncmp(location_cstr, "//", 2) == 0) {
        void *base = rt_url_parse(current_url);
        rt_string scheme = rt_url_scheme(base);
        const char *scheme_cstr = rt_string_cstr(scheme);
        size_t scheme_len = scheme_cstr ? strlen(scheme_cstr) : 0;
        size_t out_len = scheme_len + strlen(location_cstr) + 1;
        char *absolute = (char *)malloc(out_len + 1);
        rt_string full;

        if (!absolute) {
            rt_string_unref(scheme);
            if (base && rt_obj_release_check0(base))
                rt_obj_free(base);
            return rt_string_from_bytes(location_cstr, strlen(location_cstr));
        }

        snprintf(absolute, out_len + 1, "%s:%s", scheme_cstr ? scheme_cstr : "http", location_cstr);
        full = rt_string_from_bytes(absolute, strlen(absolute));
        free(absolute);
        rt_string_unref(scheme);
        if (base && rt_obj_release_check0(base))
            rt_obj_free(base);
        return full;
    }

    {
        void *base = rt_url_parse(current_url);
        void *resolved = rt_url_resolve(base, location);
        rt_string full = rt_url_full(resolved);
        if (resolved && rt_obj_release_check0(resolved))
            rt_obj_free(resolved);
        if (base && rt_obj_release_check0(base))
            rt_obj_free(base);
        return full;
    }
}

/// @brief Resolve and transactionally replace a parsed redirect target.
/// @details Serializes the current target, resolves @p location through the
///          managed Url API, parses the result into independent native storage,
///          and replaces @p current only after complete success.
/// @param current Parsed URL to replace on success.
/// @param location Nonempty raw Location header value.
/// @return Zero on success, otherwise -1 with @p current unchanged.
static int resolve_redirect_target(parsed_url_t *current, const char *location) {
    int ok = -1;
    char *current_full = NULL;
    rt_string current_rt = NULL;
    rt_string location_rt = NULL;
    rt_string resolved_full = NULL;
    parsed_url_t next = {0};

    if (!location || !*location)
        return -1;

    current_full = build_absolute_url(current);
    if (!current_full)
        return -1;

    current_rt = rt_string_from_bytes(current_full, strlen(current_full));
    location_rt = rt_string_from_bytes(location, strlen(location));
    resolved_full = rt_http_resolve_redirect_url(current_rt, location_rt);

    if (resolved_full && parse_url(rt_string_cstr(resolved_full), &next) == 0) {
        free_parsed_url(current);
        *current = next;
        memset(&next, 0, sizeof(next));
        ok = 0;
    }

    if (resolved_full)
        rt_string_unref(resolved_full);
    if (location_rt)
        rt_string_unref(location_rt);
    if (current_rt)
        rt_string_unref(current_rt);
    free(current_full);
    free_parsed_url(&next);
    return ok;
}

/// @brief Finalize a managed HTTP response.
/// @details Frees native status/body storage, releases the owned headers Map,
///          and clears scalar state. NULL is a no-op.
/// @param obj HttpRes payload being finalized, or NULL.
static void rt_http_res_finalize(void *obj) {
    if (!obj)
        return;
    rt_http_res_t *res = (rt_http_res_t *)obj;
    free(res->status_text);
    res->status_text = NULL;
    free(res->body);
    res->body = NULL;
    res->body_len = 0;
    if (res->headers && rt_obj_release_check0(res->headers))
        rt_obj_free(res->headers);
    res->headers = NULL;
    res->status = 0;
}

/// @brief Detect CR or LF bytes used in HTTP injection attempts.
/// @param s Non-NULL NUL-terminated string.
/// @return true when either line-break byte is present, otherwise false.
static bool has_crlf(const char *s) {
    for (; *s; s++) {
        if (*s == '\r' || *s == '\n')
            return true;
    }
    return false;
}

/// @brief Allocate a complete detached request-header node.
/// @details Validation occurs before allocation. The node is not linked until
///          both native string copies exist, making it suitable for atomic
///          append and replacement operations.
/// @param name HTTP field-name token.
/// @param value Field value without CR or LF bytes.
/// @return Detached owned node, or NULL on invalid input/OOM.
static http_header_t *http_header_new(const char *name, const char *value) {
    if (!name || !value || !http_header_field_name_is_token(name) || has_crlf(value))
        return NULL;
    http_header_t *h = (http_header_t *)malloc(sizeof(http_header_t));
    if (!h)
        return NULL;
    h->name = strdup(name);
    if (!h->name) {
        free(h);
        return NULL;
    }
    h->value = strdup(value);
    if (!h->value) {
        free(h->name);
        free(h);
        return NULL;
    }
    h->next = NULL;
    return h;
}

/// @brief Prepend a validated header while preserving request state on failure.
/// @param req Request that owns the header list.
/// @param name HTTP field-name token.
/// @param value Field value without CR or LF.
/// @return One after publication, otherwise zero with the list unchanged.
int add_header(rt_http_req_t *req, const char *name, const char *value) {
    if (!req)
        return 0;
    http_header_t *h = http_header_new(name, value);
    if (!h)
        return 0;
    h->next = req->headers;
    req->headers = h;
    return 1;
}

/// @brief Atomically replace all case-insensitive matches for one header name.
/// @details Allocates the replacement before removing old fields so validation
///          or allocation failure leaves the request unchanged.
/// @param req Request that owns the header list.
/// @param name HTTP field-name token.
/// @param value Replacement field value without CR or LF.
/// @return One after replacement, otherwise zero with prior fields preserved.
int set_header(rt_http_req_t *req, const char *name, const char *value) {
    if (!req)
        return 0;
    http_header_t *replacement = http_header_new(name, value);
    if (!replacement)
        return 0;
    remove_header(req, name);
    replacement->next = req->headers;
    req->headers = replacement;
    return 1;
}

/// @brief Check whether a request contains a header name.
/// @param req Request whose list is searched.
/// @param name Header name compared case-insensitively.
/// @return true on the first match, otherwise false.
bool has_header(rt_http_req_t *req, const char *name) {
    if (!req || !name)
        return false;
    for (http_header_t *h = req->headers; h; h = h->next) {
        if (h->name && strcasecmp(h->name, name) == 0)
            return true;
    }
    return false;
}

/// @brief Remove every request header matching a name case-insensitively.
/// @param req Request whose owned list is mutated.
/// @param name Header name to remove.
void remove_header(rt_http_req_t *req, const char *name) {
    if (!req || !name)
        return;
    http_header_t **link = &req->headers;
    while (*link) {
        http_header_t *h = *link;
        if (strcasecmp(h->name, name) == 0) {
            *link = h->next;
            free(h->name);
            free(h->value);
            free(h);
        } else {
            link = &h->next;
        }
    }
}

/// @brief Release one managed key snapshot used by header-map transactions.
/// @param keys Caller-owned Seq returned by `rt_map_keys`, or NULL.
static void http_header_map_release_keys(void *keys) {
    if (keys && rt_obj_release_check0(keys))
        rt_obj_free(keys);
}

/// @brief Remove every case-insensitive header-map match transactionally.
/// @details A complete key snapshot is required before the first mutation.
///          Allocation traps are contained and the snapshot is always released.
/// @param map Header-name-keyed managed Map.
/// @param name NUL-terminated HTTP field name.
/// @return One on success (including a miss); zero without mutation on failure.
int rt_http_header_map_remove_ci(void *map, const char *name) {
    if (!map || !name)
        return 0;
    void *volatile keys = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        rt_trap_clear_recovery();
        http_header_map_release_keys((void *)keys);
        return 0;
    }
    keys = rt_map_keys(map);
    if (!keys) {
        rt_trap_clear_recovery();
        return 0;
    }
    int64_t len = rt_seq_len(keys);
    for (int64_t i = 0; i < len; i++) {
        rt_string key = (rt_string)rt_seq_get(keys, i); // borrowed
        const char *key_cstr = key ? rt_string_cstr(key) : NULL;
        if (key_cstr && strcasecmp(key_cstr, name) == 0)
            rt_map_remove(map, key);
    }
    rt_trap_clear_recovery();
    http_header_map_release_keys((void *)keys);
    return 1;
}

/// @brief Insert a replacement before removing differently cased aliases.
/// @details The pre-mutation key snapshot ensures snapshot OOM changes nothing.
///          `rt_map_set` either publishes the new exact spelling or traps before
///          aliases are removed. A recovery frame converts all managed failures
///          to a zero return so a lock-owning caller can unlock safely.
/// @param map Header-name-keyed managed Map.
/// @param name Exact spelling to publish.
/// @param value Managed value retained by the Map.
/// @return One on complete replacement; zero with prior aliases preserved.
int rt_http_header_map_set_ci(void *map, rt_string name, void *value) {
    if (!map || !name)
        return 0;
    const char *name_cstr = rt_string_cstr(name);
    if (!name_cstr)
        return 0;

    void *volatile keys = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        rt_trap_clear_recovery();
        http_header_map_release_keys((void *)keys);
        return 0;
    }
    keys = rt_map_keys(map);
    if (!keys) {
        rt_trap_clear_recovery();
        return 0;
    }
    rt_map_set(map, name, value);
    if (!rt_map_has(map, name)) {
        rt_trap_clear_recovery();
        http_header_map_release_keys((void *)keys);
        return 0;
    }

    int64_t len = rt_seq_len((void *)keys);
    for (int64_t i = 0; i < len; i++) {
        rt_string key = (rt_string)rt_seq_get((void *)keys, i);
        const char *key_cstr = key ? rt_string_cstr(key) : NULL;
        if (key_cstr && strcmp(key_cstr, name_cstr) != 0 && strcasecmp(key_cstr, name_cstr) == 0) {
            rt_map_remove(map, key);
        }
    }
    rt_trap_clear_recovery();
    http_header_map_release_keys((void *)keys);
    return 1;
}

/// @brief Test a comma-separated HTTP field value for a token.
/// @details Ignores surrounding optional whitespace, compares
///          case-insensitively, and ignores semicolon-delimited token
///          parameters.
/// @param value NUL-terminated field value.
/// @param token Exact token to find.
/// @return One when present, otherwise zero.
int8_t rt_http_header_value_has_token(const char *value, const char *token) {
    size_t token_len;
    if (!value || !token)
        return 0;

    token_len = strlen(token);
    while (*value) {
        const char *segment_start;
        const char *segment_end;
        size_t segment_len;

        while (*value == ' ' || *value == '\t' || *value == ',')
            value++;
        if (*value == '\0')
            break;

        segment_start = value;
        while (*value && *value != ',' && *value != ';')
            value++;
        segment_end = value;
        while (segment_end > segment_start && (segment_end[-1] == ' ' || segment_end[-1] == '\t')) {
            segment_end--;
        }
        segment_len = (size_t)(segment_end - segment_start);
        if (segment_len == token_len && strncasecmp(segment_start, token, token_len) == 0)
            return 1;

        while (*value && *value != ',')
            value++;
    }

    return 0;
}

/// @brief Validate the supported Transfer-Encoding grammar.
/// @details This runtime accepts exactly one `chunked` coding with optional
///          surrounding whitespace and rejects duplicates, empty elements, and
///          every unsupported coding.
/// @param value NUL-terminated Transfer-Encoding field value.
/// @param chunked_out Optional output, cleared first and set to one on success.
/// @return One only for the supported chunked form, otherwise zero.
static int parse_transfer_encoding_supported(const char *value, int *chunked_out) {
    const char *p = value;
    const char *end = value ? value + strlen(value) : NULL;
    int saw_chunked = 0;

    if (chunked_out)
        *chunked_out = 0;
    if (!value)
        return 0;

    while (p < end) {
        const char *token_start;
        const char *token_end;

        while (p < end && (*p == ' ' || *p == '\t'))
            p++;
        if (p >= end)
            return 0;

        token_start = p;
        while (p < end && *p != ',')
            p++;
        token_end = p;
        while (token_end > token_start && (token_end[-1] == ' ' || token_end[-1] == '\t'))
            token_end--;
        if (token_end == token_start)
            return 0;
        if ((size_t)(token_end - token_start) != 7 || strncasecmp(token_start, "chunked", 7) != 0) {
            return 0;
        }
        if (saw_chunked)
            return 0;
        saw_chunked = 1;

        if (p < end) {
            p++;
            while (p < end && (*p == ' ' || *p == '\t'))
                p++;
            if (p >= end)
                return 0;
        }
    }

    if (!saw_chunked)
        return 0;
    if (chunked_out)
        *chunked_out = 1;
    return 1;
}

/// @brief Expose Transfer-Encoding validation to focused runtime tests.
/// @param value NUL-terminated field value.
/// @param chunked_out Optional output receiving the parsed chunked flag.
/// @return One for the supported grammar, otherwise zero.
int rt_http_transfer_encoding_supported_for_test(const char *value, int *chunked_out) {
    return parse_transfer_encoding_supported(value, chunked_out);
}

/// @brief Copy a managed String into an empty native request body.
/// @details Uses the String's exact logical byte length and leaves the request
///          unchanged for NULL or empty input. Invalid lengths and allocation
///          failure trap.
/// @param req Native request that receives ownership of the copied buffer.
/// @param body Managed String to copy.
void set_request_body_from_string(rt_http_req_t *req, rt_string body) {
    const char *body_str = body ? rt_string_cstr(body) : NULL;
    int64_t body_len64 = 0;
    size_t body_len = 0;

    if (!req || !body_str)
        return;
    body_len64 = rt_str_len(body);
    if (body_len64 < 0 || (uint64_t)body_len64 > (uint64_t)SIZE_MAX) {
        rt_trap("HTTP: invalid body length");
        return;
    }
    body_len = (size_t)body_len64;
    if (body_len == 0)
        return;
    req->body = (uint8_t *)malloc(body_len);
    if (!req->body) {
        rt_trap("HTTP: memory allocation failed");
        return;
    }
    memcpy(req->body, body_str, body_len);
    req->body_len = body_len;
}

/// @brief Remove every request header accepted by a predicate.
/// @param req Request whose owned header list is filtered.
/// @param predicate Field-name predicate; NULL removes nothing.
static void remove_header_if(rt_http_req_t *req, int8_t (*predicate)(const char *)) {
    http_header_t **link = req ? &req->headers : NULL;
    while (link && *link) {
        http_header_t *header = *link;
        if (predicate && predicate(header->name)) {
            *link = header->next;
            header->next = NULL;
            free_headers(header);
            continue;
        }
        link = &header->next;
    }
}

/// @brief Identify framing or representation headers tied to a request body.
/// @param name Header field name.
/// @return One for Content-Length, Content-Type, or Transfer-Encoding,
///         otherwise zero.
static int8_t is_body_specific_header(const char *name) {
    if (!name)
        return 0;
    return strcasecmp(name, "Content-Length") == 0 || strcasecmp(name, "Content-Type") == 0 ||
           strcasecmp(name, "Transfer-Encoding") == 0;
}

/// @brief Remove credentials before following a cross-origin redirect.
/// @param req Redirect request clone to sanitize.
static void strip_sensitive_redirect_headers(rt_http_req_t *req) {
    remove_header_if(req, rt_http_header_is_sensitive_for_cross_origin_redirect);
}

/// @brief Remove body metadata after a redirect rewrites the method to GET.
/// @param req Redirect request clone whose body was cleared.
static void strip_redirect_body_headers(rt_http_req_t *req) {
    remove_header_if(req, is_body_specific_header);
}

/// @brief Free the owned fields of a stack-local redirected request clone.
/// @details Redirect clones borrow only the connection pool pointer; every other
///          pointer field is allocated specifically for the clone and must be
///          released after the recursive redirect request completes.
/// @param req Stack-local request clone to clean up. Safe for zeroed structs.
static void http_request_clone_cleanup(rt_http_req_t *req) {
    if (!req)
        return;
    free(req->method);
    req->method = NULL;
    free_parsed_url(&req->url);
    free_headers(req->headers);
    req->headers = NULL;
    free(req->body);
    req->body = NULL;
    req->body_len = 0;
    req->connection_pool = NULL;
}

/// @brief Duplicate a parsed URL into clone-owned storage.
/// @param dst Destination parsed URL, overwritten on success.
/// @param src Source parsed URL.
/// @return 1 on success; 0 on allocation failure.
static int http_clone_parsed_url(parsed_url_t *dst, const parsed_url_t *src) {
    memset(dst, 0, sizeof(*dst));
    if (src->host) {
        dst->host = strdup(src->host);
        if (!dst->host)
            return 0;
    }
    if (src->path) {
        dst->path = strdup(src->path);
        if (!dst->path) {
            free_parsed_url(dst);
            return 0;
        }
    }
    dst->port = src->port;
    dst->use_tls = src->use_tls;
    return 1;
}

/// @brief Duplicate a linked list of request headers preserving list order.
/// @param dst_head Receives the cloned header list.
/// @param src Source request header list.
/// @return 1 on success; 0 on allocation failure.
static int http_clone_headers(http_header_t **dst_head, const http_header_t *src) {
    http_header_t **tail = dst_head;
    *dst_head = NULL;
    for (const http_header_t *h = src; h; h = h->next) {
        http_header_t *copy = (http_header_t *)calloc(1, sizeof(*copy));
        if (!copy)
            return 0;
        copy->name = h->name ? strdup(h->name) : NULL;
        copy->value = h->value ? strdup(h->value) : NULL;
        if ((h->name && !copy->name) || (h->value && !copy->value)) {
            free(copy->name);
            free(copy->value);
            free(copy);
            return 0;
        }
        *tail = copy;
        tail = &copy->next;
    }
    return 1;
}

/// @brief Clone a request into owned storage for redirect follow-up.
/// @details The source request can be a public builder object or a one-shot
///          stack request whose body points into caller-owned Bytes. This helper
///          copies method, URL, headers, and body so redirect rewriting never
///          mutates or frees source-owned memory.
/// @param dst Destination request clone, zeroed by this function.
/// @param src Source request to clone.
/// @return 1 on success; 0 after trapping on allocation failure.
static int http_clone_request_for_redirect(rt_http_req_t *dst, const rt_http_req_t *src) {
    memset(dst, 0, sizeof(*dst));
    dst->timeout_ms = src->timeout_ms;
    dst->tls_verify = src->tls_verify;
    dst->follow_redirects = src->follow_redirects;
    dst->max_redirects = src->max_redirects;
    dst->accept_gzip = src->accept_gzip;
    dst->decode_gzip = src->decode_gzip;
    dst->keep_alive = src->keep_alive;
    dst->connection_pool = src->connection_pool;
    dst->force_http1 = src->force_http1;

    dst->method = src->method ? strdup(src->method) : NULL;
    if (src->method && !dst->method)
        goto oom;
    if (!http_clone_parsed_url(&dst->url, &src->url))
        goto oom;
    if (!http_clone_headers(&dst->headers, src->headers))
        goto oom;
    if (src->body && src->body_len > 0) {
        dst->body = (uint8_t *)malloc(src->body_len);
        if (!dst->body)
            goto oom;
        memcpy(dst->body, src->body, src->body_len);
        dst->body_len = src->body_len;
    }
    return 1;

oom:
    http_request_clone_cleanup(dst);
    rt_trap("HTTP: memory allocation failed");
    return 0;
}

/// @brief Build the request object used for one redirect hop.
/// @details Applies cross-origin header stripping, RFC 7231 method/body rewrite
///          rules, and Location resolution to a clone so the original request
///          object remains reusable after `Send()`.
/// @param dst Destination request clone.
/// @param src Source request that received the redirect.
/// @param status Redirect status code.
/// @param cross_origin Nonzero when sensitive headers must be stripped.
/// @param location Raw Location header value.
/// @return 1 on success; 0 after trapping on allocation or URL errors.
static int http_prepare_redirect_request(rt_http_req_t *dst,
                                         const rt_http_req_t *src,
                                         int status,
                                         int cross_origin,
                                         const char *location) {
    if (!http_clone_request_for_redirect(dst, src))
        return 0;
    if (cross_origin)
        strip_sensitive_redirect_headers(dst);

    if (status == 303 ||
        ((status == 301 || status == 302) && dst->method && strcmp(dst->method, "POST") == 0)) {
        char *get_method = strdup("GET");
        if (!get_method) {
            http_request_clone_cleanup(dst);
            rt_trap("HTTP: memory allocation failed");
            return 0;
        }
        free(dst->method);
        dst->method = get_method;
        free(dst->body);
        dst->body = NULL;
        dst->body_len = 0;
        strip_redirect_body_headers(dst);
    }

    if (resolve_redirect_target(&dst->url, location) != 0) {
        http_request_clone_cleanup(dst);
        rt_trap_net("HTTP: invalid redirect URL", Err_InvalidUrl);
        return 0;
    }
    return 1;
}

/// @brief Prepare and execute one redirect while consuming its Location copy.
/// @details The cloned request lives in native heap storage so its fields remain
///          well-defined after `longjmp`. One recovery frame owns both the
///          Location buffer and every cloned request allocation across the
///          recursive request. The original trap category and network code are
///          re-raised only after cleanup.
/// @param source Request that received the redirect.
/// @param status Redirect status code controlling method rewrite semantics.
/// @param cross_origin Nonzero when sensitive headers must be stripped.
/// @param location_owned Owned native Location string; always consumed.
/// @param redirects_remaining Remaining redirect count for the recursive call.
/// @param deadline_us Shared absolute monotonic request deadline, or zero.
/// @return Caller-owned final response, or NULL after a returning trap hook.
static rt_http_res_t *http_follow_redirect(rt_http_req_t *source,
                                           int status,
                                           int cross_origin,
                                           char *location_owned,
                                           int redirects_remaining,
                                           int64_t deadline_us) {
    rt_http_req_t *const next_request = (rt_http_req_t *)calloc(1, sizeof(*next_request));
    if (!next_request) {
        free(location_owned);
        rt_trap("HTTP: redirect request allocation failed");
        return NULL;
    }

    char *volatile location = location_owned;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        char saved_error[256];
        int saved_net_code = rt_trap_get_net_code();
        const char *error = rt_trap_get_error();
        snprintf(saved_error,
                 sizeof(saved_error),
                 "%s",
                 error && error[0] ? error : "HTTP: redirect failed");
        rt_trap_clear_recovery();
        http_request_clone_cleanup(next_request);
        free((void *)location);
        free(next_request);
        if (saved_net_code)
            rt_trap_net(saved_error, saved_net_code);
        else
            rt_trap(saved_error);
        return NULL;
    }

    if (!http_prepare_redirect_request(
            next_request, source, status, cross_origin, (const char *)location)) {
        rt_trap_clear_recovery();
        http_request_clone_cleanup(next_request);
        free((void *)location);
        free(next_request);
        return NULL;
    }
    free((void *)location);
    location = NULL;
    rt_http_res_t *response =
        do_http_request_deadline(next_request, redirects_remaining, deadline_us);
    rt_trap_clear_recovery();
    http_request_clone_cleanup(next_request);
    free(next_request);
    return response;
}

/// @brief Fetch and retain a boxed String from the response-header map.
/// @details The temporary lookup key is released on success, miss, and trap.
///          The returned String owns a fresh reference supplied by
///          `rt_unbox_str`; callers must release it.
/// @param headers_map Stable response Map.
/// @param name NUL-terminated lowercase field name.
/// @return Retained header value, or NULL when absent.
static rt_string get_header_value(void *headers_map, const char *name) {
    rt_string volatile key = NULL;
    rt_string volatile result = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        char saved_error[256];
        const char *error = rt_trap_get_error();
        snprintf(saved_error,
                 sizeof(saved_error),
                 "%s",
                 error && error[0] ? error : "HTTP: header lookup failed");
        rt_trap_clear_recovery();
        if (result)
            rt_string_unref((rt_string)result);
        if (key)
            rt_string_unref((rt_string)key);
        rt_trap(saved_error);
        return NULL;
    }

    key = rt_const_cstr(name);
    if (!key) {
        rt_trap_clear_recovery();
        return NULL;
    }
    void *boxed = rt_map_get(headers_map, (rt_string)key);
    if (boxed && rt_box_type(boxed) == RT_BOX_STR)
        result = rt_unbox_str(boxed);
    rt_trap_clear_recovery();
    rt_string_unref((rt_string)key);
    return (rt_string)result;
}

/// @brief Replace or remove one lowercase response-header entry trap-safely.
/// @details Temporary key/value Strings and the box are released on every
///          path. Map insertion retains the box before local ownership is
///          dropped. A returning trap hook is converted to a zero result
///          without attempting a second diagnostic.
/// @param headers_map Stable response Map.
/// @param name NUL-terminated lowercase field name.
/// @param value Replacement C string, or NULL to remove the field.
/// @return One when the requested mutation completed; zero after a returning trap.
static int set_header_value(void *headers_map, const char *name, const char *value) {
    rt_string volatile key = NULL;
    rt_string volatile value_string = NULL;
    void *volatile boxed = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        char saved_error[256];
        const char *error = rt_trap_get_error();
        snprintf(saved_error,
                 sizeof(saved_error),
                 "%s",
                 error && error[0] ? error : "HTTP: header update failed");
        rt_trap_clear_recovery();
        if (boxed && rt_obj_release_check0((void *)boxed))
            rt_obj_free((void *)boxed);
        if (value_string)
            rt_string_unref((rt_string)value_string);
        if (key)
            rt_string_unref((rt_string)key);
        rt_trap(saved_error);
        return 0;
    }

    key = rt_const_cstr(name);
    if (!key) {
        rt_trap_clear_recovery();
        return 0;
    }
    if (!value) {
        rt_map_remove(headers_map, (rt_string)key);
        rt_trap_clear_recovery();
        rt_string_unref((rt_string)key);
        return 1;
    }

    value_string = rt_string_from_bytes(value, strlen(value));
    if (!value_string) {
        rt_trap_clear_recovery();
        rt_string_unref((rt_string)key);
        return 0;
    }
    boxed = rt_box_str((rt_string)value_string);
    if (!boxed) {
        rt_trap_clear_recovery();
        rt_string_unref((rt_string)value_string);
        rt_string_unref((rt_string)key);
        return 0;
    }
    rt_map_set(headers_map, (rt_string)key, (void *)boxed);
    if (!rt_map_has(headers_map, (rt_string)key)) {
        rt_trap_clear_recovery();
        if (rt_obj_release_check0((void *)boxed))
            rt_obj_free((void *)boxed);
        rt_string_unref((rt_string)value_string);
        rt_string_unref((rt_string)key);
        return 0;
    }
    rt_trap_clear_recovery();
    if (rt_obj_release_check0((void *)boxed))
        rt_obj_free((void *)boxed);
    rt_string_unref((rt_string)value_string);
    rt_string_unref((rt_string)key);
    return 1;
}

/// @brief Replace a response Content-Length with a body size.
/// @param headers_map Managed response-header Map.
/// @param body_len Exact decoded body length.
/// @return One after successful header publication, otherwise zero.
static int set_content_length_header(void *headers_map, size_t body_len) {
    char len_buf[32];
    snprintf(len_buf, sizeof(len_buf), "%zu", body_len);
    return set_header_value(headers_map, "content-length", len_buf);
}

/// @brief Decode an advertised gzip response body transactionally.
/// @details When decoding is enabled and Content-Encoding contains `gzip`,
///          decodes directly from the native body into native bounded storage,
///          enforcing both the absolute body ceiling and an expansion-ratio
///          budget during growth. It removes Content-Encoding, updates
///          Content-Length, and only then replaces the caller's buffer.
///          Temporary native storage is recovered across traps.
/// @param req Request containing the decode policy.
/// @param headers_map Mutable managed response-header Map.
/// @param body_io Receives the replacement owned native body buffer.
/// @param body_len_io Receives the decoded byte length.
/// @return One for no-op or successful decoding, otherwise zero.
static int maybe_decode_gzip_body(const rt_http_req_t *req,
                                  void *headers_map,
                                  uint8_t **body_io,
                                  size_t *body_len_io) {
    uint8_t *body = body_io ? *body_io : NULL;
    size_t body_len = body_len_io ? *body_len_io : 0;
    rt_string volatile content_encoding = NULL;
    uint8_t *volatile decoded_body = NULL;
    volatile int body_transferred = 0;

    if (!req || !req->decode_gzip || !headers_map || !body || body_len == 0)
        return 1;

    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        char saved_error[256];
        const char *error = rt_trap_get_error();
        snprintf(saved_error,
                 sizeof(saved_error),
                 "%s",
                 error && error[0] ? error : "HTTP: gzip decode allocation failed");
        rt_trap_clear_recovery();
        if (content_encoding)
            rt_string_unref((rt_string)content_encoding);
        if (!body_transferred)
            free((void *)decoded_body);
        rt_trap(saved_error);
        return 0;
    }

    content_encoding = get_header_value(headers_map, "content-encoding");
    if (!content_encoding ||
        !rt_http_header_value_has_token(rt_string_cstr((rt_string)content_encoding), "gzip")) {
        if (content_encoding)
            rt_string_unref((rt_string)content_encoding);
        rt_trap_clear_recovery();
        return 1;
    }
    rt_string_unref((rt_string)content_encoding);
    content_encoding = NULL;

    size_t decoded_len = 0;
    uint8_t *decoded_native = NULL;
    if (!rt_compress_gunzip_raw(body,
                                body_len,
                                HTTP_MAX_BODY_SIZE,
                                HTTP_MAX_GZIP_EXPANSION_RATIO,
                                HTTP_GZIP_EXPANSION_SLACK,
                                &decoded_native,
                                &decoded_len)) {
        rt_trap_clear_recovery();
        return 0;
    }
    decoded_body = decoded_native;

    if (!set_header_value(headers_map, "content-encoding", NULL) ||
        !set_content_length_header(headers_map, decoded_len)) {
        rt_trap_clear_recovery();
        free((void *)decoded_body);
        return 0;
    }
    free(body);
    *body_io = (uint8_t *)decoded_body;
    *body_len_io = decoded_len;
    body_transferred = 1;
    decoded_body = NULL;
    rt_trap_clear_recovery();
    return 1;
}

/// @brief Serialize an HTTP/1.1 request head.
/// @details Validates method, host, and origin-form target; emits Host with
///          bracketed IPv6 and a nondefault port; synthesizes Content-Length,
///          Connection, and Accept-Encoding when needed; appends validated user
///          headers; and checks all size arithmetic and formatting bounds.
///          The request body is deliberately not included.
/// @param req Fully initialized native request.
/// @return Newly allocated NUL-terminated request head, or NULL on invalid
///         state, overflow, formatting failure, or allocation failure.
static char *build_request(rt_http_req_t *req) {
    if (!req || !http_method_is_token(req->method) || !http_host_is_valid(req->url.host) ||
        !http_request_target_is_valid(req->url.path)) {
        return NULL;
    }

    int add_default_connection = !has_header(req, "Connection");
    int want_keep_alive = req->keep_alive ? 1 : 0;

    // Calculate total size
    size_t size = 0;
    char *host_header = NULL;
#define HTTP_REQUEST_ADD_SIZE(amount_)                                                             \
    do {                                                                                           \
        size_t http_request_amount_ = (amount_);                                                   \
        if (http_request_amount_ > SIZE_MAX - size) {                                              \
            free(host_header);                                                                     \
            return NULL;                                                                           \
        }                                                                                          \
        size += http_request_amount_;                                                              \
    } while (0)

    HTTP_REQUEST_ADD_SIZE(strlen(req->method));
    HTTP_REQUEST_ADD_SIZE(1);
    HTTP_REQUEST_ADD_SIZE(strlen(req->url.path));
    HTTP_REQUEST_ADD_SIZE(11); // " HTTP/1.1\r\n"

    // Host header
    {
        int needed = 0;
        int with_port = req->url.port != 80 && req->url.port != 443;
        int bracketed = host_needs_brackets(req->url.host);
        if (with_port && bracketed)
            needed = snprintf(NULL, 0, "Host: [%s]:%d\r\n", req->url.host, req->url.port);
        else if (with_port)
            needed = snprintf(NULL, 0, "Host: %s:%d\r\n", req->url.host, req->url.port);
        else if (bracketed)
            needed = snprintf(NULL, 0, "Host: [%s]\r\n", req->url.host);
        else
            needed = snprintf(NULL, 0, "Host: %s\r\n", req->url.host);
        if (needed < 0)
            return NULL;
        host_header = (char *)malloc((size_t)needed + 1);
        if (!host_header)
            return NULL;
        if (with_port && bracketed)
            snprintf(
                host_header, (size_t)needed + 1, "Host: [%s]:%d\r\n", req->url.host, req->url.port);
        else if (with_port)
            snprintf(
                host_header, (size_t)needed + 1, "Host: %s:%d\r\n", req->url.host, req->url.port);
        else if (bracketed)
            snprintf(host_header, (size_t)needed + 1, "Host: [%s]\r\n", req->url.host);
        else
            snprintf(host_header, (size_t)needed + 1, "Host: %s\r\n", req->url.host);
    }
    HTTP_REQUEST_ADD_SIZE(strlen(host_header));

    // Content-Length if body
    char content_len_header[64] = "";
    if (req->body && req->body_len > 0) {
        snprintf(content_len_header,
                 sizeof(content_len_header),
                 "Content-Length: %zu\r\n",
                 req->body_len);
        HTTP_REQUEST_ADD_SIZE(strlen(content_len_header));
    }

    if (add_default_connection)
        HTTP_REQUEST_ADD_SIZE(want_keep_alive ? 24 : 19); // keep-alive / close

    if (req->accept_gzip && !has_header(req, "Accept-Encoding"))
        HTTP_REQUEST_ADD_SIZE(23); // "Accept-Encoding: gzip\r\n"

    // User headers
    for (http_header_t *h = req->headers; h; h = h->next) {
        HTTP_REQUEST_ADD_SIZE(strlen(h->name));
        HTTP_REQUEST_ADD_SIZE(2);
        HTTP_REQUEST_ADD_SIZE(strlen(h->value));
        HTTP_REQUEST_ADD_SIZE(2); // "Name: Value\r\n"
    }

    HTTP_REQUEST_ADD_SIZE(2); // Final CRLF
    HTTP_REQUEST_ADD_SIZE(1); // Null terminator
#undef HTTP_REQUEST_ADD_SIZE

    char *request = (char *)malloc(size);
    if (!request) {
        free(host_header);
        return NULL;
    }

    char *p = request;
    size_t remaining = size;
    int written;

#define SNPRINTF_OR_FAIL(fmt, ...)                                                                 \
    do {                                                                                           \
        written = snprintf(p, remaining, fmt, __VA_ARGS__);                                        \
        if (written < 0 || (size_t)written >= remaining) {                                         \
            free(host_header);                                                                     \
            free(request);                                                                         \
            return NULL;                                                                           \
        }                                                                                          \
        p += written;                                                                              \
        remaining -= (size_t)written;                                                              \
    } while (0)

    SNPRINTF_OR_FAIL("%s %s HTTP/1.1\r\n", req->method, req->url.path);
    SNPRINTF_OR_FAIL("%s", host_header);

    if (content_len_header[0])
        SNPRINTF_OR_FAIL("%s", content_len_header);

    if (add_default_connection)
        SNPRINTF_OR_FAIL("%s",
                         want_keep_alive ? "Connection: keep-alive\r\n" : "Connection: close\r\n");
    if (req->accept_gzip && !has_header(req, "Accept-Encoding"))
        SNPRINTF_OR_FAIL("%s", "Accept-Encoding: gzip\r\n");

    // User headers
    for (http_header_t *h = req->headers; h; h = h->next) {
        SNPRINTF_OR_FAIL("%s: %s\r\n", h->name, h->value);
    }

    SNPRINTF_OR_FAIL("%s", "\r\n");

#undef SNPRINTF_OR_FAIL

    if (remaining == 0) {
        free(host_header);
        free(request);
        return NULL;
    }
    *p = '\0';
    free(host_header);
    return request;
}

/// @brief Public test hook into the otherwise-static URL parser.
///
/// Lets unit tests verify URL parsing without going through the
/// HTTP request path. All output pointers are optional. Strings
/// returned via `host_out`/`path_out` are heap-allocated and must
/// be `free()`d by the caller.
/// @param url_str Candidate NUL-terminated HTTP URL.
/// @param host_out Optional output for a newly allocated host copy.
/// @param port_out Optional output for the parsed port.
/// @param path_out Optional output for a newly allocated request-target copy.
/// @param use_tls_out Optional output receiving one for HTTPS.
/// @return 1 on success, 0 on parse failure (in which case no
///         outputs are written and no allocations leak).
int rt_http_parse_url_for_test(
    const char *url_str, char **host_out, int *port_out, char **path_out, int *use_tls_out) {
    parsed_url_t parsed;
    if (parse_url(url_str, &parsed) != 0)
        return 0;

    if (host_out)
        *host_out = parsed.host ? strdup(parsed.host) : NULL;
    if (port_out)
        *port_out = parsed.port;
    if (path_out)
        *path_out = parsed.path ? strdup(parsed.path) : NULL;
    if (use_tls_out)
        *use_tls_out = parsed.use_tls;

    free_parsed_url(&parsed);
    return 1;
}

#include "rt_network_http_response.inc"

typedef enum http_body_read_status {
    HTTP_BODY_READ_OK = 0,
    HTTP_BODY_READ_OOM,
    HTTP_BODY_READ_TOO_LARGE,
    HTTP_BODY_READ_IO
} http_body_read_status_t;

/// @brief Read response body until connection closes.
/// @details EOF is a successful delimiter for this HTTP/1.0-style body mode.
///          `status_out` distinguishes EOF success from allocation, size-limit,
///          and transport failures.
/// @param conn Open connection positioned at the response body.
/// @param out_len Receives the exact body length, or zero on failure.
/// @param status_out Optional detailed body-read status.
/// @return Newly allocated owned body buffer, or NULL on failure.
static uint8_t *read_body_until_close_conn(http_conn_t *conn,
                                           size_t *out_len,
                                           http_body_read_status_t *status_out) {
    if (status_out)
        *status_out = HTTP_BODY_READ_OK;
    size_t body_cap = HTTP_BUFFER_SIZE;
    size_t body_len = 0;
    uint8_t *body = (uint8_t *)malloc(body_cap);
    if (!body) {
        if (status_out)
            *status_out = HTTP_BODY_READ_OOM;
        return NULL;
    }

    while (1) {
        size_t to_read = HTTP_BUFFER_SIZE;
        if (body_len == HTTP_MAX_BODY_SIZE) {
            uint8_t extra = 0;
            long extra_len = http_conn_recv(conn, &extra, 1);
            if (extra_len == 0)
                break;
            if (extra_len < 0) {
                free(body);
                *out_len = 0;
                if (status_out)
                    *status_out = HTTP_BODY_READ_IO;
                return NULL;
            }
            free(body);
            *out_len = 0;
            if (status_out)
                *status_out = HTTP_BODY_READ_TOO_LARGE;
            return NULL;
        }
        if (to_read > HTTP_MAX_BODY_SIZE - body_len)
            to_read = HTTP_MAX_BODY_SIZE - body_len;

        // Expand buffer if needed
        if (body_len + to_read > body_cap) {
            if (body_cap > HTTP_MAX_BODY_SIZE / 2)
                body_cap = HTTP_MAX_BODY_SIZE;
            else
                body_cap *= 2;
            if (body_cap < body_len + to_read) {
                free(body);
                *out_len = 0;
                if (status_out)
                    *status_out = HTTP_BODY_READ_TOO_LARGE;
                return NULL;
            }
            uint8_t *new_body = (uint8_t *)realloc(body, body_cap);
            if (!new_body) {
                free(body);
                *out_len = 0;
                if (status_out)
                    *status_out = HTTP_BODY_READ_OOM;
                return NULL;
            }
            body = new_body;
        }

        long len = http_conn_recv(conn, body + body_len, to_read);
        if (len == 0)
            break;
        if (len < 0) {
            free(body);
            *out_len = 0;
            if (status_out)
                *status_out = HTTP_BODY_READ_IO;
            return NULL;
        }

        body_len += (size_t)len;

        /* Reject bodies that exceed the limit to prevent server-driven OOM (mirrors
           the chunked path guard using HTTP_MAX_BODY_SIZE). */
        if (body_len > HTTP_MAX_BODY_SIZE) {
            free(body);
            *out_len = 0;
            if (status_out)
                *status_out = HTTP_BODY_READ_TOO_LARGE;
            return NULL;
        }
    }

    *out_len = body_len;
    return body;
}

/// @brief Stream an exact Content-Length body directly into a file.
/// @param conn Open connection positioned at the response body.
/// @param content_length Exact promised byte count.
/// @param out Open writable destination stream.
/// @param out_len Receives bytes written, or zero for transport/size failure.
/// @return One only after every promised byte is written, otherwise zero.
static int write_body_content_length_conn(http_conn_t *conn,
                                          size_t content_length,
                                          FILE *out,
                                          size_t *out_len) {
    size_t total_read = 0;
    uint8_t buffer[HTTP_BUFFER_SIZE];

    if (content_length > HTTP_MAX_BODY_SIZE) {
        *out_len = 0;
        return 0;
    }

    while (total_read < content_length) {
        size_t remaining = content_length - total_read;
        size_t chunk_size = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
        long len = http_conn_recv(conn, buffer, chunk_size);
        if (len <= 0) {
            *out_len = 0;
            return 0;
        }
        if (fwrite(buffer, 1, (size_t)len, out) != (size_t)len) {
            *out_len = total_read;
            return 0;
        }
        total_read += (size_t)len;
    }

    *out_len = total_read;
    return 1;
}

/// @brief Decode and stream a chunked response body into a file.
/// @details Validates chunk-size lines, aggregate size, each chunk terminator,
///          and bounded trailers while writing only decoded payload bytes.
/// @param conn Open connection positioned at the first chunk-size line.
/// @param out Open writable destination stream.
/// @param out_len Receives payload bytes written before success or failure.
/// @return One after the terminating chunk and valid trailers, otherwise zero.
static int write_body_chunked_conn(http_conn_t *conn, FILE *out, size_t *out_len) {
    size_t total_written = 0;
    uint8_t buffer[HTTP_BUFFER_SIZE];

    while (1) {
        char *size_line = read_line_conn(conn);
        if (!size_line) {
            *out_len = total_written;
            return 0;
        }

        size_t chunk_size = 0;
        int parsed = parse_http_chunk_size_line(size_line, &chunk_size);
        free(size_line);

        if (!parsed || total_written > HTTP_MAX_BODY_SIZE ||
            chunk_size > HTTP_MAX_BODY_SIZE - total_written) {
            *out_len = total_written;
            return 0;
        }

        if (chunk_size == 0) {
            int trailers_ok = drain_chunk_trailers_conn(conn);
            *out_len = total_written;
            return trailers_ok;
        }

        size_t chunk_read = 0;
        while (chunk_read < chunk_size) {
            size_t remaining = chunk_size - chunk_read;
            size_t to_read = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
            long len = http_conn_recv(conn, buffer, to_read);
            if (len <= 0) {
                *out_len = total_written;
                return 0;
            }
            if (fwrite(buffer, 1, (size_t)len, out) != (size_t)len) {
                *out_len = total_written;
                return 0;
            }
            chunk_read += (size_t)len;
            total_written += (size_t)len;
        }

        char *chunk_end = read_line_conn(conn);
        if (!chunk_end || chunk_end[0] != '\0') {
            free(chunk_end);
            *out_len = total_written;
            return 0;
        }
        free(chunk_end);
    }
}

/// @brief Stream a close-delimited response body into a file.
/// @param conn Open connection positioned at the body.
/// @param out Open writable destination stream.
/// @param out_len Receives bytes written before success or failure.
/// @return One when orderly EOF delimits a body within the size limit,
///         otherwise zero.
static int write_body_until_close_conn(http_conn_t *conn, FILE *out, size_t *out_len) {
    size_t total_written = 0;
    uint8_t buffer[HTTP_BUFFER_SIZE];

    while (1) {
        long len = http_conn_recv(conn, buffer, sizeof(buffer));
        if (len == 0)
            break;
        if (len < 0) {
            *out_len = total_written;
            return 0;
        }
        if (total_written + (size_t)len > HTTP_MAX_BODY_SIZE) {
            *out_len = total_written;
            return 0;
        }
        if (fwrite(buffer, 1, (size_t)len, out) != (size_t)len) {
            *out_len = total_written;
            return 0;
        }
        total_written += (size_t)len;
    }

    *out_len = total_written;
    return 1;
}

/// @brief Duplicate the canonical reason phrase for a common status code.
/// @param status Numeric HTTP response status.
/// @return Newly allocated reason phrase, using `Unknown` for an unlisted code,
///         or NULL on allocation failure.
static char *http_status_text_dup(int status) {
    const char *text = NULL;
    switch (status) {
        case 100:
            text = "Continue";
            break;
        case 101:
            text = "Switching Protocols";
            break;
        case 200:
            text = "OK";
            break;
        case 201:
            text = "Created";
            break;
        case 202:
            text = "Accepted";
            break;
        case 204:
            text = "No Content";
            break;
        case 206:
            text = "Partial Content";
            break;
        case 301:
            text = "Moved Permanently";
            break;
        case 302:
            text = "Found";
            break;
        case 303:
            text = "See Other";
            break;
        case 304:
            text = "Not Modified";
            break;
        case 307:
            text = "Temporary Redirect";
            break;
        case 308:
            text = "Permanent Redirect";
            break;
        case 400:
            text = "Bad Request";
            break;
        case 401:
            text = "Unauthorized";
            break;
        case 403:
            text = "Forbidden";
            break;
        case 404:
            text = "Not Found";
            break;
        case 405:
            text = "Method Not Allowed";
            break;
        case 500:
            text = "Internal Server Error";
            break;
        case 502:
            text = "Bad Gateway";
            break;
        case 503:
            text = "Service Unavailable";
            break;
        case 504:
            text = "Gateway Timeout";
            break;
        default:
            text = "Unknown";
            break;
    }
    return strdup(text);
}

/// @brief Format the HTTP/2 `:authority` value for a parsed URL.
/// @details Brackets bare IPv6 hosts and includes only nondefault ports.
/// @param url Parsed HTTP/HTTPS URL.
/// @return Newly allocated authority string, or NULL for invalid input or
///         allocation failure.
static char *http_format_authority(const parsed_url_t *url) {
    char buf[320];
    int default_port = 0;
    if (!url || !url->host)
        return NULL;
    default_port = url->use_tls ? 443 : 80;
    if (url->port != default_port) {
        snprintf(buf,
                 sizeof(buf),
                 host_needs_brackets(url->host) ? "[%s]:%d" : "%s:%d",
                 url->host,
                 url->port);
    } else {
        snprintf(buf, sizeof(buf), host_needs_brackets(url->host) ? "[%s]" : "%s", url->host);
    }
    return strdup(buf);
}

/// @brief Identify HTTP/1.x fields forbidden in HTTP/2.
/// @details Filters connection management, upgrade, keep-alive, proxy
///          connection, and transfer coding fields; TE is allowed only with
///          the exact `trailers` value.
/// @param name Header field name.
/// @param value Header value used to validate TE.
/// @return One when the field must be omitted from HTTP/2, otherwise zero.
static int http2_header_is_connection_specific(const char *name, const char *value) {
    if (!name)
        return 0;
    if (strcasecmp(name, "connection") == 0 || strcasecmp(name, "proxy-connection") == 0 ||
        strcasecmp(name, "keep-alive") == 0 || strcasecmp(name, "upgrade") == 0 ||
        strcasecmp(name, "transfer-encoding") == 0) {
        return 1;
    }
    if (strcasecmp(name, "te") == 0 && value && strcasecmp(value, "trailers") != 0)
        return 1;
    return 0;
}

/// @brief Build the ordered HTTP/2 header array for a request.
/// @details Emits required pseudo-headers, filters connection-specific HTTP/1
///          fields and duplicate Host/Content-Length fields, synthesizes body
///          length and gzip acceptance as needed, and duplicates every name and
///          value into owned array storage.
/// @param req Fully initialized request.
/// @param headers_out Receives an owned linked header list.
/// @return One on success, otherwise zero after freeing partial header storage.
static int http2_build_request_headers(rt_http_req_t *req, rt_http2_header_t **headers_out) {
    char content_len_buf[64];
    rt_http2_header_t *headers = NULL;
    if (!req || !headers_out)
        return 0;
    *headers_out = NULL;
    for (http_header_t *h = req->headers; h; h = h->next) {
        if (!h->name || !h->value)
            continue;
        if (strcasecmp(h->name, "Host") == 0 ||
            http2_header_is_connection_specific(h->name, h->value))
            continue;
        if (!rt_http2_header_append_copy(&headers, h->name, h->value)) {
            rt_http2_headers_free(headers);
            return 0;
        }
    }
    if (req->accept_gzip && !has_header(req, "Accept-Encoding") &&
        !rt_http2_header_append_copy(&headers, "accept-encoding", "gzip")) {
        rt_http2_headers_free(headers);
        return 0;
    }
    if (req->body && req->body_len > 0 && !has_header(req, "Content-Length")) {
        snprintf(content_len_buf, sizeof(content_len_buf), "%zu", req->body_len);
        if (!rt_http2_header_append_copy(&headers, "content-length", content_len_buf)) {
            rt_http2_headers_free(headers);
            return 0;
        }
    }
    *headers_out = headers;
    return 1;
}

/// @brief Convert one native HTTP/2 header list into the managed response map.
/// @details Pseudo-headers are omitted, duplicate field values are joined by
///          the shared response-header helper, and the first Location value is
///          copied for redirect handling. A local recovery frame releases the
///          partial Map and native Location copy before propagating any managed
///          allocation trap.
/// @param headers Borrowed HTTP/2 response-header list.
/// @param headers_map_out Receives a caller-owned managed Map on success.
/// @param redirect_location_out Receives an optional caller-owned native copy
///        of the first Location value.
/// @return One after publishing all outputs; zero after a returning trap hook
///         or native allocation failure. Outputs are NULL on failure.
static int http2_headers_to_map(const rt_http2_header_t *headers,
                                void **headers_map_out,
                                char **redirect_location_out) {
    void *volatile headers_map = NULL;
    char *volatile redirect_location = NULL;
    if (headers_map_out)
        *headers_map_out = NULL;
    if (redirect_location_out)
        *redirect_location_out = NULL;

    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        char saved_error[256];
        const char *error = rt_trap_get_error();
        snprintf(saved_error,
                 sizeof(saved_error),
                 "%s",
                 error && error[0] ? error : "HTTP/2: response header allocation failed");
        rt_trap_clear_recovery();
        if (headers_map && rt_obj_release_check0((void *)headers_map))
            rt_obj_free((void *)headers_map);
        free((void *)redirect_location);
        rt_trap(saved_error);
        return 0;
    }

    headers_map = rt_map_new();
    if (!headers_map) {
        rt_trap_clear_recovery();
        return 0;
    }
    for (const rt_http2_header_t *it = headers; it; it = it->next) {
        if (!it->name || !it->value || it->name[0] == ':')
            continue;
        if (!append_response_header_value((void *)headers_map, it->name, it->value)) {
            rt_trap_clear_recovery();
            if (rt_obj_release_check0((void *)headers_map))
                rt_obj_free((void *)headers_map);
            free((void *)redirect_location);
            return 0;
        }
        if (!redirect_location && strcasecmp(it->name, "location") == 0) {
            redirect_location = strdup(it->value);
            if (!redirect_location) {
                rt_trap_clear_recovery();
                if (rt_obj_release_check0((void *)headers_map))
                    rt_obj_free((void *)headers_map);
                return 0;
            }
        }
    }
    if (headers_map_out) {
        *headers_map_out = (void *)headers_map;
        headers_map = NULL;
    }
    if (redirect_location_out) {
        *redirect_location_out = (char *)redirect_location;
        redirect_location = NULL;
    }
    rt_trap_clear_recovery();
    if (headers_map && rt_obj_release_check0((void *)headers_map))
        rt_obj_free((void *)headers_map);
    free((void *)redirect_location);
    return 1;
}

/// @brief Publish fully owned response components as a managed HttpRes.
/// @details This function consumes @p status_text, @p headers_map, and @p body
///          on every path. The finalizer is installed before ownership is
///          transferred into the object, and a recovery frame destroys a
///          partial object or the individual components after allocation traps.
/// @param status Numeric HTTP status code.
/// @param status_text Owned native reason phrase, or NULL.
/// @param headers_map Owned managed response-header Map, or NULL.
/// @param body Owned native response body, or NULL when @p body_len is zero.
/// @param body_len Number of bytes owned by @p body.
/// @return Caller-owned HttpRes, or NULL after cleanup if a trap hook returns.
static rt_http_res_t *http_make_response_obj(
    int status, char *status_text, void *headers_map, uint8_t *body, size_t body_len) {
    rt_http_res_t *volatile res = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        char saved_error[256];
        const char *error = rt_trap_get_error();
        snprintf(saved_error,
                 sizeof(saved_error),
                 "%s",
                 error && error[0] ? error : "HTTP: response allocation failed");
        rt_trap_clear_recovery();
        if (res && rt_obj_release_check0((void *)res))
            rt_obj_free((void *)res);
        free(body);
        free(status_text);
        if (headers_map && rt_obj_release_check0(headers_map))
            rt_obj_free(headers_map);
        rt_trap(saved_error);
        return NULL;
    }
    res = (rt_http_res_t *)rt_obj_new_i64(RT_HTTP_RES_CLASS_ID, (int64_t)sizeof(rt_http_res_t));
    if (!res) {
        rt_trap_clear_recovery();
        free(body);
        free(status_text);
        if (headers_map && rt_obj_release_check0(headers_map))
            rt_obj_free(headers_map);
        return NULL;
    }
    memset((void *)res, 0, sizeof(*res));
    rt_obj_set_finalizer(res, rt_http_res_finalize);
    res->status = status;
    res->status_text = status_text;
    res->headers = headers_map;
    res->body = body;
    res->body_len = body_len;
    rt_trap_clear_recovery();
    return (rt_http_res_t *)res;
}

/// @brief Execute a request on an already negotiated HTTP/2 connection.
/// @details Builds and submits one stream, translates response headers/body into
///          managed HTTP/1-compatible response storage, applies redirect and
///          gzip policies, enforces body limits, and returns or discards the
///          connection pool lease according to HTTP/2 usability.
/// @param req Request configuration.
/// @param conn Open HTTP/2 connection, consumed into pool release paths.
/// @param redirects_remaining Remaining redirect-hop budget.
/// @return Newly owned managed response, or NULL after a returning trap.
static rt_http_res_t *do_http2_request_opened(rt_http_req_t *req,
                                              http_conn_t *conn,
                                              int redirects_remaining) {
    rt_http2_header_t *request_headers = NULL;
    rt_http2_response_t h2res;
    char *authority = NULL;
    char *status_text = NULL;
    char *redirect_location = NULL;
    void *headers_map = NULL;
    uint8_t *body = NULL;
    size_t body_len = 0;
    int status = 0;
    int reusable = 0;
    int64_t deadline_us = conn ? conn->deadline_us : 0;

    memset(&h2res, 0, sizeof(h2res));
    if (http_conn_deadline_expired(conn))
        return NULL;
    authority = http_format_authority(&req->url);
    if (!authority || !http2_build_request_headers(req, &request_headers)) {
        free(authority);
        rt_http2_headers_free(request_headers);
        return NULL;
    }

    if (!rt_http2_client_roundtrip(conn->http2,
                                   req->method,
                                   req->url.use_tls ? "https" : "http",
                                   authority,
                                   req->url.path,
                                   request_headers,
                                   req->body,
                                   req->body ? req->body_len : 0,
                                   HTTP_MAX_BODY_SIZE,
                                   &h2res)) {
        (void)http_conn_deadline_expired(conn);
        free(authority);
        rt_http2_headers_free(request_headers);
        return NULL;
    }
    free(authority);
    rt_http2_headers_free(request_headers);
    if (http_conn_deadline_expired(conn)) {
        rt_http2_response_free(&h2res);
        return NULL;
    }

    status = h2res.status;
    status_text = http_status_text_dup(status);
    int headers_ok = 0;
    jmp_buf header_recovery;
    rt_trap_set_recovery(&header_recovery);
    if (RT_SETJMP(header_recovery) != 0) {
        char saved_error[256];
        int saved_net_code = rt_trap_get_net_code();
        const char *error = rt_trap_get_error();
        snprintf(saved_error,
                 sizeof(saved_error),
                 "%s",
                 error && error[0] ? error : "HTTP/2: response header allocation failed");
        rt_trap_clear_recovery();
        free(status_text);
        rt_http2_response_free(&h2res);
        if (headers_map && rt_obj_release_check0(headers_map))
            rt_obj_free(headers_map);
        free(redirect_location);
        http_conn_pool_release(conn, 0);
        if (saved_net_code)
            rt_trap_net(saved_error, saved_net_code);
        else
            rt_trap(saved_error);
        return NULL;
    }
    if (status_text)
        headers_ok = http2_headers_to_map(h2res.headers, &headers_map, &redirect_location);
    rt_trap_clear_recovery();
    if (!status_text || !headers_ok) {
        free(status_text);
        rt_http2_response_free(&h2res);
        if (headers_map && rt_obj_release_check0(headers_map))
            rt_obj_free(headers_map);
        free(redirect_location);
        return NULL;
    }

    body = h2res.body;
    body_len = h2res.body_len;
    h2res.body = NULL;
    rt_http2_response_free(&h2res);

    if (req->follow_redirects &&
        (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) &&
        redirect_location) {
        int cross_origin = 0;
        if (redirects_remaining <= 0) {
            http_conn_pool_release(conn, 0);
            free(body);
            free(status_text);
            free(redirect_location);
            if (headers_map && rt_obj_release_check0(headers_map))
                rt_obj_free(headers_map);
            rt_trap_net("HTTP: too many redirects", Err_ProtocolError);
            return NULL;
        }

        cross_origin = redirect_cross_origin_or_unknown(&req->url, redirect_location);
        reusable = http_request_wants_pool(req) && rt_http2_conn_is_usable(conn->http2);
        http_conn_pool_release(conn, reusable);
        free(body);
        free(status_text);
        if (headers_map && rt_obj_release_check0(headers_map))
            rt_obj_free(headers_map);
        return http_follow_redirect(
            req, status, cross_origin, redirect_location, redirects_remaining - 1, deadline_us);
    }
    free(redirect_location);

    if (response_has_no_body(req, status)) {
        free(body);
        body = NULL;
        body_len = 0;
    }

    int transform_ok = 0;
    jmp_buf transform_recovery;
    rt_trap_set_recovery(&transform_recovery);
    if (RT_SETJMP(transform_recovery) != 0) {
        char saved_error[256];
        int saved_net_code = rt_trap_get_net_code();
        const char *error = rt_trap_get_error();
        snprintf(saved_error,
                 sizeof(saved_error),
                 "%s",
                 error && error[0] ? error : "HTTP/2: response transformation failed");
        rt_trap_clear_recovery();
        free(body);
        free(status_text);
        if (headers_map && rt_obj_release_check0(headers_map))
            rt_obj_free(headers_map);
        http_conn_pool_release(conn, 0);
        if (saved_net_code)
            rt_trap_net(saved_error, saved_net_code);
        else
            rt_trap(saved_error);
        return NULL;
    }
    transform_ok = !http_conn_deadline_expired(conn) &&
                   set_content_length_header(headers_map, body_len) &&
                   (!body || maybe_decode_gzip_body(req, headers_map, &body, &body_len)) &&
                   !http_conn_deadline_expired(conn);
    rt_trap_clear_recovery();
    if (!transform_ok) {
        int transform_error_code = http_conn_failure_code(conn, Err_ProtocolError);
        free(body);
        free(status_text);
        if (headers_map && rt_obj_release_check0(headers_map))
            rt_obj_free(headers_map);
        http_conn_pool_release(conn, 0);
        rt_trap_net(transform_error_code == Err_Timeout ? "HTTP: request timed out"
                                                        : "HTTP: invalid gzip response body",
                    transform_error_code);
        return NULL;
    }

    reusable = http_request_wants_pool(req) && rt_http2_conn_is_usable(conn->http2);
    http_conn_pool_release(conn, reusable);
    return http_make_response_obj(status, status_text, headers_map, body, body_len);
}

/// @brief Execute an HTTP request and construct its managed response.
/// @details Opens or leases a transport, selects the HTTP/2 path when
///          negotiated, otherwise serializes HTTP/1.1, permits one safe retry
///          for an idempotent method on stale pooled reuse, parses bounded
///          response framing, follows redirects, optionally decodes gzip, and
///          returns reusable connections only after complete framed bodies.
/// @param req Fully initialized request configuration.
/// @param redirects_remaining Remaining redirect-hop budget.
/// @param deadline_us Shared absolute monotonic request deadline, or zero.
/// @return Newly owned managed HttpRes, or NULL after a returning trap.
static rt_http_res_t *do_http_request_deadline(rt_http_req_t *req,
                                               int redirects_remaining,
                                               int64_t deadline_us) {
    rt_net_init_wsa();

    http_conn_t conn;
    int open_err = Err_NetworkError;
    int request_retry_attempted = 0;

open_connection:
    open_err = Err_NetworkError;
    if (!http_open_connection(req, &conn, deadline_us, &open_err)) {
        if (req->url.use_tls && open_err == Err_TlsError)
            http_trap_tls_error("HTTPS: connection failed", g_http_tls_open_error);
        rt_trap_net(open_err == Err_Timeout ? "HTTP: request timed out"
                                            : (req->url.use_tls ? "HTTPS: connection failed"
                                                                : "HTTP: connection failed"),
                    open_err);
        return NULL;
    }

    if (conn.http2) {
        rt_http_res_t *res = do_http2_request_opened(req, &conn, redirects_remaining);
        if (!res) {
            int request_error_code = http_conn_failure_code(&conn, Err_ProtocolError);
            http_set_tls_open_error(rt_http2_get_error(conn.http2));
            http_conn_pool_release(&conn, 0);
            rt_trap_net(request_error_code == Err_Timeout ? "HTTP: request timed out"
                                                          : "HTTPS: HTTP/2 request failed",
                        request_error_code);
            return NULL;
        }
        return res;
    }

    // Build and send request
    char *request_str = build_request(req);
    if (!request_str) {
        http_conn_pool_release(&conn, 0);
        rt_trap("HTTP: failed to build request");
        return NULL;
    }

    size_t header_len = strlen(request_str);
    size_t req_body_len = req->body ? req->body_len : 0;
    if (req_body_len > SIZE_MAX - header_len) {
        free(request_str);
        http_conn_pool_release(&conn, 0);
        rt_trap_net("HTTP: request too large", Err_ProtocolError);
        return NULL;
    }
    size_t request_len = header_len + req_body_len;
    uint8_t *request_buf = (uint8_t *)malloc(request_len);
    if (!request_buf) {
        free(request_str);
        http_conn_pool_release(&conn, 0);
        rt_trap("HTTP: memory allocation failed");
        return NULL;
    }

    memcpy(request_buf, request_str, header_len);
    if (req->body && req->body_len > 0)
        memcpy(request_buf + header_len, req->body, req->body_len);

    free(request_str);

    if (http_conn_send(&conn, request_buf, request_len) < 0) {
        int send_error_code = http_conn_failure_code(&conn, Err_NetworkError);
        if (send_error_code != Err_Timeout && !request_retry_attempted && conn.reused_from_pool &&
            !conn.http2 && http_method_retryable_on_stale_reuse(req->method)) {
            free(request_buf);
            http_conn_pool_release(&conn, 0);
            request_retry_attempted = 1;
            goto open_connection;
        }
        free(request_buf);
        http_conn_pool_release(&conn, 0);
        rt_trap_net(send_error_code == Err_Timeout ? "HTTP: request timed out"
                                                   : "HTTP: send failed",
                    send_error_code);
        return NULL;
    }
    free(request_buf);

    int status = -1;
    int http_minor = 1;
    char *status_text = NULL;
    void *headers_map = NULL;
    char *redirect_location = NULL;
    int response_head_ok = 0;
    http_conn_t response_head_cleanup_conn = conn;
    jmp_buf response_head_recovery;
    rt_trap_set_recovery(&response_head_recovery);
    if (RT_SETJMP(response_head_recovery) != 0) {
        char saved_error[256];
        int saved_net_code = rt_trap_get_net_code();
        const char *error = rt_trap_get_error();
        snprintf(saved_error,
                 sizeof(saved_error),
                 "%s",
                 error && error[0] ? error : "HTTP: response-head allocation failed");
        rt_trap_clear_recovery();
        http_conn_pool_release(&response_head_cleanup_conn, 0);
        if (saved_net_code)
            rt_trap_net(saved_error, saved_net_code);
        else
            rt_trap(saved_error);
        return NULL;
    }
    response_head_ok = read_response_head(
        &conn, &status, &http_minor, &status_text, &headers_map, &redirect_location);
    rt_trap_clear_recovery();
    if (!response_head_ok) {
        int response_error_code = http_conn_failure_code(&conn, Err_ProtocolError);
        if (response_error_code != Err_Timeout && !request_retry_attempted &&
            conn.reused_from_pool && !conn.http2 &&
            http_method_retryable_on_stale_reuse(req->method)) {
            http_conn_pool_release(&conn, 0);
            request_retry_attempted = 1;
            goto open_connection;
        }
        http_conn_pool_release(&conn, 0);
        rt_trap_net(response_error_code == Err_Timeout ? "HTTP: request timed out"
                                                       : "HTTP: invalid response",
                    response_error_code);
        return NULL;
    }

    // Handle redirects (3xx with Location)
    if (req->follow_redirects &&
        (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) &&
        redirect_location) {
        int cross_origin = 0;
        if (redirects_remaining <= 0) {
            http_conn_pool_release(&conn, 0);
            free(status_text);
            free(redirect_location);
            if (headers_map && rt_obj_release_check0(headers_map))
                rt_obj_free(headers_map);
            rt_trap_net("HTTP: too many redirects", Err_ProtocolError);
            return NULL;
        }

        cross_origin = redirect_cross_origin_or_unknown(&req->url, redirect_location);
        http_conn_pool_release(&conn, 0);
        free(status_text);
        if (headers_map && rt_obj_release_check0(headers_map))
            rt_obj_free(headers_map);
        return http_follow_redirect(
            req, status, cross_origin, redirect_location, redirects_remaining - 1, deadline_us);
    }
    free(redirect_location);

    // Determine how to read body
    size_t body_len = 0;
    uint8_t *body = NULL;
    int body_read_failed = 0;
    const char *body_error_msg = "HTTP: incomplete response body";

    rt_string volatile content_length_owned = NULL;
    rt_string volatile transfer_encoding_owned = NULL;
    rt_string volatile connection_owned = NULL;
    http_conn_t header_lookup_cleanup_conn = conn;
    jmp_buf header_lookup_recovery;
    rt_trap_set_recovery(&header_lookup_recovery);
    if (RT_SETJMP(header_lookup_recovery) != 0) {
        char saved_error[256];
        int saved_net_code = rt_trap_get_net_code();
        const char *error = rt_trap_get_error();
        snprintf(saved_error,
                 sizeof(saved_error),
                 "%s",
                 error && error[0] ? error : "HTTP: response header lookup failed");
        rt_trap_clear_recovery();
        if (connection_owned)
            rt_string_unref((rt_string)connection_owned);
        if (transfer_encoding_owned)
            rt_string_unref((rt_string)transfer_encoding_owned);
        if (content_length_owned)
            rt_string_unref((rt_string)content_length_owned);
        if (headers_map && rt_obj_release_check0(headers_map))
            rt_obj_free(headers_map);
        free(status_text);
        http_conn_pool_release(&header_lookup_cleanup_conn, 0);
        if (saved_net_code)
            rt_trap_net(saved_error, saved_net_code);
        else
            rt_trap(saved_error);
        return NULL;
    }
    content_length_owned = get_header_value(headers_map, "content-length");
    transfer_encoding_owned = get_header_value(headers_map, "transfer-encoding");
    connection_owned = get_header_value(headers_map, "connection");
    rt_trap_clear_recovery();

    rt_string content_length_val = (rt_string)content_length_owned;
    rt_string transfer_encoding_val = (rt_string)transfer_encoding_owned;
    rt_string connection_val = (rt_string)connection_owned;
    int response_closes =
        connection_val && rt_http_header_value_has_token(rt_string_cstr(connection_val), "close");
    int response_keepalive = connection_val && rt_http_header_value_has_token(
                                                   rt_string_cstr(connection_val), "keep-alive");
    int has_content_length = content_length_val != NULL;

    bool no_body = response_has_no_body(req, status) != 0;
    bool chunked_transfer = false;
    bool unsupported_transfer_encoding = false;
    if (transfer_encoding_val) {
        int parsed_chunked = 0;
        if (!parse_transfer_encoding_supported(rt_string_cstr(transfer_encoding_val),
                                               &parsed_chunked)) {
            unsupported_transfer_encoding = true;
        } else {
            chunked_transfer = parsed_chunked != 0;
        }
    }

    if (no_body) {
        body = NULL;
        body_len = 0;
    } else if (unsupported_transfer_encoding) {
        http_conn_pool_release(&conn, 0);
        if (connection_val)
            rt_string_unref(connection_val);
        if (transfer_encoding_val)
            rt_string_unref(transfer_encoding_val);
        if (content_length_val)
            rt_string_unref(content_length_val);
        if (headers_map && rt_obj_release_check0(headers_map))
            rt_obj_free(headers_map);
        free(status_text);
        rt_trap_net("HTTP: unsupported Transfer-Encoding", Err_ProtocolError);
        return NULL;
    } else if (chunked_transfer) {
        body = read_body_chunked_conn(&conn, &body_len);
        if (!body) {
            body_read_failed = 1;
            body_error_msg = "HTTP: invalid chunked response body";
        }
    } else if (content_length_val) {
        size_t content_len = 0;
        if (parse_content_length_strict(rt_string_cstr(content_length_val), &content_len) != 0) {
            http_conn_pool_release(&conn, 0);
            if (connection_val)
                rt_string_unref(connection_val);
            if (transfer_encoding_val)
                rt_string_unref(transfer_encoding_val);
            if (content_length_val)
                rt_string_unref(content_length_val);
            if (headers_map && rt_obj_release_check0(headers_map))
                rt_obj_free(headers_map);
            free(status_text);
            rt_trap_net("HTTP: invalid Content-Length", Err_ProtocolError);
            return NULL;
        }
        if (content_len == 0) {
            body = NULL;
            body_len = 0;
        } else {
            body = read_body_content_length_conn(&conn, content_len, &body_len);
            if (!body) {
                body_read_failed = 1;
                body_error_msg = "HTTP: incomplete Content-Length response body";
            }
        }
    } else if (response_keepalive && !response_closes && http_minor >= 1) {
        body_read_failed = 1;
        body_error_msg = "HTTP: close-delimited keep-alive response lacks length";
    } else {
        // Read until connection closes
        http_body_read_status_t close_status = HTTP_BODY_READ_OK;
        body = read_body_until_close_conn(&conn, &body_len, &close_status);
        if (close_status != HTTP_BODY_READ_OK) {
            body_read_failed = 1;
            if (close_status == HTTP_BODY_READ_TOO_LARGE)
                body_error_msg = "HTTP: close-delimited response body too large";
            else if (close_status == HTTP_BODY_READ_OOM)
                body_error_msg = "HTTP: response body allocation failed";
            else
                body_error_msg = "HTTP: close-delimited response read failed";
        }
    }

    if (!no_body && body_read_failed) {
        int body_error_code = http_conn_failure_code(&conn, Err_ProtocolError);
        http_conn_pool_release(&conn, 0);
        if (connection_val)
            rt_string_unref(connection_val);
        if (transfer_encoding_val)
            rt_string_unref(transfer_encoding_val);
        if (content_length_val)
            rt_string_unref(content_length_val);
        if (headers_map && rt_obj_release_check0(headers_map))
            rt_obj_free(headers_map);
        free(status_text);
        rt_trap_net(body_error_code == Err_Timeout ? "HTTP: request timed out" : body_error_msg,
                    body_error_code);
        return NULL;
    }

    if (!no_body && body) {
        int transform_ok = 0;
        http_conn_t transform_cleanup_conn = conn;
        jmp_buf transform_recovery;
        rt_trap_set_recovery(&transform_recovery);
        if (RT_SETJMP(transform_recovery) != 0) {
            char saved_error[256];
            int saved_net_code = rt_trap_get_net_code();
            const char *error = rt_trap_get_error();
            snprintf(saved_error,
                     sizeof(saved_error),
                     "%s",
                     error && error[0] ? error : "HTTP: response transformation failed");
            rt_trap_clear_recovery();
            if (connection_val)
                rt_string_unref(connection_val);
            if (transfer_encoding_val)
                rt_string_unref(transfer_encoding_val);
            if (content_length_val)
                rt_string_unref(content_length_val);
            free(body);
            if (headers_map && rt_obj_release_check0(headers_map))
                rt_obj_free(headers_map);
            free(status_text);
            http_conn_pool_release(&transform_cleanup_conn, 0);
            if (saved_net_code)
                rt_trap_net(saved_error, saved_net_code);
            else
                rt_trap(saved_error);
            return NULL;
        }
        transform_ok =
            !http_conn_deadline_expired(&conn) &&
            (!chunked_transfer || set_header_value(headers_map, "transfer-encoding", NULL)) &&
            set_content_length_header(headers_map, body_len) &&
            maybe_decode_gzip_body(req, headers_map, &body, &body_len) &&
            !http_conn_deadline_expired(&conn);
        rt_trap_clear_recovery();
        if (!transform_ok) {
            int transform_error_code = http_conn_failure_code(&conn, Err_ProtocolError);
            http_conn_pool_release(&conn, 0);
            if (connection_val)
                rt_string_unref(connection_val);
            if (transfer_encoding_val)
                rt_string_unref(transfer_encoding_val);
            if (content_length_val)
                rt_string_unref(content_length_val);
            free(body);
            if (headers_map && rt_obj_release_check0(headers_map))
                rt_obj_free(headers_map);
            free(status_text);
            rt_trap_net(transform_error_code == Err_Timeout ? "HTTP: request timed out"
                                                            : "HTTP: invalid gzip response body",
                        transform_error_code);
            return NULL;
        }
    }

    if (http_conn_deadline_expired(&conn)) {
        http_conn_pool_release(&conn, 0);
        if (connection_val)
            rt_string_unref(connection_val);
        if (transfer_encoding_val)
            rt_string_unref(transfer_encoding_val);
        if (content_length_val)
            rt_string_unref(content_length_val);
        free(body);
        if (headers_map && rt_obj_release_check0(headers_map))
            rt_obj_free(headers_map);
        free(status_text);
        rt_trap_net("HTTP: request timed out", Err_Timeout);
        return NULL;
    }

    {
        int reusable = http_request_wants_pool(req) &&
                       (no_body || chunked_transfer || has_content_length) && !response_closes &&
                       (http_minor >= 1 || response_keepalive);
        http_conn_pool_release(&conn, reusable);
    }
    if (connection_val)
        rt_string_unref(connection_val);
    if (transfer_encoding_val)
        rt_string_unref(transfer_encoding_val);
    if (content_length_val)
        rt_string_unref(content_length_val);

    return http_make_response_obj(status, status_text, headers_map, body, body_len);
}

/// @brief Execute one public HTTP request under a single elapsed-time budget.
/// @details The wrapper creates the deadline exactly once. Redirect recursion,
///          stale-pool retry, address iteration, TLS, framing, and response
///          transformation all receive the same absolute value.
/// @param req Fully initialized request configuration.
/// @param redirects_remaining Remaining redirect-hop budget.
/// @return Newly owned managed response, or NULL after a returning trap.
rt_http_res_t *do_http_request(rt_http_req_t *req, int redirects_remaining) {
    int64_t deadline_us = req ? http_deadline_from_timeout_ms(req->timeout_ms) : 0;
    return do_http_request_deadline(req, redirects_remaining, deadline_us);
}

/// @brief Follow one streaming-download redirect and consume its Location copy.
/// @details A heap request record remains valid across recovery. Preparation
///          and recursive streaming are caught locally so the Boolean download
///          contract never leaks a managed trap, while all clone allocations
///          and the owned Location buffer are released exactly once.
/// @param source Request that received the redirect.
/// @param status Redirect status code.
/// @param cross_origin Nonzero when sensitive headers must be stripped.
/// @param location_owned Owned native Location string; always consumed.
/// @param redirects_remaining Remaining recursion budget.
/// @param out Open destination stream receiving the eventual response body.
/// @param deadline_us Shared absolute monotonic download deadline, or zero.
/// @return One on complete success; zero on preparation, transport, or trap failure.
static int http_follow_download_redirect(rt_http_req_t *source,
                                         int status,
                                         int cross_origin,
                                         char *location_owned,
                                         int redirects_remaining,
                                         FILE *out,
                                         int64_t deadline_us) {
    rt_http_req_t *const next_request = (rt_http_req_t *)calloc(1, sizeof(*next_request));
    if (!next_request) {
        free(location_owned);
        return 0;
    }

    char *volatile location = location_owned;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        rt_trap_clear_recovery();
        http_request_clone_cleanup(next_request);
        free((void *)location);
        free(next_request);
        return 0;
    }
    if (!http_prepare_redirect_request(
            next_request, source, status, cross_origin, (const char *)location)) {
        rt_trap_clear_recovery();
        http_request_clone_cleanup(next_request);
        free((void *)location);
        free(next_request);
        return 0;
    }
    free((void *)location);
    location = NULL;
    int ok = do_http_download_request_deadline(next_request, redirects_remaining, out, deadline_us);
    rt_trap_clear_recovery();
    http_request_clone_cleanup(next_request);
    free(next_request);
    return ok;
}

/// @brief Execute an HTTP download and stream a successful response to a file.
/// @details Supports HTTP/1.1 and HTTP/2, bounded redirects, Content-Length,
///          chunked, and close-delimited bodies. Only 2xx responses succeed;
///          transport, protocol, size-limit, allocation, and short-write
///          failures return zero and release all connection resources.
/// @param req Fully initialized download request.
/// @param redirects_remaining Remaining redirect-hop budget.
/// @param out Open writable destination stream owned by the caller.
/// @param deadline_us Shared absolute monotonic download deadline, or zero.
/// @return One after the complete 2xx body is written, otherwise zero.
static int do_http_download_request_deadline(rt_http_req_t *req,
                                             int redirects_remaining,
                                             FILE *out,
                                             int64_t deadline_us) {
    http_conn_t conn;
    char *request_str = NULL;
    uint8_t *request_buf = NULL;
    int status = -1;
    int http_minor = 1;
    char *status_text = NULL;
    void *headers_map = NULL;
    char *redirect_location = NULL;
    rt_string content_length_val = NULL;
    rt_string transfer_encoding_val = NULL;
    int ok = 0;
    int open_err = Err_NetworkError;

    rt_net_init_wsa();
    memset(&conn, 0, sizeof(conn));
    conn.socket_fd = INVALID_SOCK;

    if (!http_open_connection(req, &conn, deadline_us, &open_err))
        return 0;

    if (conn.http2) {
        rt_http_res_t *res = NULL;
        jmp_buf h2_recovery;
        rt_trap_set_recovery(&h2_recovery);
        if (RT_SETJMP(h2_recovery) != 0) {
            rt_trap_clear_recovery();
            return 0;
        }
        res = do_http2_request_opened(req, &conn, redirects_remaining);
        rt_trap_clear_recovery();
        if (!res)
            goto cleanup;
        if (res->status < 200 || res->status >= 300) {
            if (rt_obj_release_check0(res))
                rt_obj_free(res);
            goto cleanup;
        }
        {
            if (res->body_len > 0 && fwrite(res->body, 1, res->body_len, out) != res->body_len) {
                if (rt_obj_release_check0(res))
                    rt_obj_free(res);
                goto cleanup;
            }
        }
        if (rt_obj_release_check0(res))
            rt_obj_free(res);
        return 1;
    }

    request_str = build_request(req);
    if (!request_str)
        goto cleanup;

    size_t header_len = strlen(request_str);
    if (req->body_len > SIZE_MAX - header_len)
        goto cleanup;

    request_buf = (uint8_t *)malloc(header_len + req->body_len);
    if (!request_buf)
        goto cleanup;

    memcpy(request_buf, request_str, header_len);
    if (req->body && req->body_len > 0)
        memcpy(request_buf + header_len, req->body, req->body_len);

    if (http_conn_send(&conn, request_buf, header_len + req->body_len) < 0)
        goto cleanup;

    int response_head_ok = 0;
    http_conn_t response_head_cleanup_conn = conn;
    jmp_buf response_head_recovery;
    rt_trap_set_recovery(&response_head_recovery);
    if (RT_SETJMP(response_head_recovery) != 0) {
        rt_trap_clear_recovery();
        free(request_buf);
        free(request_str);
        http_conn_close(&response_head_cleanup_conn);
        return 0;
    }
    response_head_ok = read_response_head(
        &conn, &status, &http_minor, &status_text, &headers_map, &redirect_location);
    rt_trap_clear_recovery();
    if (!response_head_ok)
        goto cleanup;
    (void)http_minor;

    if (req->follow_redirects &&
        (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) &&
        redirect_location) {
        int cross_origin = 0;
        if (redirects_remaining <= 0)
            goto cleanup;

        cross_origin = redirect_cross_origin_or_unknown(&req->url, redirect_location);
        http_conn_close(&conn);
        if (headers_map && rt_obj_release_check0(headers_map))
            rt_obj_free(headers_map);
        headers_map = NULL;
        free(status_text);
        status_text = NULL;
        char *redirect_owned = redirect_location;
        redirect_location = NULL;
        free(request_buf);
        request_buf = NULL;
        free(request_str);
        request_str = NULL;
        return http_follow_download_redirect(
            req, status, cross_origin, redirect_owned, redirects_remaining - 1, out, deadline_us);
    }

    if (status < 200 || status >= 300)
        goto cleanup;

    {
        rt_string volatile content_length_owned = NULL;
        rt_string volatile transfer_encoding_owned = NULL;
        jmp_buf header_recovery;
        rt_trap_set_recovery(&header_recovery);
        if (RT_SETJMP(header_recovery) != 0) {
            rt_trap_clear_recovery();
            if (transfer_encoding_owned)
                rt_string_unref((rt_string)transfer_encoding_owned);
            if (content_length_owned)
                rt_string_unref((rt_string)content_length_owned);
            goto cleanup;
        }
        content_length_owned = get_header_value(headers_map, "content-length");
        transfer_encoding_owned = get_header_value(headers_map, "transfer-encoding");
        rt_trap_clear_recovery();
        content_length_val = (rt_string)content_length_owned;
        transfer_encoding_val = (rt_string)transfer_encoding_owned;
    }

    if (response_has_no_body(req, status)) {
        ok = 1;
    } else if (transfer_encoding_val) {
        int parsed_chunked = 0;
        if (!parse_transfer_encoding_supported(rt_string_cstr(transfer_encoding_val),
                                               &parsed_chunked) ||
            !parsed_chunked) {
            goto cleanup;
        }
        size_t streamed_len = 0;
        ok = write_body_chunked_conn(&conn, out, &streamed_len);
    } else if (content_length_val) {
        size_t content_len = 0;
        size_t streamed_len = 0;
        if (parse_content_length_strict(rt_string_cstr(content_length_val), &content_len) != 0)
            goto cleanup;
        ok = write_body_content_length_conn(&conn, content_len, out, &streamed_len);
    } else {
        size_t streamed_len = 0;
        ok = write_body_until_close_conn(&conn, out, &streamed_len);
    }

cleanup:
    if (transfer_encoding_val)
        rt_string_unref(transfer_encoding_val);
    if (content_length_val)
        rt_string_unref(content_length_val);
    free(redirect_location);
    if (headers_map && rt_obj_release_check0(headers_map))
        rt_obj_free(headers_map);
    free(status_text);
    free(request_buf);
    free(request_str);
    http_conn_close(&conn);
    return ok;
}

/// @brief Stream one public HTTP download under a single elapsed-time budget.
/// @param req Fully initialized download request.
/// @param redirects_remaining Remaining redirect-hop budget.
/// @param out Open writable destination stream owned by the caller.
/// @return One after complete success, otherwise zero.
int do_http_download_request(rt_http_req_t *req, int redirects_remaining, FILE *out) {
    int64_t deadline_us = req ? http_deadline_from_timeout_ms(req->timeout_ms) : 0;
    return do_http_download_request_deadline(req, redirects_remaining, out, deadline_us);
}
