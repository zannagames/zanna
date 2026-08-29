//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/network/rt_sse.c
// Purpose: Strict, reconnecting HTTP(S) EventSource client with lossless deadlines.
// Key invariants:
//   - Response heads, chunk framing, lines, and accumulated events are bounded.
//   - One receive owner retains parser/transport state while Close requests shutdown.
//   - Untimed receives poll cancellation without surfacing synthetic timeouts.
//   - A monotonic whole-event timeout never discards partial framing or field bytes.
//   - Public receivers and managed Strings are validated before payload access.
// Ownership/Lifetime:
//   - Clients are GC-managed and own URL, native transport, parser, and metadata storage.
// Links: rt_sse.h (C ABI), rt_network.h (TCP), rt_tls.h (TLS),
//        docs/zannalib/network.md (runtime contract)
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_sse.c
 * @brief Implements a strict reconnecting HTTP EventSource client.
 * @details The client performs bounded HTTP or HTTPS setup, parses response
 *          framing and Server-Sent Event fields incrementally, preserves
 *          partial parser state across whole-event timeouts, and coordinates a
 *          single receive owner with concurrent cancellation and close.
 */

// Feature-test macros must appear before every system header. Defining them is
// harmless on non-POSIX toolchains and avoids raw OS checks in runtime modules.
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE 1
#endif
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include "rt_sse.h"

#include "rt_ascii.h"
#include "rt_internal.h"
#include "rt_network_http_internal.h"
#include "rt_network_internal.h"
#include "rt_object.h"
#include "rt_platform.h"
#include "rt_result.h"
#include "rt_string.h"
#include "rt_threads.h"
#include "rt_time.h"
#include "rt_tls.h"
#include "rt_trap.h"

#include <setjmp.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if RT_PLATFORM_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
typedef CRITICAL_SECTION sse_mutex_t;
#else
#include <pthread.h>
typedef pthread_mutex_t sse_mutex_t;
#endif

/// @brief Release one optional native SSE allocation.
/// @details Centralizing native disposal keeps cleanup ledgers readable and
///          makes ownership-transfer sites distinguishable from managed
///          `rt_obj_free` operations. Passing NULL is valid, matching `free`.
/// @param allocation Native heap allocation to release, or NULL.
static inline void sse_native_discard(void *allocation) {
    free(allocation);
}

/// @brief Maximum accumulated `data:` payload bytes for a single SSE event.
/// @details The SSE spec permits multiple `data:` lines per event. This cap
///          bounds memory use when a peer sends a never-ending event without a
///          blank-line delimiter.
#define SSE_MAX_EVENT_DATA (4u * 1024u * 1024u)

/// @brief Maximum HTTP response-head bytes accepted before the event stream.
#define SSE_MAX_HTTP_HEAD_BYTES (16u * 1024u)

/// @brief Maximum number of HTTP response fields and chunk trailer fields.
#define SSE_MAX_HTTP_FIELDS 100u

/// @brief Maximum encoded chunk payload accepted from one HTTP/1.1 chunk.
#define SSE_MAX_CHUNK_BYTES (16u * 1024u * 1024u)

/// @brief Maximum native bytes in one HTTP or event-stream line.
#define SSE_MAX_LINE_BYTES (64u * 1024u)

/// @brief Maximum delay before an untimed receive observes concurrent Close.
/// @details Some WinSock providers do not cancel an already-blocked synchronous
///          `recv` when another thread calls `shutdown`. A bounded socket timeout
///          lets the receive owner poll cancellation without closing its handle
///          concurrently or changing the public blocking-receive contract.
#define SSE_CLOSE_POLL_TIMEOUT_MS 100

/// @brief Internal reason the current parser read could not produce a byte or line.
typedef enum {
    SSE_READ_NONE = 0,
    SSE_READ_TIMEOUT,
    SSE_READ_EOF,
    SSE_READ_PROTOCOL,
    SSE_READ_OOM,
    SSE_READ_LIMIT
} sse_read_failure_t;

/// @brief Initialize one SSE-native mutex.
/// @param mutex Zeroed native mutex storage owned by a partial client.
/// @return Nonzero on success; zero when native initialization fails.
static int sse_mutex_init(sse_mutex_t *mutex) {
#if RT_PLATFORM_WINDOWS
    return InitializeCriticalSectionEx(mutex, 0, 0) != FALSE ? 1 : 0;
#else
    return pthread_mutex_init(mutex, NULL) == 0 ? 1 : 0;
#endif
}

/// @brief Acquire an initialized SSE-native mutex.
/// @param mutex Mutex owned by a live SSE client.
static void sse_mutex_lock(sse_mutex_t *mutex) {
#if RT_PLATFORM_WINDOWS
    EnterCriticalSection(mutex);
#else
    (void)pthread_mutex_lock(mutex);
#endif
}

/// @brief Release an SSE-native mutex held by the current thread.
/// @param mutex Locked mutex owned by a live SSE client.
static void sse_mutex_unlock(sse_mutex_t *mutex) {
#if RT_PLATFORM_WINDOWS
    LeaveCriticalSection(mutex);
#else
    (void)pthread_mutex_unlock(mutex);
#endif
}

/// @brief Destroy a quiescent initialized SSE-native mutex.
/// @param mutex Mutex whose client is being finalized.
static void sse_mutex_destroy(sse_mutex_t *mutex) {
#if RT_PLATFORM_WINDOWS
    DeleteCriticalSection(mutex);
#else
    (void)pthread_mutex_destroy(mutex);
#endif
}

//=============================================================================
// Internal Structure
//=============================================================================

typedef struct {
    socket_t socket_fd;    // Connected socket for http://, or TLS-owned socket for https://
    rt_tls_session_t *tls; // TLS session for https://
    bool is_open;
    bool close_requested; ///< Close was requested while a receive owns transport state.
    bool recv_active;     ///< Exactly one receive operation currently owns parser state.
    char *url;
    char *last_event_type; // Most recent "event:" field
    size_t last_event_type_len;
    char *last_event_id; // Most recent "id:" field
    size_t last_event_id_len;
    int64_t retry_ms;
    uint8_t read_buf[4096];
    size_t read_buf_len;
    size_t read_buf_pos;
    int chunked;
    size_t chunk_remaining;
    int chunk_crlf_remaining;
    int chunk_in_trailers;
    size_t chunk_trailer_count;
    size_t chunk_trailer_bytes;
    int payload_eof;
    int content_length_present;
    uint64_t content_remaining;
    int payload_has_pushback;
    uint8_t payload_pushback;
    int payload_skip_optional_lf;
    int event_stream_at_start;
    // Timed-receive state (VDOC-151): one monotonic deadline is carried
    // through every transport read, and a timeout preserves — rather than
    // corrupts — partial line and partial event state.
    int64_t read_deadline_us; ///< 0 = untimed receive
    int read_timed_out;       ///< last transport failure was a recv timeout
    sse_read_failure_t read_failure;
    char *raw_pending; ///< partial raw line preserved across a timeout
    size_t raw_pending_len;
    char *payload_pending; ///< partial payload line preserved across a timeout
    size_t payload_pending_len;
    char *event_data; ///< partial event accumulation preserved across a timeout
    size_t event_len;
    size_t event_cap;
    int event_saw_data;
    size_t event_data_lines;
    char *pending_event_type; ///< `event:` value for the event being built
    size_t pending_event_type_len;
    int last_recv_delivered; ///< 1 when the last receive dispatched an event
    sse_mutex_t state_lock;  ///< Protects lifecycle, transport publication, and metadata reads.
    sse_mutex_t recv_lock;   ///< Serializes the stateful event-stream parser.
    int state_lock_initialized;
    int recv_lock_initialized;
} rt_sse_impl;

/// @brief Compare two fixed-length byte ranges using locale-independent ASCII folding.
/// @param lhs First byte range; must contain at least @p count bytes.
/// @param rhs Second byte range; must contain at least @p count bytes.
/// @param count Number of bytes to compare.
/// @return Zero when equal ignoring ASCII case, otherwise the first folded-byte difference.
static int sse_ascii_ncasecmp(const char *lhs, const char *rhs, size_t count) {
    for (size_t index = 0; index < count; index++) {
        int left = rt_ascii_tolower((unsigned char)lhs[index]);
        int right = rt_ascii_tolower((unsigned char)rhs[index]);
        if (left != right)
            return left - right;
    }
    return 0;
}

/// @brief Validate and cast an opaque SSE client receiver.
/// @details Class identity, complete payload size, and both initialization
///          markers are checked before any native mutex or transport field is
///          touched. This remains safe when an embedding trap hook returns.
/// @param object Candidate managed receiver.
/// @param operation Operation-specific diagnostic text.
/// @return Initialized SSE payload, or NULL after one trap.
static rt_sse_impl *sse_require(void *object, const char *operation) {
    if (!rt_obj_is_instance(object, RT_SSE_CLASS_ID, sizeof(rt_sse_impl)) ||
        !((rt_sse_impl *)object)->state_lock_initialized ||
        !((rt_sse_impl *)object)->recv_lock_initialized) {
        rt_trap(operation ? operation : "SSE: invalid client");
        return NULL;
    }
    return (rt_sse_impl *)object;
}

/// @brief Test whether a runtime String is a complete NUL-free C-string view.
/// @details Network URL entry points need a C-string for the URL parser but
///          must validate the managed handle and its full byte length before
///          dereferencing it.
/// @param value Candidate runtime String.
/// @return Nonzero for a valid handle without embedded NUL bytes.
static int sse_string_is_cstr(rt_string value) {
    if (!value || !rt_string_is_handle(value))
        return 0;
    int64_t length = rt_str_len(value);
    const char *bytes = rt_string_cstr(value);
    if (!bytes || length < 0 || (uint64_t)length > (uint64_t)SIZE_MAX)
        return 0;
    return length == 0 || memchr(bytes, '\0', (size_t)length) == NULL;
}

/// @brief Drop partial line and event state for restart or teardown.
/// @details Releases native raw/payload fragments and pending event type,
///          clears accumulated data metadata, and resets payload pushback.
/// @param sse Client parser state to reset.
static void sse_reset_partial_state(rt_sse_impl *sse) {
    sse_native_discard(sse->raw_pending);
    sse->raw_pending = NULL;
    sse->raw_pending_len = 0;
    sse_native_discard(sse->payload_pending);
    sse->payload_pending = NULL;
    sse->payload_pending_len = 0;
    sse->event_len = 0;
    sse->event_saw_data = 0;
    sse->event_data_lines = 0;
    sse_native_discard(sse->pending_event_type);
    sse->pending_event_type = NULL;
    sse->pending_event_type_len = 0;
    sse->payload_has_pushback = 0;
    sse->payload_skip_optional_lf = 0;
}

/// @brief Check whether a host requires brackets in an HTTP authority.
/// @param host NUL-terminated parsed host.
/// @return One for an unbracketed colon-bearing host, typically IPv6;
///         otherwise zero.
static int sse_host_needs_brackets(const char *host) {
    return host && strchr(host, ':') != NULL && host[0] != '[';
}

/// @brief Test whether bytes can be emitted as one Last-Event-ID header value.
/// @param value Candidate event-ID bytes.
/// @param length Exact byte length.
/// @return Nonzero when the value contains no C0 control, DEL, or embedded NUL.
static int sse_header_value_is_valid(const char *value, size_t length) {
    if (!value && length != 0)
        return 0;
    for (size_t index = 0; index < length; index++) {
        unsigned char byte = (unsigned char)value[index];
        // Reject the full C0 control range plus DEL so stored IDs can never
        // smuggle control bytes into the reconnect request header.
        if (byte < 0x20u || byte == 0x7Fu)
            return 0;
    }
    return 1;
}

/// @brief Validate a parsed host before placing it in an HTTP Host field.
/// @param host Nonempty NUL-terminated host text.
/// @return One when no whitespace, controls, DEL, or URL delimiters are
///         present; otherwise zero.
static int sse_host_is_valid(const char *host) {
    if (!host || !*host)
        return 0;
    for (const unsigned char *p = (const unsigned char *)host; *p; ++p) {
        if (*p <= 0x20 || *p == 0x7Fu || *p == '/' || *p == '?' || *p == '#')
            return 0;
    }
    return 1;
}

