//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/network/rt_tls_api.c
// Purpose: Zanna-facing wrappers for the TLS runtime — the boxed rt_zanna_tls
//          object plus the Zanna.Crypto.Tls methods (connect, send/recv, close,
//          accessors). Split out of rt_tls.c; calls the low-level rt_tls_*
//          API declared in rt_tls.h.
//
// Key invariants:
//   - These wrappers convert between Zanna types (rt_string, Bytes) and the C
//     TLS session API; the boxed object owns its session + host string.
//   - The underlying handshake/record engine lives in rt_tls.c and is reached
//     only through the public rt_tls_* functions.
//
// Ownership/Lifetime:
//   - rt_zanna_tls_t is a GC object finalized via rt_zanna_tls_finalize, which
//     closes the session and frees the duplicated host string.
//
// Links: src/runtime/network/rt_tls.c (TLS protocol engine),
//        src/runtime/network/rt_tls.h (low-level API),
//        src/runtime/network/rt_tls_internal.h (shared TLS types/ids)
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_tls_api.c
 * @brief Implements language-facing wrappers around the TLS session API.
 * @details These entry points validate managed Tls receivers and runtime
 *          Strings or Bytes, translate them to low-level session calls, expose
 *          connection metadata, and convert recoverable outcomes to managed
 *          Results. Each wrapper object owns its session and copied host text.
 */

#include "rt_tls.h"
#include "rt_tls_internal.h"

#include "rt_internal.h"

#include <limits.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//=============================================================================
// Zanna API Wrappers
//=============================================================================
//
// These functions wrap the low-level TLS API for use by the Zanna runtime.
// They handle conversion between Zanna types (rt_string, Bytes) and C types.
//
//=============================================================================

#include "rt_bytes.h"
#include "rt_object.h"
#include "rt_result.h"
#include "rt_string.h"

/// @brief Internal structure for Zanna TLS objects.
typedef struct rt_zanna_tls {
    rt_tls_session_t *session;
    char *host;
    int64_t port;
} rt_zanna_tls_t;

/// @brief Finalize a managed TLS wrapper and its owned session.
/// @param obj Managed wrapper allocation being finalized, or NULL.
static void rt_zanna_tls_finalize(void *obj) {
    if (!obj)
        return;
    rt_zanna_tls_t *tls = (rt_zanna_tls_t *)obj;
    if (tls->session) {
        rt_tls_close(tls->session);
        tls->session = NULL;
    }
    if (tls->host) {
        free(tls->host);
        tls->host = NULL;
    }
}

/// @brief Release a temporary managed object after ownership transfer.
/// @param obj Caller-owned runtime object, or NULL.
static void rt_zanna_tls_release_temp_object(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief Return the best current TLS connection diagnostic as stable native text.
/// @details The TLS layer stores its last connection error in thread-local
///          native storage, so the returned pointer is valid until the next
///          low-level TLS operation on this thread. The Result helper copies
///          it into a managed String before exposing it to Zanna code.
/// @param fallback Fallback message when the TLS layer has no captured text.
/// @return NUL-terminated diagnostic owned by the TLS layer or caller.
static const char *rt_zanna_tls_connect_error_message(const char *fallback) {
    const char *err = rt_tls_last_error();
    if (!err || !err[0])
        err = fallback && fallback[0] ? fallback : "Tls.Connect failed";
    return err;
}

/// @brief Copy the active trap diagnostic before clearing its recovery frame.
/// @param output Destination buffer.
/// @param capacity Destination capacity including the terminator.
/// @param fallback Diagnostic used when the active text is empty.
static void rt_zanna_tls_save_trap(char *output, size_t capacity, const char *fallback) {
    if (!output || capacity == 0)
        return;
    const char *error = rt_trap_get_error();
    snprintf(output, capacity, "%s", error && error[0] ? error : fallback);
}

/// @brief Build a caller-owned `Result.ErrStr` from native diagnostic bytes.
/// @details String and Result construction run under a fresh recovery frame.
///          The temporary String reference is released after Result retains it,
///          and every partial value is released before an allocation trap is
///          re-raised. This prevents connect failures from leaking their error
///          strings or recursively jumping into an older recovery frame.
/// @param message NUL-terminated diagnostic, with a fixed fallback for NULL.
/// @return Caller-owned error Result, or NULL after a returning trap hook.
static void *rt_zanna_tls_error_result(const char *message) {
    rt_string volatile error_string = NULL;
    void *volatile result = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[512];
        rt_zanna_tls_save_trap(
            saved_error, sizeof(saved_error), "Tls: error Result allocation failed");
        rt_trap_clear_recovery();
        rt_zanna_tls_release_temp_object((void *)result);
        rt_zanna_tls_release_temp_object((void *)error_string);
        rt_trap(saved_error);
        return NULL;
    }

    const char *stable = message && message[0] ? message : "Tls.Connect failed";
    error_string = rt_string_from_bytes(stable, strlen(stable));
    if (!error_string) {
        rt_trap_clear_recovery();
        return NULL;
    }
    result = rt_result_err_str((rt_string)error_string);
    if (!result) {
        rt_trap_clear_recovery();
        rt_zanna_tls_release_temp_object((void *)error_string);
        return NULL;
    }
    rt_trap_clear_recovery();
    rt_zanna_tls_release_temp_object((void *)error_string);
    return (void *)result;
}

