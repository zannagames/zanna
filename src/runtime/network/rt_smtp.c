//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/network/rt_smtp.c
// Purpose: Strict, cancellation-safe SMTP client with optional TLS and AUTH LOGIN.
// Key invariants:
//   - EHLO → optional STARTTLS → optional AUTH LOGIN → MAIL FROM → RCPT TO → DATA.
//   - Replies require bounded CRLF framing and consistent multiline status codes.
//   - One operation owns published transport state; Close cancels bounded-poll I/O.
//   - MIME bodies stream with bounded memory, normalized newlines, and dot transparency.
//   - AUTH LOGIN credentials and encoded copies are wiped before native release.
// Ownership/Lifetime:
//   - Client objects are GC-managed; copied host/error/credential bytes are finalized.
// Links: rt_smtp.h (C ABI), rt_network.h (TCP), rt_tls.h (TLS),
//        rt_tls_internal.h (cancellation-aware TLS session state),
//        docs/zannalib/network.md (runtime contract)
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_smtp.c
 * @brief Implements a strict, cancellation-aware SMTP client with optional TLS.
 * @details Each serialized send performs bounded SMTP reply parsing, EHLO,
 *          optional implicit TLS or STARTTLS, optional AUTH LOGIN, envelope
 *          submission, and streamed MIME DATA. Native synchronization exposes
 *          active transport state to Close so blocking work can be cancelled
 *          without leaking sockets, sessions, or credential material.
 */

// Feature-test macros must appear before every system header. They are benign
// on non-POSIX toolchains and avoid raw OS-condition checks in this module.
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE 1
#endif
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include "rt_smtp.h"

#include "rt_ascii.h"
#include "rt_internal.h"
#include "rt_network_internal.h"
#include "rt_object.h"
#include "rt_platform.h"
#include "rt_result.h"
#include "rt_string.h"
#include "rt_time.h"
#include "rt_tls.h"
#include "rt_tls_internal.h"
#include "rt_trap.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if RT_PLATFORM_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
typedef CRITICAL_SECTION smtp_mutex_t;
#else
#include <pthread.h>
typedef pthread_mutex_t smtp_mutex_t;
#endif

/// @brief RFC 5321 maximum reply-line bytes excluding CRLF.
#define SMTP_MAX_LINE_LEN 510u

/// @brief Maximum lines accepted in one multiline SMTP reply.
#define SMTP_MAX_REPLY_LINES 100u

/// @brief Maximum authentication credential bytes retained per field.
#define SMTP_MAX_CREDENTIAL_BYTES (16u * 1024u)

/// @brief Fixed native staging capacity used while streaming SMTP DATA.
#define SMTP_OUTPUT_BUFFER_SIZE 4096u

/// @brief Existing logical timeout for one SMTP transport operation.
#define SMTP_IO_TIMEOUT_MS 30000

/// @brief Native I/O slice used to observe concurrent cancellation promptly.
#define SMTP_CANCEL_POLL_TIMEOUT_MS 100

/// @brief Initialize one native SMTP mutex.
/// @param mutex Zeroed storage owned by a partial client.
/// @return Nonzero on success; zero when POSIX initialization fails.
static int smtp_mutex_init(smtp_mutex_t *mutex) {
#if RT_PLATFORM_WINDOWS
    InitializeCriticalSection(mutex);
    return 1;
#else
    return pthread_mutex_init(mutex, NULL) == 0 ? 1 : 0;
#endif
}

/// @brief Acquire one initialized native SMTP mutex.
/// @param mutex Mutex owned by a live SMTP client.
static void smtp_mutex_lock(smtp_mutex_t *mutex) {
#if RT_PLATFORM_WINDOWS
    EnterCriticalSection(mutex);
#else
    (void)pthread_mutex_lock(mutex);
#endif
}

/// @brief Release one SMTP mutex held by the current thread.
/// @param mutex Locked mutex owned by a live SMTP client.
static void smtp_mutex_unlock(smtp_mutex_t *mutex) {
#if RT_PLATFORM_WINDOWS
    LeaveCriticalSection(mutex);
#else
    (void)pthread_mutex_unlock(mutex);
#endif
}

/// @brief Destroy one quiescent SMTP mutex during finalization.
/// @param mutex Initialized mutex that has no active users.
static void smtp_mutex_destroy(smtp_mutex_t *mutex) {
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
    void *tcp;             // Plain TCP connection before STARTTLS / for non-TLS SMTP
    rt_tls_session_t *tls; // TLS session for implicit TLS or STARTTLS
    char *host;
    int port;
    char *username;
    char *password;
    char *last_error;
    int8_t use_tls;
    uint8_t read_buf[4096];
    size_t read_buf_len;
    size_t read_buf_pos;
    smtp_mutex_t state_lock;     ///< Protects transport publication, cancellation, and errors.
    smtp_mutex_t operation_lock; ///< Serializes sends and mutable configuration.
    int state_lock_initialized;
    int operation_lock_initialized;
    int operation_active;
    int cancel_requested;
} rt_smtp_impl;

typedef struct {
    int8_t supports_starttls;
    int8_t supports_auth_login;
} smtp_caps_t;

/// @brief Compare fixed byte ranges with locale-independent ASCII case folding.
/// @param lhs First byte range.
/// @param rhs Second byte range.
/// @param count Number of bytes to compare.
/// @return Zero for an ASCII-case-insensitive match.
static int smtp_ascii_ncasecmp(const char *lhs, const char *rhs, size_t count) {
    for (size_t index = 0; index < count; index++) {
        int left = rt_ascii_tolower((unsigned char)lhs[index]);
        int right = rt_ascii_tolower((unsigned char)rhs[index]);
        if (left != right)
            return left - right;
    }
    return 0;
}

/// @brief Validate and cast an opaque SMTP receiver before native state access.
/// @details Stable identity, full payload size, and both lock initialization
///          markers are required. Returning trap hooks therefore cannot fall
///          through into forged mutex, credential, or transport storage.
/// @param object Candidate managed receiver.
/// @param operation Operation-specific diagnostic.
/// @return Initialized SMTP payload, or NULL after one trap.
static rt_smtp_impl *smtp_require(void *object, const char *operation) {
    if (!rt_obj_is_instance(object, RT_SMTP_CLASS_ID, sizeof(rt_smtp_impl)) ||
        !((rt_smtp_impl *)object)->state_lock_initialized ||
        !((rt_smtp_impl *)object)->operation_lock_initialized) {
        rt_trap(operation ? operation : "SmtpClient: invalid client");
        return NULL;
    }
    return (rt_smtp_impl *)object;
}

/// @brief Validate a runtime String and expose its complete C-string view.
/// @param value Candidate String; NULL is accepted only when @p allow_null is nonzero.
/// @param allow_null Whether NULL represents an omitted optional field.
/// @param bytes_out Optional C-string output.
/// @param length_out Optional exact byte-length output.
/// @return Nonzero for a valid handle without embedded NUL bytes.
static int smtp_string_view(rt_string value,
                            int allow_null,
                            const char **bytes_out,
                            size_t *length_out) {
    if (bytes_out)
        *bytes_out = NULL;
    if (length_out)
        *length_out = 0;
    if (!value)
        return allow_null ? 1 : 0;
    if (!rt_string_is_handle(value))
        return 0;
    int64_t length = rt_str_len(value);
    const char *bytes = rt_string_cstr(value);
    if (!bytes || length < 0 || (uint64_t)length > (uint64_t)SIZE_MAX ||
        (length > 0 && memchr(bytes, '\0', (size_t)length))) {
        return 0;
    }
    if (bytes_out)
        *bytes_out = bytes;
    if (length_out)
        *length_out = (size_t)length;
    return 1;
}

/// @brief Overwrite a memory range containing SMTP credentials or authentication material.
/// @details Uses a volatile byte pointer so the compiler cannot elide the wipe as an unused
///          store before free. NULL pointers are accepted and treated as no-ops.
/// @param ptr Start of the memory range to wipe.
/// @param len Number of bytes to overwrite with zero.
static void smtp_secure_zero(void *ptr, size_t len) {
    volatile unsigned char *p = (volatile unsigned char *)ptr;
    while (p && len-- > 0)
        *p++ = 0;
}

/// @brief Wipe and free an owned NUL-terminated secret string.
/// @details Intended for stored SMTP passwords and transient replacement values. The string is
///          measured before wiping so the entire current secret is cleared before release.
/// @param secret Heap-owned secret string, or NULL.
static void smtp_free_secret(char *secret) {
    if (!secret)
        return;
    smtp_secure_zero(secret, strlen(secret));
    free(secret);
}

static void smtp_close_transport(rt_smtp_impl *s);
static void smtp_release_managed(void *object);