/// @brief Validate an origin-form HTTP request target.
/// @param target NUL-terminated path and optional query.
/// @return One when the target begins with slash and contains no whitespace,
///         controls, DEL, or fragment marker; otherwise zero.
static int sse_request_target_is_valid(const char *target) {
    if (!target || target[0] != '/')
        return 0;
    for (const unsigned char *p = (const unsigned char *)target; *p; ++p) {
        if (*p <= 0x20 || *p == 0x7Fu || *p == '#')
            return 0;
    }
    return 1;
}

/// @brief Apply a receive timeout to the currently published SSE socket.
/// @param sse Live client whose transport is owned by the receive operation.
/// @param timeout_ms Timeout in milliseconds; zero restores blocking I/O.
/// @return Nonzero when no socket is active or the native option was applied.
static int sse_set_recv_timeout(rt_sse_impl *sse, int timeout_ms) {
    if (!sse || sse->socket_fd == INVALID_SOCK)
        return 1;
    return set_socket_timeout(sse->socket_fd, timeout_ms, true) ? 1 : 0;
}

/// @brief Complete one TCP connect using a bounded non-blocking readiness wait.
/// @details Blocking mode is restored on every path, and SO_ERROR query
///          failure is treated as a failed connect rather than false success.
/// @param sock Newly created socket owned by the caller.
/// @param addr Destination address.
/// @param addrlen Native address length.
/// @param timeout_ms Per-attempt portion of the caller's overall deadline.
/// @param err_out Optional native error output.
/// @return True only for a connected socket restored to blocking mode.
static bool sse_connect_socket_with_timeout(
    socket_t sock, const struct sockaddr *addr, socklen_t addrlen, int timeout_ms, int *err_out) {
    if (err_out)
        *err_out = 0;

    if (timeout_ms > 0) {
        if (!rt_socket_set_nonblocking(sock, true)) {
            if (err_out)
                *err_out = GET_LAST_ERROR();
            return false;
        }

        if (connect(sock, addr, addrlen) == SOCK_ERROR) {
            int err = GET_LAST_ERROR();
            if (rt_socket_error_is_in_progress(err)) {
                int ready = wait_socket(sock, timeout_ms, true);
                if (ready <= 0) {
                    if (err_out)
                        *err_out = ready == 0 ? ETIMEDOUT : GET_LAST_ERROR();
                    (void)rt_socket_set_nonblocking(sock, false);
                    return false;
                }

                int so_error = 0;
                if (!rt_socket_pending_error(sock, &so_error) || so_error != 0) {
                    if (err_out)
                        *err_out = so_error != 0 ? so_error : GET_LAST_ERROR();
                    (void)rt_socket_set_nonblocking(sock, false);
                    return false;
                }
            } else {
                if (err_out)
                    *err_out = err;
                (void)rt_socket_set_nonblocking(sock, false);
                return false;
            }
        }

        if (!rt_socket_set_nonblocking(sock, false)) {
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

/// @brief Resolve and connect a plain SSE TCP transport under one overall deadline.
/// @details Address candidates share @p timeout_ms instead of each receiving a
///          fresh full timeout. Every failed candidate is closed before the
///          next address is attempted.
/// @param host Valid DNS name or numeric address.
/// @param port TCP port in 1..65535.
/// @param timeout_ms Overall connect budget in milliseconds.
/// @param err_code Receives a public `Err_*` category on failure.
/// @return Connected blocking socket, or @ref INVALID_SOCK.
static socket_t sse_create_tcp_socket(const char *host, int port, int timeout_ms, int *err_code) {
    struct addrinfo hints, *res = NULL, *rp = NULL;
    socket_t sock = INVALID_SOCK;
    int last_err = 0;
    char port_str[16];
    uint64_t started_ms = rt_socket_monotonic_ms();

    if (err_code)
        *err_code = Err_NetworkError;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(port_str, sizeof(port_str), "%d", port);

    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) {
        if (err_code)
            *err_code = Err_HostNotFound;
        return INVALID_SOCK;
    }

    for (rp = res; rp; rp = rp->ai_next) {
        int attempt_timeout = timeout_ms;
        if (timeout_ms > 0 && started_ms != 0) {
            uint64_t now_ms = rt_socket_monotonic_ms();
            if (now_ms != 0 && now_ms >= started_ms) {
                uint64_t elapsed_ms = now_ms - started_ms;
                if (elapsed_ms >= (uint64_t)timeout_ms) {
                    last_err = ETIMEDOUT;
                    break;
                }
                attempt_timeout = timeout_ms - (int)elapsed_ms;
            }
        }
        socket_t candidate = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (candidate == INVALID_SOCK)
            continue;
        suppress_sigpipe(candidate);
        if (sse_connect_socket_with_timeout(
                candidate, rp->ai_addr, (socklen_t)rp->ai_addrlen, attempt_timeout, &last_err)) {
            sock = candidate;
            break;
        }
        CLOSE_SOCKET(candidate);
    }
    freeaddrinfo(res);

    if (sock == INVALID_SOCK && err_code) {
        if (last_err == CONN_REFUSED)
            *err_code = Err_ConnectionRefused;
        else if (rt_socket_error_is_timeout(last_err))
            *err_code = Err_Timeout;
        else
            *err_code = Err_NetworkError;
    }

    return sock;
}

/// @brief Format a stable SSE connection diagnostic from a network category.
/// @param msg Destination byte buffer.
/// @param msg_cap Destination capacity including the terminator.
/// @param prefix Operation-specific diagnostic prefix.
/// @param err_code Runtime network error category.
static void sse_format_connect_error(char *msg, size_t msg_cap, const char *prefix, int err_code) {
    const char *detail = "connection failed";
    if (err_code == Err_HostNotFound)
        detail = "host not found";
    else if (err_code == Err_ConnectionRefused)
        detail = "connection refused";
    else if (err_code == Err_Timeout)
        detail = "connection timeout";
    snprintf(msg, msg_cap, "%s: %s", prefix, detail);
}

/// @brief Detach and close the currently published SSE transport.
/// @details Transport and all HTTP body-framing/read-ahead state are reset
///          atomically under the state mutex. TLS or the plain socket is then
///          closed outside the critical section. The operation is idempotent.
/// @param sse Client whose transport is consumed; NULL is a no-op.
static void sse_close_transport(rt_sse_impl *sse) {
    if (!sse)
        return;
    rt_tls_session_t *tls = NULL;
    socket_t socket_fd = INVALID_SOCK;
    if (sse->state_lock_initialized)
        sse_mutex_lock(&sse->state_lock);
    tls = sse->tls;
    socket_fd = tls ? INVALID_SOCK : sse->socket_fd;
    sse->tls = NULL;
    sse->socket_fd = INVALID_SOCK;
    sse->read_buf_len = 0;
    sse->read_buf_pos = 0;
    sse->chunk_remaining = 0;
    sse->chunk_crlf_remaining = 0;
    sse->chunk_in_trailers = 0;
    sse->chunk_trailer_count = 0;
    sse->chunk_trailer_bytes = 0;
    sse->chunked = 0;
    sse->content_length_present = 0;
    sse->content_remaining = 0;
    sse->payload_has_pushback = 0;
    sse->payload_skip_optional_lf = 0;
    sse->payload_eof = 0;
    sse->event_stream_at_start = 1;
    if (sse->state_lock_initialized)
        sse_mutex_unlock(&sse->state_lock);

    if (tls)
        rt_tls_close(tls);
    else if (socket_fd != INVALID_SOCK)
        CLOSE_SOCKET(socket_fd);
}

/// @brief Publish a newly connected transport unless concurrent Close won the race.
/// @details The caller retains ownership on failure. On success the SSE client
///          owns either @p tls (and its descriptor) or @p socket_fd until the
///          receive owner detaches it through @ref sse_close_transport.
/// @param sse Initialized client.
/// @param tls Connected TLS session, or NULL for plain TCP.
/// @param socket_fd TLS descriptor or connected plain socket.
/// @return Nonzero when ownership transferred to the client.
static int sse_publish_transport(rt_sse_impl *sse, rt_tls_session_t *tls, socket_t socket_fd) {
    int published = 0;
    sse_mutex_lock(&sse->state_lock);
    if (!sse->close_requested && !sse->tls && sse->socket_fd == INVALID_SOCK) {
        sse->tls = tls;
        sse->socket_fd = socket_fd;
        published = 1;
    }
    sse_mutex_unlock(&sse->state_lock);
    return published;
}

/// @brief Test whether a receive should stop after concurrent Close.
/// @param sse Initialized client.
/// @return Nonzero after Close marked the lifecycle cancelled.
static int sse_close_was_requested(rt_sse_impl *sse) {
    int requested = 0;
    sse_mutex_lock(&sse->state_lock);
    requested = sse->close_requested ? 1 : 0;
    sse_mutex_unlock(&sse->state_lock);
    return requested;
}

/// @brief Finalize an SSE client and every native resource it owns.
/// @details Closes the transport, discards partial parser state, event storage,
///          URL and metadata snapshots, then destroys initialized mutexes.
/// @param obj SSE client allocation being finalized, or NULL.
static void rt_sse_finalize(void *obj) {
    if (!obj)
        return;
    rt_sse_impl *sse = (rt_sse_impl *)obj;
    sse_close_transport(sse);
    sse_reset_partial_state(sse);
    sse_native_discard(sse->event_data);
    sse->event_data = NULL;
    sse->event_cap = 0;
    sse_native_discard(sse->url);
    sse_native_discard(sse->last_event_type);
    sse_native_discard(sse->last_event_id);
    if (sse->recv_lock_initialized) {
        sse_mutex_destroy(&sse->recv_lock);
        sse->recv_lock_initialized = 0;
    }
    if (sse->state_lock_initialized) {
        sse_mutex_destroy(&sse->state_lock);
        sse->state_lock_initialized = 0;
    }
}

//=============================================================================
// Transport Helpers
//=============================================================================

/// @brief Send an entire byte span through the active TLS or TCP transport.
/// @details Both paths drain short writes; the plain path retries interrupted
///          system calls.
/// @param sse Active SSE client with a published transport.
/// @param data Byte span to transmit.
/// @param len Number of bytes to send.
/// @return One after all bytes are written; zero on closure or transport
///         failure.
static int sse_transport_send_all(rt_sse_impl *sse, const void *data, size_t len) {
    if (!sse || (!data && len > 0))
        return 0;
    if (len == 0)
        return 1;
    if (sse->tls) {
        size_t total = 0;
        while (total < len) {
            long sent = rt_tls_send(sse->tls, (const uint8_t *)data + total, len - total);
            if (sent <= 0)
                return 0;
            total += (size_t)sent;
        }
        return 1;
    }
    if (sse->socket_fd == INVALID_SOCK)
        return 0;
    size_t total = 0;
    while (total < len) {
        int sent = send(sse->socket_fd,
                        (const char *)data + total,
                        (int)((len - total) > INT_MAX ? INT_MAX : (len - total)),
                        SEND_FLAGS);
        if (sent < 0 && rt_socket_error_is_interrupted(GET_LAST_ERROR()))
            continue;
        if (sent <= 0)
            return 0;
        total += (size_t)sent;
    }
    return 1;
}

/// @brief Read bytes directly from the active TLS or TCP transport.
/// @details The TCP path retries interrupted system calls; the TLS path
///          delegates record processing to the session.
/// @param sse Active SSE client with a published transport.
/// @param buffer Destination byte buffer.
/// @param len Maximum bytes to receive.
/// @return Positive byte count, zero for orderly closure, or a negative
///         transport result on failure.
static long sse_transport_read(rt_sse_impl *sse, void *buffer, size_t len) {
    if (!sse || !buffer || len == 0)
        return -1;
    if (sse->tls)
        return rt_tls_recv(sse->tls, buffer, len);
    if (sse->socket_fd == INVALID_SOCK)
        return -1;
    for (;;) {
        long received =
            recv(sse->socket_fd, (char *)buffer, (int)(len > INT_MAX ? INT_MAX : len), 0);
        if (received < 0 && rt_socket_error_is_interrupted(GET_LAST_ERROR()))
            continue;
        return received;
    }
}

/// @brief Read one raw HTTP byte through the client's native read-ahead buffer.
/// @details Enforces the receive operation's single monotonic deadline, updates
///          timeout/EOF/error classification, and preserves unread buffered
///          bytes for later framing consumers.
/// @param sse Active receive owner.
/// @param byte Destination for one received byte.
/// @return One when a byte is produced; otherwise zero with read-failure state
///         recorded on @p sse.
static int sse_raw_recv_byte(rt_sse_impl *sse, uint8_t *byte) {
    if (sse->read_buf_pos < sse->read_buf_len) {
        *byte = sse->read_buf[sse->read_buf_pos++];
        sse->read_failure = SSE_READ_NONE;
        return 1;
    }

    // One monotonic deadline across every read of a timed receive: a peer
    // trickling one byte per interval cannot extend the call past its budget.
    if (sse->read_deadline_us > 0) {
        int64_t remaining_us = sse->read_deadline_us - rt_clock_ticks_us();
        if (remaining_us <= 0) {
            sse->read_timed_out = 1;
            sse->read_failure = SSE_READ_TIMEOUT;
            return 0;
        }
        int64_t remaining_ms = (remaining_us + 999) / 1000;
        if (remaining_ms > INT_MAX)
            remaining_ms = INT_MAX;
        sse_set_recv_timeout(sse, (int)remaining_ms);
    }

    long n = sse_transport_read(sse, sse->read_buf, sizeof(sse->read_buf));
    if (n <= 0) {
        sse->read_timed_out = (n < 0 && rt_socket_recv_timed_out()) ? 1 : 0;
        sse->read_failure = sse->read_timed_out ? SSE_READ_TIMEOUT : SSE_READ_EOF;
        return 0;
    }

    sse->read_buf_len = (size_t)n;
    sse->read_buf_pos = 0;
    *byte = sse->read_buf[sse->read_buf_pos++];
    sse->read_failure = SSE_READ_NONE;
    return 1;
}

/// @brief Read one strictly CRLF-terminated HTTP framing line into native memory.
/// @details Partial bytes are retained across a timed receive. Bare LF is a
///          protocol error, allocation growth is overflow checked, and no
///          managed allocation occurs while the native line is owned.
/// @param sse Live parser state.
/// @param max_bytes Maximum line bytes excluding CRLF and the NUL terminator.
/// @param length_out Optional output for the returned byte length.
/// @return Heap-owned NUL-terminated line, or NULL with `read_failure` set.
static char *sse_raw_recv_line(rt_sse_impl *sse, size_t max_bytes, size_t *length_out) {
    size_t cap = 256;
    size_t len = 0;
    char *line = NULL;

    if (length_out)
        *length_out = 0;
    if (!sse || max_bytes == 0 || max_bytes == SIZE_MAX) {
        if (sse)
            sse->read_failure = SSE_READ_LIMIT;
        return NULL;
    }

    // Resume a partial line preserved across a retryable timeout so timeout
    // recovery never turns a fragment into a complete logical line.
    if (sse->raw_pending) {
        line = sse->raw_pending;
        len = sse->raw_pending_len;
        if (len > max_bytes) {
            sse_native_discard(line);
            sse->raw_pending = NULL;
            sse->raw_pending_len = 0;
            sse->read_failure = SSE_READ_LIMIT;
            return NULL;
        }
        cap = len <= max_bytes - (max_bytes >= 255 ? 255 : max_bytes) ? len + 256 : max_bytes + 1;
        if (cap <= len)
            cap = len + 1;
        char *grown = (char *)realloc(line, cap);
        if (!grown) {
            sse_native_discard(line);
            sse->raw_pending = NULL;
            sse->raw_pending_len = 0;
            sse->read_failure = SSE_READ_OOM;
            return NULL;
        }
        line = grown;
        sse->raw_pending = NULL;
        sse->raw_pending_len = 0;
    } else {
        if (cap > max_bytes + 1)
            cap = max_bytes + 1;
        line = (char *)malloc(cap);
        if (!line) {
            sse->read_failure = SSE_READ_OOM;
            return NULL;
        }
    }

    while (1) {
        uint8_t byte = 0;
        if (!sse_raw_recv_byte(sse, &byte)) {
            if (sse->read_timed_out && len > 0) {
                // Retryable timeout mid-line: stash the fragment for resume.
                sse->raw_pending = line;
                sse->raw_pending_len = len;
                return NULL;
            }
            // EOF / hard failure: an unterminated final fragment is discarded
            // (per the EventSource incomplete-line rule) instead of being
            // delivered as a complete line.
            sse_native_discard(line);
            return NULL;
        }

        if (byte == '\n') {
            if (len == 0 || line[len - 1] != '\r') {
                sse_native_discard(line);
                sse->read_failure = SSE_READ_PROTOCOL;
                return NULL;
            }
            len--;
            break;
        }

        if (len >= max_bytes && byte != '\r') {
            sse_native_discard(line);
            sse->read_failure = SSE_READ_LIMIT;
            return NULL;
        }

        if (len >= cap) {
            size_t maximum_cap = max_bytes + 1;
            size_t new_cap = cap > maximum_cap / 2 ? maximum_cap : cap * 2;
            if (new_cap <= cap) {
                sse_native_discard(line);
                sse->read_failure = SSE_READ_LIMIT;
                return NULL;
            }
            char *grown = (char *)realloc(line, new_cap);
            if (!grown) {
                sse_native_discard(line);
                sse->read_failure = SSE_READ_OOM;
                return NULL;
            }
            line = grown;
            cap = new_cap;
        }

        line[len++] = (char)byte;
    }

    line[len] = '\0';
    sse->read_failure = SSE_READ_NONE;
    if (length_out)
        *length_out = len;
    return line;
}

/// @brief Test whether a byte is permitted in an RFC 7230 token.
/// @param byte Unsigned byte value.
/// @return Nonzero for an ASCII token character.
static int sse_http_token_char(unsigned char byte) {
    if (rt_ascii_isalnum(byte))
        return 1;
    switch (byte) {
        case '!':
        case '#':
        case '$':
        case '%':
        case '&':
        case '\'':
        case '*':
        case '+':
        case '-':
        case '.':
        case '^':
        case '_':
        case '`':
        case '|':
        case '~':
            return 1;
        default:
            return 0;
    }
}

/// @brief Parse one bounded HTTP/1.1 chunk-size line including extensions.
/// @details Extensions are preserved as supported syntax but must follow the
///          token/quoted-string grammar; the payload size is capped before it
///          reaches parser state.
/// @param line NUL-terminated line without CRLF.
/// @param size_out Receives the decoded chunk size.
/// @return Nonzero for a canonical, bounded chunk-size line.
static int sse_parse_chunk_size(const char *line, size_t *size_out) {
    size_t size = 0;
    const char *p = line;
    int saw_digit = 0;
    if (!p || !*p)
        return 0;

    while (*p) {
        char c = *p;
        int digit;
        if (c >= '0' && c <= '9')
            digit = c - '0';
        else if (c >= 'a' && c <= 'f')
            digit = 10 + (c - 'a');
        else if (c >= 'A' && c <= 'F')
            digit = 10 + (c - 'A');
        else
            break;

        saw_digit = 1;
        if (size > (SIZE_MAX >> 4))
            return 0;
        size = (size << 4) | (size_t)digit;
        p++;
    }
    if (!saw_digit)
        return 0;
    while (*p == ' ' || *p == '\t')
        p++;

    while (*p != '\0') {
        if (*p++ != ';')
            return 0;
        while (*p == ' ' || *p == '\t')
            p++;
        const char *name = p;
        while (sse_http_token_char((unsigned char)*p))
            p++;
        if (p == name)
            return 0;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '=') {
            p++;
            while (*p == ' ' || *p == '\t')
                p++;
            if (*p == '"') {
                p++;
                int closed = 0;
                while (*p) {
                    unsigned char byte = (unsigned char)*p++;
                    if (byte == '"') {
                        closed = 1;
                        break;
                    }
                    if (byte == '\\') {
                        unsigned char escaped = (unsigned char)*p++;
                        if (escaped == '\0' ||
                            !(escaped == '\t' || (escaped >= 0x20u && escaped != 0x7Fu))) {
                            return 0;
                        }
                    } else if (!(byte == '\t' || byte == ' ' || byte == '!' ||
                                 (byte >= 0x23u && byte <= 0x5Bu) || byte >= 0x5Du)) {
                        return 0;
                    }
                }
                if (!closed)
                    return 0;
            } else {
                const char *value = p;
                while (sse_http_token_char((unsigned char)*p))
                    p++;
                if (p == value)
                    return 0;
            }
            while (*p == ' ' || *p == '\t')
                p++;
        }
        if (*p != '\0' && *p != ';')
            return 0;
    }

    if (size > SSE_MAX_CHUNK_BYTES)
        return 0;

    *size_out = size;
    return 1;
}

/// @brief Non-owning view of one parsed HTTP field line.
typedef struct {
    const char *name;
    size_t name_len;
    const char *value;
    size_t value_len;
} sse_http_field_view_t;

/// @brief Parse and validate one HTTP field line in-place.
/// @details Field names must be non-empty tokens; leading whitespace (obsolete
///          folding), embedded NUL, CR/LF, DEL, and non-tab controls in values
///          are rejected. Surrounding optional whitespace is trimmed.
/// @param line Mutable NUL-terminated line buffer.
/// @param line_len Exact byte length excluding the terminator.
/// @param field_out Receives non-owning name/value spans within @p line.
/// @return Nonzero for a valid field line.
static int sse_parse_http_field_line(char *line,
                                     size_t line_len,
                                     sse_http_field_view_t *field_out) {
    if (!line || !field_out || line_len == 0 || strlen(line) != line_len || line[0] == ' ' ||
        line[0] == '\t') {
        return 0;
    }
    size_t colon = 0;
    while (colon < line_len && line[colon] != ':') {
        if (!sse_http_token_char((unsigned char)line[colon]))
            return 0;
        colon++;
    }
    if (colon == 0 || colon == line_len)
        return 0;

    size_t value_start = colon + 1;
    while (value_start < line_len && (line[value_start] == ' ' || line[value_start] == '\t'))
        value_start++;
    size_t value_end = line_len;
    while (value_end > value_start && (line[value_end - 1] == ' ' || line[value_end - 1] == '\t')) {
        value_end--;
    }
    for (size_t index = value_start; index < value_end; index++) {
        unsigned char byte = (unsigned char)line[index];
        if ((byte < 0x20u && byte != '\t') || byte == 0x7Fu)
            return 0;
    }

    line[colon] = '\0';
    line[value_end] = '\0';
    field_out->name = line;
    field_out->name_len = colon;
    field_out->value = line + value_start;
    field_out->value_len = value_end - value_start;
    return 1;
}

/// @brief Compare an HTTP field-name view with one lowercase literal.
/// @param field Parsed field view.
/// @param literal NUL-terminated ASCII field name.
/// @return Nonzero when lengths and bytes match ignoring ASCII case.
static int sse_http_field_is(const sse_http_field_view_t *field, const char *literal) {
    size_t literal_len = literal ? strlen(literal) : 0;
    return field && literal && field->name_len == literal_len &&
           sse_ascii_ncasecmp(field->name, literal, literal_len) == 0;
}

/// @brief Validate one chunk trailer and reject fields that alter framing.
/// @param line Mutable trailer line without CRLF.
/// @param line_len Exact trailer byte length.
/// @return Nonzero for a syntactically valid, permitted trailer field.
static int sse_chunk_trailer_is_valid(char *line, size_t line_len) {
    sse_http_field_view_t field;
    if (!sse_parse_http_field_line(line, line_len, &field))
        return 0;
    return !sse_http_field_is(&field, "transfer-encoding") &&
           !sse_http_field_is(&field, "content-length") && !sse_http_field_is(&field, "host") &&
           !sse_http_field_is(&field, "trailer");
}

/// @brief Read one decoded SSE payload byte from fixed-length or chunked HTTP framing.
/// @details Chunk delimiters and trailers have resumable state, so a timeout at
///          any framing boundary does not make the next call interpret a CRLF
///          or trailer as a new chunk size. Content-Length is enforced exactly.
/// @param sse Live receive parser.
/// @param byte Receives one decoded payload byte.
/// @return Nonzero when a byte was produced; zero on timeout, EOF, or framing failure.
static int sse_payload_recv_byte(rt_sse_impl *sse, uint8_t *byte) {
    if (sse->payload_has_pushback) {
        sse->payload_has_pushback = 0;
        *byte = sse->payload_pushback;
        return 1;
    }
    if (sse->payload_eof) {
        sse->read_failure = SSE_READ_EOF;
        return 0;
    }
    if (!sse->chunked) {
        if (sse->content_length_present && sse->content_remaining == 0) {
            sse->payload_eof = 1;
            sse->read_failure = SSE_READ_EOF;
            return 0;
        }
        if (!sse_raw_recv_byte(sse, byte))
            return 0;
        if (sse->content_length_present)
            sse->content_remaining--;
        return 1;
    }

    for (;;) {
        while (sse->chunk_crlf_remaining > 0) {
            uint8_t delimiter = 0;
            if (!sse_raw_recv_byte(sse, &delimiter))
                return 0;
            uint8_t expected = sse->chunk_crlf_remaining == 2 ? '\r' : '\n';
            if (delimiter != expected) {
                sse->read_failure = SSE_READ_PROTOCOL;
                return 0;
            }
            sse->chunk_crlf_remaining--;
        }

        if (sse->chunk_in_trailers) {
            size_t trailer_len = 0;
            char *trailer = sse_raw_recv_line(sse, SSE_MAX_LINE_BYTES, &trailer_len);
            if (!trailer)
                return 0;
            if (trailer_len == 0) {
                sse_native_discard(trailer);
                sse->chunk_in_trailers = 0;
                sse->payload_eof = 1;
                sse->read_failure = SSE_READ_EOF;
                return 0;
            }
            if (sse->chunk_trailer_count >= SSE_MAX_HTTP_FIELDS ||
                trailer_len > SSE_MAX_HTTP_HEAD_BYTES - sse->chunk_trailer_bytes ||
                !sse_chunk_trailer_is_valid(trailer, trailer_len)) {
                sse_native_discard(trailer);
                sse->read_failure = SSE_READ_PROTOCOL;
                return 0;
            }
            sse->chunk_trailer_count++;
            sse->chunk_trailer_bytes += trailer_len;
            sse_native_discard(trailer);
            continue;
        }

        if (sse->chunk_remaining == 0) {
            size_t size_line_len = 0;
            char *size_line = sse_raw_recv_line(sse, 1024u, &size_line_len);
            if (!size_line)
                return 0;
            size_t next_chunk = 0;
            int ok =
                strlen(size_line) == size_line_len && sse_parse_chunk_size(size_line, &next_chunk);
            sse_native_discard(size_line);
            if (!ok) {
                sse->read_failure = SSE_READ_PROTOCOL;
                return 0;
            }
            if (next_chunk == 0) {
                sse->chunk_in_trailers = 1;
                continue;
            }
            sse->chunk_remaining = next_chunk;
        }

        if (!sse_raw_recv_byte(sse, byte))
            return 0;
        sse->chunk_remaining--;
        if (sse->chunk_remaining == 0)
            sse->chunk_crlf_remaining = 2;
        return 1;
    }
}

/// @brief Read one SSE event-stream line into native memory.
/// @details Event streams accept CRLF, lone CR, or lone LF delimiters. A byte
///          read after lone CR is preserved as pushback, while partial content
///          survives timeout and all growth is bounded and overflow checked.
/// @param sse Live receive parser.
/// @param length_out Optional exact line-length output.
/// @return Heap-owned NUL-terminated line, or NULL with `read_failure` set.
static char *sse_payload_recv_line(rt_sse_impl *sse, size_t *length_out) {
    size_t cap = 256;
    size_t len = 0;
    char *line = NULL;

    if (length_out)
        *length_out = 0;

    if (sse->payload_skip_optional_lf) {
        uint8_t next = 0;
        if (!sse_payload_recv_byte(sse, &next))
            return NULL;
        sse->payload_skip_optional_lf = 0;
        if (next != '\n') {
            sse->payload_has_pushback = 1;
            sse->payload_pushback = next;
        }
    }

    // Resume a partial line preserved across a retryable timeout so timeout
    // recovery never turns a fragment into a complete logical line.
    if (sse->payload_pending) {
        line = sse->payload_pending;
        len = sse->payload_pending_len;
        if (len > SSE_MAX_LINE_BYTES) {
            sse_native_discard(line);
            sse->payload_pending = NULL;
            sse->payload_pending_len = 0;
            sse->read_failure = SSE_READ_LIMIT;
            return NULL;
        }
        cap = len <= SSE_MAX_LINE_BYTES - 255u ? len + 256u : SSE_MAX_LINE_BYTES + 1u;
        char *grown = (char *)realloc(line, cap);
        if (!grown) {
            sse_native_discard(line);
            sse->payload_pending = NULL;
            sse->payload_pending_len = 0;
            sse->read_failure = SSE_READ_OOM;
            return NULL;
        }
        line = grown;
        sse->payload_pending = NULL;
        sse->payload_pending_len = 0;
    } else {
        line = (char *)malloc(cap);
        if (!line) {
            sse->read_failure = SSE_READ_OOM;
            return NULL;
        }
    }

    while (1) {
        uint8_t byte = 0;
        if (!sse_payload_recv_byte(sse, &byte)) {
            if (sse->read_timed_out && len > 0) {
                // Retryable timeout mid-line: stash the fragment for resume.
                sse->payload_pending = line;
                sse->payload_pending_len = len;
                return NULL;
            }
            // EOF / hard failure: an unterminated final fragment is discarded
            // (per the EventSource incomplete-line rule) instead of being
            // delivered as a complete line.
            sse_native_discard(line);
            return NULL;
        }

        if (byte == '\n') {
            break;
        }

        if (byte == '\r') {
            sse->payload_skip_optional_lf = 1;
            break;
        }

        if (len >= SSE_MAX_LINE_BYTES) {
            sse_native_discard(line);
            sse->read_failure = SSE_READ_LIMIT;
            return NULL;
        }

        if (len + 1 >= cap) {
            size_t maximum_cap = SSE_MAX_LINE_BYTES + 1u;
            size_t new_cap = cap > maximum_cap / 2u ? maximum_cap : cap * 2u;
            if (new_cap <= cap) {
                sse_native_discard(line);
                sse->read_failure = SSE_READ_LIMIT;
                return NULL;
            }
            char *grown = (char *)realloc(line, new_cap);
            if (!grown) {
                sse_native_discard(line);
                sse->read_failure = SSE_READ_OOM;
                return NULL;
            }
            line = grown;
            cap = new_cap;
        }

        line[len++] = (char)byte;
    }

    line[len] = '\0';
    sse->read_failure = SSE_READ_NONE;
    if (length_out)
        *length_out = len;
    return line;
}

/// @brief Parse a strict HTTP/1.x status line returned to the SSE request.
/// @param status_line NUL-terminated line without CRLF.
/// @param line_len Exact status-line byte length.
/// @return Numeric status in 100..599, or -1 for malformed syntax.
static int sse_parse_http_status_line(const char *status_line, size_t line_len) {
    if (!status_line || line_len < 13 || strlen(status_line) != line_len)
        return -1;
    if (strncmp(status_line, "HTTP/1.1 ", 9) != 0 && strncmp(status_line, "HTTP/1.0 ", 9) != 0)
        return -1;
    if (status_line[9] < '0' || status_line[9] > '9' || status_line[10] < '0' ||
        status_line[10] > '9' || status_line[11] < '0' || status_line[11] > '9') {
        return -1;
    }
    if (status_line[12] != ' ')
        return -1;
    for (size_t index = 13; index < line_len; index++) {
        unsigned char byte = (unsigned char)status_line[index];
        if ((byte < 0x20u && byte != '\t') || byte == 0x7Fu)
            return -1;
    }
    int status =
        (status_line[9] - '0') * 100 + (status_line[10] - '0') * 10 + (status_line[11] - '0');
    return status >= 100 && status <= 599 ? status : -1;
}

/// @brief Validate Content-Encoding for an SSE stream.
/// @details The runtime exposes raw event bytes and therefore accepts only one
///          or more comma-separated `identity` codings. Empty elements,
///          parameters, and compression codings are rejected.
/// @param value Trimmed NUL-terminated field value.
/// @return Nonzero when every coding is exactly `identity`.
static int sse_content_encoding_supported(const char *value) {
    const char *p = value;
    int saw_token = 0;
    if (!value || !*value)
        return 0;
    while (*p) {
        const char *start;
        const char *end;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '\0')
            return 0;
        start = p;
        while (*p && *p != ',')
            p++;
        end = p;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
            end--;
        if ((size_t)(end - start) != 8u || sse_ascii_ncasecmp(start, "identity", 8u) != 0)
            return 0;
        saw_token = 1;
        if (*p == ',') {
            p++;
            if (*p == '\0')
                return 0;
        }
    }
    return saw_token ? 1 : 0;
}

