//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/network/rt_tls_internal.h
// Purpose: Internal shared TLS definitions for the network module. Exposes the
//          TLS session struct and certificate verification functions.
// Key invariants:
//   - This header is internal to the network module; not part of the public API.
//   - The rt_tls_session struct layout must match across both translation units.
//   - Optional owner cancellation probes are observed only by bounded socket I/O.
// Ownership/Lifetime:
//   - Session objects are owned by callers of the public API (rt_tls.h).
// Links: rt_tls.c (core TLS), rt_tls_verify.c (certificate validation),
//        rt_smtp.c (cancellation-aware TLS owner)
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_tls_internal.h
 * @brief Defines private TLS protocol state shared across runtime modules.
 * @details The header fixes the managed session layout, handshake states,
 *          record traffic keys, bounded buffers, parsed peer-certificate
 *          storage, cancellation hooks, and internal DER or credential helper
 *          declarations used by the TLS engine and verification adapters.
 */

#pragma once

#include "rt_crypto.h"
#include "rt_rsa.h"
#include "rt_socket_platform.h"
#include "rt_tls.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct rt_tls_server_ctx;

// Max sizes
#define TLS_MAX_RECORD_SIZE 16384
#define TLS_MAX_CIPHERTEXT (TLS_MAX_RECORD_SIZE + 256)
#define TLS_INLINE_SERVER_CERT_DER_MAX 16384

/// @brief TLS 1.3 client-side handshake state machine positions.
/// @details RFC 8446 §A.1 client state diagram. The session advances
///          through each value monotonically; ERROR and CLOSED are
///          sinks that no transition can leave.
typedef enum {
    TLS_STATE_INITIAL,                   ///< No bytes on wire yet.
    TLS_STATE_CLIENT_HELLO_SENT,         ///< ClientHello flushed; waiting for response.
    TLS_STATE_SERVER_HELLO_RECEIVED,     ///< Cipher suite + key_share negotiated.
    TLS_STATE_WAIT_ENCRYPTED_EXTENSIONS, ///< Server-handshake keys derived; awaiting EE.
    TLS_STATE_WAIT_CERTIFICATE,          ///< Awaiting peer Certificate message.
    TLS_STATE_WAIT_CERTIFICATE_VERIFY,   ///< Certificate seen; awaiting CertificateVerify.
    TLS_STATE_WAIT_FINISHED,             ///< CertificateVerify validated; awaiting Finished.
    TLS_STATE_CONNECTED,                 ///< Application traffic keys live, ready for I/O.
    TLS_STATE_CLOSED,                    ///< Peer or local close_notify processed.
    TLS_STATE_ERROR                      ///< Unrecoverable handshake / protocol failure.
} tls_state_t;

/// @brief One direction's record-layer traffic keys + monotonic sequence number.
/// @details The AEAD nonce per record is @c iv XOR @c seq_num (big-endian
///          padded). Sequence counters never wrap — sender bails out
///          before key reuse becomes possible.
typedef struct {
    uint8_t key[32];  ///< Application traffic key (AES-128/256-GCM or ChaCha20-Poly1305).
    uint8_t iv[12];   ///< Per-direction static IV xored with @c seq_num to form the nonce.
    uint64_t seq_num; ///< Records sent or received in this direction since key install.
} traffic_keys_t;

// TLS session structure
struct rt_tls_session {
    // Socket handle owned by the TLS session.
    socket_t socket_fd;
    tls_state_t state;
    const char *error;
    int is_server;

    // Configuration
    char hostname[256];
    char alpn_protocols[128];
    char negotiated_alpn[64];
    char ca_file[512];
    int verify_cert;
    int timeout_ms;
    uint8_t legacy_session_id[32];
    size_t legacy_session_id_len;
    const struct rt_tls_server_ctx *server_ctx;
    /// @brief Optional cooperative-cancellation probe for bounded server I/O.
    /// @param context Opaque state stored in @ref io_cancel_context.
    /// @return Nonzero when the current server operation should stop.
    int (*io_cancel_requested)(void *context);
    void *io_cancel_context;
    int64_t io_deadline_us;

    // Handshake state
    uint8_t client_private_key[32];
    uint8_t client_public_key[32];
    uint8_t server_public_key[32];
    uint8_t client_random[32];
    uint8_t server_random[32];
    uint16_t cipher_suite;
    uint16_t server_sig_scheme;