/// @brief Finalize an SMTP client and all native resources it owns.
/// @details Detaches and closes TLS/TCP transport state, releases the host and
///          diagnostic, securely erases credentials, and destroys each
///          initialized mutex.
/// @param obj SMTP client allocation being finalized, or NULL.
static void rt_smtp_finalize(void *obj) {
    if (!obj)
        return;
    rt_smtp_impl *s = (rt_smtp_impl *)obj;
    smtp_close_transport(s);
    free(s->host);
    smtp_free_secret(s->username);
    smtp_free_secret(s->password);
    free(s->last_error);
    if (s->operation_lock_initialized) {
        smtp_mutex_destroy(&s->operation_lock);
        s->operation_lock_initialized = 0;
    }
    if (s->state_lock_initialized) {
        smtp_mutex_destroy(&s->state_lock);
        s->state_lock_initialized = 0;
    }
}

//=============================================================================
// SMTP Helpers
//=============================================================================

/// @brief Replace the synchronized last-error message with an owned copy.
/// @details Allocation completes before the state lock is acquired, leaving the
///          prior diagnostic intact if a returning trap hook resumes execution.
/// @param s Initialized SMTP client.
/// @param msg New diagnostic, or NULL to clear it.
static void set_error(rt_smtp_impl *s, const char *msg) {
    if (!s)
        return;
    char *copy = msg ? strdup(msg) : NULL;
    if (msg && !copy) {
        rt_trap("SmtpClient: error allocation failed");
        return;
    }
    char *old_error = NULL;
    smtp_mutex_lock(&s->state_lock);
    old_error = s->last_error;
    s->last_error = copy;
    smtp_mutex_unlock(&s->state_lock);
    free(old_error);
}

/// @brief Drop the cached last-error so a successful call doesn't leak a stale prior failure.
/// @details The pointer is detached under the state mutex and released after
///          unlocking.
/// @param s Initialized SMTP client; NULL is a no-op.
static void clear_error(rt_smtp_impl *s) {
    if (!s)
        return;
    char *old_error = NULL;
    smtp_mutex_lock(&s->state_lock);
    old_error = s->last_error;
    s->last_error = NULL;
    smtp_mutex_unlock(&s->state_lock);
    free(old_error);
}

/// @brief Tear down whichever SMTP transport is currently published.
/// @details TLS and TCP owners are atomically detached with read-ahead state
///          reset, then closed outside the mutex. TLS is closed first because a
///          STARTTLS session may already own the detached socket. The operation
///          is idempotent.
/// @param s SMTP client whose transport is consumed; NULL is a no-op.
static void smtp_close_transport(rt_smtp_impl *s) {
    if (!s)
        return;
    rt_tls_session_t *tls = NULL;
    void *tcp = NULL;
    if (s->state_lock_initialized)
        smtp_mutex_lock(&s->state_lock);
    tls = s->tls;
    tcp = s->tcp;
    s->tls = NULL;
    s->tcp = NULL;
    s->read_buf_len = 0;
    s->read_buf_pos = 0;
    if (s->state_lock_initialized)
        smtp_mutex_unlock(&s->state_lock);

    if (tls) {
        tls->io_cancel_requested = NULL;
        tls->io_cancel_context = NULL;
        tls->io_deadline_us = 0;
        rt_tls_close(tls);
    }
    if (tcp) {
        rt_tcp_close(tcp);
        if (rt_obj_release_check0(tcp))
            rt_obj_free(tcp);
    }
}

/// @brief Test whether concurrent Close cancelled the active SMTP operation.
/// @param s Initialized client.
/// @return Nonzero after cancellation was requested.
static int smtp_operation_cancelled(rt_smtp_impl *s) {
    int cancelled = 0;
    smtp_mutex_lock(&s->state_lock);
    cancelled = s->cancel_requested ? 1 : 0;
    smtp_mutex_unlock(&s->state_lock);
    return cancelled;
}

/// @brief Adapt SMTP's synchronized cancellation state to the TLS owner probe.
/// @param context Active SMTP client owning the TLS session.
/// @return Nonzero after concurrent Close requested cancellation.
static int smtp_tls_cancel_requested(void *context) {
    return context ? smtp_operation_cancelled((rt_smtp_impl *)context) : 1;
}

/// @brief Create one monotonic deadline for a logical SMTP transport operation.
/// @return Absolute runtime-clock deadline in microseconds.
static int64_t smtp_io_deadline_us(void) {
    return rt_clock_ticks_us() + (int64_t)SMTP_IO_TIMEOUT_MS * 1000;
}

/// @brief Test cancellation and the logical operation deadline between native polls.
/// @param s Active SMTP client.
/// @param deadline_us Absolute monotonic deadline.
/// @return Nonzero when the transport operation must stop.
static int smtp_io_should_stop(rt_smtp_impl *s, int64_t deadline_us) {
    return smtp_operation_cancelled(s) || rt_clock_ticks_us() >= deadline_us;
}

/// @brief Publish a newly connected plain TCP wrapper to the active operation.
/// @details The caller retains ownership on cancellation or an unexpected
///          occupied slot. Publication and Close's shutdown snapshot are
///          serialized by the state mutex.
/// @param s Active SMTP client.
/// @param tcp Caller-owned connected TCP object.
/// @return Nonzero when ownership transferred to @p s.
static int smtp_publish_tcp(rt_smtp_impl *s, void *tcp) {
    int published = 0;
    smtp_mutex_lock(&s->state_lock);
    if (!s->cancel_requested && !s->tcp && !s->tls) {
        s->tcp = tcp;
        s->read_buf_len = 0;
        s->read_buf_pos = 0;
        published = 1;
    }
    smtp_mutex_unlock(&s->state_lock);
    return published;
}

/// @brief Transfer an existing TCP descriptor into a STARTTLS session exactly once.
/// @details The TCP wrapper is detached while the state mutex excludes Close's
///          snapshot, then released outside the lock. TLS remains published
///          even if cancellation won so the operation owner can close the
///          descriptor through the correct protocol owner.
/// @param s Active client with a published TCP wrapper.
/// @param tls Newly allocated TLS session that owns the TCP descriptor.
/// @return Nonzero when the transition completed without prior cancellation.
static int smtp_publish_starttls(rt_smtp_impl *s, rt_tls_session_t *tls) {
    void *tcp = NULL;
    int cancelled = 0;
    smtp_mutex_lock(&s->state_lock);
    tcp = s->tcp;
    if (tcp)
        rt_tcp_detach_socket(tcp);
    s->tcp = NULL;
    s->tls = tls;
    s->read_buf_len = 0;
    s->read_buf_pos = 0;
    cancelled = s->cancel_requested ? 1 : 0;
    smtp_mutex_unlock(&s->state_lock);
    if (tcp && rt_obj_release_check0(tcp))
        rt_obj_free(tcp);
    if (cancelled) {
        socket_t socket_fd = (socket_t)rt_tls_get_socket(tls);
        if (socket_fd != INVALID_SOCK)
            (void)rt_socket_shutdown_both(socket_fd);
    }
    return cancelled ? 0 : 1;
}

/// @brief Mark the beginning of a serialized SMTP operation.
/// @details The caller already holds `operation_lock`; stale cancellation is
///          cleared only after every prior transport has been detached.
/// @param s Serialized client.
static void smtp_begin_operation(rt_smtp_impl *s) {
    smtp_close_transport(s);
    smtp_mutex_lock(&s->state_lock);
    s->cancel_requested = 0;
    s->operation_active = 1;
    smtp_mutex_unlock(&s->state_lock);
}

/// @brief End a serialized SMTP operation and release transport ownership.
/// @param s Client whose operation mutex remains held by the caller.
static void smtp_finish_operation(rt_smtp_impl *s) {
    smtp_close_transport(s);
    smtp_mutex_lock(&s->state_lock);
    s->operation_active = 0;
    smtp_mutex_unlock(&s->state_lock);
}

/// @brief Configure logical timeouts with bounded native cancellation polling.
/// @details SMTP retains its 30-second operation timeout while the socket wakes
///          every short slice so Close is observed consistently on every OS.
/// @param s Active client with a published transport.
/// @param timeout_ms Positive logical operation timeout.
/// @return Nonzero when both native socket options were applied.
static int smtp_set_transport_timeouts(rt_smtp_impl *s, int timeout_ms) {
    if (!s)
        return 0;
    socket_t fd = s->tls ? (socket_t)rt_tls_get_socket(s->tls)
                         : (s->tcp ? rt_tcp_socket_fd(s->tcp) : INVALID_SOCK);
    if (fd == INVALID_SOCK || timeout_ms <= 0)
        return 0;
    if (s->tls)
        s->tls->timeout_ms = timeout_ms;
    int native_timeout_ms =
        timeout_ms > SMTP_CANCEL_POLL_TIMEOUT_MS ? SMTP_CANCEL_POLL_TIMEOUT_MS : timeout_ms;
    return set_socket_timeout(fd, native_timeout_ms, true) &&
           set_socket_timeout(fd, native_timeout_ms, false);
}