/// @brief Validate the media type of an SSE response.
/// @details The type must be exactly `text/event-stream` ignoring ASCII case;
///          optional MIME parameters may follow only after a semicolon.
/// @param value Trimmed NUL-terminated Content-Type value.
/// @return Nonzero for the SSE media type with syntactically separated parameters.
static int sse_content_type_supported(const char *value) {
    if (!value || strlen(value) < 17u || sse_ascii_ncasecmp(value, "text/event-stream", 17u) != 0)
        return 0;
    const char *tail = value + 17;
    while (*tail == ' ' || *tail == '\t')
        tail++;
    return *tail == '\0' || *tail == ';';
}

/// @brief Parse one canonical unsigned decimal HTTP field value.
/// @param value Trimmed NUL-terminated bytes.
/// @param result_out Receives the parsed value.
/// @return Nonzero for one or more digits without overflow or trailing bytes.
static int sse_parse_u64_decimal(const char *value, uint64_t *result_out) {
    if (!value || !*value || !result_out)
        return 0;
    uint64_t value_number = 0;
    for (const char *cursor = value; *cursor; cursor++) {
        if (!rt_ascii_isdigit((unsigned char)*cursor))
            return 0;
        unsigned digit = (unsigned)(*cursor - '0');
        if (value_number > (UINT64_MAX - digit) / UINT64_C(10))
            return 0;
        value_number = value_number * UINT64_C(10) + digit;
    }
    *result_out = value_number;
    return 1;
}

