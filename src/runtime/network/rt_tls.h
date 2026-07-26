//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/network/rt_tls.h
// Purpose: TLS 1.3 transport for secure networking using AES-128-GCM-SHA256
// and ChaCha20-Poly1305-SHA256 AEAD with X25519 key exchange, implemented in
// pure C without external TLS libraries.
//
// Key invariants:
//   - Implements TLS 1.3 handshake with X25519 key exchange and X25519 HelloRetryRequest retry.
//   - Supports AES-128-GCM-SHA256 (0x1301) and ChaCha20-Poly1305-SHA256 (0x1303).
//   - Certificate verification uses the configured/system trust source, the
//     server-supplied chain, and hostname/IP SAN verification.
//   - `verify_cert=0` skips trust-chain and hostname policy only; TLS 1.3
//     CertificateVerify proof-of-key-possession is always checked.
//   - Session record buffers, keys, and sequence state are mutable and unprotected;
//     callers must serialize operations on one session.
//   - The public low-level API remains client-oriented; server-side TLS is consumed
//     internally by `Zanna.Network.HttpsServer` / `Zanna.Network.WssServer`.
//
// Ownership/Lifetime:
//   - Low-level sessions are managed objects. rt_tls_new transfers socket
//     ownership only on success; rt_tls_close consumes one session reference.
//   - The session finalizer closes and scrubs abandoned connections without
//     network I/O. Callers provide send/receive buffers and retain ownership.
//
// Links: src/runtime/network/rt_tls.c (implementation), src/runtime/network/rt_crypto.h,
// src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_tls.h
 * @brief Declares the low-level managed TLS 1.3 transport API.
 * @details The interface configures verified client connections, exposes
 *          handshake and record I/O, reports negotiated protocol and peer
 *          identity, and closes session-owned sockets. Buffers remain owned by
 *          callers, while successful session construction transfers transport
 *          ownership to the returned managed handle.
 */

#pragma once

#include "rt_string.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Stable class identity for language-visible `Zanna.Crypto.Tls` wrappers.
#define RT_TLS_CLASS_ID INT64_C(-0x720101)

/// @brief Stable class identity for low-level TLS protocol sessions.
/// @details Low-level sessions are managed objects even though the C API uses
///          an opaque typed pointer. Protocol entry points validate this ID and
///          the complete session payload before accessing socket or key state.
#define RT_TLS_SESSION_CLASS_ID INT64_C(-0x720213)

/// @brief TLS status / error codes returned by low-level entry points.
typedef enum {
    RT_TLS_OK = 0,                 ///< Operation completed successfully.
    RT_TLS_ERROR = -1,             ///< Generic / unclassified TLS error.
    RT_TLS_ERROR_SOCKET = -2,      ///< Underlying socket read/write or close failed.
    RT_TLS_ERROR_HANDSHAKE = -3,   ///< TLS 1.3 handshake violated the protocol.
    RT_TLS_ERROR_CERTIFICATE = -4, ///< Peer certificate failed chain or name verification.
    RT_TLS_ERROR_CLOSED = -5,      ///< Peer issued a clean close_notify.
    RT_TLS_ERROR_TIMEOUT = -6,     ///< A configured per-operation timeout expired.
    RT_TLS_ERROR_MEMORY = -7,      ///< Allocation failed inside the TLS state machine.
    RT_TLS_ERROR_INVALID_ARG = -8, ///< Caller supplied NULL or out-of-range arguments.
} rt_tls_status_t;

/// @brief Opaque TLS session handle.
typedef struct rt_tls_session rt_tls_session_t;

/// @brief TLS configuration.
typedef struct rt_tls_config {
    const char *hostname; ///< Server hostname for cert verification and DNS-name SNI.
    const char
        *alpn_protocol;  ///< Optional comma-separated ALPN preference list (e.g. "h2,http/1.1").
    const char *ca_file; ///< Optional PEM bundle override for trust anchors.
    int verify_cert;     ///< 1 = verify trust/name policy; 0 = skip those checks.
    int timeout_ms;      ///< Per-address/per-I/O timeout in ms (nonpositive = default 30 s).
} rt_tls_config_t;