    // Key schedule
    uint8_t handshake_secret[32];
    uint8_t client_handshake_traffic_secret[32];
    uint8_t server_handshake_traffic_secret[32];
    uint8_t master_secret[32];
    uint8_t client_application_traffic_secret[32];
    uint8_t server_application_traffic_secret[32];

    // Transcript hash
    uint8_t transcript_hash[32];
    rt_sha256_ctx transcript_ctx;
    size_t transcript_len;
    uint8_t client_hello_hash[32];
    int have_client_hello_hash;
    uint8_t *hello_retry_cookie;
    size_t hello_retry_cookie_len;
    int hello_retry_seen;

    // Record layer
    traffic_keys_t write_keys;
    traffic_keys_t read_keys;
    int keys_established;

    // Read buffer
    uint8_t read_buffer[TLS_MAX_CIPHERTEXT];
    size_t read_buffer_len;
    size_t read_buffer_pos;

    // Decrypted application data buffer
    uint8_t app_buffer[TLS_MAX_RECORD_SIZE];
    size_t app_buffer_len;
    size_t app_buffer_pos;

    // Certificate validation state (CS-1/CS-2/CS-3)
    uint8_t server_cert_der[TLS_INLINE_SERVER_CERT_DER_MAX]; // Inline end-entity DER bytes
    uint8_t *server_cert_der_heap; // Heap end-entity DER bytes when the leaf exceeds inline storage
    size_t server_cert_der_len;    // Bytes stored in the active inline/heap certificate buffer
    uint8_t *server_cert_list;     // Raw TLS certificate_list entries (leaf + intermediates)
    size_t server_cert_list_len;   // Bytes stored in server_cert_list
    size_t server_cert_count;      // Number of parsed certificate entries
    uint8_t cert_transcript_hash[32]; // Transcript hash saved AFTER Certificate (for CS-3)
};

/// @brief Return the active end-entity certificate DER buffer for a TLS session.
/// @details Small certificates live in @ref rt_tls_session::server_cert_der for compatibility with
///          existing internal tests and stack-allocated session helpers. Large certificates are
///          stored in @ref rt_tls_session::server_cert_der_heap. Callers should always use this
///          accessor when reading the parsed leaf certificate.
/// @param session TLS session whose certificate state is being inspected.
/// @return Pointer to the active DER bytes, or NULL when no certificate has been stored.
static inline const uint8_t *tls_session_server_cert_der(const rt_tls_session_t *session) {
    if (!session || session->server_cert_der_len == 0)
        return NULL;
    return session->server_cert_der_heap ? session->server_cert_der_heap : session->server_cert_der;
}

/// @brief Parse the TLS 1.3 Certificate handshake message and store the
///        first (end-entity) certificate DER bytes in the session certificate buffer.
/// @param session Client session receiving owned leaf and chain storage.
/// @param data Certificate handshake body.
/// @param len Number of body bytes.
/// @return @ref RT_TLS_OK or a negative handshake/memory status.
int tls_parse_certificate_msg(rt_tls_session_t *session, const uint8_t *data, size_t len);

/// @brief Verify that session->hostname matches the certificate DER.
/// @param session Client session with hostname and parsed leaf certificate.
/// @return @ref RT_TLS_OK on match; otherwise a negative handshake status.
int tls_verify_hostname(rt_tls_session_t *session);

/// @brief Verify certificate chain against the OS trust store.
/// @param session Client session with parsed peer certificate chain.
/// @return @ref RT_TLS_OK for a trusted valid chain; otherwise a negative
///         certificate status.
int tls_verify_chain(rt_tls_session_t *session);