//=============================================================================
// Public API
//=============================================================================

static void sse_release_managed(void *object);
static void sse_save_trap(char *output, size_t capacity, const char *fallback);

/// @brief Resources that must survive a non-local exit from SSE URL setup.
/// @details The record is volatile in @ref sse_open_url, so every successfully
///          acquired native or managed owner remains defined after longjmp.
typedef struct {
    rt_string url;
    void *url_obj;
    rt_string scheme;
    rt_string host;
    rt_string path;
    rt_string query;
    rt_string redirect_current;
    rt_string redirect_location;
    rt_string redirect_next;
    char *current_url;
    char *target;
    char *request;
    char *location_value;
    char *next_url;
    char *permanent_url;
    rt_tls_session_t *pending_tls;
    socket_t pending_socket;
} sse_open_recovery_t;

/// @brief Release every URL-open resource recorded for trap recovery.
/// @details A published transport is detached through @ref sse_close_transport;
///          an unpublished TLS session or socket is closed directly. Managed
///          references and native buffers are each consumed exactly once.
/// @param sse Client receiving the new transport.
/// @param recovery Volatile ownership ledger populated after each acquisition.
static void sse_open_recovery_cleanup(rt_sse_impl *sse, volatile sse_open_recovery_t *recovery) {
    if (!recovery)
        return;
    if (recovery->pending_tls)
        rt_tls_close((rt_tls_session_t *)recovery->pending_tls);
    else if (recovery->pending_socket != INVALID_SOCK)
        CLOSE_SOCKET(recovery->pending_socket);
    recovery->pending_tls = NULL;
    recovery->pending_socket = INVALID_SOCK;
    sse_close_transport(sse);

    sse_release_managed((void *)recovery->redirect_next);
    sse_release_managed((void *)recovery->redirect_location);
    sse_release_managed((void *)recovery->redirect_current);
    sse_release_managed((void *)recovery->query);
    sse_release_managed((void *)recovery->path);
    sse_release_managed((void *)recovery->host);
    sse_release_managed((void *)recovery->scheme);
    sse_release_managed(recovery->url_obj);
    sse_release_managed((void *)recovery->url);
    recovery->redirect_next = NULL;
    recovery->redirect_location = NULL;
    recovery->redirect_current = NULL;
    recovery->query = NULL;
    recovery->path = NULL;
    recovery->host = NULL;
    recovery->scheme = NULL;
    recovery->url_obj = NULL;
    recovery->url = NULL;

    sse_native_discard((void *)recovery->next_url);
    sse_native_discard((void *)recovery->permanent_url);
    sse_native_discard((void *)recovery->location_value);
    sse_native_discard((void *)recovery->request);
    sse_native_discard((void *)recovery->target);
    sse_native_discard((void *)recovery->current_url);
    recovery->next_url = NULL;
    recovery->permanent_url = NULL;
    recovery->location_value = NULL;
    recovery->request = NULL;
    recovery->target = NULL;
    recovery->current_url = NULL;
}