/// @brief Loop-write bytes through TLS or the published plain socket.
/// @details Short native timeouts are retried within one logical deadline and
///          cancellation is checked between every slice.
/// @param s Active SMTP client with a published transport.
/// @param data Byte span to transmit.
/// @param len Number of bytes to send.
/// @return Zero on cancellation, timeout, partial write, or error; one on success.
static int smtp_transport_send_all(rt_smtp_impl *s, const void *data, size_t len) {
    if (smtp_operation_cancelled(s))
        return 0;
    int64_t deadline_us = smtp_io_deadline_us();
    if (s->tls) {
        size_t total = 0;
        while (total < len) {
            s->tls->io_deadline_us = deadline_us;
            long sent = rt_tls_send(s->tls, (const uint8_t *)data + total, len - total);
            s->tls->io_deadline_us = 0;
            if (sent <= 0 || smtp_io_should_stop(s, deadline_us))
                return 0;
            total += (size_t)sent;
        }
        return 1;
    }

    socket_t socket_fd = s->tcp ? rt_tcp_socket_fd(s->tcp) : INVALID_SOCK;
    if (socket_fd == INVALID_SOCK)
        return 0;
    size_t total = 0;
    while (total < len) {
        if (smtp_io_should_stop(s, deadline_us))
            return 0;
        size_t remaining = len - total;
        int chunk = (int)(remaining > INT_MAX ? INT_MAX : remaining);
        int sent = send(socket_fd, (const char *)data + total, chunk, SEND_FLAGS);
        if (sent > 0) {
            total += (size_t)sent;
            continue;
        }
        if (sent == 0)
            return 0;
        int error = GET_LAST_ERROR();
        if (rt_socket_error_is_interrupted(error) || rt_socket_error_is_would_block(error) ||
            rt_socket_error_is_timeout(error)) {
            continue;
        }
        return 0;
    }
    return 1;
}

/// @brief Read native bytes directly from the active TLS or TCP transport.
/// @details The plain path bypasses managed Bytes allocation. Both paths retry
///          short native timeout slices within the logical operation deadline.
/// @param s Active SMTP client with a published transport.
/// @param buffer Destination byte buffer.
/// @param len Maximum bytes to read.
/// @return Positive byte count, zero for closure or absent transport, or -1 on
///         cancellation, timeout, or transport failure.
static long smtp_transport_read(rt_smtp_impl *s, void *buffer, size_t len) {
    int64_t deadline_us = smtp_io_deadline_us();
    if (smtp_io_should_stop(s, deadline_us))
        return -1;
    if (s->tls) {
        s->tls->io_deadline_us = deadline_us;
        long received = rt_tls_recv(s->tls, buffer, len);
        s->tls->io_deadline_us = 0;
        return received;
    }
    socket_t socket_fd = s->tcp ? rt_tcp_socket_fd(s->tcp) : INVALID_SOCK;
    if (socket_fd == INVALID_SOCK)
        return 0;
    for (;;) {
        if (smtp_io_should_stop(s, deadline_us))
            return -1;
        long received = recv(socket_fd, (char *)buffer, (int)(len > INT_MAX ? INT_MAX : len), 0);
        if (received < 0) {
            int error = GET_LAST_ERROR();
            if (rt_socket_error_is_interrupted(error) || rt_socket_error_is_would_block(error) ||
                rt_socket_error_is_timeout(error)) {
                continue;
            }
        }
        return received;
    }
}

/// @brief Return one byte from the SMTP client's native read-ahead buffer.
/// @details Refill reads up to 4 KiB, amortizing TLS record processing and
///          native receive syscalls across reply lines without consuming bytes
///          belonging to a later SMTP command.
/// @param s Live active-operation client.
/// @param byte_out Receives one byte.
/// @return Nonzero on success, zero on EOF or transport failure.
static int smtp_transport_read_byte(rt_smtp_impl *s, uint8_t *byte_out) {
    if (s->read_buf_pos >= s->read_buf_len) {
        long received = smtp_transport_read(s, s->read_buf, sizeof(s->read_buf));
        if (received <= 0)
            return 0;
        s->read_buf_len = (size_t)received;
        s->read_buf_pos = 0;
    }
    *byte_out = s->read_buf[s->read_buf_pos++];
    return 1;
}

/// @brief Read one strict CRLF-terminated SMTP reply line into native memory.
/// @details Bare LF, embedded CR, NUL, C0 controls other than HTAB, DEL, and
///          lines beyond RFC 5321's 510-byte content limit are rejected. No
///          managed object is allocated by the reply parser.
/// @param s Live active-operation client.
/// @return Heap-owned NUL-terminated line, or NULL with LastError populated.
static char *smtp_recv_line(rt_smtp_impl *s) {
    size_t cap = SMTP_MAX_LINE_LEN + 1u;
    size_t len = 0;
    char *line = (char *)malloc(cap);
    if (!line) {
        set_error(s, "SMTP: OOM");
        return NULL;
    }

    while (1) {
        uint8_t c = 0;
        if (!smtp_transport_read_byte(s, &c)) {
            free(line);
            set_error(s, "SMTP: no response");
            return NULL;
        }

        if (c == '\r') {
            uint8_t lf = 0;
            if (!smtp_transport_read_byte(s, &lf) || lf != '\n') {
                free(line);
                set_error(s, "SMTP: reply did not end with CRLF");
                return NULL;
            }
            break;
        }
        if (c == '\n') {
            free(line);
            set_error(s, "SMTP: reply used bare LF");
            return NULL;
        }

        if (len >= SMTP_MAX_LINE_LEN) {
            free(line);
            set_error(s, "SMTP: response line too long");
            return NULL;
        }
        if ((c < 0x20u && c != '\t') || c == 0x7Fu) {
            free(line);
            set_error(s, "SMTP: invalid reply byte");
            return NULL;
        }
        line[len++] = (char)c;
    }

    line[len] = '\0';
    return line;
}

/// @brief Validate one SMTP mailbox path used in an envelope command.
/// @details The runtime currently accepts one unquoted mailbox without surrounding
///          angle brackets. Whitespace, controls, DEL, and angle brackets are
///          rejected so the value cannot terminate or extend `MAIL FROM`/`RCPT TO`.
/// @param value Exact NUL-free mailbox bytes.
/// @param len Exact byte length.
/// @return Nonzero when the mailbox can be embedded in one bounded SMTP command.
static int smtp_validate_mailbox_path(const char *value, size_t len) {
    if (len == 0 || len > 512)
        return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)value[i];
        if (c <= 0x20 || c == 0x7f || c == '<' || c == '>')
            return 0;
    }
    return 1;
}

/// @brief Validate a fixed NUL-terminated value for SMTP command/header use.
/// @param value Nonempty candidate string.
/// @return One when the value contains no CR, LF, or DEL bytes; otherwise zero.
static int smtp_header_value_is_command_safe(const char *value) {
    if (!value || !*value)
        return 0;
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        if (*p == '\r' || *p == '\n' || *p == 0x7f)
            return 0;
    }
    return 1;
}

/// @brief Fixed-memory output staging state for one SMTP DATA transfer.
typedef struct {
    uint8_t bytes[SMTP_OUTPUT_BUFFER_SIZE]; ///< Unsent bytes staged for transport.
    size_t len;                             ///< Number of valid staged bytes.
} smtp_output_buffer_t;

/// @brief Flush all staged DATA bytes to the active SMTP transport.
/// @param s Live active-operation client.
/// @param output Caller-owned staging buffer.
/// @return Nonzero on success; zero with LastError populated on failure.
static int smtp_output_flush(rt_smtp_impl *s, smtp_output_buffer_t *output) {
    if (!output || output->len == 0)
        return 1;
    if (!smtp_transport_send_all(s, output->bytes, output->len)) {
        set_error(s, "SMTP: DATA send failed");
        return 0;
    }
    output->len = 0;
    return 1;
}

/// @brief Append an exact byte range to bounded SMTP DATA staging.
/// @details Full buffers are flushed incrementally, so memory use remains
///          constant regardless of subject or body size.
/// @param s Live active-operation client.
/// @param output Caller-owned staging buffer.
/// @param bytes Source bytes; may be NULL only when @p len is zero.
/// @param len Exact number of bytes to append.
/// @return Nonzero on success; zero with LastError populated on failure.
static int smtp_output_append(rt_smtp_impl *s,
                              smtp_output_buffer_t *output,
                              const void *bytes,
                              size_t len) {
    if (!output || (!bytes && len != 0)) {
        set_error(s, "SMTP: invalid DATA byte range");
        return 0;
    }
    const uint8_t *source = (const uint8_t *)bytes;
    while (len > 0) {
        if (output->len == sizeof(output->bytes) && !smtp_output_flush(s, output))
            return 0;
        size_t available = sizeof(output->bytes) - output->len;
        size_t count = len < available ? len : available;
        memcpy(output->bytes + output->len, source, count);
        output->len += count;
        source += count;
        len -= count;
    }
    return 1;
}