/// @brief Wrap and consume one caller-owned TLS wrapper in `Result.Ok`.
/// @details Result retains the wrapper payload. The connection's producer
///          reference is consumed after success and on every allocation-failure
///          path, which also closes its low-level TLS session through the
///          wrapper finalizer.
/// @param conn Caller-owned stable TLS wrapper.
/// @return Caller-owned success Result, or NULL after a returning trap hook.
static void *rt_zanna_tls_success_result_owned(void *conn) {
    void *volatile result = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[512];
        rt_zanna_tls_save_trap(
            saved_error, sizeof(saved_error), "Tls: success Result allocation failed");
        rt_trap_clear_recovery();
        rt_zanna_tls_release_temp_object((void *)result);
        rt_zanna_tls_release_temp_object(conn);
        rt_trap(saved_error);
        return NULL;
    }
    result = rt_result_ok(conn);
    if (!result) {
        rt_trap_clear_recovery();
        rt_zanna_tls_release_temp_object(conn);
        return NULL;
    }
    rt_trap_clear_recovery();
    rt_zanna_tls_release_temp_object(conn);
    return (void *)result;
}

/// @brief Convert a TLS connection handle into Result.Ok or Result.ErrStr.
/// @param conn Newly-created TLS object, or NULL on connection failure.
/// @param fallback Error text used when the TLS layer has no detailed message.
/// @return Caller-owned Zanna.Result object.
static void *rt_zanna_tls_connect_to_result(void *conn, const char *fallback) {
    if (!conn)
        return rt_zanna_tls_error_result(rt_zanna_tls_connect_error_message(fallback));
    return rt_zanna_tls_success_result_owned(conn);
}

/// @brief Validate and cast an opaque Zanna TLS receiver.
/// @details Stable class identity and minimum payload size are checked before
///          any native pointer field is read. Invalid, stale, and forged
///          receivers emit one trap and return NULL so returning trap hooks
///          cannot permit unsafe continuation.
/// @param obj Candidate managed TLS wrapper.
/// @return Valid wrapper payload, or NULL after trapping.
static rt_zanna_tls_t *rt_zanna_tls_require(void *obj) {
    if (!rt_obj_is_instance(obj, RT_TLS_CLASS_ID, sizeof(rt_zanna_tls_t))) {
        rt_trap("Tls: invalid object");
        return NULL;
    }
    return (rt_zanna_tls_t *)obj;
}

/// @brief Validate a public runtime String argument and expose exact bytes.
/// @details The stable String registry is queried before length or data fields
///          are read. Embedded NUL is rejected because the low-level TLS and OS
///          path/hostname interfaces consume C strings.
/// @param value Candidate String handle; NULL is treated as empty only when allowed.
/// @param out Receives the NUL-terminated byte pointer on success.
/// @param out_len Receives the exact stored byte count on success.
/// @param allow_empty Nonzero when NULL and empty Strings are valid.
/// @param max_len Exclusive maximum including room for a destination terminator.
/// @return 1 for a valid argument, otherwise 0.
static int rt_zanna_tls_string_arg(
    rt_string value, const char **out, size_t *out_len, int allow_empty, size_t max_len) {
    if (!out || !out_len)
        return 0;
    *out = "";
    *out_len = 0;
    if (!value)
        return allow_empty;
    if (!rt_string_is_handle(value))
        return 0;
    int64_t len64 = rt_str_len(value);
    if (len64 < 0 || (!allow_empty && len64 == 0) || (size_t)len64 >= max_len)
        return 0;
    const char *cstr = rt_string_cstr(value);
    if (!cstr)
        return 0;
    if (strlen(cstr) != (size_t)len64)
        return 0;
    *out = cstr;
    *out_len = (size_t)len64;
    return 1;
}