/// @brief Derive the next connect budget from the active whole-receive deadline.
/// @details Constructor and untimed reconnects retain the 30-second transport
///          default. Timed receives clamp every redirect/connect attempt to the
///          remaining monotonic budget so reconnect cannot overrun RecvFor.
/// @param sse Live client.
/// @param timeout_out Receives a positive timeout in milliseconds.
/// @return Nonzero when time remains; zero after recording a timeout failure.
static int sse_connect_timeout(rt_sse_impl *sse, int *timeout_out) {
    int timeout_ms = 30000;
    if (sse->read_deadline_us > 0) {
        int64_t remaining_us = sse->read_deadline_us - rt_clock_ticks_us();
        if (remaining_us <= 0) {
            sse->read_timed_out = 1;
            sse->read_failure = SSE_READ_TIMEOUT;
            return 0;
        }
        int64_t remaining_ms = (remaining_us + 999) / 1000;
        if (remaining_ms < timeout_ms)
            timeout_ms = (int)remaining_ms;
    }
    if (timeout_out)
        *timeout_out = timeout_ms;
    return 1;
}

/// @brief Open and validate an EventSource stream for one absolute URL.
/// @details Parses HTTP(S), follows at most five redirects, establishes and
///          publishes TCP/TLS ownership, sends a bounded GET request, consumes
///          informational replies, and accepts only status 200 with a supported
///          event-stream content type and unambiguous body framing. Reconnects
///          may attach the last safe event ID. One recovery ledger releases
///          every partially acquired native and managed resource after traps.
/// @param sse Initialized client receiving transport and framing state.
/// @param url_str Absolute NUL-terminated HTTP or HTTPS URL.
/// @param allow_resume Nonzero to send a safe nonempty Last-Event-ID.
/// @param err_msg Destination for a stable failure diagnostic.
/// @param err_msg_cap Destination capacity including the terminator.
/// @return One when a usable stream is published; otherwise zero.
static int sse_open_url(
    rt_sse_impl *sse, const char *url_str, int allow_resume, char *err_msg, size_t err_msg_cap) {
    rt_string url = NULL;
    void *url_obj = NULL;
    rt_string scheme = NULL;
    rt_string host = NULL;
    rt_string path = NULL;
    rt_string query = NULL;
    const char *scheme_cstr;
    const char *host_cstr;
    const char *path_cstr;
    const char *query_cstr;
    char *current_url = NULL;
    char *target = NULL;
    char *request = NULL;
    char *redirect_location = NULL;
    char *permanent_url = NULL;
    char *status_line = NULL;
    rt_tls_session_t *new_tls = NULL;
    socket_t new_socket = INVALID_SOCK;
    const char *last_event_id =
        (allow_resume && sse->last_event_id && *sse->last_event_id) ? sse->last_event_id : NULL;
    size_t last_event_id_len = last_event_id ? sse->last_event_id_len : 0;
    int redirects_left = 5;
    int status;
    int is_secure;
    int saw_stream_type = 0;
    int unsupported_content_encoding = 0;
    int unsupported_transfer_encoding = 0;
    int saw_content_type = 0;
    int saw_transfer_encoding = 0;
    int saw_content_encoding = 0;
    int saw_location = 0;
    int saw_content_length = 0;
    size_t response_head_bytes;
    size_t response_field_count;
    size_t status_line_len = 0;
    int informational_count = 0;
    int rlen;
    int64_t port;
    size_t path_len;
    size_t query_len;
    size_t target_len;
    size_t request_cap;
    char host_header[512];
    int host_header_len = 0;
    int connect_timeout_ms = 30000;
    volatile sse_open_recovery_t open_recovery = {0};
    open_recovery.pending_socket = INVALID_SOCK;

    if (err_msg && err_msg_cap > 0)
        err_msg[0] = '\0';

    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        sse_save_trap(err_msg, err_msg_cap, "SSE: URL setup failed");
        rt_trap_clear_recovery();
        sse_native_discard(status_line);
        sse_open_recovery_cleanup(sse, &open_recovery);
        return 0;
    }

    current_url = url_str ? strdup(url_str) : NULL;
    open_recovery.current_url = current_url;
    if (!current_url) {
        if (err_msg && err_msg_cap > 0)
            snprintf(err_msg, err_msg_cap, "SSE: OOM");
        goto fail;
    }

retry:
    sse_close_transport(sse);
    sse->chunked = 0;
    sse->chunk_remaining = 0;
    sse->read_buf_len = 0;
    sse->read_buf_pos = 0;
    saw_stream_type = 0;
    unsupported_content_encoding = 0;
    unsupported_transfer_encoding = 0;
    saw_content_type = 0;
    saw_transfer_encoding = 0;
    saw_content_encoding = 0;
    saw_location = 0;
    saw_content_length = 0;
    if (!sse_connect_timeout(sse, &connect_timeout_ms)) {
        if (err_msg && err_msg_cap > 0)
            snprintf(err_msg, err_msg_cap, "SSE: reconnect timeout");
        goto fail;
    }

    url = rt_string_from_bytes(current_url, strlen(current_url));
    open_recovery.url = url;
    if (!url) {
        if (err_msg && err_msg_cap > 0)
            snprintf(err_msg, err_msg_cap, "SSE: OOM");
        goto fail;
    }
    url_obj = rt_url_parse(url);
    open_recovery.url_obj = url_obj;
    if (!url_obj) {
        if (err_msg && err_msg_cap > 0)
            snprintf(err_msg, err_msg_cap, "SSE: invalid URL");
        goto fail;
    }
    scheme = rt_url_scheme(url_obj);
    open_recovery.scheme = scheme;
    host = rt_url_host(url_obj);
    open_recovery.host = host;
    path = rt_url_path(url_obj);
    open_recovery.path = path;
    query = rt_url_query(url_obj);
    open_recovery.query = query;
    scheme_cstr = rt_string_cstr(scheme);
    host_cstr = rt_string_cstr(host);
    path_cstr = rt_string_cstr(path);
    query_cstr = rt_string_cstr(query);
    is_secure = scheme_cstr && strcmp(scheme_cstr, "https") == 0;

    if (!scheme_cstr || !sse_host_is_valid(host_cstr) ||
        (strcmp(scheme_cstr, "http") != 0 && strcmp(scheme_cstr, "https") != 0)) {
        if (err_msg && err_msg_cap > 0)
            snprintf(err_msg, err_msg_cap, "SSE: invalid URL");
        goto fail;
    }

    port = rt_url_port(url_obj);
    if (port == 0)
        port = is_secure ? 443 : 80;
    if (port < 1 || port > 65535) {
        if (err_msg && err_msg_cap > 0)
            snprintf(err_msg, err_msg_cap, "SSE: invalid URL port");
        goto fail;
    }
    if (is_secure) {
        rt_tls_config_t cfg;
        rt_tls_config_init(&cfg);
        cfg.hostname = host_cstr;
        cfg.alpn_protocol = "http/1.1";
        cfg.timeout_ms = connect_timeout_ms;
        new_tls = rt_tls_connect(host_cstr, (uint16_t)port, &cfg);
        open_recovery.pending_tls = new_tls;
        if (!new_tls) {
            const char *detail = rt_tls_last_error();
            if (err_msg && err_msg_cap > 0) {
                if (detail && *detail)
                    snprintf(err_msg, err_msg_cap, "SSE: TLS connection failed: %s", detail);
                else
                    snprintf(err_msg, err_msg_cap, "SSE: TLS connection failed");
            }
            goto fail;
        }
        new_socket = (socket_t)rt_tls_get_socket(new_tls);
        open_recovery.pending_socket = new_socket;
    } else {
        int err_code = Err_NetworkError;
        new_socket = sse_create_tcp_socket(host_cstr, (int)port, connect_timeout_ms, &err_code);
        open_recovery.pending_socket = new_socket;
        if (new_socket == INVALID_SOCK) {
            if (err_msg && err_msg_cap > 0)
                sse_format_connect_error(err_msg, err_msg_cap, "SSE", err_code);
            goto fail;
        }
        if (!set_socket_timeout(new_socket, 30000, true) ||
            !set_socket_timeout(new_socket, 30000, false)) {
            if (err_msg && err_msg_cap > 0)
                snprintf(err_msg, err_msg_cap, "SSE: socket timeout setup failed");
            goto fail;
        }
    }
    if (!sse_publish_transport(sse, new_tls, new_socket)) {
        if (err_msg && err_msg_cap > 0)
            snprintf(err_msg, err_msg_cap, "SSE: connection closed");
        goto fail;
    }
    new_tls = NULL;
    open_recovery.pending_tls = NULL;
    open_recovery.pending_socket = INVALID_SOCK;

    path_len = path_cstr && *path_cstr ? strlen(path_cstr) : 1;
    query_len = query_cstr && *query_cstr ? strlen(query_cstr) : 0;
    target_len = path_len + (query_len ? query_len + 1 : 0);
    target = (char *)malloc(target_len + 1);
    open_recovery.target = target;
    if (!target) {
        if (err_msg && err_msg_cap > 0)
            snprintf(err_msg, err_msg_cap, "SSE: OOM");
        goto fail;
    }
    snprintf(target,
             target_len + 1,
             query_len ? "%s?%s" : "%s",
             path_cstr && *path_cstr ? path_cstr : "/",
             query_cstr ? query_cstr : "");
    if (!sse_request_target_is_valid(target)) {
        if (err_msg && err_msg_cap > 0)
            snprintf(err_msg, err_msg_cap, "SSE: invalid URL target");
        goto fail;
    }

    if (port != (is_secure ? 443 : 80)) {
        host_header_len = snprintf(host_header,
                                   sizeof(host_header),
                                   sse_host_needs_brackets(host_cstr) ? "[%s]:%lld" : "%s:%lld",
                                   host_cstr,
                                   (long long)port);
    } else {
        host_header_len = snprintf(host_header,
                                   sizeof(host_header),
                                   sse_host_needs_brackets(host_cstr) ? "[%s]" : "%s",
                                   host_cstr);
    }
    if (host_header_len <= 0 || (size_t)host_header_len >= sizeof(host_header)) {
        if (err_msg && err_msg_cap > 0)
            snprintf(err_msg, err_msg_cap, "SSE: Host header too large");
        goto fail;
    }
    if (last_event_id && !sse_header_value_is_valid(last_event_id, last_event_id_len)) {
        last_event_id = NULL;
        last_event_id_len = 0;
    }

    {
        size_t event_id_len = last_event_id ? last_event_id_len : 0;
        size_t target_size = strlen(target);
        size_t host_size = strlen(host_header);
        if (target_size > SIZE_MAX - host_size || target_size + host_size > SIZE_MAX - 256 ||
            (last_event_id && event_id_len > SIZE_MAX - 32) ||
            (last_event_id && target_size + host_size + 256 > SIZE_MAX - event_id_len - 32)) {
            if (err_msg && err_msg_cap > 0)
                snprintf(err_msg, err_msg_cap, "SSE: request too large");
            goto fail;
        }
        request_cap = target_size + host_size + 256 + (last_event_id ? event_id_len + 32 : 0);
    }
    request = (char *)malloc(request_cap);
    open_recovery.request = request;
    if (!request) {
        if (err_msg && err_msg_cap > 0)
            snprintf(err_msg, err_msg_cap, "SSE: OOM");
        goto fail;
    }
    rlen = snprintf(request,
                    request_cap,
                    "GET %s HTTP/1.1\r\n"
                    "Host: %s\r\n"
                    "Accept: text/event-stream\r\n"
                    "Cache-Control: no-cache\r\n"
                    "Connection: keep-alive\r\n"
                    "%s%s%s"
                    "\r\n",
                    target,
                    host_header,
                    last_event_id ? "Last-Event-ID: " : "",
                    last_event_id ? last_event_id : "",
                    last_event_id ? "\r\n" : "");
    if (rlen <= 0 || (size_t)rlen >= request_cap) {
        if (err_msg && err_msg_cap > 0)
            snprintf(err_msg, err_msg_cap, "SSE: request too large");
        goto fail;
    }

    if (!sse_transport_send_all(sse, request, (size_t)rlen)) {
        if (err_msg && err_msg_cap > 0)
            snprintf(err_msg, err_msg_cap, "SSE: request send failed");
        goto fail;
    }