/// @brief Stream one untrusted MIME header value with injection bytes neutralized.
/// @details Every C0 control and DEL byte becomes one space. UTF-8 bytes are
///          preserved verbatim, and no message-sized temporary allocation is made.
/// @param s Live active-operation client.
/// @param output Caller-owned staging buffer.
/// @param value Exact NUL-free header bytes; may be NULL when @p len is zero.
/// @param len Exact header byte length.
/// @return Nonzero on success; zero with LastError populated on failure.
static int smtp_output_append_header_value(rt_smtp_impl *s,
                                           smtp_output_buffer_t *output,
                                           const char *value,
                                           size_t len) {
    if (!value && len != 0) {
        set_error(s, "SMTP: invalid header byte range");
        return 0;
    }
    size_t run_start = 0;
    for (size_t index = 0; index < len; index++) {
        unsigned char byte = (unsigned char)value[index];
        if (byte >= 0x20u && byte != 0x7Fu)
            continue;
        if (index > run_start &&
            !smtp_output_append(s, output, value + run_start, index - run_start)) {
            return 0;
        }
        if (!smtp_output_append(s, output, " ", 1u))
            return 0;
        run_start = index + 1u;
    }
    return run_start < len ? smtp_output_append(s, output, value + run_start, len - run_start) : 1;
}

/// @brief Stream a body using RFC 5321 newline normalization and dot transparency.
/// @details CR, LF, and CRLF inputs become CRLF. A dot at the beginning of any
///          normalized line is doubled, the content is guaranteed to end in
///          CRLF, and the final SMTP `.<CRLF>` terminator is appended. Processing
///          uses only @ref smtp_output_buffer_t regardless of body size.
/// @param s Live active-operation client.
/// @param output Caller-owned staging buffer.
/// @param body Exact NUL-free body bytes; may be NULL when @p body_len is zero.
/// @param body_len Exact body byte length.
/// @return Nonzero on success; zero with LastError populated on failure.
static int smtp_output_append_body(rt_smtp_impl *s,
                                   smtp_output_buffer_t *output,
                                   const char *body,
                                   size_t body_len) {
    int at_line_start = 1;
    int ended_with_newline = 0;
    for (size_t index = 0; index < body_len; index++) {
        unsigned char byte = (unsigned char)body[index];
        if (at_line_start && byte == '.' && !smtp_output_append(s, output, ".", 1u))
            return 0;
        if (byte == '\r' || byte == '\n') {
            if (byte == '\r' && index + 1u < body_len && body[index + 1u] == '\n')
                index++;
            if (!smtp_output_append(s, output, "\r\n", 2u))
                return 0;
            at_line_start = 1;
            ended_with_newline = 1;
            continue;
        }
        if (!smtp_output_append(s, output, &body[index], 1u))
            return 0;
        at_line_start = 0;
        ended_with_newline = 0;
    }
    if (!ended_with_newline && !smtp_output_append(s, output, "\r\n", 2u))
        return 0;
    return smtp_output_append(s, output, ".\r\n", 3u) && smtp_output_flush(s, output);
}

/// @brief Observe one validated line of a complete SMTP response.
/// @param line Borrowed NUL-terminated response line without CRLF.
/// @param ctx Opaque callback context supplied to the command reader.
typedef void (*smtp_line_callback_t)(const char *line, void *ctx);

/// @brief Parse the status code and continuation separator of an SMTP reply.
/// @param line NUL-terminated reply line without CRLF.
/// @return Status in 100..599 for a syntactically valid line; otherwise -1.
static int smtp_parse_response_code(const char *line) {
    if (!line || strlen(line) < 4)
        return -1;
    if (line[0] < '0' || line[0] > '9' || line[1] < '0' || line[1] > '9' || line[2] < '0' ||
        line[2] > '9')
        return -1;
    int code = (line[0] - '0') * 100 + (line[1] - '0') * 10 + (line[2] - '0');
    if (code < 100 || code > 599 || (line[3] != ' ' && line[3] != '-'))
        return -1;
    return code;
}

/// @brief Send one SMTP command and consume its complete strict reply.
/// @details Multiline replies are bounded, require the same three-digit code on
///          every line, and terminate only with `code SP`. Mismatched codes,
///          malformed separators, or incomplete replies close off protocol
///          desynchronization before a later command is sent.
/// @param s Live active-operation client.
/// @param cmd Command bytes including CRLF, or NULL to read a pending reply.
/// @param expected_code Required exact code, or zero to return any valid code.
/// @param line_cb Optional callback for each validated reply line.
/// @param line_ctx Callback context.
/// @return Reply code, or -1 with LastError populated.
static int smtp_command_ex(rt_smtp_impl *s,
                           const char *cmd,
                           int expected_code,
                           smtp_line_callback_t line_cb,
                           void *line_ctx) {
    if (cmd) {
        if (!smtp_transport_send_all(s, cmd, strlen(cmd))) {
            set_error(s, "SMTP: send failed");
            return -1;
        }
    }

    char *line = smtp_recv_line(s);
    if (!line) {
        return -1;
    }
    int code = smtp_parse_response_code(line);
    if (code < 0) {
        free(line);
        set_error(s, "SMTP: malformed response line");
        return -1;
    }
    int multiline = line[3] == '-';
    if (line_cb)
        line_cb(line, line_ctx);
    free(line);

    size_t line_count = 1;
    while (multiline) {
        if (line_count++ >= SMTP_MAX_REPLY_LINES) {
            set_error(s, "SMTP: multiline response exceeds limit");
            return -1;
        }
        line = smtp_recv_line(s);
        if (!line) {
            set_error(s, "SMTP: incomplete multi-line response");
            return -1;
        }
        int next_code = smtp_parse_response_code(line);
        if (next_code != code) {
            free(line);
            set_error(s, "SMTP: multiline response code changed");
            return -1;
        }
        multiline = line[3] == '-';
        if (line_cb)
            line_cb(line, line_ctx);
        free(line);
    }

    if (expected_code > 0 && code != expected_code) {
        char err[128];
        snprintf(err, sizeof(err), "SMTP: expected %d, got %d", expected_code, code);
        set_error(s, err);
        return -1;
    }

    return code;
}

/// @brief Send an SMTP command and consume its reply without a line callback.
/// @param s Live active-operation client.
/// @param cmd Command including CRLF, or NULL to read a pending reply.
/// @param expected_code Required reply code, or zero to accept any valid code.
/// @return Reply code, or -1 with LastError populated.
static int smtp_command(rt_smtp_impl *s, const char *cmd, int expected_code) {
    return smtp_command_ex(s, cmd, expected_code, NULL, NULL);
}

/// @brief Encode and send one AUTH LOGIN credential without managed secret copies.
/// @details A bounded native base64 buffer is wiped before release on every
///          reply outcome. This removes temporary managed Strings from the
///          authentication path and keeps credentials out of GC storage.
/// @param s Live active-operation client.
/// @param plain NUL-terminated credential bytes.
/// @param expected_code Required server reply code.
/// @return Reply code, or -1 with LastError populated.
static int smtp_send_base64_line(rt_smtp_impl *s, const char *plain, int expected_code) {
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const unsigned char *input = (const unsigned char *)(plain ? plain : "");
    size_t input_len = strlen((const char *)input);
    if (input_len > SMTP_MAX_CREDENTIAL_BYTES || input_len > (SIZE_MAX - 2u) / 3u) {
        set_error(s, "SMTP: credential exceeds limit");
        return -1;
    }
    size_t encoded_len = ((input_len + 2u) / 3u) * 4u;
    if (encoded_len > SIZE_MAX - 3u) {
        set_error(s, "SMTP: credential encoding overflow");
        return -1;
    }
    char *command = (char *)malloc(encoded_len + 3u);
    if (!command) {
        set_error(s, "SMTP: OOM");
        return -1;
    }

    size_t input_pos = 0;
    size_t output_pos = 0;
    while (input_pos < input_len) {
        size_t remaining = input_len - input_pos;
        uint32_t triple = (uint32_t)input[input_pos] << 16;
        if (remaining > 1u)
            triple |= (uint32_t)input[input_pos + 1u] << 8;
        if (remaining > 2u)
            triple |= (uint32_t)input[input_pos + 2u];
        command[output_pos++] = alphabet[(triple >> 18) & 0x3Fu];
        command[output_pos++] = alphabet[(triple >> 12) & 0x3Fu];
        command[output_pos++] = remaining > 1u ? alphabet[(triple >> 6) & 0x3Fu] : '=';
        command[output_pos++] = remaining > 2u ? alphabet[triple & 0x3Fu] : '=';
        input_pos += remaining > 3u ? 3u : remaining;
    }
    command[encoded_len] = '\r';
    command[encoded_len + 1u] = '\n';
    command[encoded_len + 2u] = '\0';
    int response = smtp_command(s, command, expected_code);
    smtp_secure_zero(command, encoded_len + 3u);
    free(command);
    return response;
}