/// @brief Validate public TLS connect arguments before invoking the socket/TLS layer.
/// @param host Host string.
/// @param port TCP port.
/// @param timeout_ms Timeout in milliseconds.
/// @param ca_file Optional CA bundle path.
/// @param alpn Optional ALPN list.
/// @param error_out Receives a static error message on failure.
/// @return 1 when arguments are valid, 0 otherwise.
static int rt_zanna_tls_validate_connect_args(rt_string host,
                                              int64_t port,
                                              int64_t timeout_ms,
                                              rt_string ca_file,
                                              rt_string alpn,
                                              const char **error_out) {
    const char *host_cstr = NULL;
    const char *ca_cstr = NULL;
    const char *alpn_cstr = NULL;
    size_t host_len = 0;
    size_t ca_len = 0;
    size_t alpn_len = 0;
    if (port < 1 || port > 65535) {
        if (error_out)
            *error_out = "Tls.Connect: port must be in 1..65535";
        return 0;
    }
    if (timeout_ms > INT_MAX) {
        if (error_out)
            *error_out = "Tls.Connect: timeout is too large";
        return 0;
    }
    if (!rt_zanna_tls_string_arg(
            host, &host_cstr, &host_len, 0, sizeof(((rt_tls_session_t *)0)->hostname))) {
        if (error_out)
            *error_out = "Tls.Connect: invalid host";
        return 0;
    }
    if (!rt_zanna_tls_string_arg(
            ca_file, &ca_cstr, &ca_len, 1, sizeof(((rt_tls_session_t *)0)->ca_file))) {
        if (error_out)
            *error_out = "Tls.Connect: invalid CA file";
        return 0;
    }
    if (!rt_zanna_tls_string_arg(
            alpn, &alpn_cstr, &alpn_len, 1, sizeof(((rt_tls_session_t *)0)->alpn_protocols))) {
        if (error_out)
            *error_out = "Tls.Connect: invalid ALPN list";
        return 0;
    }
    (void)host_cstr;
    (void)host_len;
    (void)ca_cstr;
    (void)ca_len;
    (void)alpn_cstr;
    (void)alpn_len;
    return 1;
}

/// @brief Box one owned low-level session in a managed Zanna TLS wrapper.
/// @details The host copy is staged before managed allocation. A local recovery
///          frame closes the owned session and frees the copy if object
///          allocation traps, so callers never retain an unobservable socket.
/// @param session Caller-owned low-level session, consumed on every path.
/// @param host_cstr Validated host text copied into the wrapper.
/// @param port Connected TCP port.
/// @return Caller-owned wrapper, or NULL after cleanup on failure.
static void *rt_zanna_tls_object_from_session(rt_tls_session_t *session,
                                              const char *host_cstr,
                                              int64_t port) {
    if (!session)
        return NULL;
    char *host_copy = strdup(host_cstr ? host_cstr : "");
    if (!host_copy) {
        rt_tls_close(session);
        return NULL;
    }

    rt_zanna_tls_t *volatile tls = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[512];
        rt_zanna_tls_save_trap(saved_error, sizeof(saved_error), "Tls: wrapper allocation failed");
        rt_trap_clear_recovery();
        free(host_copy);
        rt_tls_close(session);
        rt_trap(saved_error);
        return NULL;
    }
    tls = (rt_zanna_tls_t *)rt_obj_new_i64(RT_TLS_CLASS_ID, (int64_t)sizeof(rt_zanna_tls_t));
    if (!tls) {
        rt_trap_clear_recovery();
        free(host_copy);
        rt_tls_close(session);
        return NULL;
    }
    rt_trap_clear_recovery();

    ((rt_zanna_tls_t *)tls)->session = session;
    ((rt_zanna_tls_t *)tls)->host = host_copy;
    ((rt_zanna_tls_t *)tls)->port = port;
    rt_obj_set_finalizer((void *)tls, rt_zanna_tls_finalize);

    return (void *)tls;
}