read_response_head:
    status_line_len = 0;
    response_field_count = 0;
    status_line = sse_raw_recv_line(sse, 1024u, &status_line_len);
    if (!status_line) {
        if (err_msg && err_msg_cap > 0)
            snprintf(err_msg, err_msg_cap, "SSE: no HTTP response");
        goto fail;
    }
    response_head_bytes = status_line_len + 2u;
    status = sse_parse_http_status_line(status_line, status_line_len);
    sse_native_discard(status_line);
    status_line = NULL;
    if (status < 0) {
        if (err_msg && err_msg_cap > 0)
            snprintf(err_msg, err_msg_cap, "SSE: invalid HTTP response");
        goto fail;
    }

    while (1) {
        size_t line_len = 0;
        char *line = sse_raw_recv_line(sse, SSE_MAX_LINE_BYTES, &line_len);
        if (!line) {
            if (err_msg && err_msg_cap > 0)
                snprintf(err_msg, err_msg_cap, "SSE: incomplete HTTP headers");
            goto fail;
        }

        if (line_len > SSE_MAX_HTTP_HEAD_BYTES - response_head_bytes - 2u ||
            (line_len > 0 && response_field_count >= SSE_MAX_HTTP_FIELDS)) {
            sse_native_discard(line);
            if (err_msg && err_msg_cap > 0)
                snprintf(err_msg, err_msg_cap, "SSE: HTTP response headers exceed limits");
            goto fail;
        }
        response_head_bytes += line_len + 2u;
        if (line_len == 0) {
            sse_native_discard(line);
            break;
        }
        response_field_count++;
        sse_http_field_view_t field;
        if (!sse_parse_http_field_line(line, line_len, &field)) {
            sse_native_discard(line);
            if (err_msg && err_msg_cap > 0)
                snprintf(err_msg, err_msg_cap, "SSE: malformed HTTP response header");
            goto fail;
        }

        if (sse_http_field_is(&field, "content-type")) {
            if (saw_content_type++) {
                sse_native_discard(line);
                if (err_msg && err_msg_cap > 0)
                    snprintf(err_msg, err_msg_cap, "SSE: duplicate Content-Type");
                goto fail;
            }
            saw_stream_type = sse_content_type_supported(field.value);
        } else if (sse_http_field_is(&field, "transfer-encoding")) {
            if (saw_transfer_encoding++) {
                sse_native_discard(line);
                if (err_msg && err_msg_cap > 0)
                    snprintf(err_msg, err_msg_cap, "SSE: duplicate Transfer-Encoding");
                goto fail;
            }
            if (field.value_len == 7u && sse_ascii_ncasecmp(field.value, "chunked", 7u) == 0)
                sse->chunked = 1;
            else
                unsupported_transfer_encoding = 1;
        } else if (sse_http_field_is(&field, "location")) {
            if (saw_location++) {
                sse_native_discard(line);
                if (err_msg && err_msg_cap > 0)
                    snprintf(err_msg, err_msg_cap, "SSE: duplicate Location");
                goto fail;
            }
            redirect_location = strdup(field.value);
            open_recovery.location_value = redirect_location;
            if (!redirect_location) {
                sse_native_discard(line);
                if (err_msg && err_msg_cap > 0)
                    snprintf(err_msg, err_msg_cap, "SSE: OOM");
                goto fail;
            }
        } else if (sse_http_field_is(&field, "content-encoding")) {
            if (saw_content_encoding++) {
                sse_native_discard(line);
                if (err_msg && err_msg_cap > 0)
                    snprintf(err_msg, err_msg_cap, "SSE: duplicate Content-Encoding");
                goto fail;
            }
            if (!sse_content_encoding_supported(field.value))
                unsupported_content_encoding = 1;
        } else if (sse_http_field_is(&field, "content-length")) {
            uint64_t content_length = 0;
            if (saw_content_length++ || !sse_parse_u64_decimal(field.value, &content_length)) {
                sse_native_discard(line);
                if (err_msg && err_msg_cap > 0)
                    snprintf(err_msg, err_msg_cap, "SSE: invalid or duplicate Content-Length");
                goto fail;
            }
            sse->content_length_present = 1;
            sse->content_remaining = content_length;
        }

        sse_native_discard(line);
    }

    if (status >= 100 && status < 200) {
        if (status == 101 || ++informational_count > 8) {
            if (err_msg && err_msg_cap > 0)
                snprintf(err_msg, err_msg_cap, "SSE: invalid informational response sequence");
            goto fail;
        }
        sse_native_discard(redirect_location);
        redirect_location = NULL;
        open_recovery.location_value = NULL;
        saw_stream_type = 0;
        unsupported_content_encoding = 0;
        unsupported_transfer_encoding = 0;
        saw_content_type = 0;
        saw_transfer_encoding = 0;
        saw_content_encoding = 0;
        saw_location = 0;
        saw_content_length = 0;
        sse->chunked = 0;
        sse->content_length_present = 0;
        sse->content_remaining = 0;
        goto read_response_head;
    }

    if ((status == 301 || status == 302 || status == 303 || status == 307 || status == 308) &&
        redirect_location && *redirect_location) {
        rt_string current_rt = NULL;
        rt_string location_rt = NULL;
        rt_string next_rt = NULL;
        char *next_url = NULL;

        if (redirects_left-- <= 0) {
            if (err_msg && err_msg_cap > 0)
                snprintf(err_msg, err_msg_cap, "SSE: too many redirects");
            goto fail;
        }

        current_rt = rt_string_from_bytes(current_url, strlen(current_url));
        open_recovery.redirect_current = current_rt;
        location_rt = rt_string_from_bytes(redirect_location, strlen(redirect_location));
        open_recovery.redirect_location = location_rt;
        next_rt = rt_http_resolve_redirect_url(current_rt, location_rt);
        open_recovery.redirect_next = next_rt;
        next_url = strdup(rt_string_cstr(next_rt) ? rt_string_cstr(next_rt) : "");
        open_recovery.next_url = next_url;
        rt_string_unref(next_rt);
        open_recovery.redirect_next = NULL;
        rt_string_unref(location_rt);
        open_recovery.redirect_location = NULL;
        rt_string_unref(current_rt);
        open_recovery.redirect_current = NULL;
        if (!next_url || !*next_url) {
            sse_native_discard(next_url);
            open_recovery.next_url = NULL;
            if (err_msg && err_msg_cap > 0)
                snprintf(err_msg, err_msg_cap, "SSE: invalid redirect URL");
            goto fail;
        }

        if (status == 301 || status == 308) {
            char *replacement = strdup(next_url);
            if (!replacement) {
                if (err_msg && err_msg_cap > 0)
                    snprintf(err_msg, err_msg_cap, "SSE: OOM");
                goto fail;
            }
            sse_native_discard(permanent_url);
            permanent_url = replacement;
            open_recovery.permanent_url = permanent_url;
        }

        sse_native_discard(request);
        request = NULL;
        open_recovery.request = NULL;
        sse_native_discard(target);
        target = NULL;
        open_recovery.target = NULL;
        sse_native_discard(redirect_location);
        redirect_location = NULL;
        open_recovery.location_value = NULL;
        rt_string_unref(query);
        query = NULL;
        open_recovery.query = NULL;
        rt_string_unref(path);
        path = NULL;
        open_recovery.path = NULL;
        rt_string_unref(host);
        host = NULL;
        open_recovery.host = NULL;
        rt_string_unref(scheme);
        scheme = NULL;
        open_recovery.scheme = NULL;
        if (url_obj && rt_obj_release_check0(url_obj))
            rt_obj_free(url_obj);
        url_obj = NULL;
        open_recovery.url_obj = NULL;
        rt_string_unref(url);
        url = NULL;
        open_recovery.url = NULL;
        sse_native_discard(current_url);
        current_url = next_url;
        open_recovery.current_url = current_url;
        open_recovery.next_url = NULL;
        goto retry;
    }

    if (status != 200) {
        if (err_msg && err_msg_cap > 0)
            snprintf(err_msg, err_msg_cap, "SSE: endpoint did not return HTTP 200");
        goto fail;
    }

    if (unsupported_content_encoding) {
        if (err_msg && err_msg_cap > 0)
            snprintf(err_msg, err_msg_cap, "SSE: unsupported Content-Encoding");
        goto fail;
    }

    if (unsupported_transfer_encoding) {
        if (err_msg && err_msg_cap > 0)
            snprintf(err_msg, err_msg_cap, "SSE: unsupported Transfer-Encoding");
        goto fail;
    }

    if (sse->chunked && sse->content_length_present) {
        if (err_msg && err_msg_cap > 0)
            snprintf(err_msg, err_msg_cap, "SSE: conflicting response framing");
        goto fail;
    }

    if (!saw_stream_type) {
        if (err_msg && err_msg_cap > 0)
            snprintf(err_msg, err_msg_cap, "SSE: response is not text/event-stream");
        goto fail;
    }

    sse_mutex_lock(&sse->state_lock);
    if (sse->close_requested) {
        sse_mutex_unlock(&sse->state_lock);
        if (err_msg && err_msg_cap > 0)
            snprintf(err_msg, err_msg_cap, "SSE: connection closed");
        goto fail;
    }
    sse->is_open = true;
    char *old_url = NULL;
    if (permanent_url) {
        old_url = sse->url;
        sse->url = permanent_url;
        permanent_url = NULL;
        open_recovery.permanent_url = NULL;
    }
    sse_mutex_unlock(&sse->state_lock);
    sse_native_discard(old_url);

    sse_native_discard(request);
    open_recovery.request = NULL;
    sse_native_discard(target);
    open_recovery.target = NULL;
    sse_native_discard(redirect_location);
    open_recovery.location_value = NULL;
    rt_string_unref(query);
    open_recovery.query = NULL;
    rt_string_unref(path);
    open_recovery.path = NULL;
    rt_string_unref(host);
    open_recovery.host = NULL;
    rt_string_unref(scheme);
    open_recovery.scheme = NULL;
    sse_release_managed(url_obj);
    open_recovery.url_obj = NULL;
    rt_string_unref(url);
    open_recovery.url = NULL;
    sse_native_discard(current_url);
    open_recovery.current_url = NULL;
    rt_trap_clear_recovery();
    return 1;

fail:
    rt_trap_clear_recovery();
    sse_native_discard(status_line);
    sse_open_recovery_cleanup(sse, &open_recovery);
    return 0;
}

/// @brief Sleep for the server-selected reconnect delay while remaining cancellable.
/// @details Delay is divided into short slices so concurrent Close does not
///          leave a receive thread asleep for an attacker-selected retry value.
/// @param sse Live receive owner.
/// @param delay_ms Non-negative reconnect delay.
/// @return Nonzero when the delay elapsed; zero after cancellation.
static int sse_wait_retry_delay(rt_sse_impl *sse, int64_t delay_ms) {
    while (delay_ms > 0) {
        if (sse_close_was_requested(sse))
            return 0;
        int64_t slice = delay_ms > 100 ? 100 : delay_ms;
        rt_thread_sleep(slice);
        delay_ms -= slice;
    }
    return !sse_close_was_requested(sse);
}

/// @brief Re-open the configured SSE URL and include a safe Last-Event-ID.
/// @param sse Live receive owner after an unexpected transport end.
/// @return Nonzero when a replacement HTTP event stream was established.
static int sse_try_reconnect(rt_sse_impl *sse) {
    char err_msg[512];
    if (!sse || !sse->url || sse_close_was_requested(sse))
        return 0;
    if (sse->retry_ms > 0 && !sse_wait_retry_delay(sse, sse->retry_ms))
        return 0;
    return sse_open_url(sse, sse->url, 1, err_msg, sizeof(err_msg));
}

/// @brief Release one caller-owned managed object reference.
/// @param object Runtime object or String, or NULL.
static void sse_release_managed(void *object) {
    if (object && rt_obj_release_check0(object))
        rt_obj_free(object);
}