/// @brief Accumulate supported SMTP extensions from one validated EHLO line.
/// @details Recognizes exact STARTTLS and AUTH LOGIN tokens using
///          locale-independent ASCII case folding.
/// @param line Reply line including its three-digit status prefix.
/// @param ctx Mutable @ref smtp_caps_t accumulator.
static void smtp_parse_ehlo_caps_line(const char *line, void *ctx) {
    smtp_caps_t *caps = (smtp_caps_t *)ctx;
    const char *value = line;
    if (!caps || !line)
        return;
    if (strlen(line) < 4)
        return;
    value = line + 3;
    if (*value == '-' || *value == ' ')
        value++;
    while (*value == ' ' || *value == '\t')
        value++;

    size_t value_len = strlen(value);
    if (value_len >= 8u && smtp_ascii_ncasecmp(value, "STARTTLS", 8) == 0 &&
        (value[8] == '\0' || value[8] == ' ' || value[8] == '\t')) {
        caps->supports_starttls = 1;
        return;
    }

    if (value_len >= 4u && smtp_ascii_ncasecmp(value, "AUTH", 4) == 0 &&
        (value[4] == '\0' || value[4] == ' ' || value[4] == '\t')) {
        const char *token = value + 4;
        while (*token) {
            const char *start;
            const char *end;
            while (*token == ' ' || *token == '\t')
                token++;
            if (*token == '\0')
                break;
            start = token;
            while (*token && *token != ' ' && *token != '\t')
                token++;
            end = token;
            if ((size_t)(end - start) == strlen("LOGIN") &&
                smtp_ascii_ncasecmp(start, "LOGIN", strlen("LOGIN")) == 0) {
                caps->supports_auth_login = 1;
                break;
            }
        }
    }
}

//=============================================================================
// Public API
//=============================================================================

/// @brief Copy the active trap diagnostic before clearing its recovery frame.
/// @param output Destination buffer.
/// @param capacity Destination capacity including the terminator.
/// @param fallback Message used when no trap diagnostic is active.
static void smtp_save_trap(char *output, size_t capacity, const char *fallback) {
    if (!output || capacity == 0)
        return;
    const char *message = rt_trap_get_error();
    snprintf(output,
             capacity,
             "%s",
             message && message[0]
                 ? message
                 : (fallback && fallback[0] ? fallback : "SmtpClient operation failed"));
}

/// @brief Copy LastError into independent native storage under the state mutex.
/// @details The returned bytes remain valid after another thread starts a send
///          or clears the diagnostic. Managed allocation is deliberately left
///          to the caller so no runtime trap can strand the mutex.
/// @param s Initialized SMTP client.
/// @param fallback Text copied when LastError is empty; NULL selects an empty string.
/// @return Heap-owned NUL-terminated snapshot, or NULL after one allocation trap.
static char *smtp_error_snapshot(rt_smtp_impl *s, const char *fallback) {
    if (!s) {
        rt_trap("SmtpClient: invalid error snapshot receiver");
        return NULL;
    }
    char *snapshot = NULL;
    smtp_mutex_lock(&s->state_lock);
    const char *source = s->last_error && s->last_error[0] ? s->last_error : fallback;
    if (!source)
        source = "";
    size_t length = strlen(source);
    snapshot = (char *)malloc(length + 1u);
    if (snapshot)
        memcpy(snapshot, source, length + 1u);
    smtp_mutex_unlock(&s->state_lock);
    if (!snapshot) {
        rt_trap("SmtpClient: error snapshot allocation failed");
        return NULL;
    }
    return snapshot;
}

/// @brief Build a caller-owned `Result.ErrStr` from stable native text.
/// @details String and Result creation use a fresh recovery frame. Any partial
///          managed values are released before an allocation failure is
///          re-raised, and the temporary String producer reference is consumed
///          after Result retains it.
/// @param message Stable diagnostic bytes; NULL or empty selects a fixed fallback.
/// @return Caller-owned error Result, or NULL after a returning trap hook.
static void *smtp_error_result(const char *message) {
    rt_string volatile error_string = NULL;
    void *volatile result = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[512];
        smtp_save_trap(
            saved_error, sizeof(saved_error), "SmtpClient: error Result allocation failed");
        rt_trap_clear_recovery();
        smtp_release_managed((void *)result);
        rt_str_release_maybe((rt_string)error_string);
        rt_trap(saved_error);
        return NULL;
    }
    const char *stable = message && message[0] ? message : "SmtpClient send failed";
    error_string = rt_string_from_bytes(stable, strlen(stable));
    result = rt_result_err_str((rt_string)error_string);
    rt_trap_clear_recovery();
    rt_str_release_maybe((rt_string)error_string);
    return (void *)result;
}

/// @brief Build the SMTP success Result under a cleanup recovery frame.
/// @details A partial Result is released if allocation traps. The payload is
///          the legacy success sentinel `I64(1)`.
/// @return Caller-owned `Result.OkI64(1)`, or NULL after a returning trap hook.
static void *smtp_success_result(void) {
    void *volatile result = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[512];
        smtp_save_trap(
            saved_error, sizeof(saved_error), "SmtpClient: success Result allocation failed");
        rt_trap_clear_recovery();
        smtp_release_managed((void *)result);
        rt_trap(saved_error);
        return NULL;
    }
    result = rt_result_ok_i64(1);
    rt_trap_clear_recovery();
    return (void *)result;
}

/// @brief Release one caller-owned managed reference.
/// @param object Runtime object or String, or NULL.
static void smtp_release_managed(void *object) {
    if (object && rt_obj_release_check0(object))
        rt_obj_free(object);
}

/// @brief Validate an SMTP DNS/numeric host byte range.
/// @details Empty, oversized, whitespace/control-bearing, URL-delimiter, and
///          unmatched bracket forms are rejected before DNS or TLS receives
///          the string. Bracketed IPv6 is accepted and normalized by New.
/// @param host Exact host bytes.
/// @param length Exact byte length.
/// @param content_start Receives the first stored byte after optional `[`.
/// @param content_len Receives the stored length before optional `]`.
/// @return Nonzero for a safe host representation.
static int smtp_host_is_valid(const char *host,
                              size_t length,
                              const char **content_start,
                              size_t *content_len) {
    if (!host || length == 0 || length > 1024u)
        return 0;
    size_t start = 0;
    size_t end = length;
    if (host[0] == '[') {
        if (length < 3u || host[length - 1u] != ']')
            return 0;
        start = 1;
        end--;
    } else if (host[length - 1u] == ']') {
        return 0;
    }
    for (size_t index = start; index < end; index++) {
        unsigned char byte = (unsigned char)host[index];
        if (byte <= 0x20u || byte == 0x7Fu || byte == '/' || byte == '\\' || byte == '?' ||
            byte == '#' || byte == '@' || byte == '[' || byte == ']') {
            return 0;
        }
    }
    if (end == start)
        return 0;
    if (start == 1u) {
        char numeric_host[1025];
        size_t numeric_len = end - start;
        memcpy(numeric_host, host + start, numeric_len);
        numeric_host[numeric_len] = '\0';
        struct in6_addr address;
        if (inet_pton(AF_INET6, numeric_host, &address) != 1)
            return 0;
    }
    if (content_start)
        *content_start = host + start;
    if (content_len)
        *content_len = end - start;
    return 1;
}