/// @brief Validate options, connect a low-level TLS session, and box it.
/// @param host Runtime hostname used for TCP, SNI, and identity.
/// @param port Destination port in 1..65535.
/// @param timeout_ms Timeout in milliseconds; negative normalizes to zero
///        and the low-level layer applies its default.
/// @param ca_file Optional CA bundle path.
/// @param alpn Optional comma-separated ALPN preference list.
/// @param verify_cert Nonzero to enforce chain and hostname policy.
/// @return Caller-owned managed TLS wrapper, or NULL on validation,
///         connection, handshake, or allocation failure.
static void *rt_zanna_tls_connect_impl(rt_string host,
                                       int64_t port,
                                       int64_t timeout_ms,
                                       rt_string ca_file,
                                       rt_string alpn,
                                       int verify_cert) {
    if (port < 1 || port > 65535)
        return NULL;
    if (timeout_ms > INT_MAX)
        return NULL;
    if (timeout_ms < 0)
        timeout_ms = 0;

    const char *host_cstr = NULL;
    const char *ca_cstr = NULL;
    const char *alpn_cstr = NULL;
    size_t host_len = 0;
    size_t ca_len = 0;
    size_t alpn_len = 0;
    if (!rt_zanna_tls_string_arg(
            host, &host_cstr, &host_len, 0, sizeof(((rt_tls_session_t *)0)->hostname)))
        return NULL;
    if (!rt_zanna_tls_string_arg(
            ca_file, &ca_cstr, &ca_len, 1, sizeof(((rt_tls_session_t *)0)->ca_file)))
        return NULL;
    if (!rt_zanna_tls_string_arg(
            alpn, &alpn_cstr, &alpn_len, 1, sizeof(((rt_tls_session_t *)0)->alpn_protocols)))
        return NULL;

    rt_tls_config_t config;
    rt_tls_config_init(&config);
    config.hostname = host_cstr;
    config.timeout_ms = (int)timeout_ms;
    config.verify_cert = verify_cert ? 1 : 0;
    if (ca_len > 0)
        config.ca_file = ca_cstr;
    if (alpn_len > 0)
        config.alpn_protocol = alpn_cstr;

    rt_tls_session_t *session = rt_tls_connect(host_cstr, (uint16_t)port, &config);
    if (!session)
        return NULL;
    return rt_zanna_tls_object_from_session(session, host_cstr, port);
}

/// @brief Connect to a TLS server.
/// @param host Hostname to connect to.
/// @param port Port number.
/// @return TLS object or NULL on error.
void *rt_zanna_tls_connect(rt_string host, int64_t port) {
    return rt_zanna_tls_connect_impl(host, port, 30000, NULL, NULL, 1);
}

/// @brief Connect to a TLS server and return the outcome as a Result.
/// @param host Hostname to connect to.
/// @param port Port number.
/// @return Owned `Zanna.Result` carrying the TLS handle or an error string.
void *rt_zanna_tls_connect_result(rt_string host, int64_t port) {
    const char *arg_error = NULL;
    if (!rt_zanna_tls_validate_connect_args(host, port, 30000, NULL, NULL, &arg_error))
        return rt_zanna_tls_error_result(arg_error);
    void *conn = rt_zanna_tls_connect_impl(host, port, 30000, NULL, NULL, 1);
    return rt_zanna_tls_connect_to_result(conn, "Tls.Connect failed");
}

/// @brief Connect to a TLS server with timeout.
/// @param host Hostname to connect to.
/// @param port Port number.
/// @param timeout_ms Timeout in milliseconds.
/// @return TLS object or NULL on error.
void *rt_zanna_tls_connect_for(rt_string host, int64_t port, int64_t timeout_ms) {
    return rt_zanna_tls_connect_impl(host, port, timeout_ms, NULL, NULL, 1);
}