/// @brief Verify the CertificateVerify handshake message signature.
/// @param session Client session with leaf key and saved transcript hash.
/// @param data CertificateVerify body.
/// @param len Number of body bytes.
/// @return @ref RT_TLS_OK for a valid proof; otherwise a negative handshake
///         status.
int tls_verify_cert_verify(rt_tls_session_t *session, const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

/// @brief Decode one definite-length DER TLV header.
/// @param buf DER bytes beginning at a TLV.
/// @param buf_len Available bytes.
/// @param tag Destination tag octet.
/// @param val_len Destination content length.
/// @param hdr_len Destination header length.
/// @return Zero on success; -1 for malformed or truncated DER.
int tls_der_read_tlv(
    const uint8_t *buf, size_t buf_len, uint8_t *tag, size_t *val_len, size_t *hdr_len);
/// @brief Compare DER OID value bytes.
/// @param buf Candidate OID value.
/// @param buf_len Candidate byte count.
/// @param oid Expected OID value.
/// @param oid_len Expected byte count.
/// @return One for exact equality; otherwise zero.
int tls_oid_matches(const uint8_t *buf, size_t buf_len, const uint8_t *oid, size_t oid_len);
/// @brief Read a bounded UTF-8-path text file.
/// @param path Nonempty UTF-8 filesystem path.
/// @param len_out Optional destination for bytes excluding NUL.
/// @return Caller-owned NUL-terminated buffer, or NULL on failure.
char *tls_read_text_file(const char *path, size_t *len_out);
/// @brief Detect the supported public-key type in a leaf certificate.
/// @param cert_der Complete X.509 certificate DER.
/// @param cert_len Number of certificate bytes.
/// @return One of the @c TLS_SERVER_KEY_* values.
int tls_extract_cert_key_type(const uint8_t *cert_der, size_t cert_len);

/// @brief Decode PEM Base64 body text into DER.
/// @param pem_b64 Base64 text bytes.
/// @param b64_len Number of input bytes.
/// @param out_der Destination DER buffer.
/// @param max_der Destination capacity.
/// @return Decoded byte count, or zero for invalid input/overflow.
size_t tls_pem_base64_decode(const char *pem_b64, size_t b64_len, uint8_t *out_der, size_t max_der);
/// @brief Locate one delimited PEM block.
/// @param pem NUL-terminated PEM document suffix.
/// @param begin_marker Begin delimiter.
/// @param end_marker End delimiter.
/// @param body_out Destination borrowed body pointer.
/// @param body_len_out Destination body length.
/// @param next_out Optional destination for the remaining suffix.
/// @return One when a complete block is found; otherwise zero.
int tls_find_pem_block(const char *pem,
                       const char *begin_marker,
                       const char *end_marker,
                       const char **body_out,
                       size_t *body_len_out,
                       const char **next_out);
/// @brief Extract a P-256 scalar from SEC 1 ECPrivateKey DER.
/// @param der Complete SEC 1 DER bytes.
/// @param der_len Number of DER bytes.
/// @param out_priv Destination 32-byte scalar.
/// @return One on success; otherwise zero.
int tls_parse_sec1_ec_private_key(const uint8_t *der, size_t der_len, uint8_t out_priv[32]);
/// @brief Extract a P-256 scalar from PKCS#8 PrivateKeyInfo DER.
/// @param der Complete PKCS#8 DER bytes.
/// @param der_len Number of DER bytes.
/// @param out_priv Destination 32-byte scalar.
/// @return One on success; otherwise zero.
int tls_parse_pkcs8_ec_private_key(const uint8_t *der, size_t der_len, uint8_t out_priv[32]);
/// @brief Extract an uncompressed P-256 public point from a certificate.
/// @param cert_der Complete certificate DER.
/// @param cert_len Number of certificate bytes.
/// @param x_out Destination 32-byte x coordinate.
/// @param y_out Destination 32-byte y coordinate.
/// @return One on success; otherwise zero.
int tls_extract_cert_ec_pubkey(const uint8_t *cert_der,
                               size_t cert_len,
                               uint8_t x_out[32],
                               uint8_t y_out[32]);
/// @brief Extract RSA public components from a certificate.
/// @param cert_der Complete certificate DER.
/// @param cert_len Number of certificate bytes.
/// @param out Initialized RSA key receiving owned public components.
/// @return One on success; otherwise zero.
int tls_extract_cert_rsa_pubkey(const uint8_t *cert_der, size_t cert_len, rt_rsa_key_t *out);

// Server key/signature type (shared: cert detection in rt_tls_certs.c sets it,
// the handshake in rt_tls.c selects the signature algorithm from it).
enum {
    TLS_SERVER_KEY_NONE = 0,
    TLS_SERVER_KEY_ECDSA_P256 = 1,
    TLS_SERVER_KEY_RSA_PSS_SHA256 = 2,
};