/// @brief Construct a managed SMTP client for a validated host and port.
/// @details Bracketed IPv6 is normalized to its literal contents. Port 465
///          enables implicit TLS; other ports default to plaintext until
///          STARTTLS is enabled. Both mutexes and copied host storage are built
///          transactionally before the object is returned.
/// @param host DNS name, numeric address, or bracketed IPv6 literal without
///        embedded NUL.
/// @param port Destination port in 1..65535.
/// @return Caller-owned managed SMTP client, or NULL after a returning trap.
void *rt_smtp_new(rt_string host, int64_t port) {
    const char *host_bytes = NULL;
    size_t host_len = 0;
    const char *stored_host = NULL;
    size_t stored_host_len = 0;
    if (!smtp_string_view(host, 0, &host_bytes, &host_len) ||
        !smtp_host_is_valid(host_bytes, host_len, &stored_host, &stored_host_len) || port < 1 ||
        port > 65535) {
        rt_trap("SmtpClient: invalid host or port");
        return NULL;
    }

    rt_smtp_impl *volatile s = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[512];
        smtp_save_trap(saved_error, sizeof(saved_error), "SmtpClient: construction failed");
        rt_trap_clear_recovery();
        smtp_release_managed((void *)s);
        rt_trap(saved_error);
        return NULL;
    }

    s = (rt_smtp_impl *)rt_obj_new_i64(RT_SMTP_CLASS_ID, (int64_t)sizeof(rt_smtp_impl));
    if (!s) {
        rt_trap_clear_recovery();
        return NULL;
    }
    memset((void *)s, 0, sizeof(*s));
    rt_obj_set_finalizer((void *)s, rt_smtp_finalize);
    if (!smtp_mutex_init(&((rt_smtp_impl *)s)->state_lock))
        rt_trap("SmtpClient: state mutex initialization failed");
    ((rt_smtp_impl *)s)->state_lock_initialized = 1;
    if (!smtp_mutex_init(&((rt_smtp_impl *)s)->operation_lock))
        rt_trap("SmtpClient: operation mutex initialization failed");
    ((rt_smtp_impl *)s)->operation_lock_initialized = 1;
    ((rt_smtp_impl *)s)->host = (char *)malloc(stored_host_len + 1u);
    if (!((rt_smtp_impl *)s)->host)
        rt_trap("SmtpClient: host allocation failed");
    memcpy(((rt_smtp_impl *)s)->host, stored_host, stored_host_len);
    ((rt_smtp_impl *)s)->host[stored_host_len] = '\0';
    ((rt_smtp_impl *)s)->port = (int)port;
    ((rt_smtp_impl *)s)->use_tls = (port == 465) ? 1 : 0;
    rt_trap_clear_recovery();
    return (void *)s;
}

/// @brief Transactionally replace credentials used for AUTH LOGIN.
/// @details Both values must be present or both NULL. Valid credentials are
///          NUL-free and at most 16 KiB, copied before the serialized exchange,
///          and securely erased when superseded. Two NULLs disable AUTH.
/// @param obj SMTP client receiver; NULL is a no-op.
/// @param username Username String, or NULL with @p password.
/// @param password Password String, or NULL with @p username.
void rt_smtp_set_auth(void *obj, rt_string username, rt_string password) {
    if (!obj)
        return;
    rt_smtp_impl *s = smtp_require(obj, "SmtpClient.SetAuth: invalid client");
    if (!s)
        return;
    const char *username_bytes = NULL;
    const char *password_bytes = NULL;
    size_t username_len = 0;
    size_t password_len = 0;
    if ((username == NULL) != (password == NULL) ||
        !smtp_string_view(username, 1, &username_bytes, &username_len) ||
        !smtp_string_view(password, 1, &password_bytes, &password_len) ||
        username_len > SMTP_MAX_CREDENTIAL_BYTES || password_len > SMTP_MAX_CREDENTIAL_BYTES) {
        rt_trap("SmtpClient: invalid auth credentials");
        return;
    }
    char *new_username = username ? (char *)malloc(username_len + 1u) : NULL;
    if (new_username) {
        memcpy(new_username, username_bytes, username_len);
        new_username[username_len] = '\0';
    }
    char *new_password = password ? (char *)malloc(password_len + 1u) : NULL;
    if (new_password) {
        memcpy(new_password, password_bytes, password_len);
        new_password[password_len] = '\0';
    }
    if ((username && !new_username) || (password && !new_password)) {
        smtp_free_secret(new_username);
        smtp_free_secret(new_password);
        rt_trap("SmtpClient: auth allocation failed");
        return;
    }

    smtp_mutex_lock(&s->operation_lock);
    char *old_username = s->username;
    char *old_password = s->password;
    s->username = new_username;
    s->password = new_password;
    smtp_mutex_unlock(&s->operation_lock);
    smtp_free_secret(old_username);
    smtp_free_secret(old_password);
}

/// @brief Configure encrypted transport for subsequent sends.
/// @details Nonzero selects implicit TLS on port 465 or required STARTTLS on
///          other ports. Zero selects plaintext, but configured credentials
///          are still never transmitted without encryption. The update waits
///          for any active send.
/// @param obj SMTP client receiver; NULL is a no-op.
/// @param enable Nonzero to enable TLS; zero to disable it.
void rt_smtp_set_tls(void *obj, int8_t enable) {
    if (!obj)
        return;
    rt_smtp_impl *s = smtp_require(obj, "SmtpClient.SetTls: invalid client");
    if (!s)
        return;
    smtp_mutex_lock(&s->operation_lock);
    s->use_tls = enable ? 1 : 0;
    smtp_mutex_unlock(&s->operation_lock);
}

/// @brief Connect a plain runtime TCP wrapper without leaking its temporary host String.
/// @details Nested transport traps are re-raised only after the host String and
///          any partially returned TCP object are released. The caller owns the
///          returned TCP reference.
/// @param host NUL-terminated validated host.
/// @param port TCP port.
/// @return Caller-owned connected TCP wrapper, or NULL after a trap.
static void *smtp_connect_plain_transaction(const char *host, int port) {
    rt_string volatile host_string = NULL;
    void *volatile tcp = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[512];
        smtp_save_trap(saved_error, sizeof(saved_error), "SMTP: TCP connection failed");
        rt_trap_clear_recovery();
        smtp_release_managed((void *)tcp);
        rt_str_release_maybe((rt_string)host_string);
        rt_trap(saved_error);
        return NULL;
    }
    host_string = rt_string_from_bytes(host, strlen(host));
    tcp = rt_tcp_connect_for((rt_string)host_string, port, SMTP_IO_TIMEOUT_MS);
    rt_trap_clear_recovery();
    rt_str_release_maybe((rt_string)host_string);
    return (void *)tcp;
}

/// @brief Upgrade a published TCP wrapper into a cancellation-visible TLS session.
/// @details The TLS owner is published before the blocking handshake begins,
///          allowing concurrent Close to snapshot and shut down its descriptor.
///          The old TCP wrapper is detached and released exactly once when TLS
///          assumes descriptor ownership. This helper is shared by implicit
///          TLS on port 465 and explicit STARTTLS after the 220 response.
/// @param s Active serialized client with one published plain TCP wrapper.
/// @return Zero after a successful handshake; -1 with LastError populated otherwise.
static int smtp_upgrade_published_tcp_to_tls(rt_smtp_impl *s) {
    rt_tls_config_t cfg;
    rt_tls_config_init(&cfg);
    cfg.hostname = s->host;
    cfg.timeout_ms = SMTP_IO_TIMEOUT_MS;
    socket_t socket_fd = s->tcp ? rt_tcp_socket_fd(s->tcp) : INVALID_SOCK;
    rt_tls_session_t *tls =
        socket_fd != INVALID_SOCK ? rt_tls_new((intptr_t)socket_fd, &cfg) : NULL;
    if (!tls) {
        const char *detail = rt_tls_last_error();
        char message[512];
        if (detail && detail[0])
            snprintf(message, sizeof(message), "SMTP: TLS setup failed: %s", detail);
        else
            snprintf(message, sizeof(message), "SMTP: TLS setup failed");
        set_error(s, message);
        return -1;
    }
    if (!smtp_publish_starttls(s, tls)) {
        set_error(s, "SMTP: operation cancelled during TLS setup");
        return -1;
    }
    s->tls->io_cancel_requested = smtp_tls_cancel_requested;
    s->tls->io_cancel_context = s;
    s->tls->io_deadline_us = smtp_io_deadline_us();
    if (!smtp_set_transport_timeouts(s, SMTP_IO_TIMEOUT_MS)) {
        set_error(s, "SMTP: failed to configure TLS I/O timeout");
        smtp_close_transport(s);
        return -1;
    }
    if (rt_tls_handshake(s->tls) != RT_TLS_OK) {
        const char *detail = rt_tls_get_error(s->tls);
        char message[512];
        if (detail && detail[0])
            snprintf(message, sizeof(message), "SMTP: TLS handshake failed: %s", detail);
        else
            snprintf(message, sizeof(message), "SMTP: TLS handshake failed");
        set_error(s, message);
        smtp_close_transport(s);
        return -1;
    }
    s->tls->io_deadline_us = 0;
    return 0;
}