/// @brief Connect to a TLS server with timeout and return a Result.
/// @param host Hostname to connect to.
/// @param port Port number.
/// @param timeout_ms Timeout in milliseconds.
/// @return Owned `Zanna.Result` carrying the TLS handle or an error string.
void *rt_zanna_tls_connect_for_result(rt_string host, int64_t port, int64_t timeout_ms) {
    const char *arg_error = NULL;
    if (!rt_zanna_tls_validate_connect_args(host, port, timeout_ms, NULL, NULL, &arg_error))
        return rt_zanna_tls_error_result(arg_error);
    void *conn = rt_zanna_tls_connect_impl(host, port, timeout_ms, NULL, NULL, 1);
    return rt_zanna_tls_connect_to_result(conn, "Tls.ConnectFor failed");
}

/// @brief Connect to @p host:@p port with explicit verification and ALPN policy.
/// @details Full-feature constructor: caller supplies a CA bundle path (or
///          empty for the system store), an ALPN preference list, the
///          verify-cert flag, and a timeout reused by each address attempt and
///          socket I/O operation. All four
///          options thread directly into @c rt_tls_connect.
/// @param host TLS hostname (also used for SNI and cert verification).
/// @param port TCP port.
/// @param ca_file Optional PEM bundle path; empty string falls back to the OS store.
/// @param alpn Optional comma-separated ALPN list (e.g. "h2,http/1.1").
/// @param verify_cert 0 to skip chain verification (development only); 1 (default) to enforce.
/// @param timeout_ms Per-attempt/per-I/O timeout in ms; nonpositive uses the 30 s default.
/// @return New TLS connection handle, or NULL on failure.
void *rt_zanna_tls_connect_options(rt_string host,
                                   int64_t port,
                                   rt_string ca_file,
                                   rt_string alpn,
                                   int8_t verify_cert,
                                   int64_t timeout_ms) {
    return rt_zanna_tls_connect_impl(host, port, timeout_ms, ca_file, alpn, verify_cert ? 1 : 0);
}

/// @brief Connect with explicit TLS options and return a Result.
/// @param host TLS hostname.
/// @param port TCP port.
/// @param ca_file Optional PEM bundle path.
/// @param alpn Optional comma-separated ALPN list.
/// @param verify_cert 1 to verify certificates, 0 to skip verification.
/// @param timeout_ms Per-attempt/per-I/O timeout in milliseconds.
/// @return Owned `Zanna.Result` carrying the TLS handle or an error string.
void *rt_zanna_tls_connect_options_result(rt_string host,
                                          int64_t port,
                                          rt_string ca_file,
                                          rt_string alpn,
                                          int8_t verify_cert,
                                          int64_t timeout_ms) {
    const char *arg_error = NULL;
    if (!rt_zanna_tls_validate_connect_args(host, port, timeout_ms, ca_file, alpn, &arg_error))
        return rt_zanna_tls_error_result(arg_error);
    void *conn =
        rt_zanna_tls_connect_impl(host, port, timeout_ms, ca_file, alpn, verify_cert ? 1 : 0);
    return rt_zanna_tls_connect_to_result(conn, "Tls.ConnectOptions failed");
}

/// @brief Copy the host stored on a managed TLS connection.
/// @param obj Managed TLS receiver; NULL yields an empty String.
/// @return Caller-owned host String.
rt_string rt_zanna_tls_host(void *obj) {
    if (!obj)
        return rt_string_from_bytes("", 0);
    rt_zanna_tls_t *tls = rt_zanna_tls_require(obj);
    if (!tls)
        return NULL;
    const char *h = tls->host ? tls->host : "";
    return rt_string_from_bytes(h, strlen(h));
}

/// @brief Read the destination port stored on a managed TLS connection.
/// @param obj Managed TLS receiver; NULL yields zero.
/// @return Destination port, or zero for NULL/invalid receivers.
int64_t rt_zanna_tls_port(void *obj) {
    if (!obj)
        return 0;
    rt_zanna_tls_t *tls = rt_zanna_tls_require(obj);
    if (!tls)
        return 0;
    return tls->port;
}

/// @brief Return the ALPN protocol negotiated on this TLS connection.
/// @details Returns an empty string when no ALPN extension was exchanged
///          or when the peer did not select any advertised protocol.
///          Useful for HTTPS callers that need to know whether the
///          connection is HTTP/2 vs HTTP/1.1.
/// @param obj Managed TLS receiver; NULL yields an empty String.
/// @return Caller-owned negotiated protocol String, possibly empty.
rt_string rt_zanna_tls_negotiated_alpn(void *obj) {
    if (!obj)
        return rt_string_from_bytes("", 0);
    rt_zanna_tls_t *tls = rt_zanna_tls_require(obj);
    if (!tls)
        return NULL;
    if (!tls->session)
        return rt_string_from_bytes("", 0);
    const char *alpn = rt_tls_get_negotiated_alpn(tls->session);
    return rt_string_from_bytes(alpn ? alpn : "", alpn ? strlen(alpn) : 0);
}