/// @brief Copy the active trap diagnostic before clearing its recovery frame.
/// @param output Destination buffer.
/// @param capacity Destination capacity including the terminator.
/// @param fallback Text used when the trap subsystem has no diagnostic.
static void sse_save_trap(char *output, size_t capacity, const char *fallback) {
    if (!output || capacity == 0)
        return;
    const char *message = rt_trap_get_error();
    snprintf(output,
             capacity,
             "%s",
             message && message[0] ? message
                                   : (fallback && fallback[0] ? fallback : "SSE operation failed"));
}

/// @brief Construct and connect one GC-managed SSE client transactionally.
/// @details Native locks and URL storage are installed before transport setup;
///          any nested managed trap releases the partially built object before
///          the diagnostic is re-raised outside the recovery frame.
/// @param url NUL-free HTTP(S) runtime String.
/// @return Connected SSE handle, or NULL after one trap.
void *rt_sse_connect(rt_string url) {
    if (!sse_string_is_cstr(url) || rt_str_len(url) == 0) {
        rt_trap("SSE: invalid URL String");
        return NULL;
    }
    const char *url_str = rt_string_cstr(url);

    rt_sse_impl *volatile sse = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        char saved_error[512];
        sse_save_trap(saved_error, sizeof(saved_error), "SSE: construction failed");
        rt_trap_clear_recovery();
        sse_release_managed((void *)sse);
        rt_trap(saved_error);
        return NULL;
    }

    sse = (rt_sse_impl *)rt_obj_new_i64(RT_SSE_CLASS_ID, (int64_t)sizeof(rt_sse_impl));
    if (!sse) {
        rt_trap_clear_recovery();
        return NULL;
    }
    memset((void *)sse, 0, sizeof(*sse));
    rt_obj_set_finalizer((void *)sse, rt_sse_finalize);
    ((rt_sse_impl *)sse)->socket_fd = INVALID_SOCK;
    ((rt_sse_impl *)sse)->retry_ms = 3000;
    ((rt_sse_impl *)sse)->event_stream_at_start = 1;
    if (!sse_mutex_init(&((rt_sse_impl *)sse)->state_lock))
        rt_trap("SSE: state mutex initialization failed");
    ((rt_sse_impl *)sse)->state_lock_initialized = 1;
    if (!sse_mutex_init(&((rt_sse_impl *)sse)->recv_lock))
        rt_trap("SSE: receive mutex initialization failed");
    ((rt_sse_impl *)sse)->recv_lock_initialized = 1;
    ((rt_sse_impl *)sse)->url = strdup(url_str);
    if (!((rt_sse_impl *)sse)->url)
        rt_trap("SSE: URL allocation failed");

    char err_msg[512];
    if (!sse_open_url((rt_sse_impl *)sse, url_str, 0, err_msg, sizeof(err_msg))) {
        rt_trap_clear_recovery();
        sse_release_managed((void *)sse);
        sse = NULL;
        // Exactly one categorized trap, then stop: a returning trap hook must
        // not see a second generic trap or receive the freed pointer.
        if (strstr(err_msg, "TLS") != NULL) {
            rt_trap_net(err_msg[0] ? err_msg : "SSE: TLS connection failed", Err_TlsError);
            return NULL;
        }
        rt_trap(err_msg[0] ? err_msg : "SSE: connection failed");
        return NULL;
    }
    rt_trap_clear_recovery();
    return (void *)sse;
}

/// @brief Grow the connection's event-accumulation buffer to hold @p needed
///        bytes plus a trailing NUL slot.
/// @details The impl-owned buffer survives timed receives. Geometric growth is
///          checked for overflow and capped at @ref SSE_MAX_EVENT_DATA.
/// @param sse Live parser state.
/// @param needed Required event payload bytes excluding the NUL slot.
/// @return True when capacity is available; false after one trap.
static bool sse_reserve_event_data(rt_sse_impl *sse, size_t needed) {
    if (!sse || needed == SIZE_MAX || sse->event_len > needed) {
        rt_trap("SSE.Recv: event data length overflow");
        return false;
    }
    if (needed > SSE_MAX_EVENT_DATA) {
        rt_trap("SSE.Recv: event data exceeds maximum size");
        return false;
    }
    size_t required = needed + 1u;
    while (required > sse->event_cap) {
        size_t next_cap = sse->event_cap ? (sse->event_cap * 2u) : 4096u;
        if (next_cap <= sse->event_cap || next_cap < required)
            next_cap = required;
        char *grown = (char *)realloc(sse->event_data, next_cap);
        if (!grown) {
            rt_trap("SSE.Recv: memory allocation failed");
            return false;
        }
        sse->event_data = grown;
        sse->event_cap = next_cap;
    }
    return true;
}

/// @brief Outcome of one serialized event receive.
typedef enum {
    SSE_RECEIVE_DELIVERED = 0,
    SSE_RECEIVE_TIMEOUT,
    SSE_RECEIVE_CLOSED
} sse_receive_outcome_t;

/// @brief Allocate an exact native byte copy plus NUL terminator.
/// @param bytes Source bytes; may be NULL only when @p length is zero.
/// @param length Exact source byte length.
/// @param operation Diagnostic used for an allocation trap.
/// @return Heap-owned copy, or NULL after one trap.
static char *sse_duplicate_bytes(const char *bytes, size_t length, const char *operation) {
    if ((!bytes && length != 0) || length == SIZE_MAX) {
        rt_trap(operation ? operation : "SSE: invalid byte range");
        return NULL;
    }
    char *copy = (char *)malloc(length + 1u);
    if (!copy) {
        rt_trap(operation ? operation : "SSE: memory allocation failed");
        return NULL;
    }
    if (length > 0)
        memcpy(copy, bytes, length);
    copy[length] = '\0';
    return copy;
}

/// @brief Mark the logical SSE stream closed under its state mutex.
/// @param sse Initialized client.
static void sse_mark_closed(rt_sse_impl *sse) {
    sse_mutex_lock(&sse->state_lock);
    sse->is_open = false;
    sse_mutex_unlock(&sse->state_lock);
}

/// @brief Process one decoded event-stream line according to the EventSource algorithm.
/// @details Supports fields with or without a colon, removes at most one leading
///          U+0020 from values, preserves empty `data` lines, ignores `id`
///          values containing NUL, and updates metadata transactionally.
/// @param sse Live serialized parser.
/// @param line Exact line bytes, excluding its delimiter.
/// @param line_len Exact line length.
/// @param dispatch_out Set when a blank line dispatches an accumulated event.
/// @return Nonzero on success; false after one allocation/limit trap.
static int sse_process_event_line(rt_sse_impl *sse,
                                  const char *line,
                                  size_t line_len,
                                  int *dispatch_out) {
    if (dispatch_out)
        *dispatch_out = 0;

    if (sse->event_stream_at_start) {
        sse->event_stream_at_start = 0;
        if (line_len >= 3u && (unsigned char)line[0] == 0xEFu && (unsigned char)line[1] == 0xBBu &&
            (unsigned char)line[2] == 0xBFu) {
            line += 3;
            line_len -= 3u;
        }
    }

    if (line_len == 0) {
        if (sse->event_saw_data) {
            char *old_type = NULL;
            sse_mutex_lock(&sse->state_lock);
            old_type = sse->last_event_type;
            sse->last_event_type = sse->pending_event_type;
            sse->last_event_type_len = sse->pending_event_type_len;
            sse->pending_event_type = NULL;
            sse->pending_event_type_len = 0;
            sse_mutex_unlock(&sse->state_lock);
            sse_native_discard(old_type);
            if (dispatch_out)
                *dispatch_out = 1;
        }
        return 1;
    }
    if (line[0] == ':')
        return 1;

    size_t colon = 0;
    while (colon < line_len && line[colon] != ':')
        colon++;
    size_t field_len = colon;
    size_t value_start = colon < line_len ? colon + 1u : line_len;
    if (value_start < line_len && line[value_start] == ' ')
        value_start++;
    const char *value = line + value_start;
    size_t value_len = line_len - value_start;

    if (field_len == 4u && memcmp(line, "data", 4u) == 0) {
        size_t separator = sse->event_data_lines > 0 ? 1u : 0u;
        if (value_len > SIZE_MAX - sse->event_len - separator ||
            !sse_reserve_event_data(sse, sse->event_len + separator + value_len)) {
            return 0;
        }
        if (separator)
            sse->event_data[sse->event_len++] = '\n';
        if (value_len > 0)
            memcpy(sse->event_data + sse->event_len, value, value_len);
        sse->event_len += value_len;
        sse->event_data_lines++;
        sse->event_saw_data = 1;
    } else if (field_len == 5u && memcmp(line, "event", 5u) == 0) {
        char *replacement =
            sse_duplicate_bytes(value, value_len, "SSE: event type allocation failed");
        if (!replacement)
            return 0;
        sse_native_discard(sse->pending_event_type);
        sse->pending_event_type = replacement;
        sse->pending_event_type_len = value_len;
    } else if (field_len == 2u && memcmp(line, "id", 2u) == 0) {
        if (!memchr(value, '\0', value_len)) {
            char *replacement =
                sse_duplicate_bytes(value, value_len, "SSE: event ID allocation failed");
            if (!replacement)
                return 0;
            char *old_id = NULL;
            sse_mutex_lock(&sse->state_lock);
            old_id = sse->last_event_id;
            sse->last_event_id = replacement;
            sse->last_event_id_len = value_len;
            sse_mutex_unlock(&sse->state_lock);
            sse_native_discard(old_id);
        }
    } else if (field_len == 5u && memcmp(line, "retry", 5u) == 0 && value_len > 0) {
        int64_t retry_ms = 0;
        int valid = 1;
        for (size_t index = 0; index < value_len; index++) {
            unsigned char byte = (unsigned char)value[index];
            if (!rt_ascii_isdigit(byte)) {
                valid = 0;
                break;
            }
            int digit = (int)(byte - '0');
            if (retry_ms > (INT64_MAX - digit) / 10) {
                valid = 0;
                break;
            }
            retry_ms = retry_ms * 10 + digit;
        }
        if (valid)
            sse->retry_ms = retry_ms;
    }
    return 1;
}

/// @brief Read and parse until an event dispatch, timeout, or terminal close.
/// @param sse Live client whose receive mutex and active ownership are held.
/// @return Detailed outcome; a delivered event remains in `event_data` for publication.
static sse_receive_outcome_t sse_receive_core(rt_sse_impl *sse) {
    for (;;) {
        if (sse_close_was_requested(sse))
            return SSE_RECEIVE_CLOSED;

        size_t line_len = 0;
        char *line = sse_payload_recv_line(sse, &line_len);
        if (!line) {
            sse_read_failure_t failure = sse->read_failure;
            if (failure == SSE_READ_TIMEOUT) {
                sse->read_timed_out = 0;
                sse->read_failure = SSE_READ_NONE;
                if (sse->read_deadline_us == 0)
                    continue;
                return SSE_RECEIVE_TIMEOUT;
            }
            if (failure == SSE_READ_OOM) {
                rt_trap("SSE.Recv: line allocation failed");
                return SSE_RECEIVE_CLOSED;
            }
            if (failure == SSE_READ_LIMIT || failure == SSE_READ_PROTOCOL) {
                sse_mark_closed(sse);
                sse_close_transport(sse);
                rt_trap(failure == SSE_READ_LIMIT ? "SSE.Recv: line or framing limit exceeded"
                                                  : "SSE.Recv: malformed HTTP stream framing");
                return SSE_RECEIVE_CLOSED;
            }

            sse_reset_partial_state(sse);
            if (!sse_close_was_requested(sse) && sse_try_reconnect(sse))
                continue;
            if (sse->read_failure == SSE_READ_TIMEOUT) {
                sse->read_timed_out = 0;
                sse->read_failure = SSE_READ_NONE;
                return SSE_RECEIVE_TIMEOUT;
            }
            sse_mark_closed(sse);
            sse_close_transport(sse);
            return SSE_RECEIVE_CLOSED;
        }

        int dispatch = 0;
        int processed = sse_process_event_line(sse, line, line_len, &dispatch);
        sse_native_discard(line);
        if (!processed)
            return SSE_RECEIVE_CLOSED;
        if (dispatch)
            return SSE_RECEIVE_DELIVERED;
    }
}