/// @brief Open the transport and walk the SMTP greeting + (optional) STARTTLS + (optional)
/// AUTH LOGIN sequence. Sequence:
///   1. Connect through a published TCP wrapper. Port 465 upgrades and publishes
///      a TLS owner before its handshake so Close can interrupt blocked TLS I/O.
///   2. Read 220 greeting; send `EHLO localhost`, expect 250.
///   3. If `use_tls && port != 465`: send STARTTLS (220), wrap the existing socket in a TLS
///      session, detach the TCP wrapper as soon as the session owns the descriptor, then run
///      the handshake — so a handshake failure closes the descriptor exactly once (via TLS).
///   4. If credentials are set: send AUTH LOGIN (334) → base64(username) (334) → base64(password)
///      (235). Each credential is encoded in bounded native storage that is
///      securely wiped before release.
/// All steps populate `last_error` on failure and return -1; success returns 0.
/// @param s Active serialized SMTP client with immutable configuration.
/// @return Zero after a complete greeting, upgrade, and authentication;
///         otherwise -1 with LastError populated.
static int smtp_connect_and_handshake(rt_smtp_impl *s) {
    smtp_caps_t caps = {0, 0};
    smtp_close_transport(s);
    clear_error(s);

    void *tcp = smtp_connect_plain_transaction(s->host, s->port);
    if (!tcp || !rt_tcp_is_open(tcp)) {
        smtp_release_managed(tcp);
        set_error(s, "SMTP: connection failed");
        return -1;
    }
    if (!smtp_publish_tcp(s, tcp)) {
        rt_tcp_close(tcp);
        smtp_release_managed(tcp);
        set_error(s, "SMTP: operation cancelled");
        return -1;
    }
    if (s->use_tls && s->port == 465) {
        if (smtp_upgrade_published_tcp_to_tls(s) < 0)
            return -1;
    } else {
        if (!smtp_set_transport_timeouts(s, SMTP_IO_TIMEOUT_MS)) {
            set_error(s, "SMTP: failed to configure transport I/O timeout");
            return -1;
        }
    }

    if (smtp_command(s, NULL, 220) < 0)
        return -1;

    char ehlo[300];
    snprintf(ehlo, sizeof(ehlo), "EHLO localhost\r\n");
    int ehlo_code = smtp_command_ex(s, ehlo, 0, smtp_parse_ehlo_caps_line, &caps);
    if (ehlo_code != 250) {
        if (ehlo_code < 0)
            return -1;
        int can_fallback = ehlo_code == 500 || ehlo_code == 502 || ehlo_code == 504;
        if (!can_fallback || s->use_tls || s->username || s->password) {
            set_error(s, "SMTP: EHLO required for requested session features");
            return -1;
        }
        if (smtp_command(s, "HELO localhost\r\n", 250) < 0)
            return -1;
    }

    if (s->use_tls && s->port != 465) {
        if (!caps.supports_starttls) {
            set_error(s, "SMTP: server does not advertise STARTTLS");
            return -1;
        }
        if (smtp_command(s, "STARTTLS\r\n", 220) < 0)
            return -1;

        if (smtp_upgrade_published_tcp_to_tls(s) < 0)
            return -1;

        caps.supports_starttls = 0;
        caps.supports_auth_login = 0;
        if (smtp_command_ex(s, ehlo, 250, smtp_parse_ehlo_caps_line, &caps) < 0)
            return -1;
    }

    // AUTH LOGIN if credentials provided
    if (s->username && s->password) {
        if (!s->tls && s->port != 465) {
            set_error(s, "SMTP: refusing AUTH LOGIN over an unencrypted connection");
            return -1;
        }
        if (!caps.supports_auth_login) {
            set_error(s, "SMTP: server does not advertise AUTH LOGIN");
            return -1;
        }
        if (smtp_command(s, "AUTH LOGIN\r\n", 334) < 0)
            return -1;

        if (smtp_send_base64_line(s, s->username, 334) < 0)
            return -1;

        if (smtp_send_base64_line(s, s->password, 235) < 0)
            return -1;
    }

    return 0;
}

/// @brief Walk MAIL FROM → RCPT TO → DATA and stream one MIME message.
/// @details Envelope commands remain bounded. Header controls are neutralized,
///          body newlines are normalized, and RFC 5321 dot transparency is
///          applied incrementally through a fixed 4 KiB native buffer. No
///          allocation grows with subject or body size. The client currently
///          supports one recipient and issues one RCPT TO command.
/// @param s Live active-operation client after a successful handshake.
/// @param from Exact validated sender bytes.
/// @param from_len Sender byte length.
/// @param to Exact validated recipient bytes.
/// @param to_len Recipient byte length.
/// @param subject Exact NUL-free subject bytes.
/// @param subject_len Subject byte length.
/// @param body Exact NUL-free body bytes.
/// @param body_len Body byte length.
/// @param content_type Fixed safe MIME media type.
/// @return Zero on success; -1 with LastError populated on failure.
static int smtp_send_message(rt_smtp_impl *s,
                             const char *from,
                             size_t from_len,
                             const char *to,
                             size_t to_len,
                             const char *subject,
                             size_t subject_len,
                             const char *body,
                             size_t body_len,
                             const char *content_type) {
    char cmd[1024];
    if (!smtp_validate_mailbox_path(from, from_len) || !smtp_validate_mailbox_path(to, to_len)) {
        set_error(s, "SMTP: invalid envelope address");
        return -1;
    }
    if (!smtp_header_value_is_command_safe(content_type)) {
        set_error(s, "SMTP: invalid content type");
        return -1;
    }

    // MAIL FROM
    int wrote = snprintf(cmd, sizeof(cmd), "MAIL FROM:<%s>\r\n", from);
    if (wrote < 0 || (size_t)wrote >= sizeof(cmd)) {
        set_error(s, "SMTP: envelope address too long");
        return -1;
    }
    if (smtp_command(s, cmd, 250) < 0)
        return -1;

    // RCPT TO
    wrote = snprintf(cmd, sizeof(cmd), "RCPT TO:<%s>\r\n", to);
    if (wrote < 0 || (size_t)wrote >= sizeof(cmd)) {
        set_error(s, "SMTP: envelope address too long");
        return -1;
    }
    {
        int rcpt_code = smtp_command(s, cmd, 0);
        if (!(rcpt_code == 250 || rcpt_code == 251 || rcpt_code == 252)) {
            char err[128];
            snprintf(err, sizeof(err), "SMTP: recipient rejected (%d)", rcpt_code);
            set_error(s, err);
            return -1;
        }
    }

    // DATA
    if (smtp_command(s, "DATA\r\n", 354) < 0)
        return -1;

    smtp_output_buffer_t output = {{0}, 0};
    if (!smtp_output_append(s, &output, "From: ", sizeof("From: ") - 1u) ||
        !smtp_output_append_header_value(s, &output, from, from_len) ||
        !smtp_output_append(s, &output, "\r\nTo: ", sizeof("\r\nTo: ") - 1u) ||
        !smtp_output_append_header_value(s, &output, to, to_len) ||
        !smtp_output_append(s, &output, "\r\nSubject: ", sizeof("\r\nSubject: ") - 1u) ||
        !smtp_output_append_header_value(s, &output, subject, subject_len) ||
        !smtp_output_append(s,
                            &output,
                            "\r\nMIME-Version: 1.0\r\nContent-Type: ",
                            sizeof("\r\nMIME-Version: 1.0\r\nContent-Type: ") - 1u) ||
        !smtp_output_append(s, &output, content_type, strlen(content_type)) ||
        !smtp_output_append(
            s, &output, "; charset=utf-8\r\n\r\n", sizeof("; charset=utf-8\r\n\r\n") - 1u) ||
        !smtp_output_append_body(s, &output, body, body_len)) {
        return -1;
    }

    if (smtp_command(s, NULL, 250) < 0)
        return -1;
    return 0;
}

/// @brief Attempt a graceful SMTP QUIT without changing a completed DATA outcome.
/// @details QUIT is advisory after the server has accepted the message. A
///          transport trap or non-221 reply is consumed under a nested recovery
///          frame; the operation owner subsequently closes the transport and
///          clears any QUIT-only LastError on successful delivery.
/// @param s Live active-operation client after an accepted DATA transaction.
static void smtp_quit_best_effort(rt_smtp_impl *s) {
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        rt_trap_clear_recovery();
        return;
    }
    (void)smtp_command(s, "QUIT\r\n", 221);
    rt_trap_clear_recovery();
}