/// @brief Initialize default TLS configuration.
/// @details NULL is accepted as a no-op. Defaults enable certificate
///          verification and use a 30-second per-operation timeout.
/// @param config Configuration storage to initialize, or NULL.
void rt_tls_config_init(rt_tls_config_t *config);

/// @brief Create TLS session over existing socket.
/// @details The pointer-width descriptor preserves Windows `SOCKET` values.
///          On success the session owns the descriptor and consumes one
///          producer reference when closed. On failure the caller still owns
///          and must close the descriptor.
/// @param socket_fd Connected native TCP socket represented without narrowing.
/// @param config TLS configuration (NULL for defaults).
/// @return TLS session or NULL on error.
rt_tls_session_t *rt_tls_new(intptr_t socket_fd, const rt_tls_config_t *config);

/// @brief Perform the client-side TLS 1.3 handshake.
/// @details The candidate handle is validated before protocol state is read.
///          Call this exactly once on a newly created client session; server
///          sessions complete their handshake inside the internal accept API.
/// @param session Stable low-level session returned by @ref rt_tls_new.
/// @return `RT_TLS_OK` on success, `RT_TLS_ERROR_INVALID_ARG` for an invalid
///         handle, or another negative status for protocol/transport failure.
int rt_tls_handshake(rt_tls_session_t *session);

/// @brief Send an exact byte span over a connected TLS session.
/// @details A zero length is a state-independent no-op. Positive lengths
///          require non-NULL data and are split into legal TLS record sizes.
/// @param session Stable low-level TLS session.
/// @param data Caller-owned bytes, or NULL only when @p len is zero.
/// @param len Byte count to send.
/// @return Bytes sent on success, or a negative @ref rt_tls_status_t value.
long rt_tls_send(rt_tls_session_t *session, const void *data, size_t len);

/// @brief Receive decrypted application data into caller-owned storage.
/// @details A zero length is a state-independent no-op. Buffered bytes from a
///          prior record are consumed before the transport is read.
/// @param session Stable low-level TLS session.
/// @param buffer Writable destination, or NULL only when @p len is zero.
/// @param len Maximum byte count to copy.
/// @return Bytes received, zero on clean EOF/empty request, or a negative status.
long rt_tls_recv(rt_tls_session_t *session, void *buffer, size_t len);

/// @brief Replace the per-operation TLS socket/readiness timeout.
/// @details Both native receive and send timeout options are updated before
///          the session publishes the new value. If either adapter operation
///          fails, the prior timeout is restored best-effort and the session
///          retains its previous logical value. Sessions are not internally
///          synchronized; callers must serialize this with send/receive.
/// @param session Stable low-level TLS session.
/// @param timeout_ms Positive timeout in milliseconds.
/// @return `RT_TLS_OK`, `RT_TLS_ERROR_INVALID_ARG`, or `RT_TLS_ERROR_SOCKET`.
int rt_tls_set_io_timeout(rt_tls_session_t *session, int timeout_ms);

/// @brief Close a TLS session and consume the caller's session reference.
/// @details Sends a bounded `close_notify` exchange when connected, securely
///          wipes secrets, closes the native socket, and releases one managed
///          reference. NULL and invalid/stale handles are safe no-ops.
/// @param session Session reference to consume; NULL, invalid, and stale handles are ignored.
void rt_tls_close(rt_tls_session_t *session);

/// @brief Get the last diagnostic stored on a low-level session.
/// @param session Candidate session handle.
/// @return Stable native text, including explicit NULL/invalid-session diagnostics.
const char *rt_tls_get_error(rt_tls_session_t *session);

/// @brief Get the most recent connect/handshake diagnostic on this thread.
/// @return Thread-local native text valid until the next connect/session setup operation.
const char *rt_tls_last_error(void);

/// @brief Convenience: connect, handshake, return session.
/// @details Uses one monotonic deadline for name resolution, every address
///          attempt, and the handshake. Socket mode/status/timeout adapter
///          operations must all succeed before the session is published.
/// @param host Hostname to connect to.
/// @param port Port number.
/// @param config TLS configuration (NULL for defaults).
/// @return Connected TLS session or NULL on error.
rt_tls_session_t *rt_tls_connect(const char *host, uint16_t port, const rt_tls_config_t *config);

/// @brief Get the underlying pointer-width native socket descriptor.
/// @param session Valid low-level TLS session.
/// @return Native descriptor, or -1 for NULL, invalid, or closed sessions.
intptr_t rt_tls_get_socket(rt_tls_session_t *session);