/// @brief Check whether the boxed low-level TLS session is connected.
/// @param obj Managed TLS receiver; NULL yields zero.
/// @return One only in @c TLS_STATE_CONNECTED; otherwise zero.
int8_t rt_zanna_tls_is_open(void *obj) {
    if (!obj)
        return 0;
    rt_zanna_tls_t *tls = rt_zanna_tls_require(obj);
    if (!tls)
        return 0;
    return tls->session && tls->session->state == TLS_STATE_CONNECTED;
}

/// @brief Send bytes over TLS connection.
/// @param obj TLS object.
/// @param data Bytes object to send.
/// @return Number of bytes sent, or -1 on error.
int64_t rt_zanna_tls_send(void *obj, void *data) {
    if (!obj || !data)
        return -1;

    rt_zanna_tls_t *tls = rt_zanna_tls_require(obj);
    if (!tls)
        return -1;
    if (!tls->session)
        return -1;

    if (!rt_bytes_is_bytes(data))
        return -1;
    int64_t len = rt_bytes_len(data);
    if (len < 0)
        return -1;
    if (len == 0)
        return 0;

    const uint8_t *buffer = rt_bytes_data_const(data);
    if (!buffer)
        return -1;
    long result = rt_tls_send(tls->session, buffer, (size_t)len);
    return (int64_t)result;
}

/// @brief Send a string's complete stored byte sequence over TLS.
/// @param obj TLS object.
/// @param text String to send.
/// @return Number of bytes sent, or -1 on error.
int64_t rt_zanna_tls_send_str(void *obj, rt_string text) {
    if (!obj || !text)
        return -1;
    if (!rt_string_is_handle(text))
        return -1;

    rt_zanna_tls_t *tls = rt_zanna_tls_require(obj);
    if (!tls)
        return -1;
    if (!tls->session)
        return -1;

    const char *cstr = rt_string_cstr(text);
    if (!cstr)
        return 0;
    int64_t len64 = rt_str_len(text);
    if (len64 <= 0)
        return 0;
    size_t len = (size_t)len64;

    long result = rt_tls_send(tls->session, cstr, len);
    return (int64_t)result;
}

/// @brief Receive bytes from TLS connection.
/// @param obj TLS object.
/// @param max_bytes Maximum bytes to receive.
/// @return Bytes object with received data, or NULL on error.
void *rt_zanna_tls_recv(void *obj, int64_t max_bytes) {
    if (!obj || max_bytes <= 0)
        return NULL;
    if (max_bytes > TLS_MAX_RECORD_SIZE)
        max_bytes = TLS_MAX_RECORD_SIZE;

    rt_zanna_tls_t *tls = rt_zanna_tls_require(obj);
    if (!tls)
        return NULL;
    if (!tls->session)
        return NULL;

    size_t buf_size = (size_t)max_bytes;
    uint8_t buffer[TLS_MAX_RECORD_SIZE];

    long received = rt_tls_recv(tls->session, buffer, buf_size);
    if (received <= 0)
        return received == 0 ? rt_bytes_new(0) : NULL;

    return rt_bytes_from_raw(buffer, (size_t)received);
}

/// @brief Receive bytes into a length-aware string without UTF-8 validation.
/// @param obj TLS object.
/// @param max_bytes Maximum bytes to receive.
/// @return String with received data, or empty string on error.
rt_string rt_zanna_tls_recv_str(void *obj, int64_t max_bytes) {
    if (!obj || max_bytes <= 0)
        return rt_string_from_bytes("", 0);
    if (max_bytes > TLS_MAX_RECORD_SIZE)
        max_bytes = TLS_MAX_RECORD_SIZE;

    rt_zanna_tls_t *tls = rt_zanna_tls_require(obj);
    if (!tls)
        return NULL;
    if (!tls->session)
        return rt_string_from_bytes("", 0);

    size_t buf_size = (size_t)max_bytes;
    char buffer[TLS_MAX_RECORD_SIZE];

    long received = rt_tls_recv(tls->session, buffer, buf_size);
    if (received <= 0)
        return rt_string_from_bytes("", 0);

    return rt_string_from_bytes(buffer, (size_t)received);
}