/// @brief Finish active receive ownership and honor a deferred Close.
/// @param sse Client whose receive mutex is held.
static void sse_finish_receive(rt_sse_impl *sse) {
    int close_transport = 0;
    sse_mutex_lock(&sse->state_lock);
    sse->recv_active = false;
    close_transport = sse->close_requested || !sse->is_open;
    sse_mutex_unlock(&sse->state_lock);
    if (close_transport)
        sse_close_transport(sse);
    sse_mutex_unlock(&sse->recv_lock);
}

/// @brief Execute one serialized receive and publish its data as a managed String.
/// @details A single monotonic deadline covers the whole event. The recovery
///          boundary restores the baseline socket timeout, releases active
///          ownership and the receive mutex, and consumes an already-dispatched
///          event before re-raising any managed allocation failure.
/// @param sse Valid initialized client.
/// @param timeout_ms Zero for blocking or a finite value in 1..INT_MAX.
/// @param outcome_out Receives delivered, timeout, or closed.
/// @return Caller-owned String (possibly empty), or NULL after a trap.
static rt_string sse_receive_operation(rt_sse_impl *sse,
                                       int64_t timeout_ms,
                                       sse_receive_outcome_t *outcome_out) {
    if (outcome_out)
        *outcome_out = SSE_RECEIVE_CLOSED;
    if (timeout_ms < 0 || timeout_ms > INT_MAX) {
        rt_trap("SSE: invalid receive timeout");
        return NULL;
    }

    sse_mutex_lock(&sse->recv_lock);
    sse_mutex_lock(&sse->state_lock);
    if (!sse->is_open || sse->close_requested) {
        sse_mutex_unlock(&sse->state_lock);
        sse_mutex_unlock(&sse->recv_lock);
        return rt_str_empty();
    }
    sse->recv_active = true;
    sse_mutex_unlock(&sse->state_lock);

    rt_string volatile result = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        char saved_error[512];
        sse_save_trap(saved_error, sizeof(saved_error), "SSE.Recv: receive failed");
        rt_trap_clear_recovery();
        sse->read_deadline_us = 0;
        (void)sse_set_recv_timeout(sse, 30000);
        sse_finish_receive(sse);
        rt_str_release_maybe((rt_string)result);
        rt_trap(saved_error);
        return NULL;
    }

    sse->last_recv_delivered = 0;
    sse->read_timed_out = 0;
    sse->read_failure = SSE_READ_NONE;
    if (timeout_ms > 0) {
        int64_t now_us = rt_clock_ticks_us();
        int64_t budget_us = timeout_ms * INT64_C(1000);
        if (now_us < 0 || now_us > INT64_MAX - budget_us)
            rt_trap("SSE: receive deadline overflow");
        sse->read_deadline_us = now_us + budget_us;
    } else {
        sse->read_deadline_us = 0;
        if (!sse_set_recv_timeout(sse, SSE_CLOSE_POLL_TIMEOUT_MS))
            rt_trap("SSE: receive timeout setup failed");
    }

    sse_receive_outcome_t outcome = sse_receive_core(sse);
    if (outcome == SSE_RECEIVE_DELIVERED) {
        size_t event_len = sse->event_len;
        sse->event_len = 0;
        sse->event_saw_data = 0;
        sse->event_data_lines = 0;
        sse->last_recv_delivered = 1;
        result = rt_string_from_bytes(sse->event_data ? sse->event_data : "", event_len);
    } else {
        result = rt_str_empty();
    }

    sse->read_deadline_us = 0;
    if (!sse_set_recv_timeout(sse, 30000))
        rt_trap("SSE: failed to restore receive timeout");
    rt_trap_clear_recovery();
    sse_finish_receive(sse);
    if (outcome_out)
        *outcome_out = outcome;
    return (rt_string)result;
}

/// @brief Consume an owned receive String and publish a caller-owned Result.
/// @details Result creation runs in a fresh recovery frame. Both the temporary
///          data/error String and any partial Result are released before an
///          allocation failure is re-raised.
/// @param data Owned event or empty String; consumed on every path.
/// @param delivered Nonzero selects `Result.OkStr(data)`.
/// @param error_text Diagnostic used for `Result.ErrStr`.
/// @return Caller-owned Result, or NULL after one trap.
static void *sse_receive_result_owned(rt_string data, int delivered, const char *error_text) {
    rt_string volatile error_string = NULL;
    void *volatile result = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        char saved_error[512];
        sse_save_trap(saved_error, sizeof(saved_error), "SSE: Result allocation failed");
        rt_trap_clear_recovery();
        sse_release_managed((void *)result);
        rt_str_release_maybe((rt_string)error_string);
        rt_str_release_maybe(data);
        rt_trap(saved_error);
        return NULL;
    }

    if (delivered) {
        result = rt_result_ok_str(data);
    } else {
        const char *message = error_text && error_text[0] ? error_text : "SSE: stream closed";
        error_string = rt_string_from_bytes(message, strlen(message));
        result = rt_result_err_str((rt_string)error_string);
    }
    rt_trap_clear_recovery();
    rt_str_release_maybe((rt_string)error_string);
    rt_str_release_maybe(data);
    return (void *)result;
}

/// @brief Receive the next SSE event's accumulated `data:` payload.
/// @details Receives are serialized. Consecutive `data` fields, including empty
///          fields, are joined with one LF. Empty is returned after close; use
///          @ref rt_sse_recv_for_result to distinguish empty data from timeout.
/// @param obj Valid SSE client, or NULL for the legacy empty sentinel.
/// @return Caller-owned event-data String, or an empty String after terminal
///         close.
rt_string rt_sse_recv(void *obj) {
    if (!obj)
        return rt_str_empty();
    rt_sse_impl *sse = sse_require(obj, "SSE.Recv: invalid client");
    if (!sse)
        return NULL;
    sse_receive_outcome_t outcome;
    return sse_receive_operation(sse, 0, &outcome);
}

/// @brief Receive the next event under one monotonic whole-event deadline.
/// @details Partial line, event, and chunk-framing state is retained on timeout
///          and resumed by the next serialized receive.
/// @param obj Valid SSE client, or NULL for the legacy empty sentinel.
/// @param timeout_ms Zero for blocking, otherwise 1..INT_MAX milliseconds.
/// @return Event data, or the empty legacy sentinel on timeout/close.
rt_string rt_sse_recv_for(void *obj, int64_t timeout_ms) {
    if (!obj)
        return rt_str_empty();
    rt_sse_impl *sse = sse_require(obj, "SSE.RecvFor: invalid client");
    if (!sse)
        return NULL;
    sse_receive_outcome_t outcome;
    return sse_receive_operation(sse, timeout_ms, &outcome);
}

/// @brief Receive with a timeout and preserve delivered/timeout/closed distinctions.
/// @param obj Valid SSE client.
/// @param timeout_ms Zero for blocking, otherwise 1..INT_MAX milliseconds.
/// @return `OkStr(data)` for an event, or `ErrStr` for timeout, close, or a caught receive trap.
void *rt_sse_recv_for_result(void *obj, int64_t timeout_ms) {
    if (!obj) {
        rt_trap("SSE.RecvForResult: NULL client");
        return NULL;
    }
    rt_sse_impl *sse = sse_require(obj, "SSE.RecvForResult: invalid client");
    if (!sse)
        return NULL;

    rt_string volatile data = NULL;
    sse_receive_outcome_t outcome = SSE_RECEIVE_CLOSED;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        char saved_error[512];
        sse_save_trap(saved_error, sizeof(saved_error), "SSE: receive failed");
        rt_trap_clear_recovery();
        rt_str_release_maybe((rt_string)data);
        return sse_receive_result_owned(NULL, 0, saved_error);
    }
    data = sse_receive_operation(sse, timeout_ms, &outcome);
    rt_trap_clear_recovery();
    return sse_receive_result_owned((rt_string)data,
                                    outcome == SSE_RECEIVE_DELIVERED,
                                    outcome == SSE_RECEIVE_TIMEOUT ? "SSE: timeout"
                                                                   : "SSE: stream closed");
}

/// @brief Return whether the client still owns a locally open-looking transport.
/// @details This is a synchronized local-state query, not a remote liveness probe.
/// @param obj Valid SSE client; NULL yields zero.
/// @return One while local state is open and owns a transport; otherwise zero.
int8_t rt_sse_is_open(void *obj) {
    if (!obj)
        return 0;
    rt_sse_impl *sse = sse_require(obj, "SSE.IsOpen: invalid client");
    if (!sse)
        return 0;
    sse_mutex_lock(&sse->state_lock);
    int8_t open =
        sse->is_open && !sse->close_requested && (sse->socket_fd != INVALID_SOCK || sse->tls) ? 1
                                                                                              : 0;
    sse_mutex_unlock(&sse->state_lock);
    return open;
}

/// @brief Cancel active I/O and permanently close the SSE client.
/// @details Close marks cancellation under the state mutex and uses native
///          shutdown to wake an active receive without freeing its TLS/session
///          storage. The receive owner performs the final close; an idle client
///          is detached and closed immediately. The operation is idempotent.
/// @param obj Valid SSE client; NULL is a no-op.
void rt_sse_close(void *obj) {
    if (!obj)
        return;
    rt_sse_impl *sse = sse_require(obj, "SSE.Close: invalid client");
    if (!sse)
        return;

    int recv_active = 0;
    socket_t socket_fd = INVALID_SOCK;
    sse_mutex_lock(&sse->state_lock);
    sse->is_open = false;
    sse->close_requested = true;
    recv_active = sse->recv_active ? 1 : 0;
    socket_fd = sse->socket_fd;
    if (recv_active && socket_fd != INVALID_SOCK)
        (void)rt_socket_shutdown_both(socket_fd);
    sse_mutex_unlock(&sse->state_lock);
    if (!recv_active)
        sse_close_transport(sse);
}

/// @brief Snapshot native metadata and publish it as a managed String without locking across traps.
/// @param sse Valid SSE client.
/// @param event_type Nonzero snapshots event type; zero snapshots event ID.
/// @param operation Diagnostic used if native or managed allocation fails.
/// @return Caller-owned metadata String, including embedded bytes for event type.
static rt_string sse_metadata_snapshot(rt_sse_impl *sse, int event_type, const char *operation) {
    char *snapshot = NULL;
    size_t length = 0;
    sse_mutex_lock(&sse->state_lock);
    const char *source = event_type ? sse->last_event_type : sse->last_event_id;
    length = event_type ? sse->last_event_type_len : sse->last_event_id_len;
    snapshot = (char *)malloc(length + 1u);
    if (snapshot) {
        if (length > 0)
            memcpy(snapshot, source, length);
        snapshot[length] = '\0';
    }
    sse_mutex_unlock(&sse->state_lock);
    if (!snapshot) {
        rt_trap(operation ? operation : "SSE: metadata allocation failed");
        return NULL;
    }

    rt_string volatile result = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        char saved_error[512];
        sse_save_trap(saved_error, sizeof(saved_error), operation);
        rt_trap_clear_recovery();
        sse_native_discard(snapshot);
        rt_str_release_maybe((rt_string)result);
        rt_trap(saved_error);
        return NULL;
    }
    result = rt_string_from_bytes(snapshot, length);
    rt_trap_clear_recovery();
    sse_native_discard(snapshot);
    return (rt_string)result;
}

/// @brief Return the type of the most recently dispatched event.
/// @details Event type does not carry over when a later event omits its
///          `event:` field.
/// @param obj Valid SSE client; NULL yields an empty String.
/// @return Caller-owned synchronized event-type snapshot.
rt_string rt_sse_last_event_type(void *obj) {
    if (!obj)
        return rt_str_empty();
    rt_sse_impl *sse = sse_require(obj, "SSE.LastEventType: invalid client");
    return sse ? sse_metadata_snapshot(sse, 1, "SSE.LastEventType: allocation failed") : NULL;
}

/// @brief Return the most recently accepted EventSource ID.
/// @param obj Valid SSE client; NULL yields an empty String.
/// @return Caller-owned synchronized event-ID snapshot.
rt_string rt_sse_last_event_id(void *obj) {
    if (!obj)
        return rt_str_empty();
    rt_sse_impl *sse = sse_require(obj, "SSE.LastEventId: invalid client");
    return sse ? sse_metadata_snapshot(sse, 0, "SSE.LastEventId: allocation failed") : NULL;
}