/// @brief Get the negotiated ALPN protocol.
/// @param session Candidate session handle.
/// @return Session-owned NUL-terminated protocol, or an empty static string for
///         no selection or an invalid handle.
const char *rt_tls_get_negotiated_alpn(rt_tls_session_t *session);

/// @brief Check if the TLS session has buffered application data available.
/// @param session Candidate session handle.
/// @return 1 if buffered data exists, 0 for none or an invalid handle.
int rt_tls_has_buffered_data(rt_tls_session_t *session);

//=========================================================================
// Zanna API wrappers (Zanna.Crypto.Tls)
//=========================================================================

/// @brief Connect to a host with verified TLS and the default timeout.
/// @param host Runtime String used for TCP, SNI, and certificate identity.
/// @param port Destination port in 1..65535.
/// @return Caller-owned managed Tls wrapper, or NULL after a returning trap.
void *rt_zanna_tls_connect(rt_string host, int64_t port);

/// @brief Connect to host:port with TLS and return a Zanna.Result.
/// @details Returns `Ok(Tls)` on success and `Err(message)` for invalid
///          arguments, TCP setup failure, timeout, certificate verification
///          failure, or TLS handshake failure.
/// @param host Hostname to connect to.
/// @param port TCP port number.
/// @return Opaque Zanna.Result object containing a TLS handle or error string.
void *rt_zanna_tls_connect_result(rt_string host, int64_t port);

/// @brief Connect to a host with verified TLS and a custom timeout.
/// @param host Runtime String used for TCP, SNI, and certificate identity.
/// @param port Destination port in 1..65535.
/// @param timeout_ms Per-address/per-I/O timeout; nonpositive selects the
///        runtime default.
/// @return Caller-owned managed Tls wrapper, or NULL after a returning trap.
void *rt_zanna_tls_connect_for(rt_string host, int64_t port, int64_t timeout_ms);

/// @brief Connect to host:port with TLS and a custom timeout as a Zanna.Result.
/// @details Returns `Ok(Tls)` on success and `Err(message)` for routine
///          connection or handshake failures.
/// @param host Hostname to connect to.
/// @param port TCP port number.
/// @param timeout_ms Per-address/per-I/O timeout; nonpositive uses the runtime default.
/// @return Opaque Zanna.Result object containing a TLS handle or error string.
void *rt_zanna_tls_connect_for_result(rt_string host, int64_t port, int64_t timeout_ms);

/// @brief Connect with explicit trust, ALPN, verification, and timeout options.
/// @param host TLS host used for TCP, SNI, and certificate identity.
/// @param port Destination port in 1..65535.
/// @param ca_file Optional CA bundle path; NULL or empty uses system trust.
/// @param alpn Optional comma-separated protocol preference list.
/// @param verify_cert Nonzero to enforce chain and hostname verification.
/// @param timeout_ms Per-address/per-I/O timeout; nonpositive selects default.
/// @return Caller-owned managed Tls wrapper, or NULL after a returning trap.
void *rt_zanna_tls_connect_options(rt_string host,
                                   int64_t port,
                                   rt_string ca_file,
                                   rt_string alpn,
                                   int8_t verify_cert,
                                   int64_t timeout_ms);

/// @brief Connect with explicit TLS options and return a Zanna.Result.
/// @details Returns `Ok(Tls)` on success and `Err(message)` for invalid
///          options, TCP setup failure, timeout, certificate verification
///          failure, or TLS handshake failure.
/// @param host TLS hostname used for TCP, SNI, and certificate verification.
/// @param port TCP port number.
/// @param ca_file Optional PEM bundle path, or empty for the system trust source.
/// @param alpn Optional comma-separated ALPN preference list.
/// @param verify_cert 1 to verify trust/name policy, 0 to skip those checks.
/// @param timeout_ms Per-address/per-I/O timeout; nonpositive uses the runtime default.
/// @return Opaque Zanna.Result object containing a TLS handle or error string.
void *rt_zanna_tls_connect_options_result(rt_string host,
                                          int64_t port,
                                          rt_string ca_file,
                                          rt_string alpn,
                                          int8_t verify_cert,
                                          int64_t timeout_ms);