/// @brief Convert an owned native line buffer to a managed String and free it.
/// @details A local recovery frame ensures the native allocation is freed if
///          managed String construction traps. The saved diagnostic is
///          re-raised only after the frame has been removed, preventing a jump
///          back into this cleanup scope.
/// @param line Heap buffer owned by this helper.
/// @param len Exact byte count to preserve, including any embedded NUL bytes.
/// @return Caller-owned String, or NULL after a returning trap hook.
static rt_string rt_zanna_tls_string_from_owned_line(char *line, size_t len) {
    rt_string volatile result = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[512];
        rt_zanna_tls_save_trap(saved_error, sizeof(saved_error), "Tls.RecvLine allocation failed");
        rt_trap_clear_recovery();
        free(line);
        rt_trap(saved_error);
        return NULL;
    }
    result = rt_string_from_bytes(line, len);
    rt_trap_clear_recovery();
    free(line);
    return (rt_string)result;
}

/// @brief Read through LF, strip a preceding CR, and return the completed line.
/// @details Returns empty when EOF/error occurs before LF or a nonterminated line
///          grows beyond 64 KiB; partial bytes are discarded.
/// @param obj Managed TLS receiver; NULL yields an empty String.
/// @return Caller-owned line without CRLF, or an empty String on EOF, error, or
///         limit failure.
rt_string rt_zanna_tls_recv_line(void *obj) {
    if (!obj)
        return rt_string_from_bytes("", 0);

    rt_zanna_tls_t *tls = rt_zanna_tls_require(obj);
    if (!tls)
        return NULL;
    if (!tls->session)
        return rt_string_from_bytes("", 0);

    size_t cap = 256;
    size_t len = 0;
    int saw_newline = 0;
    char *line = (char *)malloc(cap);
    if (!line)
        return rt_string_from_bytes("", 0);

    while (1) {
        char c;
        long received = rt_tls_recv(tls->session, &c, 1);
        if (received <= 0) {
            break;
        }

        if (c == '\n') {
            // Strip trailing CR if present
            if (len > 0 && line[len - 1] == '\r')
                len--;
            saw_newline = 1;
            break;
        }

        // Cap at 64KB to prevent unbounded memory growth from a malicious peer
        // (matches the limit in rt_tcp_recv_line).
        if (len >= 65536) {
            free(line);
            return rt_string_from_bytes("", 0);
        }

        if (len >= cap) {
            cap *= 2;
            if (cap > 65536)
                cap = 65536;
            char *new_line = (char *)realloc(line, cap);
            if (!new_line) {
                free(line);
                return rt_string_from_bytes("", 0);
            }
            line = new_line;
        }
        line[len++] = c;
    }

    if (!saw_newline) {
        free(line);
        return rt_string_from_bytes("", 0);
    }

    return rt_zanna_tls_string_from_owned_line(line, len);
}

/// @brief Send close_notify, perform the bounded peer-alert drain, and close.
/// @param obj Managed TLS receiver; NULL is a no-op.
void rt_zanna_tls_close(void *obj) {
    if (!obj)
        return;

    rt_zanna_tls_t *tls = rt_zanna_tls_require(obj);
    if (!tls)
        return;
    if (tls->session) {
        rt_tls_close(tls->session);
        tls->session = NULL;
    }
}

/// @brief Copy the latest diagnostic from the boxed TLS session.
/// @param obj Managed TLS receiver; NULL reports `null object`.
/// @return Caller-owned diagnostic String.
rt_string rt_zanna_tls_error(void *obj) {
    const char *msg;
    if (!obj) {
        msg = "null object";
        return rt_string_from_bytes(msg, strlen(msg));
    }

    rt_zanna_tls_t *tls = rt_zanna_tls_require(obj);
    if (!tls) {
        return NULL;
    }
    if (!tls->session) {
        msg = "connection closed";
        return rt_string_from_bytes(msg, strlen(msg));
    }

    const char *err = rt_tls_get_error(tls->session);
    msg = err ? err : "no error";
    return rt_string_from_bytes(msg, strlen(msg));
}