/// @brief Execute one serialized fresh-connection SMTP send.
/// @details Runtime String identity and embedded NULs are validated before any
///          native access. `operation_lock` serializes configuration and sends;
///          a recovery boundary always detaches transport state and releases
///          the lock before re-raising lower-level traps. Concurrent Close only
///          marks cancellation and shuts down the descriptor, leaving final
///          ownership to this operation.
/// @param obj Candidate SMTP receiver; NULL preserves the legacy false result.
/// @param from Sender mailbox String; NULL is rejected as an envelope error.
/// @param to Recipient mailbox String; NULL is rejected as an envelope error.
/// @param subject Optional subject String; NULL means empty.
/// @param body Optional plain or HTML body String; NULL means empty.
/// @param content_type Fixed safe MIME media type.
/// @param operation Diagnostic used for forged String handles.
/// @return One after server acceptance, otherwise zero; lower traps are re-raised after cleanup.
static int8_t smtp_send_common(void *obj,
                               rt_string from,
                               rt_string to,
                               rt_string subject,
                               rt_string body,
                               const char *content_type,
                               const char *operation) {
    if (!obj)
        return 0;
    rt_smtp_impl *s = smtp_require(obj, operation);
    if (!s)
        return 0;

    const char *from_bytes = NULL;
    const char *to_bytes = NULL;
    const char *subject_bytes = NULL;
    const char *body_bytes = NULL;
    size_t from_len = 0;
    size_t to_len = 0;
    size_t subject_len = 0;
    size_t body_len = 0;
    if (!smtp_string_view(from, 1, &from_bytes, &from_len) ||
        !smtp_string_view(to, 1, &to_bytes, &to_len) ||
        !smtp_string_view(subject, 1, &subject_bytes, &subject_len) ||
        !smtp_string_view(body, 1, &body_bytes, &body_len)) {
        rt_trap("SmtpClient.Send: invalid String or embedded NUL");
        return 0;
    }
    if (!from_bytes)
        from_bytes = "";
    if (!to_bytes)
        to_bytes = "";
    if (!subject_bytes)
        subject_bytes = "";
    if (!body_bytes)
        body_bytes = "";

    smtp_mutex_lock(&s->operation_lock);
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[512];
        smtp_save_trap(saved_error, sizeof(saved_error), "SmtpClient.Send: transport failure");
        rt_trap_clear_recovery();
        smtp_finish_operation(s);
        smtp_mutex_unlock(&s->operation_lock);
        rt_trap(saved_error);
        return 0;
    }

    smtp_begin_operation(s);
    int result = smtp_connect_and_handshake(s);
    if (result == 0) {
        result = smtp_send_message(s,
                                   from_bytes,
                                   from_len,
                                   to_bytes,
                                   to_len,
                                   subject_bytes,
                                   subject_len,
                                   body_bytes,
                                   body_len,
                                   content_type);
    }
    if (result == 0)
        smtp_quit_best_effort(s);
    smtp_finish_operation(s);
    if (result == 0)
        clear_error(s);
    rt_trap_clear_recovery();
    smtp_mutex_unlock(&s->operation_lock);
    return result == 0 ? 1 : 0;
}

/// @brief Convert one Boolean SMTP send into an allocation-safe Result.
/// @details An outer recovery frame catches receiver, String, transport, TLS,
///          and cleanup traps only after the send operation has released its
///          mutex and transport. Protocol failures snapshot LastError in native
///          memory before Result construction begins.
/// @param obj Candidate SMTP receiver.
/// @param from Sender mailbox String.
/// @param to Recipient mailbox String.
/// @param subject Optional subject String.
/// @param body Optional message body String.
/// @param content_type Fixed safe MIME media type.
/// @param fallback Stable error text when no detailed diagnostic exists.
/// @return Caller-owned `OkI64(1)` or `ErrStr` Result.
static void *smtp_send_result_common(void *obj,
                                     rt_string from,
                                     rt_string to,
                                     rt_string subject,
                                     rt_string body,
                                     const char *content_type,
                                     const char *fallback) {
    char *volatile snapshot = NULL;
    int8_t volatile ok = 0;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[512];
        smtp_save_trap(saved_error, sizeof(saved_error), fallback);
        rt_trap_clear_recovery();
        free((void *)snapshot);
        return smtp_error_result(saved_error);
    }

    ok = smtp_send_common(obj, from, to, subject, body, content_type, fallback);
    if (!ok && obj)
        snapshot = smtp_error_snapshot((rt_smtp_impl *)obj, fallback);
    rt_trap_clear_recovery();
    if (ok)
        return smtp_success_result();
    void *result = smtp_error_result((const char *)snapshot ? (const char *)snapshot : fallback);
    free((void *)snapshot);
    return result;
}

/// @brief Send one plain UTF-8 message on a fresh serialized SMTP session.
/// @details The transport is always closed after the attempt. Concurrent sends
///          wait in call order, and a concurrent Close interrupts active I/O.
/// @param obj SMTP client receiver; NULL yields zero.
/// @param from Validated sender mailbox String.
/// @param to Validated recipient mailbox String.
/// @param subject Optional subject; NULL means empty.
/// @param body Optional plain-text body; NULL means empty.
/// @return One after the server accepts DATA; zero for protocol/network failure.
int8_t rt_smtp_send(void *obj, rt_string from, rt_string to, rt_string subject, rt_string body) {
    return smtp_send_common(
        obj, from, to, subject, body, "text/plain", "SmtpClient.Send: invalid client");
}

/// @brief Send a plain-text SMTP message and return Result instead of LastError state.
/// @param obj Opaque Zanna.Network.SmtpClient object.
/// @param from Sender mailbox path.
/// @param to Recipient mailbox path.
/// @param subject Message subject.
/// @param body Plain-text message body.
/// @return Zanna.Result.OkI64(1) on success or Zanna.Result.ErrStr(message) on failure.
void *rt_smtp_send_result(
    void *obj, rt_string from, rt_string to, rt_string subject, rt_string body) {
    return smtp_send_result_common(
        obj, from, to, subject, body, "text/plain", "SmtpClient.SendResult: send failed");
}

/// @brief Send one HTML UTF-8 message on a fresh serialized SMTP session.
/// @details Header injection and body framing receive the same treatment as
///          plain text. HTML escaping remains the caller's responsibility.
/// @param obj SMTP client receiver; NULL yields zero.
/// @param from Validated sender mailbox String.
/// @param to Validated recipient mailbox String.
/// @param subject Optional subject; NULL means empty.
/// @param html_body Optional HTML body; NULL means empty.
/// @return One after the server accepts DATA; zero for protocol/network failure.
int8_t rt_smtp_send_html(
    void *obj, rt_string from, rt_string to, rt_string subject, rt_string html_body) {
    return smtp_send_common(
        obj, from, to, subject, html_body, "text/html", "SmtpClient.SendHtml: invalid client");
}

/// @brief Send an HTML SMTP message and return Result instead of LastError state.
/// @param obj Opaque Zanna.Network.SmtpClient object.
/// @param from Sender mailbox path.
/// @param to Recipient mailbox path.
/// @param subject Message subject.
/// @param html_body HTML message body.
/// @return Zanna.Result.OkI64(1) on success or Zanna.Result.ErrStr(message) on failure.
void *rt_smtp_send_html_result(
    void *obj, rt_string from, rt_string to, rt_string subject, rt_string html_body) {
    return smtp_send_result_common(
        obj, from, to, subject, html_body, "text/html", "SmtpClient.SendHtmlResult: send failed");
}

/// @brief Return a synchronized snapshot of the most recent SMTP failure.
/// @details Native bytes are copied while holding the state mutex; managed
///          String allocation happens afterward under a cleanup recovery frame.
///          A successful send clears the diagnostic. NULL preserves the legacy
///          empty-string sentinel, while forged receivers trap safely.
/// @param obj Candidate SMTP receiver.
/// @return Caller-owned runtime String snapshot, possibly empty.
rt_string rt_smtp_last_error(void *obj) {
    if (!obj)
        return rt_string_from_bytes("", 0);
    rt_smtp_impl *s = smtp_require(obj, "SmtpClient.LastError: invalid client");
    if (!s)
        return NULL;
    char *snapshot = smtp_error_snapshot(s, NULL);
    if (!snapshot)
        return NULL;

    rt_string volatile result = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[512];
        smtp_save_trap(saved_error, sizeof(saved_error), "SmtpClient.LastError: allocation failed");
        rt_trap_clear_recovery();
        free(snapshot);
        rt_str_release_maybe((rt_string)result);
        rt_trap(saved_error);
        return NULL;
    }
    result = rt_string_from_bytes(snapshot, strlen(snapshot));
    rt_trap_clear_recovery();
    free(snapshot);
    return (rt_string)result;
}

/// @brief Cancel active SMTP I/O or close an idle transport immediately.
/// @details Close marks cancellation while holding the state mutex and invokes
///          native shutdown in addition to the transport's bounded cancellation
///          polling. It never frees TLS or TCP state owned by an active operation;
///          that owner detaches and releases the transport before unlocking. The
///          next serialized send clears cancellation and reconnects from scratch.
/// @param obj Candidate SMTP receiver; NULL is an idempotent no-op.
void rt_smtp_close(void *obj) {
    if (!obj)
        return;
    rt_smtp_impl *s = smtp_require(obj, "SmtpClient.Close: invalid client");
    if (!s)
        return;

    int operation_active = 0;
    socket_t socket_fd = INVALID_SOCK;
    smtp_mutex_lock(&s->state_lock);
    s->cancel_requested = 1;
    operation_active = s->operation_active ? 1 : 0;
    if (s->tls)
        socket_fd = (socket_t)rt_tls_get_socket(s->tls);
    else if (s->tcp)
        socket_fd = rt_tcp_socket_fd(s->tcp);
    if (operation_active && socket_fd != INVALID_SOCK)
        (void)rt_socket_shutdown_both(socket_fd);
    smtp_mutex_unlock(&s->state_lock);
    if (!operation_active)
        smtp_close_transport(s);
}