/// @brief Copy the hostname configured on a managed Tls wrapper.
/// @param obj Valid managed Tls receiver.
/// @return Caller-owned hostname String, or an empty String when unavailable.
rt_string rt_zanna_tls_host(void *obj);

/// @brief Read the destination port of a managed Tls wrapper.
/// @param obj Valid managed Tls receiver.
/// @return Destination port, or zero when unavailable.
int64_t rt_zanna_tls_port(void *obj);

/// @brief Get negotiated ALPN protocol, or empty string when none was selected.
/// @param obj Valid managed Tls receiver.
/// @return Caller-owned protocol String, possibly empty.
rt_string rt_zanna_tls_negotiated_alpn(void *obj);

/// @brief Check whether a managed TLS connection remains locally open.
/// @param obj Valid managed Tls receiver.
/// @return One while open; otherwise zero.
int8_t rt_zanna_tls_is_open(void *obj);

/// @brief Send an exact managed Bytes payload over TLS.
/// @param obj Valid open managed Tls receiver.
/// @param data Managed Bytes payload.
/// @return Number of plaintext bytes accepted, or a negative TLS status.
int64_t rt_zanna_tls_send(void *obj, void *data);

/// @brief Send the exact bytes of a runtime String over TLS.
/// @param obj Valid open managed Tls receiver.
/// @param text Runtime String payload.
/// @return Number of plaintext bytes accepted, or a negative TLS status.
int64_t rt_zanna_tls_send_str(void *obj, rt_string text);

/// @brief Receive decrypted data into a new managed Bytes object.
/// @param obj Valid open managed Tls receiver.
/// @param max_bytes Positive receive bound.
/// @return Caller-owned Bytes containing up to @p max_bytes, or NULL after a
///         returning trap.
void *rt_zanna_tls_recv(void *obj, int64_t max_bytes);

/// @brief Receive decrypted data into a new runtime String.
/// @param obj Valid open managed Tls receiver.
/// @param max_bytes Positive receive bound.
/// @return Caller-owned String containing up to @p max_bytes.
rt_string rt_zanna_tls_recv_str(void *obj, int64_t max_bytes);

/// @brief Read one decrypted line through and including newline.
/// @param obj Valid open managed Tls receiver.
/// @return Caller-owned line String, or the final partial/empty line at EOF.
rt_string rt_zanna_tls_recv_line(void *obj);

/// @brief Close the managed TLS connection idempotently.
/// @param obj Managed Tls receiver; NULL is a no-op.
void rt_zanna_tls_close(void *obj);

/// @brief Copy the low-level session's latest diagnostic.
/// @param obj Managed Tls receiver.
/// @return Caller-owned diagnostic String.
rt_string rt_zanna_tls_error(void *obj);

//=========================================================================
// Internal functions exposed for unit testing (CS-1/CS-2/CS-3)
//=========================================================================

/// @brief Match a hostname pattern against a target hostname (RFC 6125).
/// @param pattern Certificate DNS pattern, optionally with a leftmost wildcard.
/// @param hostname Target DNS hostname.
/// @return 1 if match, 0 otherwise.
int tls_match_hostname(const char *pattern, const char *hostname);

/// @brief Extract SubjectAltName DNS names from a certificate DER.
/// @param der Complete certificate DER bytes.
/// @param der_len Number of bytes in @p der.
/// @param san_out Caller array receiving NUL-terminated DNS names.
/// @param max_names Capacity of @p san_out.
/// @return Number of names found.
int tls_extract_san_names(const uint8_t *der, size_t der_len, char san_out[][256], int max_names);

/// @brief Return whether a certificate contains a SubjectAltName extension.
/// @param der Complete certificate DER bytes.
/// @param der_len Number of bytes in @p der.
/// @return One when the extension is present; otherwise zero.
int tls_cert_has_san_extension(const uint8_t *der, size_t der_len);

/// @brief Extract CommonName from a certificate DER Subject.
/// @param der Complete certificate DER bytes.
/// @param der_len Number of bytes in @p der.
/// @param cn_out Destination 256-byte NUL-terminated name buffer.
/// @return 1 if found, 0 otherwise.
int tls_extract_cn(const uint8_t *der, size_t der_len, char cn_out[256]);

#ifdef __cplusplus
}
#endif
