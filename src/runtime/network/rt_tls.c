//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/network/rt_tls.c
// Purpose: TLS 1.3 client and server implementation in pure C. Provides the
//          Zanna.Crypto.Tls class: X25519 key exchange, AES-128-GCM-SHA256 and
//          ChaCha20-Poly1305-SHA256 AEAD, certificate chain verification against
//          the system trust store, SNI/ALPN extensions, and KeyUpdate.
//
// Key invariants:
//   - All handshake transcript bytes are accumulated into a running SHA-256 state.
//   - Traffic secrets are derived via HKDF-SHA256 per RFC 8446 §7.
//   - Record encryption uses per-direction sequence counters; nonces are XORed
//     with the IV per RFC 8446 §5.3.
//   - Certificate verification delegates to rt_tls_verify.c for chain building.
//   - No global mutable state; per-session state lives in rt_tls_session_t.
//
// Ownership/Lifetime:
//   - rt_tls_session_t is heap-allocated; caller must call rt_tls_close().
//   - rt_tls_server_ctx_t is heap-allocated; caller must call rt_tls_server_ctx_free().
//
// Links: src/runtime/network/rt_tls.h (public API),
//        src/runtime/network/rt_tls_internal.h (session struct),
//        src/runtime/network/rt_tls_verify.c (certificate chain verification),
//        src/runtime/network/rt_crypto.h (AEAD/hash primitives)
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_tls.c
 * @brief Implements the in-tree TLS 1.3 client, server, and record engine.
 * @details This translation unit drives TLS handshakes, transcript hashing,
 *          X25519 key exchange, HKDF traffic-secret derivation, AEAD record
 *          protection, ALPN, KeyUpdate, and orderly shutdown. Each managed
 *          session owns its socket and mutable cryptographic state, while
 *          server contexts retain parsed credentials and negotiation policy.
 */

#include "rt_crypto.h"
#include "rt_crypto_internal.h"
#include "rt_crypto_module.h"
#include "rt_ecdsa_p256.h"
#include "rt_internal.h"
#include "rt_object.h"
#include "rt_rsa.h"
#include "rt_socket_platform.h"
#include "rt_time.h"
#include "rt_tls_internal.h"
#include "rt_tls_server_internal.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static RT_THREAD_LOCAL char g_tls_last_error[256];
static RT_THREAD_LOCAL char g_tls_server_last_error[256];

struct rt_tls_server_ctx {
    uint8_t *cert_list_entries;
    size_t cert_list_entries_len;
    uint8_t *leaf_cert_der;
    size_t leaf_cert_der_len;
    int key_type;
    uint8_t private_key[32];
    rt_rsa_key_t rsa_key;
    char alpn_protocols[128];
    int timeout_ms;
    rt_tls_server_cancel_fn cancel_requested;
    void *cancel_context;
};

#define TLS_SERVER_CANCEL_POLL_MS 100

static void tls_set_server_last_error_msg(const char *msg);
static int tls_hostname_is_ip_literal(const char *hostname);
static int tls_dns_hostname_is_valid(const char *hostname);
static int tls_parse_client_sni(rt_tls_session_t *session, const uint8_t *data, size_t len);
static int tls_server_name_matches_leaf_cert(const rt_tls_server_ctx_t *ctx, const char *hostname);
static int tls_alpn_list_next_token(const char *list,
                                    size_t *offset_io,
                                    const char **token_out,
                                    size_t *token_len_out);
static int tls_alpn_list_contains(const char *list, const uint8_t *wanted, size_t wanted_len);
static int tls_alpn_list_wire_len(const char *list, size_t *wire_len_out);
static int tls_alpn_write_wire_list(uint8_t *dst,
                                    size_t dst_cap,
                                    const char *list,
                                    size_t *wire_len_out);
static int tls_select_alpn_from_wire_list(const char *preferred_list,
                                          const uint8_t *wire_list,
                                          size_t wire_list_len,
                                          char selected_out[64]);
static void tls_session_finalize(void *obj);

/// @brief Validate a low-level TLS session without dereferencing forged memory.
/// @details Low-level C entry points return their documented invalid-session
///          sentinel rather than trapping. This helper therefore performs only
///          stable managed identity and complete-payload validation.
/// @param session Candidate opaque session handle.
/// @return Valid session payload, or NULL for NULL, stale, or unrelated input.
static rt_tls_session_t *tls_session_checked(rt_tls_session_t *session) {
    return rt_obj_is_instance(session, RT_TLS_SESSION_CLASS_ID, sizeof(rt_tls_session_t)) ? session
                                                                                          : NULL;
}

/// @brief Zero and free a heap buffer that may contain TLS secrets.
/// @details This helper pairs secure wiping with ownership release for dynamic
///          handshake buffers. The pointer value itself is not modified; callers
///          clear their owning field after this returns.
/// @param ptr Heap allocation to wipe and free; NULL is accepted.
/// @param len Number of bytes to wipe before freeing.
static void tls_secure_free(void *ptr, size_t len) {
    if (!ptr)
        return;
    if (len > 0)
        rt_secure_zero(ptr, len);
    free(ptr);
}

/// @brief Clear all fixed-size secret-bearing fields in a TLS session.
/// @details Leaves descriptor, state, and object-management fields intact so
///          callers can still close sockets and release the runtime object, but
///          removes private keys, traffic secrets, AEAD keys, and plaintext
///          buffers before the session becomes unreachable.
/// @param session TLS session to scrub; NULL is accepted.
static void tls_scrub_session_secrets(rt_tls_session_t *session) {
    if (!session)
        return;
    rt_secure_zero(session->client_private_key, sizeof(session->client_private_key));
    rt_secure_zero(session->handshake_secret, sizeof(session->handshake_secret));
    rt_secure_zero(session->client_handshake_traffic_secret,
                   sizeof(session->client_handshake_traffic_secret));
    rt_secure_zero(session->server_handshake_traffic_secret,
                   sizeof(session->server_handshake_traffic_secret));
    rt_secure_zero(session->master_secret, sizeof(session->master_secret));
    rt_secure_zero(session->client_application_traffic_secret,
                   sizeof(session->client_application_traffic_secret));
    rt_secure_zero(session->server_application_traffic_secret,
                   sizeof(session->server_application_traffic_secret));
    rt_secure_zero(&session->write_keys, sizeof(session->write_keys));
    rt_secure_zero(&session->read_keys, sizeof(session->read_keys));
    rt_secure_zero(session->read_buffer, sizeof(session->read_buffer));
    rt_secure_zero(session->app_buffer, sizeof(session->app_buffer));
    rt_secure_zero(session->server_cert_der, sizeof(session->server_cert_der));
    if (session->server_cert_der_heap)
        rt_secure_zero(session->server_cert_der_heap, session->server_cert_der_len);
    rt_secure_zero(session->transcript_hash, sizeof(session->transcript_hash));
    rt_secure_zero(&session->transcript_ctx, sizeof(session->transcript_ctx));
    rt_secure_zero(session->client_hello_hash, sizeof(session->client_hello_hash));
    rt_secure_zero(session->cert_transcript_hash, sizeof(session->cert_transcript_hash));
}

/// @brief Free heap-allocated handshake scratch space hanging off a session.
///
/// Releases the buffered server certificate chain and any
/// HelloRetryRequest cookie, and resets the application data
/// reassembly buffer to empty. Safe to call on a half-initialised
/// session and idempotent — pointers are nulled after free.
/// @param session TLS session whose dynamic state is released; NULL is a
///        no-op.
static void tls_release_dynamic_state(rt_tls_session_t *session) {
    if (!session)
        return;

    if (session->server_cert_list) {
        tls_secure_free(session->server_cert_list, session->server_cert_list_len);
        session->server_cert_list = NULL;
        session->server_cert_list_len = 0;
        session->server_cert_count = 0;
    }
    if (session->server_cert_der_heap) {
        tls_secure_free(session->server_cert_der_heap, session->server_cert_der_len);
        session->server_cert_der_heap = NULL;
        session->server_cert_der_len = 0;
    }
    if (session->hello_retry_cookie) {
        tls_secure_free(session->hello_retry_cookie, session->hello_retry_cookie_len);
        session->hello_retry_cookie = NULL;
        session->hello_retry_cookie_len = 0;
    }
    rt_secure_zero(session->read_buffer, sizeof(session->read_buffer));
    rt_secure_zero(session->app_buffer, sizeof(session->app_buffer));
    session->app_buffer_len = 0;
    session->app_buffer_pos = 0;
}

/// @brief Append one DER-encoded certificate to the server context's wire-format list.
///        The entry is encoded as a 3-byte big-endian length prefix followed by the DER
///        bytes and a 2-byte zero extension field (TLS 1.3 CertificateEntry format).
///        If is_leaf is non-zero, a copy is also stored in ctx->leaf_cert_der for later
///        hostname and signature verification.
/// @param ctx Server context receiving the encoded entry.
/// @param der Certificate DER byte span.
/// @param der_len Number of certificate bytes.
/// @param is_leaf Nonzero when this is the leaf certificate.
/// @return One on success; zero for invalid lengths or allocation failure.
static int tls_server_ctx_append_cert(rt_tls_server_ctx_t *ctx,
                                      const uint8_t *der,
                                      size_t der_len,
                                      int is_leaf) {
    size_t entry_len = 0;
    uint8_t *grown = NULL;
    uint8_t *leaf_copy = NULL;
    if (!ctx || !der || der_len == 0 || der_len > 0xFFFFFFu - 5u)
        return 0;
    entry_len = 3 + der_len + 2;
    if (ctx->cert_list_entries_len > 0xFFFFFFu - entry_len ||
        ctx->cert_list_entries_len > SIZE_MAX - entry_len)
        return 0;
    if (is_leaf) {
        leaf_copy = (uint8_t *)malloc(der_len);
        if (!leaf_copy)
            return 0;
        memcpy(leaf_copy, der, der_len);
    }

    grown = (uint8_t *)realloc(ctx->cert_list_entries, ctx->cert_list_entries_len + entry_len);
    if (!grown) {
        free(leaf_copy);
        return 0;
    }
    ctx->cert_list_entries = grown;
    grown += ctx->cert_list_entries_len;
    grown[0] = (uint8_t)((der_len >> 16) & 0xFF);
    grown[1] = (uint8_t)((der_len >> 8) & 0xFF);
    grown[2] = (uint8_t)(der_len & 0xFF);
    memcpy(grown + 3, der, der_len);
    grown[3 + der_len] = 0;
    grown[3 + der_len + 1] = 0;
    ctx->cert_list_entries_len += entry_len;

    if (is_leaf) {
        free(ctx->leaf_cert_der);
        ctx->leaf_cert_der = leaf_copy;
        ctx->leaf_cert_der_len = der_len;
    }

    return 1;
}

/// @brief Zero-initialise a server TLS configuration and set safe defaults.
///        Sets timeout_ms to 30 s; all other fields default to NULL / 0.
/// @param config Configuration storage to initialize; NULL is a no-op.
void rt_tls_server_config_init(rt_tls_server_config_t *config) {
    if (!config)
        return;
    memset(config, 0, sizeof(*config));
    config->timeout_ms = 30000;
}

/// @brief Create a TLS server context from a PEM certificate chain and private key file.
///        Reads and decodes the PEM files, appends each certificate to the wire-format
///        cert list, loads the ECDSA P-256 or RSA-PSS-SHA256 private key, and validates
///        that the key matches the leaf certificate public key.
/// @param config Certificate, key, ALPN, timeout, and cancellation settings.
/// @return Newly allocated context on success, NULL with server last-error set on failure.
rt_tls_server_ctx_t *rt_tls_server_ctx_new(const rt_tls_server_config_t *config) {
    char *cert_pem = NULL;
    char *key_pem = NULL;
    const char *cursor = NULL;
    const char *body = NULL;
    const char *next = NULL;
    size_t body_len = 0;
    size_t file_len = 0;
    int cert_count = 0;
    int parsed_key_type = TLS_SERVER_KEY_NONE;
    int raw_key_loaded = 0;
    rt_tls_server_ctx_t *ctx = NULL;
    uint8_t cert_pub_x[32];
    uint8_t cert_pub_y[32];
    uint8_t key_pub_x[32];
    uint8_t key_pub_y[32];
    rt_rsa_key_t cert_rsa_key;

    rt_rsa_key_init(&cert_rsa_key);

    tls_set_server_last_error_msg(NULL);

    if (!config || !config->cert_file || !config->key_file) {
        tls_set_server_last_error_msg("TLS server: certificate and key files are required");
        return NULL;
    }

    ctx = (rt_tls_server_ctx_t *)calloc(1, sizeof(*ctx));
    if (!ctx) {
        tls_set_server_last_error_msg("TLS server: context allocation failed");
        return NULL;
    }

    ctx->timeout_ms = config->timeout_ms > 0 ? config->timeout_ms : 30000;
    ctx->cancel_requested = config->cancel_requested;
    ctx->cancel_context = config->cancel_context;
    if (config->alpn_protocol) {
        size_t wire_len = 0;
        if (!tls_alpn_list_wire_len(config->alpn_protocol, &wire_len) ||
            strlen(config->alpn_protocol) >= sizeof(ctx->alpn_protocols)) {
            tls_set_server_last_error_msg("TLS server: invalid ALPN protocol list");
            goto fail;
        }
        strncpy(ctx->alpn_protocols, config->alpn_protocol, sizeof(ctx->alpn_protocols) - 1);
    }

    cert_pem = tls_read_text_file(config->cert_file, &file_len);
    if (!cert_pem) {
        tls_set_server_last_error_msg("TLS server: failed to read certificate file");
        goto fail;
    }

    cursor = cert_pem;
    while (tls_find_pem_block(cursor,
                              "-----BEGIN CERTIFICATE-----",
                              "-----END CERTIFICATE-----",
                              &body,
                              &body_len,
                              &next)) {
        size_t max_der = body_len;
        uint8_t *der = (uint8_t *)malloc(max_der);
        size_t der_len = 0;
        if (!der) {
            tls_set_server_last_error_msg("TLS server: certificate buffer allocation failed");
            goto fail;
        }
        der_len = tls_pem_base64_decode(body, body_len, der, max_der);
        if (der_len == 0 || !tls_server_ctx_append_cert(ctx, der, der_len, cert_count == 0)) {
            free(der);
            tls_set_server_last_error_msg("TLS server: failed to decode certificate chain");
            goto fail;
        }
        free(der);
        cert_count++;
        cursor = next;
    }

    if (cert_count == 0) {
        tls_set_server_last_error_msg(
            "TLS server: certificate file does not contain a PEM certificate");
        goto fail;
    }

    parsed_key_type = tls_extract_cert_key_type(ctx->leaf_cert_der, ctx->leaf_cert_der_len);
    if (parsed_key_type == TLS_SERVER_KEY_NONE) {
        tls_set_server_last_error_msg(
            "TLS server: leaf certificate must use an RSA or P-256 ECDSA public key");
        goto fail;
    }

    key_pem = tls_read_text_file(config->key_file, &file_len);
    if (!key_pem) {
        tls_set_server_last_error_msg("TLS server: failed to read private key file");
        goto fail;
    }

    if (parsed_key_type == TLS_SERVER_KEY_ECDSA_P256) {
        if (tls_find_pem_block(key_pem,
                               "-----BEGIN PRIVATE KEY-----",
                               "-----END PRIVATE KEY-----",
                               &body,
                               &body_len,
                               NULL)) {
            uint8_t *der = (uint8_t *)malloc(body_len);
            size_t der_len = 0;
            if (!der) {
                tls_set_server_last_error_msg("TLS server: private key buffer allocation failed");
                goto fail;
            }
            der_len = tls_pem_base64_decode(body, body_len, der, body_len);
            if (der_len > 0 && tls_parse_pkcs8_ec_private_key(der, der_len, ctx->private_key))
                raw_key_loaded = 1;
            free(der);
        } else if (tls_find_pem_block(key_pem,
                                      "-----BEGIN EC PRIVATE KEY-----",
                                      "-----END EC PRIVATE KEY-----",
                                      &body,
                                      &body_len,
                                      NULL)) {
            uint8_t *der = (uint8_t *)malloc(body_len);
            size_t der_len = 0;
            if (!der) {
                tls_set_server_last_error_msg("TLS server: private key buffer allocation failed");
                goto fail;
            }
            der_len = tls_pem_base64_decode(body, body_len, der, body_len);
            if (der_len > 0 && tls_parse_sec1_ec_private_key(der, der_len, ctx->private_key))
                raw_key_loaded = 1;
            free(der);
        }
    }

    if (parsed_key_type == TLS_SERVER_KEY_ECDSA_P256 && raw_key_loaded) {
        if (!tls_extract_cert_ec_pubkey(
                ctx->leaf_cert_der, ctx->leaf_cert_der_len, cert_pub_x, cert_pub_y)) {
            tls_set_server_last_error_msg(
                "TLS server: leaf certificate must use a P-256 ECDSA public key");
            goto fail;
        }

        if (ecdsa_p256_public_from_private(ctx->private_key, key_pub_x, key_pub_y) &&
            memcmp(cert_pub_x, key_pub_x, sizeof(cert_pub_x)) == 0 &&
            memcmp(cert_pub_y, key_pub_y, sizeof(cert_pub_y)) == 0) {
            ctx->key_type = TLS_SERVER_KEY_ECDSA_P256;
            free(cert_pem);
            free(key_pem);
            return ctx;
        }
    }

    if (parsed_key_type == TLS_SERVER_KEY_RSA_PSS_SHA256) {
        rt_rsa_key_t private_rsa_key;
        rt_rsa_key_init(&private_rsa_key);

        if (tls_find_pem_block(key_pem,
                               "-----BEGIN PRIVATE KEY-----",
                               "-----END PRIVATE KEY-----",
                               &body,
                               &body_len,
                               NULL)) {
            uint8_t *der = (uint8_t *)malloc(body_len);
            size_t der_len = 0;
            if (!der) {
                tls_set_server_last_error_msg("TLS server: private key buffer allocation failed");
                rt_rsa_key_free(&private_rsa_key);
                goto fail;
            }
            der_len = tls_pem_base64_decode(body, body_len, der, body_len);
            if (der_len > 0)
                raw_key_loaded = rt_rsa_parse_private_key_pkcs8(der, der_len, &private_rsa_key);
            free(der);
        } else if (tls_find_pem_block(key_pem,
                                      "-----BEGIN RSA PRIVATE KEY-----",
                                      "-----END RSA PRIVATE KEY-----",
                                      &body,
                                      &body_len,
                                      NULL)) {
            uint8_t *der = (uint8_t *)malloc(body_len);
            size_t der_len = 0;
            if (!der) {
                tls_set_server_last_error_msg("TLS server: private key buffer allocation failed");
                rt_rsa_key_free(&private_rsa_key);
                goto fail;
            }
            der_len = tls_pem_base64_decode(body, body_len, der, body_len);
            if (der_len > 0)
                raw_key_loaded = rt_rsa_parse_private_key_pkcs1(der, der_len, &private_rsa_key);
            free(der);
        }

        if (raw_key_loaded &&
            tls_extract_cert_rsa_pubkey(
                ctx->leaf_cert_der, ctx->leaf_cert_der_len, &cert_rsa_key) &&
            rt_rsa_public_equals(&cert_rsa_key, &private_rsa_key)) {
            ctx->rsa_key = private_rsa_key;
            rt_rsa_key_init(&private_rsa_key);
            ctx->key_type = TLS_SERVER_KEY_RSA_PSS_SHA256;
            free(cert_pem);
            free(key_pem);
            rt_rsa_key_free(&cert_rsa_key);
            return ctx;
        }

        rt_rsa_key_free(&private_rsa_key);
    }

    if (parsed_key_type == TLS_SERVER_KEY_ECDSA_P256) {
        if (raw_key_loaded) {
            tls_set_server_last_error_msg("TLS server: certificate and private key do not match");
        } else {
            tls_set_server_last_error_msg("TLS server: unsupported ECDSA private key format; "
                                          "expected an unencrypted P-256 PEM");
        }
    } else {
        if (raw_key_loaded)
            tls_set_server_last_error_msg("TLS server: certificate and private key do not match");
        else
            tls_set_server_last_error_msg("TLS server: unsupported RSA private key format; "
                                          "expected an unencrypted PKCS#1 or PKCS#8 PEM");
    }
    goto fail;

fail:
    free(cert_pem);
    free(key_pem);
    rt_rsa_key_free(&cert_rsa_key);
    rt_tls_server_ctx_free(ctx);
    return NULL;
}

/// @brief Free a server TLS context and all associated resources.
///        Releases the RSA key, the wire-format certificate list, and the leaf cert copy.
/// @param ctx Server context to securely consume; NULL is a no-op.
void rt_tls_server_ctx_free(rt_tls_server_ctx_t *ctx) {
    if (!ctx)
        return;
    rt_rsa_key_free(&ctx->rsa_key);
    tls_secure_free(ctx->cert_list_entries, ctx->cert_list_entries_len);
    tls_secure_free(ctx->leaf_cert_der, ctx->leaf_cert_der_len);
    rt_secure_zero(ctx->private_key, sizeof(ctx->private_key));
    rt_secure_zero(ctx->alpn_protocols, sizeof(ctx->alpn_protocols));
    rt_secure_zero(ctx, sizeof(*ctx));
    free(ctx);
}

// TLS constants
#define TLS_VERSION_1_2 0x0303
#define TLS_VERSION_1_3 0x0304

// Content types
#define TLS_CONTENT_CHANGE_CIPHER 20
#define TLS_CONTENT_ALERT 21
#define TLS_CONTENT_HANDSHAKE 22
#define TLS_CONTENT_APPLICATION 23

// Handshake types
#define TLS_HS_CLIENT_HELLO 1
#define TLS_HS_SERVER_HELLO 2
#define TLS_HS_ENCRYPTED_EXTENSIONS 8
#define TLS_HS_CERTIFICATE 11
#define TLS_HS_CERTIFICATE_VERIFY 15
#define TLS_HS_FINISHED 20

// Cipher suites
#define TLS_AES_128_GCM_SHA256 0x1301
#define TLS_CHACHA20_POLY1305_SHA256 0x1303

// Extensions
#define TLS_EXT_SERVER_NAME 0
#define TLS_EXT_ALPN 16
#define TLS_EXT_SUPPORTED_GROUPS 10
#define TLS_EXT_SIGNATURE_ALGORITHMS 13
#define TLS_EXT_COOKIE 44
#define TLS_EXT_SUPPORTED_VERSIONS 43
#define TLS_EXT_KEY_SHARE 51

static const uint8_t TLS_HELLO_RETRY_RANDOM[32] = {
    0xCF, 0x21, 0xAD, 0x74, 0xE5, 0x9A, 0x61, 0x11, 0xBE, 0x1D, 0x8C, 0x02, 0x1E, 0x65, 0xB8, 0x91,
    0xC2, 0xA2, 0x11, 0x16, 0x7A, 0xBB, 0x8C, 0x5E, 0x07, 0x9E, 0x09, 0xE2, 0xC8, 0xA8, 0x33, 0x9C};

// Note: TLS_MAX_RECORD_SIZE, TLS_MAX_CIPHERTEXT, tls_state_t, traffic_keys_t,
// and struct rt_tls_session are defined in rt_tls_internal.h.

// ---------------------------------------------------------------------------
// Wire-format byte helpers. TLS uses network byte order (big-endian) for all
// length and version fields. The 24-bit width is required by the handshake
// header (RFC 8446 §4: `Handshake.length` is a `uint24`).
// ---------------------------------------------------------------------------

/// @brief Write an unsigned 16-bit value in TLS network byte order.
/// @param p Writable two-byte destination.
/// @param v Value to encode.
static void write_u16(uint8_t *p, uint16_t v) {
    p[0] = (v >> 8) & 0xFF;
    p[1] = v & 0xFF;
}

/// @brief Write the low 24 bits of a value in TLS network byte order.
/// @param p Writable three-byte destination.
/// @param v Value whose low 24 bits are encoded.
static void write_u24(uint8_t *p, uint32_t v) {
    p[0] = (v >> 16) & 0xFF;
    p[1] = (v >> 8) & 0xFF;
    p[2] = v & 0xFF;
}

/// @brief Decode an unsigned 16-bit TLS network-order value.
/// @param p Readable two-byte source.
/// @return Decoded host-order value.
static uint16_t read_u16(const uint8_t *p) {
    return ((uint16_t)p[0] << 8) | p[1];
}

/// @brief Decode an unsigned 24-bit TLS network-order value.
/// @param p Readable three-byte source.
/// @return Decoded value in the low 24 bits.
static uint32_t read_u24(const uint8_t *p) {
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

/// @brief Parse one ASN.1 BER/DER TLV triple from buf.
///        Populates *tag, *val_len (value byte count), and *hdr_len (tag + length octets).
///        Supports both short-form and multi-byte definite-length encodings (up to 4 bytes).
///        Returns 0 on success, -1 if the buffer is too small or the encoding is malformed.
/// @param buf DER bytes beginning at a TLV.
/// @param buf_len Available bytes at @p buf.
/// @param tag Destination for the tag octet.
/// @param val_len Destination for the content length.
/// @param hdr_len Destination for tag-plus-length size.
/// @return Zero on success; -1 for invalid arguments or malformed/truncated DER.
int tls_der_read_tlv(
    const uint8_t *buf, size_t buf_len, uint8_t *tag, size_t *val_len, size_t *hdr_len) {
    if (!buf || !tag || !val_len || !hdr_len || buf_len < 2)
        return -1;

    *tag = buf[0];
    if (buf[1] < 0x80) {
        *val_len = buf[1];
        *hdr_len = 2;
    } else {
        size_t num_len_bytes = buf[1] & 0x7F;
        size_t value = 0;
        if (num_len_bytes == 0 || num_len_bytes > 4 || 2 + num_len_bytes > buf_len)
            return -1;
        if (buf[2] == 0)
            return -1;
        for (size_t i = 0; i < num_len_bytes; i++)
            value = (value << 8) | buf[2 + i];
        if (value < 0x80)
            return -1;
        *val_len = value;
        *hdr_len = 2 + num_len_bytes;
    }

    return (*hdr_len <= buf_len && *val_len <= buf_len - *hdr_len) ? 0 : -1;
}

/// @brief Compare a DER OID payload with an expected encoding.
/// @param buf OID payload bytes.
/// @param buf_len Number of bytes in @p buf.
/// @param oid Expected OID encoding.
/// @param oid_len Number of expected bytes.
/// @return One for exact length and byte equality; otherwise zero.
int tls_oid_matches(const uint8_t *buf, size_t buf_len, const uint8_t *oid, size_t oid_len) {
    return buf_len == oid_len && memcmp(buf, oid, oid_len) == 0;
}

/// @brief Return 1 if hostname is a numeric IPv4 or IPv6 literal, 0 otherwise.
///        IP-literal hostnames skip DNS-name SAN matching and are verified by IP SAN or exact
///        match.
/// @param hostname NUL-terminated host text.
/// @return One for a numeric IPv4 or IPv6 literal; otherwise zero.
static int tls_hostname_is_ip_literal(const char *hostname) {
    struct in_addr ipv4;
    struct in6_addr ipv6;
    if (!hostname || !*hostname)
        return 0;
    if (inet_pton(AF_INET, hostname, &ipv4) == 1)
        return 1;
    if (inet_pton(AF_INET6, hostname, &ipv6) == 1)
        return 1;
    return 0;
}

/// @brief Validate one DNS label for TLS hostname and SNI processing.
/// @details Accepts only LDH labels: ASCII letters, digits, and hyphen, with a 1..63 byte length
///          and no leading or trailing hyphen. Wildcards are intentionally not accepted here.
/// @param start Pointer to the first byte of the label.
/// @param len Number of bytes in the label.
/// @return 1 if the label is valid; otherwise 0.
static int tls_dns_label_is_valid(const char *start, size_t len) {
    if (len == 0 || len > 63 || start[0] == '-' || start[len - 1] == '-')
        return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)start[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '-')) {
            return 0;
        }
    }
    return 1;
}

/// @brief Validate a full DNS hostname used by TLS client config or ClientHello SNI.
/// @details Enforces total DNS presentation length, rejects leading/trailing dots, and validates
///          every label with tls_dns_label_is_valid. IP literals are handled separately and should
///          not be passed here when they are acceptable.
/// @param hostname NUL-terminated hostname.
/// @return 1 if the hostname is a valid DNS name; otherwise 0.
static int tls_dns_hostname_is_valid(const char *hostname) {
    const char *label = hostname;
    size_t total_len;
    if (!hostname || !*hostname)
        return 0;
    total_len = strlen(hostname);
    if (total_len > 253 || hostname[0] == '.' || hostname[total_len - 1] == '.')
        return 0;
    for (const char *p = hostname;; p++) {
        if (*p == '.' || *p == '\0') {
            if (!tls_dns_label_is_valid(label, (size_t)(p - label)))
                return 0;
            if (*p == '\0')
                break;
            label = p + 1;
        }
    }
    return 1;
}

/// @brief Parse the ServerNameList from a ClientHello server_name extension.
///        Stores the first host_name entry (lowercased, NUL-terminated) into
///        session->hostname. IP literals are silently cleared so SAN checks skip them.
///        Returns RT_TLS_OK or RT_TLS_ERROR_HANDSHAKE with session->error set.
/// @param session Server-side session receiving normalized SNI.
/// @param data ServerNameList extension payload.
/// @param len Number of payload bytes.
/// @return @ref RT_TLS_OK on success or @ref RT_TLS_ERROR_HANDSHAKE.
static int tls_parse_client_sni(rt_tls_session_t *session, const uint8_t *data, size_t len) {
    size_t pos = 0;
    int saw_host_name = 0;

    if (!session || !data || len < 2)
        return RT_TLS_ERROR_HANDSHAKE;

    {
        uint16_t list_len = read_u16(data);
        if ((size_t)list_len + 2 != len) {
            session->error = "ClientHello server_name length mismatch";
            return RT_TLS_ERROR_HANDSHAKE;
        }
        pos = 2;
        while (pos + 3 <= len) {
            uint8_t name_type = data[pos++];
            uint16_t name_len = read_u16(data + pos);
            pos += 2;
            if (pos + name_len > len) {
                session->error = "ClientHello server_name entry truncated";
                return RT_TLS_ERROR_HANDSHAKE;
            }
            if (name_type == 0) {
                if (saw_host_name) {
                    session->error = "ClientHello contains multiple host_name SNI entries";
                    return RT_TLS_ERROR_HANDSHAKE;
                }
                if (name_len == 0 || name_len >= sizeof(session->hostname)) {
                    session->error = "ClientHello host_name SNI is invalid";
                    return RT_TLS_ERROR_HANDSHAKE;
                }
                for (uint16_t i = 0; i < name_len; i++) {
                    unsigned char ch = data[pos + i];
                    if (ch <= 0x20u || ch >= 0x7Fu) {
                        session->error = "ClientHello host_name SNI contains invalid characters";
                        return RT_TLS_ERROR_HANDSHAKE;
                    }
                    session->hostname[i] =
                        (char)((ch >= 'A' && ch <= 'Z') ? (ch + ('a' - 'A')) : ch);
                }
                session->hostname[name_len] = '\0';
                if (tls_hostname_is_ip_literal(session->hostname))
                    session->hostname[0] = '\0';
                else if (!tls_dns_hostname_is_valid(session->hostname)) {
                    session->error = "ClientHello host_name SNI is not a valid DNS name";
                    return RT_TLS_ERROR_HANDSHAKE;
                }
                saw_host_name = 1;
            }
            pos += name_len;
        }
        if (pos != len) {
            session->error = "ClientHello server_name malformed";
            return RT_TLS_ERROR_HANDSHAKE;
        }
    }

    return RT_TLS_OK;
}

/// @brief Return 1 if hostname matches the leaf certificate's SANs or CommonName.
///        Checks SubjectAltName DNS names first (preferred per RFC 6125); falls back to
///        CommonName only when no SANs are present. Returns 1 when no hostname
///        was supplied; otherwise fails closed if the configured leaf
///        certificate is unavailable or the SNI value is an IP literal.
/// @param ctx Server context containing the leaf certificate.
/// @param hostname Normalized client SNI, or empty when absent.
/// @return One when no name is requested or certificate identity matches;
///         otherwise zero.
static int tls_server_name_matches_leaf_cert(const rt_tls_server_ctx_t *ctx, const char *hostname) {
    char san_names[32][256];
    char cn[256];
    int san_count = 0;

    if (!hostname || !*hostname) {
        const char *require_sni = getenv("ZANNA_TLS_REQUIRE_SNI");
        return (require_sni && require_sni[0] && strcmp(require_sni, "0") != 0) ? 0 : 1;
    }
    if (!ctx || !ctx->leaf_cert_der || ctx->leaf_cert_der_len == 0)
        return 0;
    if (tls_hostname_is_ip_literal(hostname))
        return 0;

    san_count = tls_extract_san_names(ctx->leaf_cert_der, ctx->leaf_cert_der_len, san_names, 32);
    for (int i = 0; i < san_count; i++) {
        if (tls_match_hostname(san_names[i], hostname))
            return 1;
    }
    if (san_count > 0)
        return 0;

    if (tls_extract_cn(ctx->leaf_cert_der, ctx->leaf_cert_der_len, cn) && cn[0] != '\0')
        return tls_match_hostname(cn, hostname);

    return 0;
}

/// @brief Initialize an empty TLS handshake transcript.
/// @details Resets the cumulative byte count, initializes the live SHA-256
///          context, and caches the SHA-256 digest of the empty transcript.
/// @param session Session whose transcript state is reset.
static void transcript_init(rt_tls_session_t *session) {
    session->transcript_len = 0;
    rt_sha256_init(&session->transcript_ctx);
    rt_sha256(NULL, 0, session->transcript_hash);
}

/// @brief Replace the calling thread's client-side TLS diagnostic.
/// @param msg NUL-terminated text to copy; NULL or empty clears the diagnostic.
static void tls_set_last_error_msg(const char *msg) {
    if (!msg || !*msg) {
        g_tls_last_error[0] = '\0';
        return;
    }
    snprintf(g_tls_last_error, sizeof(g_tls_last_error), "%s", msg);
}

/// @brief Replace the calling thread's server-side TLS diagnostic.
/// @param msg NUL-terminated text to copy; NULL or empty clears the diagnostic.
static void tls_set_server_last_error_msg(const char *msg) {
    if (!msg || !*msg) {
        g_tls_server_last_error[0] = '\0';
        return;
    }
    snprintf(g_tls_server_last_error, sizeof(g_tls_server_last_error), "%s", msg);
}

/// @brief Return the latest client-side TLS diagnostic on this thread.
/// @return Thread-local text valid until the next client TLS setup operation.
const char *rt_tls_last_error(void) {
    return g_tls_last_error[0] ? g_tls_last_error : "no error";
}

/// @brief Return the latest server-side TLS diagnostic on this thread.
/// @return Thread-local text valid until the next server TLS setup operation.
const char *rt_tls_server_last_error(void) {
    return g_tls_server_last_error[0] ? g_tls_server_last_error : "no error";
}

/// @brief Append `len` bytes of handshake data to the transcript hash.
///
/// Maintains a snapshot in `session->transcript_hash` after each
/// update so callers can derive secrets at any handshake boundary
/// without finalising the live context. Sets `session->error` and
/// returns -1 if the cumulative length would overflow `size_t` or
/// the session is already in an error state.
/// @param session Session owning the live transcript context.
/// @param data Handshake wire bytes to append.
/// @param len Number of bytes in @p data.
/// @return 0 on success, -1 on overflow / pre-existing error.
static int transcript_update(rt_tls_session_t *session, const uint8_t *data, size_t len) {
    if (session->error)
        return -1; // Already in error state

    if (len > SIZE_MAX - session->transcript_len) {
        session->error = "TLS: handshake transcript length overflow";
        return -1;
    }

    session->transcript_len += len;
    rt_sha256_update(&session->transcript_ctx, data, len);
    rt_sha256_ctx hash_snapshot = session->transcript_ctx;
    rt_sha256_final(&hash_snapshot, session->transcript_hash);
    return 0;
}

/// @brief Derive TLS 1.3 handshake traffic keys from the ECDHE shared secret.
///
/// Implements the HKDF schedule from RFC 8446 §7.1:
///   early_secret    = HKDF-Extract(0, 0)
///   derived         = Derive-Secret(early_secret, "derived", "")
///   handshake_secret= HKDF-Extract(derived, shared_secret)
///   c_hs_traffic    = Derive-Secret(handshake_secret, "c hs traffic", H(handshake))
///   s_hs_traffic    = Derive-Secret(handshake_secret, "s hs traffic", H(handshake))
///
/// Then expands each traffic secret into an AEAD key (16 bytes for
/// AES-128-GCM, 32 bytes for ChaCha20-Poly1305) and a 12-byte IV.
/// The transcript hash must already cover ClientHello + ServerHello
/// before this function is called.
/// @param session Client session receiving handshake secrets and directional
///        keys.
/// @param shared_secret 32-byte X25519 ECDHE result.
static void derive_handshake_keys(rt_tls_session_t *session, const uint8_t shared_secret[32]) {
    uint8_t zero_key[32] = {0};
    uint8_t early_secret[32];
    uint8_t derived[32];
    uint8_t empty_hash[32];

    // early_secret = HKDF-Extract(0, 0)
    rt_hkdf_extract(NULL, 0, zero_key, 32, early_secret);

    // derived = Derive-Secret(early_secret, "derived", "")
    rt_sha256(NULL, 0, empty_hash);
    rt_hkdf_expand_label(early_secret, "derived", empty_hash, 32, derived, 32);

    // handshake_secret = HKDF-Extract(derived, shared_secret)
    rt_hkdf_extract(derived, 32, shared_secret, 32, session->handshake_secret);

    // client_handshake_traffic_secret
    rt_hkdf_expand_label(session->handshake_secret,
                         "c hs traffic",
                         session->transcript_hash,
                         32,
                         session->client_handshake_traffic_secret,
                         32);

    // server_handshake_traffic_secret
    rt_hkdf_expand_label(session->handshake_secret,
                         "s hs traffic",
                         session->transcript_hash,
                         32,
                         session->server_handshake_traffic_secret,
                         32);

    // Derive keys (AES-128-GCM uses 16-byte keys; ChaCha20 uses 32-byte keys)
    int key_len = (session->cipher_suite == TLS_AES_128_GCM_SHA256) ? 16 : 32;
    rt_hkdf_expand_label(
        session->server_handshake_traffic_secret, "key", NULL, 0, session->read_keys.key, key_len);
    rt_hkdf_expand_label(
        session->server_handshake_traffic_secret, "iv", NULL, 0, session->read_keys.iv, 12);
    session->read_keys.seq_num = 0;

    rt_hkdf_expand_label(
        session->client_handshake_traffic_secret, "key", NULL, 0, session->write_keys.key, key_len);
    rt_hkdf_expand_label(
        session->client_handshake_traffic_secret, "iv", NULL, 0, session->write_keys.iv, 12);
    session->write_keys.seq_num = 0;

    session->keys_established = 1;
    rt_secure_zero(zero_key, sizeof(zero_key));
    rt_secure_zero(early_secret, sizeof(early_secret));
    rt_secure_zero(derived, sizeof(derived));
    rt_secure_zero(empty_hash, sizeof(empty_hash));
}

/// @brief Server-side mirror of derive_handshake_keys: derives TLS 1.3 handshake secrets
///        and installs read keys from the client traffic secret and write keys from the
///        server traffic secret (reversed direction relative to the client path).
/// @param session Server session receiving handshake secrets and directional
///        keys.
/// @param shared_secret 32-byte X25519 ECDHE result.
static void derive_handshake_keys_server(rt_tls_session_t *session,
                                         const uint8_t shared_secret[32]) {
    uint8_t zero_key[32] = {0};
    uint8_t early_secret[32];
    uint8_t derived[32];
    uint8_t empty_hash[32];
    int key_len;

    rt_hkdf_extract(NULL, 0, zero_key, 32, early_secret);

    rt_sha256(NULL, 0, empty_hash);
    rt_hkdf_expand_label(early_secret, "derived", empty_hash, 32, derived, 32);

    rt_hkdf_extract(derived, 32, shared_secret, 32, session->handshake_secret);

    rt_hkdf_expand_label(session->handshake_secret,
                         "c hs traffic",
                         session->transcript_hash,
                         32,
                         session->client_handshake_traffic_secret,
                         32);
    rt_hkdf_expand_label(session->handshake_secret,
                         "s hs traffic",
                         session->transcript_hash,
                         32,
                         session->server_handshake_traffic_secret,
                         32);

    key_len = (session->cipher_suite == TLS_AES_128_GCM_SHA256) ? 16 : 32;
    rt_hkdf_expand_label(
        session->client_handshake_traffic_secret, "key", NULL, 0, session->read_keys.key, key_len);
    rt_hkdf_expand_label(
        session->client_handshake_traffic_secret, "iv", NULL, 0, session->read_keys.iv, 12);
    session->read_keys.seq_num = 0;

    rt_hkdf_expand_label(
        session->server_handshake_traffic_secret, "key", NULL, 0, session->write_keys.key, key_len);
    rt_hkdf_expand_label(
        session->server_handshake_traffic_secret, "iv", NULL, 0, session->write_keys.iv, 12);
    session->write_keys.seq_num = 0;

    session->keys_established = 1;
    rt_secure_zero(zero_key, sizeof(zero_key));
    rt_secure_zero(early_secret, sizeof(early_secret));
    rt_secure_zero(derived, sizeof(derived));
    rt_secure_zero(empty_hash, sizeof(empty_hash));
}

/// @brief Derive TLS 1.3 application traffic secrets (RFC 8446 §7.1) after Finished.
///        Computes master_secret = HKDF-Extract(derived(handshake_secret), 0), then
///        expands client_application_traffic_secret ("c ap traffic") and
///        server_application_traffic_secret ("s ap traffic") using the transcript hash
///        at the time Finished was verified.
/// @param session Session receiving the master and application traffic secrets.
static void derive_application_secrets(rt_tls_session_t *session) {
    uint8_t derived[32];
    uint8_t zero_key[32] = {0};
    uint8_t empty_hash[32];

    rt_sha256(NULL, 0, empty_hash);
    rt_hkdf_expand_label(session->handshake_secret, "derived", empty_hash, 32, derived, 32);

    // master_secret = HKDF-Extract(derived, 0)
    rt_hkdf_extract(derived, 32, zero_key, 32, session->master_secret);

    // client_application_traffic_secret
    rt_hkdf_expand_label(session->master_secret,
                         "c ap traffic",
                         session->transcript_hash,
                         32,
                         session->client_application_traffic_secret,
                         32);

    // server_application_traffic_secret
    rt_hkdf_expand_label(session->master_secret,
                         "s ap traffic",
                         session->transcript_hash,
                         32,
                         session->server_application_traffic_secret,
                         32);
    rt_secure_zero(derived, sizeof(derived));
    rt_secure_zero(zero_key, sizeof(zero_key));
    rt_secure_zero(empty_hash, sizeof(empty_hash));
}

/// @brief Expand a 32-byte TLS 1.3 traffic secret into a AEAD key and 12-byte IV.
///        Key length is 16 for AES-128-GCM or 32 for ChaCha20-Poly1305.
///        Resets the per-direction sequence counter to 0 (RFC 8446 §5.3).
/// @param secret 32-byte TLS 1.3 traffic secret.
/// @param keys Destination key, IV, and sequence-number state.
/// @param cipher_suite Negotiated supported TLS 1.3 cipher suite.
static void install_traffic_keys_from_secret(const uint8_t secret[32],
                                             traffic_keys_t *keys,
                                             uint16_t cipher_suite) {
    int key_len = (cipher_suite == TLS_AES_128_GCM_SHA256) ? 16 : 32;
    rt_hkdf_expand_label(secret, "key", NULL, 0, keys->key, key_len);
    rt_hkdf_expand_label(secret, "iv", NULL, 0, keys->iv, 12);
    keys->seq_num = 0;
}

/// @brief Install client read keys from server_application_traffic_secret (client reads from
/// server).
/// @param session Client session receiving application read keys.
static void install_client_application_read_keys(rt_tls_session_t *session) {
    install_traffic_keys_from_secret(
        session->server_application_traffic_secret, &session->read_keys, session->cipher_suite);
}

/// @brief Install client write keys from client_application_traffic_secret (client writes to
/// server).
/// @param session Client session receiving application write keys.
static void install_client_application_write_keys(rt_tls_session_t *session) {
    install_traffic_keys_from_secret(
        session->client_application_traffic_secret, &session->write_keys, session->cipher_suite);
}

/// @brief Install server read keys from client_application_traffic_secret (server reads from
/// client).
/// @param session Server session receiving application read keys.
static void install_server_application_read_keys(rt_tls_session_t *session) {
    install_traffic_keys_from_secret(
        session->client_application_traffic_secret, &session->read_keys, session->cipher_suite);
}

/// @brief Install server write keys from server_application_traffic_secret (server writes to
/// client).
/// @param session Server session receiving application write keys.
static void install_server_application_write_keys(rt_tls_session_t *session) {
    install_traffic_keys_from_secret(
        session->server_application_traffic_secret, &session->write_keys, session->cipher_suite);
}

/// @brief Perform a TLS 1.3 KeyUpdate ratchet (RFC 8446 §7.2) on secret in-place.
///        Derives the next traffic secret via HKDF-Expand-Label(secret, "traffic upd", "", 32),
///        updates secret, then re-derives key and IV and resets the sequence counter to 0.
/// @param secret In/out 32-byte application traffic secret.
/// @param keys In/out directional traffic keys.
/// @param cipher_suite Negotiated supported TLS 1.3 cipher suite.
static void update_application_secret_and_keys(uint8_t secret[32],
                                               traffic_keys_t *keys,
                                               uint16_t cipher_suite) {
    uint8_t next_secret[32];
    const int app_key_len = (cipher_suite == TLS_AES_128_GCM_SHA256) ? 16 : 32;

    rt_hkdf_expand_label(secret, "traffic upd", NULL, 0, next_secret, sizeof(next_secret));
    memcpy(secret, next_secret, sizeof(next_secret));

    rt_hkdf_expand_label(secret, "key", NULL, 0, keys->key, app_key_len);
    rt_hkdf_expand_label(secret, "iv", NULL, 0, keys->iv, sizeof(keys->iv));
    keys->seq_num = 0;
    rt_secure_zero(next_secret, sizeof(next_secret));
}

/// @brief Ratchet inbound application traffic keys after receiving KeyUpdate.
/// @param session Established client or server session.
static void update_read_application_keys(rt_tls_session_t *session) {
    if (session->is_server) {
        update_application_secret_and_keys(
            session->client_application_traffic_secret, &session->read_keys, session->cipher_suite);
    } else {
        update_application_secret_and_keys(
            session->server_application_traffic_secret, &session->read_keys, session->cipher_suite);
    }
}

/// @brief Ratchet outbound application traffic keys before sending KeyUpdate.
/// @param session Established client or server session.
static void update_write_application_keys(rt_tls_session_t *session) {
    if (session->is_server) {
        update_application_secret_and_keys(session->server_application_traffic_secret,
                                           &session->write_keys,
                                           session->cipher_suite);
    } else {
        update_application_secret_and_keys(session->client_application_traffic_secret,
                                           &session->write_keys,
                                           session->cipher_suite);
    }
}

/// @brief Check the optional owner cancellation probe and overall I/O deadline.
/// @details Owners with a cancellation probe use bounded native socket timeouts.
///          This check turns each elapsed slice into either a retry or a terminal
///          cancellation/deadline result without exposing polling to callers.
/// @param session TLS session currently driving cancellation-aware I/O.
/// @return Non-zero when the current TLS operation must stop.
static int tls_io_should_stop(rt_tls_session_t *session) {
    if (!session || !session->io_cancel_requested)
        return 0;
    if (session->io_cancel_requested(session->io_cancel_context)) {
        session->error = "TLS: I/O cancelled";
        return 1;
    }
    if (session->io_deadline_us > 0 && rt_clock_ticks_us() >= session->io_deadline_us) {
        session->error = "TLS: I/O timeout";
        return 1;
    }
    return 0;
}

/// Forward declaration — definition below (send_key_update_record calls send_record).
static int send_record(rt_tls_session_t *session,
                       uint8_t content_type,
                       const uint8_t *data,
                       size_t len);
static int send_record_fragmented(rt_tls_session_t *session,
                                  uint8_t content_type,
                                  const uint8_t *data,
                                  size_t len);

/// @brief Send a TLS 1.3 KeyUpdate handshake message to request or acknowledge a key rollover.
///        request_update non-zero asks the peer to respond with its own KeyUpdate.
/// @param session Established TLS session.
/// @param request_update Nonzero to request a reciprocal KeyUpdate.
/// @return @ref RT_TLS_OK or a negative record-send status.
static int send_key_update_record(rt_tls_session_t *session, uint8_t request_update) {
    uint8_t msg[5];
    msg[0] = 24; // KeyUpdate
    write_u24(msg + 1, 1);
    msg[4] = request_update ? 1 : 0;
    return send_record(session, TLS_CONTENT_HANDSHAKE, msg, sizeof(msg));
}

/// @brief Construct the per-record AEAD nonce per RFC 8446 §5.3.
///
/// The nonce is the static 12-byte IV XORed with the big-endian
/// 64-bit sequence number aligned to the rightmost 8 bytes. This
/// guarantees nonce uniqueness for every record under the same key
/// without sending an explicit nonce on the wire.
/// @param iv Static 12-byte directional traffic IV.
/// @param seq Current record sequence number.
/// @param nonce Destination 12-byte per-record nonce.
static void build_nonce(const uint8_t iv[12], uint64_t seq, uint8_t nonce[12]) {
    memcpy(nonce, iv, 12);
    for (int i = 0; i < 8; i++) {
        nonce[12 - 1 - i] ^= (seq >> (i * 8)) & 0xFF;
    }
}

/// @brief Allocate a heap scratch buffer for TLS record processing.
/// @details TLS record buffers can be larger than 16 KiB. Keeping them on the heap avoids
///          exhausting small platform thread stacks while preserving the existing per-call
///          ownership model. On allocation failure this helper stores @p error_msg on the
///          session so callers can return a normal TLS error code.
/// @param session TLS session whose `error` field should receive allocation failures.
/// @param size Requested byte count; zero is rounded up to one byte for portable `malloc`.
/// @param error_msg Stable error string stored in @p session on failure.
/// @return Heap-owned buffer, or NULL on allocation failure.
static uint8_t *tls_alloc_record_scratch(rt_tls_session_t *session,
                                         size_t size,
                                         const char *error_msg) {
    uint8_t *buffer = (uint8_t *)malloc(size > 0 ? size : 1);
    if (!buffer && session)
        session->error = error_msg;
    return buffer;
}

/// @brief Frame, optionally encrypt, and transmit a single TLS record.
///
/// Behaviour depends on `session->keys_established`:
///   - Pre-handshake: writes a plaintext record of `content_type`
///     with `data` as payload.
///   - Post-handshake: emits an `application_data` (23) record whose
///     plaintext is `data || content_type`, AEAD-sealed under the
///     active write key with a sequence-number-derived nonce. The
///     5-byte record header serves as the AAD.
///
/// Selects AES-128-GCM or ChaCha20-Poly1305 based on the negotiated
/// cipher suite. Bumps the write sequence number on each encrypted
/// record and refuses to send when the counter would wrap (RFC 8446
/// §5.5 nonce-reuse guard). Loops on partial `send()` and treats
/// `EINTR` as retryable.
/// @param session TLS session owning socket and write keys.
/// @param content_type Inner or plaintext TLS content type.
/// @param data Payload bytes, or NULL only when @p len is zero.
/// @param len Payload byte count no greater than one record.
/// @return RT_TLS_OK, RT_TLS_ERROR (encryption / overflow), or
///         RT_TLS_ERROR_SOCKET (write failure).
static int send_record(rt_tls_session_t *session,
                       uint8_t content_type,
                       const uint8_t *data,
                       size_t len) {
    uint8_t *record = NULL;
    uint8_t *plaintext = NULL;
    size_t record_len;
    int rc = RT_TLS_OK;
    if (len > TLS_MAX_RECORD_SIZE || (!data && len > 0)) {
        session->error = "TLS: record payload too large";
        return RT_TLS_ERROR;
    }

    record = tls_alloc_record_scratch(
        session, 5u + (size_t)TLS_MAX_CIPHERTEXT, "TLS: record allocation failed");
    if (!record)
        return RT_TLS_ERROR;

    if (session->keys_established) {
        // Encrypted record
        plaintext = tls_alloc_record_scratch(
            session, (size_t)TLS_MAX_RECORD_SIZE + 1u, "TLS: plaintext allocation failed");
        if (!plaintext) {
            rc = RT_TLS_ERROR;
            goto done;
        }
        if (len > 0)
            memcpy(plaintext, data, len);
        plaintext[len] = content_type; // Inner content type

        uint8_t aad[5];
        aad[0] = TLS_CONTENT_APPLICATION;
        write_u16(aad + 1, TLS_VERSION_1_2);
        write_u16(aad + 3, (uint16_t)(len + 1 + 16)); // plaintext + content type + tag

        uint8_t nonce[12];
        build_nonce(session->write_keys.iv, session->write_keys.seq_num, nonce);

        record[0] = TLS_CONTENT_APPLICATION;
        write_u16(record + 1, TLS_VERSION_1_2);
        size_t ciphertext_len;
        if (session->cipher_suite == TLS_AES_128_GCM_SHA256)
            ciphertext_len = rt_aes128_gcm_encrypt(
                session->write_keys.key, nonce, aad, 5, plaintext, len + 1, record + 5);
        else
            ciphertext_len = rt_chacha20_poly1305_encrypt(
                session->write_keys.key, nonce, aad, 5, plaintext, len + 1, record + 5);
        if (ciphertext_len == 0) {
            session->error = "TLS: record encryption failed";
            rc = RT_TLS_ERROR;
            goto done;
        }
        write_u16(record + 3, (uint16_t)ciphertext_len);
        record_len = 5 + ciphertext_len;

        // H-8: RFC 8446 §5.5 — close before sequence number wraps (nonce uniqueness)
        if (++session->write_keys.seq_num == 0) {
            session->error =
                "TLS: write sequence number overflow; connection must be re-established";
            rc = RT_TLS_ERROR;
            goto done;
        }
    } else {
        // Plaintext record
        record[0] = content_type;
        write_u16(record + 1, TLS_VERSION_1_2);
        write_u16(record + 3, (uint16_t)len);
        if (len > 0)
            memcpy(record + 5, data, len);
        record_len = 5 + len;
    }

    size_t sent = 0;
    while (sent < record_len) {
        if (tls_io_should_stop(session)) {
            rc = RT_TLS_ERROR_SOCKET;
            goto done;
        }
        int n = send(session->socket_fd,
                     (const char *)(record + sent),
                     (int)(record_len - sent),
                     SEND_FLAGS);
        if (n < 0) {
            int err = GET_LAST_ERROR();
            if (rt_socket_error_is_interrupted(err))
                continue;
            if (session->io_cancel_requested &&
                (rt_socket_error_is_would_block(err) || rt_socket_error_is_timeout(err))) {
                if (tls_io_should_stop(session)) {
                    rc = RT_TLS_ERROR_SOCKET;
                    goto done;
                }
                continue;
            }
            if (rt_socket_error_is_would_block(err)) {
                int ready = wait_socket(session->socket_fd, session->timeout_ms, true);
                if (ready > 0)
                    continue;
                session->error = ready == 0 ? "TLS: send timeout" : "send failed";
                rc = RT_TLS_ERROR_SOCKET;
                goto done;
            }
            session->error = "send failed";
            rc = RT_TLS_ERROR_SOCKET;
            goto done;
        }
        if (n == 0) {
            session->error = "send returned zero";
            rc = RT_TLS_ERROR_SOCKET;
            goto done;
        }
        sent += n;
    }

done:
    if (plaintext) {
        rt_secure_zero(plaintext, (size_t)TLS_MAX_RECORD_SIZE + 1u);
        free(plaintext);
    }
    if (record) {
        rt_secure_zero(record, 5u + (size_t)TLS_MAX_CIPHERTEXT);
        free(record);
    }
    return rc;
}

/// @brief Split a logical TLS payload across legal record-sized fragments.
/// @param session TLS session owning socket and write keys.
/// @param content_type Inner or plaintext TLS content type.
/// @param data Payload bytes, or NULL only when @p len is zero.
/// @param len Total payload byte count.
/// @return @ref RT_TLS_OK after all fragments, or the first negative
///         record-send status.
static int send_record_fragmented(rt_tls_session_t *session,
                                  uint8_t content_type,
                                  const uint8_t *data,
                                  size_t len) {
    size_t pos = 0;
    if (len == 0)
        return send_record(session, content_type, data, 0);
    while (pos < len) {
        size_t chunk = len - pos;
        if (chunk > TLS_MAX_RECORD_SIZE)
            chunk = TLS_MAX_RECORD_SIZE;
        int rc = send_record(session, content_type, data + pos, chunk);
        if (rc != RT_TLS_OK)
            return rc;
        pos += chunk;
    }
    return RT_TLS_OK;
}

/// @brief Read, decrypt, and unframe one TLS record from the wire.
///
/// Reads the 5-byte record header, validates the length is within
/// the spec maximum (`TLS_MAX_CIPHERTEXT`), then drains the
/// payload. Once keys are established and the record is
/// `application_data`, AEAD-decrypts under the read key with the
/// header as AAD, strips the inner content-type byte (RFC 8446
/// §5.2), and returns the unwrapped type via `*content_type`.
/// Loops on partial `recv()` and treats `EINTR`/`EAGAIN` as retryable.
/// Bumps the read sequence number on every successfully decrypted
/// record and trips an error if it wraps.
/// @param session TLS session owning socket and read keys.
/// @param[out] content_type  Unwrapped TLS content type byte.
/// @param[out] data          Caller-provided payload buffer.
/// @param[out] data_len      Bytes written to `data` on success.
/// @return RT_TLS_OK, RT_TLS_ERROR (decrypt/length), RT_TLS_ERROR_SOCKET, or RT_TLS_ERROR_CLOSED.
static int recv_record(rt_tls_session_t *session,
                       uint8_t *content_type,
                       uint8_t *data,
                       size_t *data_len) {
    // Read header
    uint8_t header[5];
    size_t pos = 0;
    uint8_t *payload = NULL;
    int rc = RT_TLS_OK;
    while (pos < 5) {
        if (tls_io_should_stop(session))
            return RT_TLS_ERROR_SOCKET;
        int n = recv(session->socket_fd, (char *)(header + pos), (int)(5 - pos), 0);
        if (n < 0) {
            int err = GET_LAST_ERROR();
            if (rt_socket_error_is_interrupted(err))
                continue;
            if (session->io_cancel_requested &&
                (rt_socket_error_is_would_block(err) || rt_socket_error_is_timeout(err))) {
                if (tls_io_should_stop(session))
                    return RT_TLS_ERROR_SOCKET;
                continue;
            }
            if (rt_socket_error_is_would_block(err)) {
                int ready = wait_socket(session->socket_fd, session->timeout_ms, false);
                if (ready > 0)
                    continue;
                session->error = ready == 0 ? "TLS: recv timeout" : "recv header failed";
                return RT_TLS_ERROR_SOCKET;
            }
            session->error = "recv header failed";
            return RT_TLS_ERROR_SOCKET;
        }
        if (n == 0) {
            session->error = "connection closed";
            return RT_TLS_ERROR_CLOSED;
        }
        pos += n;
    }

    uint8_t type = header[0];
    size_t length = read_u16(header + 3);

    if (length > TLS_MAX_CIPHERTEXT) {
        session->error = "record too large";
        return RT_TLS_ERROR;
    }

    // Read payload
    payload = tls_alloc_record_scratch(session, length, "TLS: payload allocation failed");
    if (!payload)
        return RT_TLS_ERROR;
    pos = 0;
    while (pos < length) {
        if (tls_io_should_stop(session)) {
            rc = RT_TLS_ERROR_SOCKET;
            goto done;
        }
        int n = recv(session->socket_fd, (char *)(payload + pos), (int)(length - pos), 0);
        if (n < 0) {
            int err = GET_LAST_ERROR();
            if (rt_socket_error_is_interrupted(err))
                continue;
            if (session->io_cancel_requested &&
                (rt_socket_error_is_would_block(err) || rt_socket_error_is_timeout(err))) {
                if (tls_io_should_stop(session)) {
                    rc = RT_TLS_ERROR_SOCKET;
                    goto done;
                }
                continue;
            }
            if (rt_socket_error_is_would_block(err)) {
                int ready = wait_socket(session->socket_fd, session->timeout_ms, false);
                if (ready > 0)
                    continue;
                session->error = ready == 0 ? "TLS: recv timeout" : "recv payload failed";
                rc = RT_TLS_ERROR_SOCKET;
                goto done;
            }
            session->error = "recv payload failed";
            rc = RT_TLS_ERROR_SOCKET;
            goto done;
        }
        if (n == 0) {
            session->error = "connection closed";
            rc = RT_TLS_ERROR_CLOSED;
            goto done;
        }
        pos += n;
    }

    if (session->keys_established && type == TLS_CONTENT_APPLICATION) {
        // Decrypt
        uint8_t aad[5];
        memcpy(aad, header, 5);

        uint8_t nonce[12];
        build_nonce(session->read_keys.iv, session->read_keys.seq_num, nonce);

        long plaintext_len;
        if (session->cipher_suite == TLS_AES_128_GCM_SHA256)
            plaintext_len =
                rt_aes128_gcm_decrypt(session->read_keys.key, nonce, aad, 5, payload, length, data);
        else
            plaintext_len = rt_chacha20_poly1305_decrypt(
                session->read_keys.key, nonce, aad, 5, payload, length, data);
        if (plaintext_len < 0) {
            session->error = "decryption failed";
            rc = RT_TLS_ERROR;
            goto done;
        }

        // H-8: RFC 8446 §5.5 — close before sequence number wraps (nonce uniqueness)
        if (++session->read_keys.seq_num == 0) {
            session->error =
                "TLS: read sequence number overflow; connection must be re-established";
            rc = RT_TLS_ERROR;
            goto done;
        }

        // Remove padding and get inner content type
        while (plaintext_len > 0 && data[plaintext_len - 1] == 0)
            plaintext_len--;
        if (plaintext_len == 0) {
            session->error = "empty inner record";
            rc = RT_TLS_ERROR;
            goto done;
        }
        *content_type = data[plaintext_len - 1];
        *data_len = plaintext_len - 1;
    } else {
        // Plaintext record
        *content_type = type;
        memcpy(data, payload, length);
        *data_len = length;
    }

done:
    if (payload) {
        rt_secure_zero(payload, length);
        free(payload);
    }
    return rc;
}

/// @brief Construct and transmit a TLS 1.3 ClientHello message.
///
/// Lays out the legacy `client_version` (TLS 1.2 for compatibility),
/// 32-byte client random, empty session ID, the offered cipher
/// suites (`AES-128-GCM-SHA256` then `CHACHA20_POLY1305_SHA256`),
/// the null compression method, and the following extensions:
///   - `server_name` (when `session->hostname` is set, RFC 6066)
///   - `supported_versions` (TLS 1.3 only)
///   - `alpn` (optional single protocol, e.g. `http/1.1`)
///   - `supported_groups` (X25519 only — see RFC 7748)
///   - `signature_algorithms` (ECDSA P-256, RSA-PSS SHA-256/384/512)
///   - `cookie` (only when echoing back a HelloRetryRequest)
///   - `key_share` with a freshly generated X25519 public key
///
/// Wraps the body in a handshake header, mirrors it into the
/// transcript, snapshots `client_hello_hash` for HelloRetryRequest
/// folding, and hands the bytes to `send_record`. Advances the
/// session state to `TLS_STATE_CLIENT_HELLO_SENT` on success.
/// @param session Fresh or HelloRetryRequest client session.
/// @return @ref RT_TLS_OK on success or a negative handshake/record status.
static int send_client_hello(rt_tls_session_t *session) {
    uint8_t msg[1400];
    size_t pos = 0;

    // Legacy version
    write_u16(msg + pos, TLS_VERSION_1_2);
    pos += 2;

    // Random
    rt_crypto_random_bytes(session->client_random, 32);
    memcpy(msg + pos, session->client_random, 32);
    pos += 32;

    // Session ID (empty for TLS 1.3)
    msg[pos++] = 0;

    // Cipher suites (RFC 8446 §9.1: AES-128-GCM-SHA256 is mandatory)
    write_u16(msg + pos, 4); // 2 suites × 2 bytes
    pos += 2;
    write_u16(msg + pos, TLS_AES_128_GCM_SHA256);
    pos += 2;
    write_u16(msg + pos, TLS_CHACHA20_POLY1305_SHA256);
    pos += 2;

    // Compression methods
    msg[pos++] = 1;
    msg[pos++] = 0; // null

    // Extensions
    size_t ext_start = pos;
    pos += 2; // Length placeholder

    // SNI extension
    if (session->hostname[0] != '\0' && !tls_hostname_is_ip_literal(session->hostname)) {
        size_t name_len = strlen(session->hostname);
        if (pos + name_len + 11 > sizeof(msg) - 64) {
            session->error = "ClientHello: hostname too long";
            return RT_TLS_ERROR;
        }
        write_u16(msg + pos, TLS_EXT_SERVER_NAME);
        pos += 2;
        write_u16(msg + pos, (uint16_t)(name_len + 5));
        pos += 2;
        write_u16(msg + pos, (uint16_t)(name_len + 3));
        pos += 2;
        msg[pos++] = 0; // DNS hostname
        write_u16(msg + pos, (uint16_t)name_len);
        pos += 2;
        memcpy(msg + pos, session->hostname, name_len);
        pos += name_len;
    }

    // Supported versions
    write_u16(msg + pos, TLS_EXT_SUPPORTED_VERSIONS);
    pos += 2;
    write_u16(msg + pos, 3);
    pos += 2;
    msg[pos++] = 2;
    write_u16(msg + pos, TLS_VERSION_1_3);
    pos += 2;

    // ALPN (optional ordered preference list)
    if (session->alpn_protocols[0] != '\0') {
        size_t wire_len = 0;
        if (!tls_alpn_list_wire_len(session->alpn_protocols, &wire_len) || wire_len == 0 ||
            pos + wire_len + 6 > sizeof(msg) - 64) {
            session->error = "ClientHello: invalid ALPN protocol list";
            return RT_TLS_ERROR;
        }
        write_u16(msg + pos, TLS_EXT_ALPN);
        pos += 2;
        write_u16(msg + pos, (uint16_t)(wire_len + 2));
        pos += 2;
        write_u16(msg + pos, (uint16_t)wire_len);
        pos += 2;
        if (!tls_alpn_write_wire_list(
                msg + pos, sizeof(msg) - pos, session->alpn_protocols, &wire_len)) {
            session->error = "ClientHello: failed to encode ALPN list";
            return RT_TLS_ERROR;
        }
        pos += wire_len;
    }

    // Supported groups
    write_u16(msg + pos, TLS_EXT_SUPPORTED_GROUPS);
    pos += 2;
    write_u16(msg + pos, 4);
    pos += 2;
    write_u16(msg + pos, 2);
    pos += 2;
    write_u16(msg + pos, 0x001D); // x25519
    pos += 2;

    // Signature algorithms. Advertise only schemes implemented by CertificateVerify.
    write_u16(msg + pos, TLS_EXT_SIGNATURE_ALGORITHMS);
    pos += 2;
    write_u16(msg + pos, 10);
    pos += 2;
    write_u16(msg + pos, 8);
    pos += 2;
    write_u16(msg + pos, 0x0403); // ecdsa_secp256r1_sha256
    pos += 2;
    write_u16(msg + pos, 0x0804); // rsa_pss_rsae_sha256
    pos += 2;
    write_u16(msg + pos, 0x0805); // rsa_pss_rsae_sha384
    pos += 2;
    write_u16(msg + pos, 0x0806); // rsa_pss_rsae_sha512
    pos += 2;

    // HelloRetryRequest cookie, if any
    if (session->hello_retry_cookie && session->hello_retry_cookie_len > 0) {
        if (session->hello_retry_cookie_len > 0xFFFF - 2 ||
            pos + 6 + session->hello_retry_cookie_len > sizeof(msg)) {
            session->error = "ClientHello: retry cookie too large";
            return RT_TLS_ERROR;
        }
        write_u16(msg + pos, TLS_EXT_COOKIE);
        pos += 2;
        write_u16(msg + pos, (uint16_t)(2 + session->hello_retry_cookie_len));
        pos += 2;
        write_u16(msg + pos, (uint16_t)session->hello_retry_cookie_len);
        pos += 2;
        memcpy(msg + pos, session->hello_retry_cookie, session->hello_retry_cookie_len);
        pos += session->hello_retry_cookie_len;
    }

    // Key share (X25519)
    rt_x25519_keygen(session->client_private_key, session->client_public_key);

    write_u16(msg + pos, TLS_EXT_KEY_SHARE);
    pos += 2;
    // KeyShareClientHello wraps the KeyShareEntry in a length-prefixed vector:
    // 2 bytes vector length + (2 bytes group + 2 bytes key len + 32 bytes key).
    write_u16(msg + pos, 38);
    pos += 2;
    write_u16(msg + pos, 36); // client_shares vector length
    pos += 2;
    write_u16(msg + pos, 0x001D); // x25519
    pos += 2;
    write_u16(msg + pos, 32);
    pos += 2;
    memcpy(msg + pos, session->client_public_key, 32);
    pos += 32;

    // Fill in extensions length
    write_u16(msg + ext_start, (uint16_t)(pos - ext_start - 2));

    // Wrap in handshake header
    uint8_t hs[4 + 1400];
    hs[0] = TLS_HS_CLIENT_HELLO;
    write_u24(hs + 1, (uint32_t)pos);
    memcpy(hs + 4, msg, pos);

    // Update transcript
    if (transcript_update(session, hs, 4 + pos) != 0)
        return RT_TLS_ERROR_HANDSHAKE;
    if (!session->have_client_hello_hash) {
        memcpy(session->client_hello_hash, session->transcript_hash, 32);
        session->have_client_hello_hash = 1;
    }

    // Send
    int rc = send_record_fragmented(session, TLS_CONTENT_HANDSHAKE, hs, 4 + pos);
    if (rc != RT_TLS_OK)
        return rc;

    session->state = TLS_STATE_CLIENT_HELLO_SENT;
    return RT_TLS_OK;
}

/// @brief Parse a ServerHello (or HelloRetryRequest) and finish key-exchange.
///
/// Validates the message body field by field with strict bounds
/// checks (each advance is guarded so a malformed length cannot
/// overrun the record). Detects HelloRetryRequest by the magic
/// `random` value defined in RFC 8446 §4.1.3. Reads the chosen
/// cipher suite (must be AES-128-GCM-SHA256 or
/// CHACHA20-POLY1305-SHA256) and walks the extension block to find
/// `supported_versions` (must be TLS 1.3), `key_share` (X25519
/// public point), and a `cookie` for HRR.
///
/// On a HelloRetryRequest path: substitutes the synthetic
/// "message_hash" record into the transcript per RFC 8446 §4.4.1,
/// stashes the cookie, and instructs the caller to resend the
/// ClientHello.
///
/// On a real ServerHello: completes the X25519 ECDHE shared secret
/// using the client's stored private key, derives handshake traffic
/// keys via `derive_handshake_keys`, mixes the message into the
/// transcript, and advances the state machine.
/// @param session Client session receiving negotiated state.
/// @param data       Pointer to the ServerHello body (after handshake header).
/// @param len        Length of the body.
/// @param full_msg   Pointer to the full handshake message including header.
/// @param full_len   Length of the full message (used for transcript update).
/// @return RT_TLS_OK, RT_TLS_RETRY (caller should resend ClientHello),
///         or an error code on malformed / unsupported input.
static int process_server_hello(rt_tls_session_t *session,
                                const uint8_t *data,
                                size_t len,
                                const uint8_t *full_msg,
                                size_t full_len) {
    if (len < 38) {
        session->error = "ServerHello too short";
        return RT_TLS_ERROR_HANDSHAKE;
    }

    // Skip version (2)
    memcpy(session->server_random, data + 2, 32);
    int is_hrr =
        memcmp(session->server_random, TLS_HELLO_RETRY_RANDOM, sizeof(TLS_HELLO_RETRY_RANDOM)) == 0;

    size_t pos = 34;

    // Session ID — the earlier minimum-length check guarantees this field exists.
    uint8_t session_id_len = data[pos++];
    if (pos + session_id_len + 3 > len) /* +3 for cipher(2) + compression(1) */
    {
        session->error = "ServerHello: session_id overflows message";
        return RT_TLS_ERROR_HANDSHAKE;
    }
    pos += session_id_len;

    // Cipher suite
    session->cipher_suite = read_u16(data + pos);
    pos += 2;

    if (session->cipher_suite != TLS_AES_128_GCM_SHA256 &&
        session->cipher_suite != TLS_CHACHA20_POLY1305_SHA256) {
        session->error = "unsupported cipher suite";
        return RT_TLS_ERROR_HANDSHAKE;
    }

    // Skip compression
    pos++;

    // Parse extensions
    if (pos + 2 > len) {
        session->error = "no extensions";
        return RT_TLS_ERROR_HANDSHAKE;
    }
    uint16_t ext_len = read_u16(data + pos);
    pos += 2;
    if ((size_t)ext_len > len - pos) {
        session->error = "ServerHello extensions overflow message";
        return RT_TLS_ERROR_HANDSHAKE;
    }

    size_t ext_end = pos + ext_len;
    uint16_t selected_group = 0;
    int found_key_share = 0;
    int found_cookie = 0;
    int found_supported_versions = 0; // M-11: track TLS 1.3 version confirmation

    while (pos + 4 <= ext_end) {
        uint16_t ext_type = read_u16(data + pos);
        uint16_t ext_data_len = read_u16(data + pos + 2);
        pos += 4;
        if (pos + ext_data_len > ext_end) {
            session->error = "ServerHello extension overflows message";
            return RT_TLS_ERROR_HANDSHAKE;
        }

        if (ext_type == TLS_EXT_KEY_SHARE) {
            if (is_hrr) {
                if (ext_data_len != 2) {
                    session->error = "HelloRetryRequest key_share malformed";
                    return RT_TLS_ERROR_HANDSHAKE;
                }
                selected_group = read_u16(data + pos);
                found_key_share = 1;
            } else {
                if (ext_data_len != 36) {
                    session->error = "ServerHello key_share malformed";
                    return RT_TLS_ERROR_HANDSHAKE;
                }
                uint16_t group = read_u16(data + pos);
                uint16_t key_len = read_u16(data + pos + 2);
                if ((size_t)key_len + 4 != ext_data_len) {
                    session->error = "ServerHello key_share length mismatch";
                    return RT_TLS_ERROR_HANDSHAKE;
                }
                if (group == 0x001D && key_len == 32) {
                    memcpy(session->server_public_key, data + pos + 4, 32);
                    selected_group = group;
                    found_key_share = 1;
                }
            }
        } else if (ext_type == TLS_EXT_COOKIE && is_hrr && ext_data_len >= 2) {
            uint16_t cookie_len = read_u16(data + pos);
            if ((size_t)cookie_len + 2 != ext_data_len) {
                session->error = "HelloRetryRequest cookie length mismatch";
                return RT_TLS_ERROR_HANDSHAKE;
            }
            uint8_t *cookie = (uint8_t *)malloc(cookie_len);
            if (!cookie) {
                session->error = "HelloRetryRequest cookie allocation failed";
                return RT_TLS_ERROR_MEMORY;
            }
            memcpy(cookie, data + pos + 2, cookie_len);
            if (session->hello_retry_cookie) {
                free(session->hello_retry_cookie);
                session->hello_retry_cookie = NULL;
                session->hello_retry_cookie_len = 0;
            }
            session->hello_retry_cookie = cookie;
            session->hello_retry_cookie_len = cookie_len;
            found_cookie = 1;
        }
        // M-11: RFC 8446 §4.2.1 — ServerHello must confirm TLS 1.3 via supported_versions
        else if (ext_type == TLS_EXT_SUPPORTED_VERSIONS && ext_data_len == 2) {
            uint16_t negotiated = read_u16(data + pos);
            if (negotiated == TLS_VERSION_1_3)
                found_supported_versions = 1;
        }
        pos += ext_data_len;
    }
    if (pos != ext_end) {
        session->error = "ServerHello extension header truncated";
        return RT_TLS_ERROR_HANDSHAKE;
    }

    if (!found_key_share) {
        session->error = "no key share";
        return RT_TLS_ERROR_HANDSHAKE;
    }

    if (is_hrr) {
        if (session->hello_retry_seen) {
            session->error = "TLS: multiple HelloRetryRequest messages are not supported";
            return RT_TLS_ERROR_HANDSHAKE;
        }
        if (selected_group != 0x001D) {
            session->error = "TLS: HelloRetryRequest selected an unsupported key share group";
            return RT_TLS_ERROR_HANDSHAKE;
        }
        if (!session->have_client_hello_hash) {
            session->error =
                "TLS: missing initial ClientHello transcript hash for HelloRetryRequest";
            return RT_TLS_ERROR_HANDSHAKE;
        }

        uint8_t message_hash[4 + 32];
        message_hash[0] = 0xFE; // message_hash synthetic handshake message
        write_u24(message_hash + 1, 32);
        memcpy(message_hash + 4, session->client_hello_hash, 32);

        transcript_init(session);
        if (transcript_update(session, message_hash, sizeof(message_hash)) != 0 ||
            transcript_update(session, full_msg, full_len) != 0) {
            return RT_TLS_ERROR_HANDSHAKE;
        }

        session->hello_retry_seen = 1;
        (void)found_cookie;
        return send_client_hello(session);
    }

    // M-11: Reject the handshake if the server didn't confirm TLS 1.3.
    // A middlebox performing a version downgrade attack would omit this extension.
    if (!found_supported_versions) {
        session->error = "TLS: ServerHello missing supported_versions=TLS1.3 (version downgrade?)";
        return RT_TLS_ERROR_HANDSHAKE;
    }

    // Compute shared secret and derive handshake keys
    uint8_t shared_secret[32];
    if (rt_x25519(session->client_private_key, session->server_public_key, shared_secret) != 0) {
        rt_secure_zero(shared_secret, sizeof(shared_secret));
        session->error = "TLS: invalid X25519 server key share";
        return RT_TLS_ERROR_HANDSHAKE;
    }
    derive_handshake_keys(session, shared_secret);
    rt_secure_zero(shared_secret, sizeof(shared_secret));

    session->state = TLS_STATE_WAIT_ENCRYPTED_EXTENSIONS;
    return RT_TLS_OK;
}

/// @brief Process the server EncryptedExtensions message (RFC 8446 §4.3.1).
///        Currently handles the ALPN extension: records the negotiated protocol name in
///        session->negotiated_alpn. Ignores unrecognised extensions.
/// @param session Client session receiving negotiated extensions.
/// @param data EncryptedExtensions body after its handshake header.
/// @param len Number of body bytes.
/// @return @ref RT_TLS_OK on success or @ref RT_TLS_ERROR_HANDSHAKE.
static int process_encrypted_extensions(rt_tls_session_t *session,
                                        const uint8_t *data,
                                        size_t len) {
    if (len < 2) {
        session->error = "EncryptedExtensions too short";
        return RT_TLS_ERROR_HANDSHAKE;
    }

    size_t ext_len = read_u16(data);
    if (ext_len != len - 2) {
        session->error = "EncryptedExtensions length mismatch";
        return RT_TLS_ERROR_HANDSHAKE;
    }

    const uint8_t *p = data + 2;
    const uint8_t *end = data + len;
    while (p + 4 <= end) {
        uint16_t ext_type = read_u16(p);
        uint16_t one_ext_len = read_u16(p + 2);
        p += 4;
        if ((size_t)(end - p) < one_ext_len) {
            session->error = "EncryptedExtensions truncated";
            return RT_TLS_ERROR_HANDSHAKE;
        }

        if (ext_type == TLS_EXT_ALPN) {
            if (one_ext_len < 3) {
                session->error = "EncryptedExtensions ALPN too short";
                return RT_TLS_ERROR_HANDSHAKE;
            }
            size_t list_len = read_u16(p);
            if (list_len + 2 != one_ext_len || list_len < 1) {
                session->error = "EncryptedExtensions ALPN malformed";
                return RT_TLS_ERROR_HANDSHAKE;
            }
            uint8_t proto_len = p[2];
            if ((size_t)proto_len + 3 != one_ext_len || proto_len == 0) {
                session->error = "EncryptedExtensions ALPN protocol malformed";
                return RT_TLS_ERROR_HANDSHAKE;
            }
            if (session->alpn_protocols[0] != '\0' &&
                !tls_alpn_list_contains(session->alpn_protocols, p + 3, proto_len)) {
                session->error = "EncryptedExtensions ALPN mismatch";
                return RT_TLS_ERROR_HANDSHAKE;
            }
            if (proto_len >= sizeof(session->negotiated_alpn)) {
                session->error = "EncryptedExtensions ALPN protocol too long";
                return RT_TLS_ERROR_HANDSHAKE;
            }
            memcpy(session->negotiated_alpn, p + 3, proto_len);
            session->negotiated_alpn[proto_len] = '\0';
        }

        p += one_ext_len;
    }

    if (p != end) {
        session->error = "EncryptedExtensions trailing bytes";
        return RT_TLS_ERROR_HANDSHAKE;
    }

    session->state = TLS_STATE_WAIT_CERTIFICATE;
    return RT_TLS_OK;
}

/// @brief Dispatch a post-handshake TLS 1.3 message received after the handshake completes.
///        Handles NewSessionTicket (silently ignored) and KeyUpdate (ratchets read keys;
///        if update_requested=1, ratchets write keys and responds with a KeyUpdate).
/// @param session Established client or server session.
/// @param hs_type TLS handshake message type.
/// @param hs_data Handshake body bytes.
/// @param hs_len Number of body bytes.
/// @return @ref RT_TLS_OK for handled or ignored messages, or a negative
///         status for malformed KeyUpdate or response failure.
static int process_post_handshake_message(rt_tls_session_t *session,
                                          uint8_t hs_type,
                                          const uint8_t *hs_data,
                                          size_t hs_len) {
    switch (hs_type) {
        case 4: // NewSessionTicket
            return RT_TLS_OK;

        case 24: { // KeyUpdate
            if (hs_len != 1 || hs_data[0] > 1) {
                session->error = "TLS: malformed KeyUpdate";
                return RT_TLS_ERROR_HANDSHAKE;
            }

            update_read_application_keys(session);
            if (hs_data[0] == 1) {
                int rc = send_key_update_record(session, 0);
                if (rc != RT_TLS_OK)
                    return rc;
                update_write_application_keys(session);
            }
            return RT_TLS_OK;
        }

        default:
            return RT_TLS_OK;
    }
}

/// @brief Compute and send the client Finished handshake message.
///
/// Derives the per-direction "finished_key" via HKDF-Expand-Label
/// from the client handshake traffic secret, MACs the current
/// transcript hash with HMAC-SHA256, and emits a Finished record.
/// The transcript is updated *before* sending so the application
/// keys derived afterwards mix in this message (RFC 8446 §4.4.4).
/// @param session Session whose transcript and socket are updated.
/// @param base_secret Directional 32-byte handshake traffic secret.
/// @return @ref RT_TLS_OK on success or a negative record/transcript status.
static int send_finished_with_secret(rt_tls_session_t *session, const uint8_t base_secret[32]) {
    uint8_t finished_key[32];
    rt_hkdf_expand_label(base_secret, "finished", NULL, 0, finished_key, 32);

    uint8_t verify_data[32];
    rt_hmac_sha256(finished_key, 32, session->transcript_hash, 32, verify_data);

    uint8_t msg[4 + 32];
    msg[0] = TLS_HS_FINISHED;
    write_u24(msg + 1, 32);
    memcpy(msg + 4, verify_data, 32);

    {
        int rc = send_record(session, TLS_CONTENT_HANDSHAKE, msg, 36);
        if (rc != RT_TLS_OK)
            return rc;
    }

    return transcript_update(session, msg, 36) == 0 ? RT_TLS_OK : RT_TLS_ERROR_HANDSHAKE;
}

/// @brief Send the client Finished message using its handshake traffic secret.
/// @param session Client session.
/// @return @ref RT_TLS_OK on success or a negative send/transcript status.
static int send_finished(rt_tls_session_t *session) {
    return send_finished_with_secret(session, session->client_handshake_traffic_secret);
}

/// @brief Send the server Finished message using its handshake traffic secret.
/// @param session Server session.
/// @return @ref RT_TLS_OK on success or a negative send/transcript status.
static int send_finished_server(rt_tls_session_t *session) {
    return send_finished_with_secret(session, session->server_handshake_traffic_secret);
}

/// @brief Constant-time byte comparison used for MAC and verify_data checks.
///
/// Walks all `n` bytes regardless of where they diverge so the
/// running time leaks no information about the position or count of
/// matching bytes. Required by H-9 to prevent timing side-channels
/// against the Finished MAC and any other secret-dependent compare.
/// @param a First byte span.
/// @param b Second byte span.
/// @param n Common byte count.
/// @return 0 if `a` and `b` are byte-identical, non-zero otherwise.
static int ct_memcmp(const uint8_t *a, const uint8_t *b, size_t n) {
    uint8_t diff = 0;
    for (size_t i = 0; i < n; i++)
        diff |= a[i] ^ b[i];
    return diff != 0; // non-zero means unequal
}

/// @brief Validate the server's Finished verify_data against the transcript.
///
/// Recomputes HMAC-SHA256(finished_key, transcript_hash) using the
/// server handshake traffic secret, then compares it to the
/// received `data` in constant time. A mismatch means the handshake
/// has been tampered with or the server doesn't share our key
/// schedule.
/// @param session Session providing the current transcript hash.
/// @param base_secret Directional 32-byte handshake traffic secret.
/// @param data Received Finished verify_data.
/// @param len Number of verify_data bytes.
/// @return RT_TLS_OK on match, RT_TLS_ERROR_HANDSHAKE on failure.
static int verify_finished_with_secret(rt_tls_session_t *session,
                                       const uint8_t base_secret[32],
                                       const uint8_t *data,
                                       size_t len) {
    if (len != 32) {
        session->error = "invalid Finished length";
        return RT_TLS_ERROR_HANDSHAKE;
    }

    uint8_t finished_key[32];
    rt_hkdf_expand_label(base_secret, "finished", NULL, 0, finished_key, 32);

    uint8_t expected[32];
    rt_hmac_sha256(finished_key, 32, session->transcript_hash, 32, expected);

    if (ct_memcmp(data, expected, 32)) {
        session->error = "Finished verification failed";
        return RT_TLS_ERROR_HANDSHAKE;
    }

    return RT_TLS_OK;
}

/// @brief Verify server Finished using the server handshake traffic secret.
/// @param session Client session.
/// @param data Received Finished verify_data.
/// @param len Number of verify_data bytes.
/// @return @ref RT_TLS_OK on match; otherwise @ref RT_TLS_ERROR_HANDSHAKE.
static int verify_finished(rt_tls_session_t *session, const uint8_t *data, size_t len) {
    return verify_finished_with_secret(
        session, session->server_handshake_traffic_secret, data, len);
}

/// @brief Verify client Finished using the client handshake traffic secret.
/// @param session Server session.
/// @param data Received Finished verify_data.
/// @param len Number of verify_data bytes.
/// @return @ref RT_TLS_OK on match; otherwise @ref RT_TLS_ERROR_HANDSHAKE.
static int verify_finished_server(rt_tls_session_t *session, const uint8_t *data, size_t len) {
    return verify_finished_with_secret(
        session, session->client_handshake_traffic_secret, data, len);
}

/// @brief Return 1 if the ClientHello signature_algorithms extension list contains wanted_scheme.
///        list points to the wire-format list body (pairs of uint16 scheme codes).
/// @param list Wire-format sequence of two-byte signature schemes.
/// @param list_len Number of bytes in @p list.
/// @param wanted_scheme Scheme code to find.
/// @return One for an exact offered scheme; otherwise zero.
static int clienthello_offers_sig_scheme(const uint8_t *list,
                                         size_t list_len,
                                         uint16_t wanted_scheme) {
    if (list_len < 2 || (list_len & 1) != 0)
        return 0;
    for (size_t i = 0; i + 1 < list_len; i += 2) {
        if (read_u16(list + i) == wanted_scheme)
            return 1;
    }
    return 0;
}

/// @brief Iterate through one token of a comma-separated ALPN preference string.
///        Advances *offset_io past the consumed token and its trailing comma.
///        Returns 1 if a non-empty token was found; 0 when the list is exhausted.
/// @param list NUL-terminated comma-separated protocol list.
/// @param offset_io In/out parsing offset.
/// @param token_out Destination for a borrowed token start.
/// @param token_len_out Destination for token byte length.
/// @return One when a token is produced; zero when exhausted or invalid.
static int tls_alpn_list_next_token(const char *list,
                                    size_t *offset_io,
                                    const char **token_out,
                                    size_t *token_len_out) {
    size_t offset = offset_io ? *offset_io : 0;
    const char *start = NULL;
    const char *end = NULL;

    if (!list)
        return 0;

    while (list[offset] == ' ' || list[offset] == '\t' || list[offset] == ',')
        offset++;
    if (list[offset] == '\0') {
        if (offset_io)
            *offset_io = offset;
        return 0;
    }

    start = list + offset;
    while (list[offset] != '\0' && list[offset] != ',')
        offset++;
    end = list + offset;
    while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
        end--;
    while (list[offset] == ',')
        offset++;

    if (offset_io)
        *offset_io = offset;
    if (token_out)
        *token_out = start;
    if (token_len_out)
        *token_len_out = (size_t)(end - start);
    return end > start;
}

/// @brief Test whether the ClientHello's wire-format ALPN list contains
/// `wanted_protocol[0..wanted_len)`.
/// @details Takes an explicit (ptr, len) pair for the wanted token
///          because the caller (`tls_select_alpn_from_wire_list`)
///          passes a substring pointer into a larger comma-separated
///          preferred list — that substring is *not* NUL-terminated
///          at its end, so `strlen()` would over-read. Bounded-length
///          comparison is the only correct form here.
/// @param list Client wire-format vector of length-prefixed protocols.
/// @param list_len Number of bytes in @p list.
/// @param wanted_protocol Protocol bytes to find.
/// @param wanted_len Number of wanted protocol bytes.
/// @return One when offered; otherwise zero.
static int clienthello_offers_alpn(const uint8_t *list,
                                   size_t list_len,
                                   const char *wanted_protocol,
                                   size_t wanted_len) {
    size_t pos = 0;
    if (!wanted_protocol || wanted_len == 0)
        return 1;
    while (pos < list_len) {
        uint8_t one_len = list[pos++];
        if (pos + one_len > list_len)
            return 0;
        if (one_len == wanted_len && memcmp(list + pos, wanted_protocol, wanted_len) == 0)
            return 1;
        pos += one_len;
    }
    return 0;
}

/// @brief Find a raw protocol token in a comma-separated ALPN list.
/// @param list NUL-terminated configured preference list.
/// @param wanted Protocol bytes, not necessarily NUL-terminated.
/// @param wanted_len Number of protocol bytes.
/// @return One for an exact token match; otherwise zero.
static int tls_alpn_list_contains(const char *list, const uint8_t *wanted, size_t wanted_len) {
    size_t offset = 0;
    const char *token = NULL;
    size_t token_len = 0;

    if (!wanted || wanted_len == 0)
        return 0;

    while (tls_alpn_list_next_token(list, &offset, &token, &token_len)) {
        if (token_len == wanted_len && memcmp(token, wanted, wanted_len) == 0)
            return 1;
    }
    return 0;
}

/// @brief Compute the TLS wire-format byte length for all protocols in a comma-separated ALPN list.
///        Each protocol entry adds 1-byte length prefix + protocol name bytes.
///        Returns 1 on success; 0 if any token is empty, >255 bytes, or causes overflow.
/// @param list NUL-terminated configured preference list.
/// @param wire_len_out Optional destination for encoded byte length.
/// @return One for a valid encodable list; otherwise zero.
static int tls_alpn_list_wire_len(const char *list, size_t *wire_len_out) {
    size_t offset = 0;
    size_t wire_len = 0;

    if (wire_len_out)
        *wire_len_out = 0;
    if (!list || !*list)
        return 1;

    while (list[offset] != '\0') {
        while (list[offset] == ' ' || list[offset] == '\t')
            offset++;
        if (list[offset] == '\0' || list[offset] == ',')
            return 0;

        size_t token_start = offset;
        while (list[offset] != '\0' && list[offset] != ',')
            offset++;
        size_t token_end = offset;
        while (token_end > token_start &&
               (list[token_end - 1] == ' ' || list[token_end - 1] == '\t'))
            token_end--;
        size_t token_len = token_end - token_start;
        if (token_len == 0 || token_len > 255)
            return 0;
        if (wire_len > SIZE_MAX - token_len - 1)
            return 0;
        wire_len += token_len + 1;

        if (list[offset] == ',') {
            offset++;
            if (list[offset] == '\0')
                return 0;
        }
    }

    if (wire_len_out)
        *wire_len_out = wire_len;
    return 1;
}

/// @brief Encode a comma-separated ALPN list into TLS wire format in dst.
///        Each token is written as a 1-byte length prefix followed by the protocol name.
///        Returns 1 on success; 0 if the output would exceed dst_cap.
/// @param dst Destination wire buffer.
/// @param dst_cap Destination capacity.
/// @param list NUL-terminated configured preference list.
/// @param wire_len_out Optional destination for bytes written.
/// @return One on success; zero for invalid tokens or insufficient capacity.
static int tls_alpn_write_wire_list(uint8_t *dst,
                                    size_t dst_cap,
                                    const char *list,
                                    size_t *wire_len_out) {
    size_t offset = 0;
    const char *token = NULL;
    size_t token_len = 0;
    size_t wire_len = 0;

    if (wire_len_out)
        *wire_len_out = 0;
    if (!list || !*list)
        return 1;

    while (tls_alpn_list_next_token(list, &offset, &token, &token_len)) {
        if (token_len == 0 || token_len > 255 || wire_len + token_len + 1 > dst_cap)
            return 0;
        dst[wire_len++] = (uint8_t)token_len;
        memcpy(dst + wire_len, token, token_len);
        wire_len += token_len;
    }

    if (wire_len_out)
        *wire_len_out = wire_len;
    return 1;
}

/// @brief Select the first protocol from preferred_list that the client also offered in wire_list.
///        Iterates preferred_list in order so the server's preference takes precedence.
///        Writes the selected name (NUL-terminated) to selected_out[64] and returns 1 on match;
///        returns 0 when there is no mutual protocol.
/// @param preferred_list Server's comma-separated preference list.
/// @param wire_list Client's length-prefixed ALPN protocol vector.
/// @param wire_list_len Number of client vector bytes.
/// @param selected_out Destination 64-byte NUL-terminated selection buffer.
/// @return One when a mutual protocol is selected; otherwise zero.
static int tls_select_alpn_from_wire_list(const char *preferred_list,
                                          const uint8_t *wire_list,
                                          size_t wire_list_len,
                                          char selected_out[64]) {
    size_t offset = 0;
    const char *token = NULL;
    size_t token_len = 0;

    if (!selected_out)
        return 0;
    selected_out[0] = '\0';
    if (!preferred_list || !*preferred_list || !wire_list || wire_list_len == 0)
        return 0;

    while (tls_alpn_list_next_token(preferred_list, &offset, &token, &token_len)) {
        if (token_len == 0 || token_len >= 64)
            continue;
        if (clienthello_offers_alpn(wire_list, wire_list_len, token, token_len)) {
            memcpy(selected_out, token, token_len);
            selected_out[token_len] = '\0';
            return 1;
        }
    }
    return 0;
}

/// @brief Parse a TLS 1.3 ClientHello from the server's perspective.
///        Validates protocol version, random, cipher suites, compression, and all
///        required extensions (supported_versions with 0x0304, key_share with X25519,
///        signature_algorithms with a supported scheme). Records the client key-share
///        public key in session->peer_public_key and negotiates ALPN when configured.
///        Returns RT_TLS_OK or RT_TLS_ERROR_HANDSHAKE with session->error set.
/// @param session Server session receiving ClientHello negotiation state.
/// @param data ClientHello body after the handshake header.
/// @param len Number of body bytes.
/// @return @ref RT_TLS_OK on success or a negative argument/handshake status.
static int parse_client_hello(rt_tls_session_t *session, const uint8_t *data, size_t len) {
    size_t pos = 0;
    int found_tls13 = 0;
    int found_key_share = 0;
    int found_sig_alg = 0;
    int offered_aes = 0;
    int offered_chacha = 0;
    int compression_ok = 0;
    uint16_t wanted_sig_scheme = 0;

    if (!session || !session->server_ctx) {
        return RT_TLS_ERROR_INVALID_ARG;
    }
    session->negotiated_alpn[0] = '\0';

    switch (session->server_ctx->key_type) {
        case TLS_SERVER_KEY_ECDSA_P256:
            wanted_sig_scheme = 0x0403;
            break;
        case TLS_SERVER_KEY_RSA_PSS_SHA256:
            wanted_sig_scheme = 0x0806;
            break;
        default:
            session->error = "TLS: server certificate key type is not configured";
            return RT_TLS_ERROR_HANDSHAKE;
    }

    if (len < 42) {
        session->error = "ClientHello too short";
        return RT_TLS_ERROR_HANDSHAKE;
    }

    pos += 2; // legacy_version
    memcpy(session->client_random, data + pos, 32);
    pos += 32;

    session->legacy_session_id_len = data[pos++];
    if (session->legacy_session_id_len > sizeof(session->legacy_session_id) ||
        pos + session->legacy_session_id_len + 2 > len) {
        session->error = "ClientHello session id malformed";
        return RT_TLS_ERROR_HANDSHAKE;
    }
    memcpy(session->legacy_session_id, data + pos, session->legacy_session_id_len);
    pos += session->legacy_session_id_len;

    {
        uint16_t suites_len = read_u16(data + pos);
        pos += 2;
        if (pos + suites_len + 1 > len || suites_len < 2 || (suites_len & 1) != 0) {
            session->error = "ClientHello cipher_suites malformed";
            return RT_TLS_ERROR_HANDSHAKE;
        }
        for (size_t i = 0; i < suites_len; i += 2) {
            uint16_t suite = read_u16(data + pos + i);
            if (suite == TLS_AES_128_GCM_SHA256)
                offered_aes = 1;
            else if (suite == TLS_CHACHA20_POLY1305_SHA256)
                offered_chacha = 1;
        }
        pos += suites_len;
    }

    {
        uint8_t compression_len = data[pos++];
        if (pos + compression_len + 2 > len || compression_len == 0) {
            session->error = "ClientHello compression_methods malformed";
            return RT_TLS_ERROR_HANDSHAKE;
        }
        for (size_t i = 0; i < compression_len; i++) {
            if (data[pos + i] == 0)
                compression_ok = 1;
        }
        pos += compression_len;
    }

    if (!compression_ok) {
        session->error = "ClientHello missing null compression";
        return RT_TLS_ERROR_HANDSHAKE;
    }

    {
        uint16_t ext_len = read_u16(data + pos);
        size_t ext_end;
        pos += 2;
        if (pos + ext_len != len) {
            session->error = "ClientHello extensions malformed";
            return RT_TLS_ERROR_HANDSHAKE;
        }
        ext_end = pos + ext_len;

        while (pos + 4 <= ext_end) {
            uint16_t ext_type = read_u16(data + pos);
            uint16_t one_len = read_u16(data + pos + 2);
            pos += 4;
            if (pos + one_len > ext_end) {
                session->error = "ClientHello extension truncated";
                return RT_TLS_ERROR_HANDSHAKE;
            }

            if (ext_type == TLS_EXT_SERVER_NAME) {
                int rc = tls_parse_client_sni(session, data + pos, one_len);
                if (rc != RT_TLS_OK)
                    return rc;
            } else if (ext_type == TLS_EXT_SUPPORTED_VERSIONS) {
                if (one_len < 3) {
                    session->error = "ClientHello supported_versions malformed";
                    return RT_TLS_ERROR_HANDSHAKE;
                }
                {
                    uint8_t list_len = data[pos];
                    if ((size_t)list_len + 1 != one_len || (list_len & 1) != 0) {
                        session->error = "ClientHello supported_versions length mismatch";
                        return RT_TLS_ERROR_HANDSHAKE;
                    }
                    for (size_t i = 0; i + 1 < list_len; i += 2) {
                        if (read_u16(data + pos + 1 + i) == TLS_VERSION_1_3)
                            found_tls13 = 1;
                    }
                }
            } else if (ext_type == TLS_EXT_SIGNATURE_ALGORITHMS) {
                if (one_len < 2) {
                    session->error = "ClientHello signature_algorithms malformed";
                    return RT_TLS_ERROR_HANDSHAKE;
                }
                {
                    uint16_t list_len = read_u16(data + pos);
                    if ((size_t)list_len + 2 != one_len) {
                        session->error = "ClientHello signature_algorithms length mismatch";
                        return RT_TLS_ERROR_HANDSHAKE;
                    }
                    if (session->server_ctx->key_type == TLS_SERVER_KEY_RSA_PSS_SHA256) {
                        if (clienthello_offers_sig_scheme(data + pos + 2, list_len, 0x0806))
                            session->server_sig_scheme = 0x0806;
                        else if (clienthello_offers_sig_scheme(data + pos + 2, list_len, 0x0805))
                            session->server_sig_scheme = 0x0805;
                        else if (clienthello_offers_sig_scheme(data + pos + 2, list_len, 0x0804))
                            session->server_sig_scheme = 0x0804;
                        else {
                            session->error =
                                "ClientHello does not offer an RSA-PSS signature algorithm";
                            return RT_TLS_ERROR_HANDSHAKE;
                        }
                    } else if (clienthello_offers_sig_scheme(
                                   data + pos + 2, list_len, wanted_sig_scheme)) {
                        session->server_sig_scheme = wanted_sig_scheme;
                    } else {
                        session->error = "ClientHello does not offer ecdsa_secp256r1_sha256";
                        return RT_TLS_ERROR_HANDSHAKE;
                    }
                    found_sig_alg = 1;
                }
            } else if (ext_type == TLS_EXT_KEY_SHARE) {
                if (one_len < 2) {
                    session->error = "ClientHello key_share malformed";
                    return RT_TLS_ERROR_HANDSHAKE;
                }
                {
                    uint16_t list_len = read_u16(data + pos);
                    size_t share_pos = pos + 2;
                    size_t share_end = share_pos + list_len;
                    if ((size_t)list_len + 2 != one_len || share_end > ext_end) {
                        session->error = "ClientHello key_share length mismatch";
                        return RT_TLS_ERROR_HANDSHAKE;
                    }
                    while (share_pos + 4 <= share_end) {
                        uint16_t group = read_u16(data + share_pos);
                        uint16_t key_len = read_u16(data + share_pos + 2);
                        share_pos += 4;
                        if (share_pos + key_len > share_end) {
                            session->error = "ClientHello key_share truncated";
                            return RT_TLS_ERROR_HANDSHAKE;
                        }
                        if (group == 0x001D && key_len == 32) {
                            memcpy(session->server_public_key, data + share_pos, 32);
                            found_key_share = 1;
                        }
                        share_pos += key_len;
                    }
                }
            } else if (ext_type == TLS_EXT_ALPN) {
                if (one_len < 3) {
                    session->error = "ClientHello ALPN malformed";
                    return RT_TLS_ERROR_HANDSHAKE;
                }
                {
                    uint16_t list_len = read_u16(data + pos);
                    if ((size_t)list_len + 2 != one_len) {
                        session->error = "ClientHello ALPN malformed";
                        return RT_TLS_ERROR_HANDSHAKE;
                    }
                    if (session->server_ctx->alpn_protocols[0] != '\0') {
                        (void)tls_select_alpn_from_wire_list(session->server_ctx->alpn_protocols,
                                                             data + pos + 2,
                                                             list_len,
                                                             session->negotiated_alpn);
                    }
                }
            }

            pos += one_len;
        }
    }

    if (!found_tls13) {
        session->error = "ClientHello does not offer TLS 1.3";
        return RT_TLS_ERROR_HANDSHAKE;
    }
    if (!found_sig_alg) {
        session->error = "ClientHello missing compatible signature algorithms";
        return RT_TLS_ERROR_HANDSHAKE;
    }
    if (!found_key_share) {
        session->error = "ClientHello missing X25519 key share";
        return RT_TLS_ERROR_HANDSHAKE;
    }
    if (session->hostname[0] != '\0' &&
        !tls_server_name_matches_leaf_cert(session->server_ctx, session->hostname)) {
        session->error = "ClientHello SNI does not match the configured certificate";
        return RT_TLS_ERROR_HANDSHAKE;
    }

    if (offered_aes)
        session->cipher_suite = TLS_AES_128_GCM_SHA256;
    else if (offered_chacha)
        session->cipher_suite = TLS_CHACHA20_POLY1305_SHA256;
    else {
        session->error = "ClientHello does not offer a supported TLS 1.3 cipher suite";
        return RT_TLS_ERROR_HANDSHAKE;
    }

    return RT_TLS_OK;
}

/// @brief Build and send the ServerHello handshake message.
///        Generates a fresh server random, echoes the legacy session ID, selects the
///        negotiated cipher suite, appends supported_versions (0x0304) and key_share
///        (X25519 server public key) extensions.  Performs the X25519 ECDH with the
///        client's public key and calls derive_handshake_keys_server to install keys.
/// @param session Server session with parsed ClientHello state.
/// @return @ref RT_TLS_OK on success or a negative handshake/send status.
static int send_server_hello(rt_tls_session_t *session) {
    uint8_t body[256];
    uint8_t hs[4 + 256];
    size_t pos = 0;
    uint8_t shared_secret[32];

    write_u16(body + pos, TLS_VERSION_1_2);
    pos += 2;
    rt_crypto_random_bytes(session->server_random, sizeof(session->server_random));
    memcpy(body + pos, session->server_random, sizeof(session->server_random));
    pos += sizeof(session->server_random);

    body[pos++] = (uint8_t)session->legacy_session_id_len;
    memcpy(body + pos, session->legacy_session_id, session->legacy_session_id_len);
    pos += session->legacy_session_id_len;

    write_u16(body + pos, session->cipher_suite);
    pos += 2;
    body[pos++] = 0;

    {
        size_t ext_start = pos;
        pos += 2;
        rt_x25519_keygen(session->client_private_key, session->client_public_key);

        write_u16(body + pos, TLS_EXT_SUPPORTED_VERSIONS);
        pos += 2;
        write_u16(body + pos, 2);
        pos += 2;
        write_u16(body + pos, TLS_VERSION_1_3);
        pos += 2;

        write_u16(body + pos, TLS_EXT_KEY_SHARE);
        pos += 2;
        write_u16(body + pos, 36);
        pos += 2;
        write_u16(body + pos, 0x001D);
        pos += 2;
        write_u16(body + pos, 32);
        pos += 2;
        memcpy(body + pos, session->client_public_key, 32);
        pos += 32;

        write_u16(body + ext_start, (uint16_t)(pos - ext_start - 2));
    }

    hs[0] = TLS_HS_SERVER_HELLO;
    write_u24(hs + 1, (uint32_t)pos);
    memcpy(hs + 4, body, pos);

    if (transcript_update(session, hs, 4 + pos) != 0)
        return RT_TLS_ERROR_HANDSHAKE;

    if (rt_x25519(session->client_private_key, session->server_public_key, shared_secret) != 0) {
        rt_secure_zero(shared_secret, sizeof(shared_secret));
        session->error = "TLS: invalid X25519 client key share";
        return RT_TLS_ERROR_HANDSHAKE;
    }

    // ServerHello itself is still plaintext in TLS 1.3. Switch to handshake
    // traffic keys only after the record has been transmitted.
    {
        int rc = send_record_fragmented(session, TLS_CONTENT_HANDSHAKE, hs, 4 + pos);
        if (rc != RT_TLS_OK) {
            rt_secure_zero(shared_secret, sizeof(shared_secret));
            return rc;
        }
    }

    derive_handshake_keys_server(session, shared_secret);
    rt_secure_zero(shared_secret, sizeof(shared_secret));
    return RT_TLS_OK;
}

/// @brief Build and send the server EncryptedExtensions message.
///        Includes the ALPN extension if a protocol was negotiated; otherwise sends an empty
///        extension list.
/// @param session Server session with negotiated ALPN state.
/// @return @ref RT_TLS_OK on success or a negative transcript/send status.
static int send_encrypted_extensions_server(rt_tls_session_t *session) {
    uint8_t body[256];
    uint8_t hs[4 + 256];
    size_t pos = 0;
    size_t ext_start = 0;

    ext_start = pos;
    pos += 2;
    if (session->negotiated_alpn[0] != '\0') {
        size_t proto_len = strlen(session->negotiated_alpn);
        write_u16(body + pos, TLS_EXT_ALPN);
        pos += 2;
        write_u16(body + pos, (uint16_t)(proto_len + 3));
        pos += 2;
        write_u16(body + pos, (uint16_t)(proto_len + 1));
        pos += 2;
        body[pos++] = (uint8_t)proto_len;
        memcpy(body + pos, session->negotiated_alpn, proto_len);
        pos += proto_len;
    }
    write_u16(body + ext_start, (uint16_t)(pos - ext_start - 2));

    hs[0] = TLS_HS_ENCRYPTED_EXTENSIONS;
    write_u24(hs + 1, (uint32_t)pos);
    memcpy(hs + 4, body, pos);
    if (transcript_update(session, hs, 4 + pos) != 0)
        return RT_TLS_ERROR_HANDSHAKE;
    return send_record_fragmented(session, TLS_CONTENT_HANDSHAKE, hs, 4 + pos);
}

/// @brief Build and send the TLS 1.3 Certificate message containing the server's cert chain.
///        The cert_list_entries in the server context are already in TLS wire format
///        (3-byte length prefix + DER bytes + 2-byte empty extensions per entry).
/// @param session Server session whose context owns the certificate list.
/// @return @ref RT_TLS_OK on success or a negative size, allocation,
///         transcript, or send status.
static int send_certificate_server(rt_tls_session_t *session) {
    size_t body_len = 1 + 3 + session->server_ctx->cert_list_entries_len;
    if (session->server_ctx->cert_list_entries_len > 0xFFFFFFu || body_len > 0xFFFFFFu ||
        body_len > SIZE_MAX - 4) {
        session->error = "TLS: certificate list too large";
        return RT_TLS_ERROR;
    }
    uint8_t *hs = (uint8_t *)malloc(4 + body_len);
    if (!hs) {
        session->error = "TLS: failed to allocate Certificate message";
        return RT_TLS_ERROR_MEMORY;
    }

    hs[0] = TLS_HS_CERTIFICATE;
    write_u24(hs + 1, (uint32_t)body_len);
    hs[4] = 0;
    write_u24(hs + 5, (uint32_t)session->server_ctx->cert_list_entries_len);
    memcpy(
        hs + 8, session->server_ctx->cert_list_entries, session->server_ctx->cert_list_entries_len);

    if (transcript_update(session, hs, 4 + body_len) != 0) {
        free(hs);
        return RT_TLS_ERROR_HANDSHAKE;
    }

    {
        int rc = send_record_fragmented(session, TLS_CONTENT_HANDSHAKE, hs, 4 + body_len);
        free(hs);
        return rc;
    }
}

/// @brief DER-encode an ECDSA P-256 signature (r, s) into out.
///        Each of r and s is wrapped in an INTEGER TLV with a leading 0x00 byte when the
///        high bit is set (DER positive-integer rule). Returns the total byte count written.
/// @param r Unsigned 32-byte ECDSA r scalar.
/// @param s Unsigned 32-byte ECDSA s scalar.
/// @param out Destination buffer of at least 80 bytes.
/// @return Number of DER bytes written.
static size_t encode_ecdsa_signature_der(const uint8_t r[32],
                                         const uint8_t s[32],
                                         uint8_t out[80]) {
    const uint8_t *r_ptr = r;
    const uint8_t *s_ptr = s;
    size_t r_len = 32;
    size_t s_len = 32;
    size_t pos = 0;
    int r_pad = 0;
    int s_pad = 0;

    while (r_len > 1 && *r_ptr == 0) {
        r_ptr++;
        r_len--;
    }
    while (s_len > 1 && *s_ptr == 0) {
        s_ptr++;
        s_len--;
    }
    if (r_ptr[0] & 0x80)
        r_pad = 1;
    if (s_ptr[0] & 0x80)
        s_pad = 1;

    out[pos++] = 0x30;
    out[pos++] = (uint8_t)(2 + r_pad + r_len + 2 + s_pad + s_len);
    out[pos++] = 0x02;
    out[pos++] = (uint8_t)(r_pad + r_len);
    if (r_pad)
        out[pos++] = 0x00;
    memcpy(out + pos, r_ptr, r_len);
    pos += r_len;
    out[pos++] = 0x02;
    out[pos++] = (uint8_t)(s_pad + s_len);
    if (s_pad)
        out[pos++] = 0x00;
    memcpy(out + pos, s_ptr, s_len);
    pos += s_len;
    return pos;
}

/// @brief Construct the 130-byte content buffer signed in CertificateVerify (RFC 8446 §4.4.3).
///        Format: 64 × 0x20 | context string "TLS 1.3, server CertificateVerify" | 0x00 |
///        transcript_hash.
/// @param transcript_hash Current 32-byte handshake transcript digest.
/// @param out_content Destination 130-byte signed-content buffer.
static void build_server_cert_verify_message(const uint8_t transcript_hash[32],
                                             uint8_t out_content[130]) {
    static const char context_str[] = "TLS 1.3, server CertificateVerify";
    memset(out_content, 0x20, 64);
    memcpy(out_content + 64, context_str, 33);
    out_content[97] = 0x00;
    memcpy(out_content + 98, transcript_hash, 32);
}

/// @brief Build and send the server CertificateVerify message.
///        Constructs the signed content, SHA-256 hashes it, signs with ECDSA-P256-SHA256 or
///        RSA-PSS-RSAE-SHA256 depending on the server key type, DER-encodes the signature,
///        and sends a CertificateVerify handshake record with the appropriate scheme code.
/// @param session Server session with negotiated signature scheme and key.
/// @return @ref RT_TLS_OK on success or a negative signing/transcript/send
///         status.
static int send_certificate_verify_server(rt_tls_session_t *session) {
    uint8_t content[130];
    uint8_t digest[32];
    uint8_t sig_r[32];
    uint8_t sig_s[32];
    uint8_t sig_buf[1024];
    uint8_t hs[4 + 2 + 2 + 1024];
    size_t sig_der_len = 0;

    if (!session || !session->server_ctx || session->server_sig_scheme == 0) {
        if (session)
            session->error = "TLS: server signature scheme not negotiated";
        return RT_TLS_ERROR_HANDSHAKE;
    }

    build_server_cert_verify_message(session->transcript_hash, content);
    rt_sha256(content, sizeof(content), digest);

    if (session->server_ctx->key_type == TLS_SERVER_KEY_ECDSA_P256) {
        if (!ecdsa_p256_sign(session->server_ctx->private_key, digest, sig_r, sig_s)) {
            session->error = "TLS: failed to sign CertificateVerify";
            return RT_TLS_ERROR_HANDSHAKE;
        }
        sig_der_len = encode_ecdsa_signature_der(sig_r, sig_s, sig_buf);
    } else if (session->server_ctx->key_type == TLS_SERVER_KEY_RSA_PSS_SHA256) {
        rt_rsa_hash_t hash_id = RT_RSA_HASH_SHA256;
        uint8_t hashed_content[64];
        size_t hash_len = 32;

        switch (session->server_sig_scheme) {
            case 0x0805:
                hash_id = RT_RSA_HASH_SHA384;
                rt_sha384(content, sizeof(content), hashed_content);
                hash_len = 48;
                break;
            case 0x0806:
                hash_id = RT_RSA_HASH_SHA512;
                rt_sha512(content, sizeof(content), hashed_content);
                hash_len = 64;
                break;
            case 0x0804:
            default:
                hash_id = RT_RSA_HASH_SHA256;
                memcpy(hashed_content, digest, sizeof(digest));
                hash_len = 32;
                break;
        }
        sig_der_len = sizeof(sig_buf);
        if (!rt_rsa_pss_sign(&session->server_ctx->rsa_key,
                             hash_id,
                             hashed_content,
                             hash_len,
                             sig_buf,
                             &sig_der_len)) {
            session->error = "TLS: failed to sign CertificateVerify";
            return RT_TLS_ERROR_HANDSHAKE;
        }
    } else {
        session->error = "TLS: failed to sign CertificateVerify";
        return RT_TLS_ERROR_HANDSHAKE;
    }

    hs[0] = TLS_HS_CERTIFICATE_VERIFY;
    write_u24(hs + 1, (uint32_t)(4 + sig_der_len));
    write_u16(hs + 4, session->server_sig_scheme);
    write_u16(hs + 6, (uint16_t)sig_der_len);
    memcpy(hs + 8, sig_buf, sig_der_len);

    if (transcript_update(session, hs, 8 + sig_der_len) != 0)
        return RT_TLS_ERROR_HANDSHAKE;
    return send_record_fragmented(session, TLS_CONTENT_HANDSHAKE, hs, 8 + sig_der_len);
}

/// @brief Accept an inbound TLS 1.3 connection on an already-accepted TCP socket.
///        Performs the server-side handshake: receive ClientHello, send ServerHello +
///        ChangeCipherSpec + EncryptedExtensions + Certificate + CertificateVerify + Finished,
///        receive and verify client Finished.
/// @param socket_fd Accepted native socket; ownership is consumed by the
///        returned session or failure cleanup.
/// @param ctx Immutable configured server context.
/// @return Newly allocated session in TLS_STATE_CONNECTED, or NULL with server last-error set.
rt_tls_session_t *rt_tls_server_accept_socket(intptr_t socket_fd, const rt_tls_server_ctx_t *ctx) {
    rt_tls_session_t *session = NULL;
    uint8_t content_type = 0;
    uint8_t data[TLS_MAX_RECORD_SIZE];
    size_t data_len = 0;

    tls_set_server_last_error_msg(NULL);

    if ((socket_t)socket_fd == INVALID_SOCK || !ctx) {
        tls_set_server_last_error_msg("TLS server: invalid socket or context");
        return NULL;
    }

    session = rt_tls_new(socket_fd, NULL);
    if (!session) {
        tls_set_server_last_error_msg("TLS server: failed to allocate session");
        CLOSE_SOCKET((socket_t)socket_fd);
        return NULL;
    }
    session->is_server = 1;
    session->server_ctx = ctx;
    session->verify_cert = 0;
    session->timeout_ms = ctx->timeout_ms > 0 ? ctx->timeout_ms : 30000;
    session->io_cancel_requested = ctx->cancel_requested;
    session->io_cancel_context = ctx->cancel_context;
    if (session->io_cancel_requested) {
        session->io_deadline_us = rt_clock_ticks_us() + (int64_t)session->timeout_ms * 1000;
    }
    if (ctx->alpn_protocols[0] != '\0')
        strncpy(session->alpn_protocols, ctx->alpn_protocols, sizeof(session->alpn_protocols) - 1);
    int native_timeout_ms =
        session->io_cancel_requested && session->timeout_ms > TLS_SERVER_CANCEL_POLL_MS
            ? TLS_SERVER_CANCEL_POLL_MS
            : session->timeout_ms;
    if (!set_socket_timeout(session->socket_fd, native_timeout_ms, true) ||
        !set_socket_timeout(session->socket_fd, native_timeout_ms, false)) {
        session->error = "TLS server: failed to configure handshake timeout";
        goto fail;
    }

    while (1) {
        size_t pos = 0;
        int rc = recv_record(session, &content_type, data, &data_len);
        if (rc != RT_TLS_OK)
            goto fail;
        if (content_type == TLS_CONTENT_CHANGE_CIPHER)
            continue;
        if (content_type != TLS_CONTENT_HANDSHAKE) {
            session->error = "TLS: expected ClientHello";
            goto fail;
        }

        while (pos + 4 <= data_len) {
            uint8_t hs_type = data[pos];
            uint32_t hs_len = read_u24(data + pos + 1);
            if (pos + 4 + hs_len > data_len) {
                session->error = "TLS: incomplete ClientHello record";
                goto fail;
            }
            if (hs_type != TLS_HS_CLIENT_HELLO) {
                session->error = "TLS: unexpected handshake message before ClientHello";
                goto fail;
            }
            if (parse_client_hello(session, data + pos + 4, hs_len) != RT_TLS_OK)
                goto fail;
            if (transcript_update(session, data + pos, 4 + hs_len) != 0)
                goto fail;

            if (send_server_hello(session) != RT_TLS_OK ||
                send_encrypted_extensions_server(session) != RT_TLS_OK ||
                send_certificate_server(session) != RT_TLS_OK ||
                send_certificate_verify_server(session) != RT_TLS_OK ||
                send_finished_server(session) != RT_TLS_OK) {
                goto fail;
            }

            derive_application_secrets(session);
            install_server_application_write_keys(session);

            // Expect the client's Finished under the handshake read keys.
            while (1) {
                rc = recv_record(session, &content_type, data, &data_len);
                if (rc != RT_TLS_OK)
                    goto fail;
                if (content_type == TLS_CONTENT_CHANGE_CIPHER)
                    continue;
                if (content_type == TLS_CONTENT_ALERT) {
                    session->error = "TLS: received alert during handshake";
                    goto fail;
                }
                if (content_type != TLS_CONTENT_HANDSHAKE) {
                    session->error = "TLS: expected client Finished";
                    goto fail;
                }
                if (data_len < 4 || data[0] != TLS_HS_FINISHED ||
                    read_u24(data + 1) + 4 != data_len) {
                    session->error = "TLS: malformed client Finished";
                    goto fail;
                }
                if (verify_finished_server(session, data + 4, data_len - 4) != RT_TLS_OK)
                    goto fail;
                if (transcript_update(session, data, data_len) != 0)
                    goto fail;
                install_server_application_read_keys(session);
                session->state = TLS_STATE_CONNECTED;
                if (session->io_cancel_requested &&
                    rt_tls_set_io_timeout(session, session->timeout_ms) != RT_TLS_OK) {
                    session->error = "TLS server: failed to restore session I/O timeout";
                    goto fail;
                }
                session->io_cancel_requested = NULL;
                session->io_cancel_context = NULL;
                session->io_deadline_us = 0;
                tls_set_server_last_error_msg(NULL);
                return session;
            }
        }
    }

fail:
    tls_set_server_last_error_msg(rt_tls_get_error(session));
    rt_tls_close(session);
    return NULL;
}

// Certificate validation functions (tls_parse_certificate_msg, tls_verify_hostname,
// tls_verify_chain, tls_verify_cert_verify) are implemented in rt_tls_verify.c.

//=============================================================================
// Public API
//=============================================================================

/// @brief Initialise a TLS config struct to safe defaults.
///
/// Zeros the structure, enables certificate validation
/// (chain + hostname + CertificateVerify), and sets a 30-second I/O
/// timeout. Callers may then override fields like `hostname` or
/// `verify_cert` before passing the config to `rt_tls_new`.
/// @param config Configuration storage to initialize; NULL is a no-op.
void rt_tls_config_init(rt_tls_config_t *config) {
    if (!config)
        return;
    memset(config, 0, sizeof(*config));
    // CS-6 resolved: certificate validation now implemented (CS-1/CS-2/CS-3).
    // verify_cert=1 enables full chain validation + hostname verification + CertVerify.
    config->verify_cert = 1;
    config->timeout_ms = 30000;
}

/// @brief Allocate a fresh TLS session bound to an already-connected socket.
///
/// The session is heap-allocated through the GC (`rt_obj_new_i64`)
/// so it participates in the runtime's lifecycle. State is reset to
/// `TLS_STATE_INITIAL`, the transcript hash is primed for an empty
/// handshake, and the optional hostname is copied into a
/// fixed-size buffer for SNI / hostname verification.
/// @param socket_fd  Underlying TCP socket file descriptor.
/// @param config     Optional config; if NULL, defaults match `rt_tls_config_init`.
/// @return Pointer to the new session, or NULL for an invalid native socket or
///         after a returning allocation trap hook.
rt_tls_session_t *rt_tls_new(intptr_t socket_fd, const rt_tls_config_t *config) {
    socket_t native_socket = (socket_t)socket_fd;
    size_t hostname_len = 0;
    size_t alpn_len = 0;
    size_t ca_file_len = 0;
    size_t alpn_wire_len = 0;

    if (native_socket == INVALID_SOCK) {
        tls_set_last_error_msg("TLS: invalid native socket");
        return NULL;
    }
    if (config && config->hostname) {
        hostname_len = strlen(config->hostname);
        if (hostname_len == 0 || hostname_len >= sizeof(((rt_tls_session_t *)0)->hostname)) {
            tls_set_last_error_msg("TLS: invalid hostname");
            return NULL;
        }
        for (size_t i = 0; i < hostname_len; i++) {
            unsigned char ch = (unsigned char)config->hostname[i];
            if (ch <= 0x20u || ch >= 0x7fu) {
                tls_set_last_error_msg("TLS: invalid hostname");
                return NULL;
            }
        }
        if (!tls_hostname_is_ip_literal(config->hostname) &&
            !tls_dns_hostname_is_valid(config->hostname)) {
            tls_set_last_error_msg("TLS: invalid hostname");
            return NULL;
        }
    }
    if (config && config->alpn_protocol) {
        alpn_len = strlen(config->alpn_protocol);
        if (alpn_len >= sizeof(((rt_tls_session_t *)0)->alpn_protocols) ||
            !tls_alpn_list_wire_len(config->alpn_protocol, &alpn_wire_len)) {
            tls_set_last_error_msg("TLS: invalid ALPN protocol list");
            return NULL;
        }
    }
    if (config && config->ca_file) {
        ca_file_len = strlen(config->ca_file);
        if (ca_file_len >= sizeof(((rt_tls_session_t *)0)->ca_file)) {
            tls_set_last_error_msg("TLS: invalid CA file path");
            return NULL;
        }
    }

    rt_tls_session_t *session = (rt_tls_session_t *)rt_obj_new_i64(
        RT_TLS_SESSION_CLASS_ID, (int64_t)sizeof(rt_tls_session_t));
    if (!session) {
        tls_set_last_error_msg("TLS: session allocation failed");
        return NULL;
    }
    memset(session, 0, sizeof(rt_tls_session_t));
    session->socket_fd = INVALID_SOCK;
    rt_obj_set_finalizer(session, tls_session_finalize);

    session->socket_fd = native_socket;
    session->state = TLS_STATE_INITIAL;
    session->verify_cert = config ? config->verify_cert : 1;
    session->timeout_ms = (config && config->timeout_ms > 0) ? config->timeout_ms : 30000;
    transcript_init(session);

    if (config && config->hostname) {
        for (size_t i = 0; i < hostname_len; i++) {
            unsigned char ch = (unsigned char)config->hostname[i];
            session->hostname[i] = (char)((ch >= 'A' && ch <= 'Z') ? (ch + ('a' - 'A')) : ch);
        }
        session->hostname[hostname_len] = '\0';
    }
    if (config && config->alpn_protocol) {
        memcpy(session->alpn_protocols, config->alpn_protocol, alpn_len + 1);
    }
    if (config && config->ca_file) {
        memcpy(session->ca_file, config->ca_file, ca_file_len + 1);
    }

    tls_set_last_error_msg(NULL);
    return session;
}

/// @brief Drive the TLS 1.3 handshake to completion.
///
/// Sends ClientHello, then loops reading records and dispatching
/// each parsed handshake message:
///   - ServerHello / HelloRetryRequest → key derivation
///   - EncryptedExtensions             → noted, advance state
///   - Certificate                     → parse + (optional) chain & hostname checks
///   - CertificateVerify               → signature check
///   - Finished                        → MAC verify, derive app keys, send our Finished
/// Updates the transcript hash before processing each message
/// (RFC 8446 §4.4.1) and aborts on transcript overflow. A
/// post-handshake state of `TLS_STATE_CONNECTED` indicates success.
/// @param session Fresh client-side session from @ref rt_tls_new.
/// @return RT_TLS_OK on completed handshake, an `RT_TLS_ERROR_*`
///         code otherwise; `session->error` carries a human message.
int rt_tls_handshake(rt_tls_session_t *session) {
    session = tls_session_checked(session);
    if (!session)
        return RT_TLS_ERROR_INVALID_ARG;

    if (rt_crypto_module_is_approved_mode()) {
        session->error =
            "TLS approved mode requires the validation-ready P-256/P-384 ECDHE profile";
        session->state = TLS_STATE_ERROR;
        return RT_TLS_ERROR_HANDSHAKE;
    }

    if (session->state != TLS_STATE_INITIAL) {
        session->error = "invalid state for handshake";
        return RT_TLS_ERROR;
    }

    // Send ClientHello
    int rc = send_client_hello(session);
    if (rc != RT_TLS_OK)
        return rc;

    // Process handshake messages
    while (session->state != TLS_STATE_CONNECTED && session->state != TLS_STATE_ERROR) {
        uint8_t content_type;
        uint8_t data[TLS_MAX_RECORD_SIZE];
        size_t data_len;

        rc = recv_record(session, &content_type, data, &data_len);
        if (rc != RT_TLS_OK)
            return rc;

        if (content_type == TLS_CONTENT_ALERT) {
            session->error = "received alert";
            session->state = TLS_STATE_ERROR;
            return RT_TLS_ERROR_HANDSHAKE;
        }

        if (content_type != TLS_CONTENT_HANDSHAKE) {
            session->error = "unexpected content type";
            return RT_TLS_ERROR_HANDSHAKE;
        }

        // Parse handshake messages
        size_t pos = 0;
        while (pos + 4 <= data_len) {
            uint8_t hs_type = data[pos];
            uint32_t hs_len = read_u24(data + pos + 1);

            if (pos + 4 + hs_len > data_len) {
                session->error = "incomplete handshake message";
                return RT_TLS_ERROR_HANDSHAKE;
            }

            switch (session->state) {
                case TLS_STATE_CLIENT_HELLO_SENT:
                    if (hs_type != TLS_HS_SERVER_HELLO) {
                        session->error = "unexpected TLS handshake message";
                        return RT_TLS_ERROR_HANDSHAKE;
                    }
                    break;
                case TLS_STATE_WAIT_ENCRYPTED_EXTENSIONS:
                    if (hs_type != TLS_HS_ENCRYPTED_EXTENSIONS) {
                        session->error = "unexpected TLS handshake message";
                        return RT_TLS_ERROR_HANDSHAKE;
                    }
                    break;
                case TLS_STATE_WAIT_CERTIFICATE:
                    if (hs_type != TLS_HS_CERTIFICATE) {
                        session->error = "unexpected TLS handshake message";
                        return RT_TLS_ERROR_HANDSHAKE;
                    }
                    break;
                case TLS_STATE_WAIT_CERTIFICATE_VERIFY:
                    if (hs_type != TLS_HS_CERTIFICATE_VERIFY) {
                        session->error = "unexpected TLS handshake message";
                        return RT_TLS_ERROR_HANDSHAKE;
                    }
                    break;
                case TLS_STATE_WAIT_FINISHED:
                    if (hs_type != TLS_HS_FINISHED) {
                        session->error = "unexpected TLS handshake message";
                        return RT_TLS_ERROR_HANDSHAKE;
                    }
                    break;
                default:
                    session->error = "unexpected TLS handshake state";
                    return RT_TLS_ERROR_HANDSHAKE;
            }

            if (hs_type != TLS_HS_FINISHED &&
                transcript_update(session, data + pos, 4 + hs_len) != 0) {
                return RT_TLS_ERROR_HANDSHAKE;
            }

            const uint8_t *hs_data = data + pos + 4;

            switch (hs_type) {
                case TLS_HS_SERVER_HELLO:
                    rc = process_server_hello(session, hs_data, hs_len, data + pos, 4 + hs_len);
                    if (rc != RT_TLS_OK)
                        return rc;
                    break;

                case TLS_HS_ENCRYPTED_EXTENSIONS:
                    rc = process_encrypted_extensions(session, hs_data, hs_len);
                    if (rc != RT_TLS_OK)
                        return rc;
                    break;

                case TLS_HS_CERTIFICATE:
                    // Parse the Certificate message and extract end-entity DER (always)
                    rc = tls_parse_certificate_msg(session, hs_data, hs_len);
                    if (rc != RT_TLS_OK)
                        return rc;

                    if (session->verify_cert) {
                        // CS-1: Chain validation via OS trust store
                        rc = tls_verify_chain(session);
                        if (rc != RT_TLS_OK)
                            return rc;

                        // CS-2: Hostname verification via SAN/CN
                        rc = tls_verify_hostname(session);
                        if (rc != RT_TLS_OK)
                            return rc;
                    }

                    // Save transcript hash for CS-3 CertificateVerify (before CV is added)
                    memcpy(session->cert_transcript_hash, session->transcript_hash, 32);
                    session->state = TLS_STATE_WAIT_CERTIFICATE_VERIFY;
                    break;

                case TLS_HS_CERTIFICATE_VERIFY:
                    // CS-3: Always verify the server's proof of private key ownership.
                    // verify_cert only controls trust-chain/hostname policy.
                    rc = tls_verify_cert_verify(session, hs_data, hs_len);
                    if (rc != RT_TLS_OK)
                        return rc;
                    session->state = TLS_STATE_WAIT_FINISHED;
                    break;

                case TLS_HS_FINISHED:
                    rc = verify_finished(session, hs_data, hs_len);
                    if (rc != RT_TLS_OK)
                        return rc;

                    if (transcript_update(session, data + pos, 4 + hs_len) != 0)
                        return RT_TLS_ERROR_HANDSHAKE;

                    // TLS 1.3 transitions read keys after server Finished, but
                    // the client Finished itself is still sent under handshake keys.
                    derive_application_secrets(session);
                    install_client_application_read_keys(session);

                    rc = send_finished(session);
                    if (rc != RT_TLS_OK)
                        return rc;

                    install_client_application_write_keys(session);

                    session->state = TLS_STATE_CONNECTED;
                    break;

                default:
                    session->error = "unexpected TLS handshake message";
                    return RT_TLS_ERROR_HANDSHAKE;
            }

            pos += 4 + hs_len;
        }
    }

    return session->state == TLS_STATE_CONNECTED ? RT_TLS_OK : RT_TLS_ERROR_HANDSHAKE;
}

/// @brief Encrypt and send `len` bytes of application data.
///
/// Splits the payload into TLS records of at most
/// `TLS_MAX_RECORD_SIZE` bytes (16 KiB per RFC 8446 §5.1), each
/// AEAD-sealed with the active write key. Refuses if the session
/// is not in the `CONNECTED` state. A `len == 0` call is a no-op
/// that returns 0 without touching the wire.
/// @param session Connected low-level TLS session.
/// @param data Caller-owned plaintext bytes, or NULL only for zero length.
/// @param len Number of plaintext bytes.
/// @return Number of bytes accepted (always equals `len` on success)
///         or a negative `RT_TLS_ERROR_*` code on failure.
long rt_tls_send(rt_tls_session_t *session, const void *data, size_t len) {
    session = tls_session_checked(session);
    if (!session)
        return RT_TLS_ERROR_INVALID_ARG;
    if (len == 0)
        return 0;
    if (!data)
        return RT_TLS_ERROR_INVALID_ARG;
    if (len > (size_t)LONG_MAX)
        return RT_TLS_ERROR;
    if (session->state != TLS_STATE_CONNECTED)
        return RT_TLS_ERROR;

    // Send in chunks
    const uint8_t *ptr = (const uint8_t *)data;
    size_t remaining = len;

    while (remaining > 0) {
        size_t chunk = remaining > TLS_MAX_RECORD_SIZE ? TLS_MAX_RECORD_SIZE : remaining;
        int rc = send_record(session, TLS_CONTENT_APPLICATION, ptr, chunk);
        if (rc != RT_TLS_OK)
            return rc;
        ptr += chunk;
        remaining -= chunk;
    }

    return (long)len;
}

/// @brief Receive up to `len` decrypted application bytes.
///
/// First drains any leftover bytes from a previous record (the
/// per-session `app_buffer`) before pulling a new record off the
/// wire. Transparently skips post-handshake messages such as
/// `NewSessionTicket` and `KeyUpdate` (capped at 100 retries to
/// foil a malicious server that floods them). A received `Alert`
/// transitions the session to `CLOSED` and returns 0 (EOF).
/// @param session Connected low-level TLS session.
/// @param buffer Caller-owned plaintext destination.
/// @param len Maximum bytes to copy; zero is a no-op.
/// @return Bytes copied into `buffer` (>0), 0 on clean close,
///         or a negative `RT_TLS_ERROR_*` code on failure.
long rt_tls_recv(rt_tls_session_t *session, void *buffer, size_t len) {
    session = tls_session_checked(session);
    if (!session)
        return RT_TLS_ERROR_INVALID_ARG;
    if (len == 0)
        return 0;
    if (!buffer)
        return RT_TLS_ERROR_INVALID_ARG;
    if (session->state != TLS_STATE_CONNECTED)
        return RT_TLS_ERROR;

    // Return buffered data first
    if (session->app_buffer_pos < session->app_buffer_len) {
        size_t avail = session->app_buffer_len - session->app_buffer_pos;
        size_t copy = avail < len ? avail : len;
        memcpy(buffer, session->app_buffer + session->app_buffer_pos, copy);
        session->app_buffer_pos += copy;
        return (long)copy;
    }

    // Receive new record (retry loop for skipping non-application records)
    int non_app_retries = 0;
retry_recv:;
    uint8_t content_type;
    size_t data_len;

    int rc = recv_record(session, &content_type, session->app_buffer, &data_len);
    if (rc != RT_TLS_OK)
        return rc;

    if (content_type == TLS_CONTENT_ALERT) {
        session->state = TLS_STATE_CLOSED;
        return 0;
    }

    if (content_type == TLS_CONTENT_HANDSHAKE) {
        size_t pos = 0;
        while (pos + 4 <= data_len) {
            uint8_t hs_type = session->app_buffer[pos];
            uint32_t hs_len = read_u24(session->app_buffer + pos + 1);
            if (pos + 4 + hs_len > data_len) {
                session->error = "TLS: incomplete post-handshake message";
                return RT_TLS_ERROR_HANDSHAKE;
            }
            rc = process_post_handshake_message(
                session, hs_type, session->app_buffer + pos + 4, hs_len);
            if (rc != RT_TLS_OK)
                return rc;
            pos += 4 + hs_len;
        }
        if (pos != data_len) {
            session->error = "TLS: malformed post-handshake record";
            return RT_TLS_ERROR_HANDSHAKE;
        }
    } else if (content_type != TLS_CONTENT_APPLICATION) {
        // Ignore non-application records we don't use (e.g. CCS compatibility records).
    }

    if (content_type != TLS_CONTENT_APPLICATION) {
        // Limit retries to prevent CPU starvation from malicious servers.
        if (++non_app_retries > 100) {
            session->error = "TLS: too many non-application records";
            return RT_TLS_ERROR;
        }
        goto retry_recv;
    }

    session->app_buffer_len = data_len;
    session->app_buffer_pos = 0;

    size_t copy = data_len < len ? data_len : len;
    memcpy(buffer, session->app_buffer, copy);
    session->app_buffer_pos = copy;

    return (long)copy;
}

/// @brief Transactionally replace a low-level session's I/O timeout.
/// @details The socket adapter is updated before the logical timeout is
///          published. A send-option failure rolls the receive option back to
///          the prior value so subsequent calls do not observe a deliberately
///          mixed timeout policy.
/// @param session Candidate stable TLS session.
/// @param timeout_ms Positive timeout in milliseconds.
/// @return `RT_TLS_OK` on success or a typed invalid/socket status.
int rt_tls_set_io_timeout(rt_tls_session_t *session, int timeout_ms) {
    session = tls_session_checked(session);
    if (!session || timeout_ms <= 0)
        return RT_TLS_ERROR_INVALID_ARG;
    if (session->socket_fd == INVALID_SOCK)
        return RT_TLS_ERROR_CLOSED;

    int previous = session->timeout_ms > 0 ? session->timeout_ms : 30000;
    if (!set_socket_timeout(session->socket_fd, timeout_ms, true)) {
        session->error = "TLS: failed to configure receive timeout";
        return RT_TLS_ERROR_SOCKET;
    }
    if (!set_socket_timeout(session->socket_fd, timeout_ms, false)) {
        (void)set_socket_timeout(session->socket_fd, previous, true);
        session->error = "TLS: failed to configure send timeout";
        return RT_TLS_ERROR_SOCKET;
    }
    session->timeout_ms = timeout_ms;
    return RT_TLS_OK;
}

/// @brief Dispose protocol/socket state without changing managed refcounts.
/// @details An explicit close performs a bounded close-notify exchange. A
///          finalizer skips network I/O but still wipes every dynamic secret,
///          closes the descriptor, and leaves an idempotent closed payload.
/// @param session Valid session payload owned by the caller/finalizer.
/// @param graceful Nonzero to attempt the bounded TLS close handshake.
static void tls_session_dispose(rt_tls_session_t *session, int graceful) {
    if (!session)
        return;

    if (graceful && session->state == TLS_STATE_CONNECTED) {
        const int close_timeout_ms =
            (session->timeout_ms > 0 && session->timeout_ms < 100) ? session->timeout_ms : 100;
        session->timeout_ms = close_timeout_ms;
        if (session->socket_fd != INVALID_SOCK) {
            set_socket_timeout(session->socket_fd, close_timeout_ms, true);
            set_socket_timeout(session->socket_fd, close_timeout_ms, false);
        }

        // Send close_notify alert
        uint8_t alert[2] = {1, 0}; // warning, close_notify
        send_record(session, TLS_CONTENT_ALERT, alert, 2);

        // M-12: Await the peer's close_notify briefly before closing the read side.
        // RFC 5246 §7.2.1 expects the initiator to receive the responding
        // close_notify, but close must stay bounded for peers that disappear.
        int drain = 0;
        while (drain < 4) {
            uint8_t content_type;
            size_t data_len;
            uint8_t buf[TLS_MAX_RECORD_SIZE + 256];
            int rc = recv_record(session, &content_type, buf, &data_len);
            if (rc != RT_TLS_OK)
                break; // Socket error or timeout — stop draining
            if (content_type == TLS_CONTENT_ALERT)
                break; // Received peer's close_notify (or other alert)
            drain++;   // Skip non-alert record (e.g. NewSessionTicket) and loop
        }
    }

    tls_release_dynamic_state(session);
    tls_scrub_session_secrets(session);
    if (session->socket_fd != INVALID_SOCK) {
        CLOSE_SOCKET(session->socket_fd);
        session->socket_fd = INVALID_SOCK;
    }
    rt_secure_zero(session->hostname, sizeof(session->hostname));
    rt_secure_zero(session->alpn_protocols, sizeof(session->alpn_protocols));
    rt_secure_zero(session->negotiated_alpn, sizeof(session->negotiated_alpn));
    rt_secure_zero(session->ca_file, sizeof(session->ca_file));
    session->server_ctx = NULL;
    session->state = TLS_STATE_CLOSED;
}

/// @brief Finalize an abandoned low-level TLS session without blocking.
/// @param obj Managed `rt_tls_session_t` payload.
static void tls_session_finalize(void *obj) {
    tls_session_dispose((rt_tls_session_t *)obj, 0);
}

/// @brief Politely shut down a TLS session and release its producer reference.
///
/// If still connected, sends a `close_notify` warning alert and
/// opportunistically drains a few records until the peer's matching
/// alert arrives. Shutdown uses a short bounded timeout rather than
/// the normal handshake/read timeout so a peer that drops the socket
/// or does not respond cannot stall local server teardown for tens of
/// seconds. Dynamic scratch state and secrets are wiped before the
/// pointer-width native socket is closed. NULL, forged, and stale handles are
/// safe no-ops.
/// @param session Session reference to consume; NULL, forged, or stale values
///        are ignored.
void rt_tls_close(rt_tls_session_t *session) {
    session = tls_session_checked(session);
    if (!session)
        return;

    tls_session_dispose(session, 1);

    if (rt_obj_release_check0(session))
        rt_obj_free(session);
}

/// @brief Last error message recorded by the session.
/// @param session Candidate low-level TLS session.
/// @return The most recent error string, "null session" if `session`
///         is NULL, or "no error" if no error has been recorded.
const char *rt_tls_get_error(rt_tls_session_t *session) {
    if (!session)
        return "null session";
    session = tls_session_checked(session);
    if (!session)
        return "invalid session";
    return session->error ? session->error : "no error";
}

/// @brief Test whether `rt_tls_recv` can satisfy a read without going to the wire.
/// @param session Candidate low-level TLS session.
/// @return 1 if the per-session app buffer holds undelivered bytes, 0 otherwise.
int rt_tls_has_buffered_data(rt_tls_session_t *session) {
    session = tls_session_checked(session);
    if (!session)
        return 0;
    return session->app_buffer_pos < session->app_buffer_len ? 1 : 0;
}

/// @brief Return the ALPN protocol negotiated during the handshake.
/// @param session Candidate low-level TLS session.
/// @return Session-owned NUL-terminated protocol, or a static empty string
///         when none was selected or the handle is invalid.
const char *rt_tls_get_negotiated_alpn(rt_tls_session_t *session) {
    session = tls_session_checked(session);
    if (!session)
        return "";
    return session->negotiated_alpn;
}

/// @brief Expose the pointer-width native socket descriptor.
/// @param session Candidate low-level TLS session.
/// @return The socket handle, or -1 if `session` is NULL, invalid, or closed.
intptr_t rt_tls_get_socket(rt_tls_session_t *session) {
    session = tls_session_checked(session);
    if (!session || session->socket_fd == INVALID_SOCK)
        return -1;
    return (intptr_t)session->socket_fd;
}

/// @brief One-shot TCP connect + TLS handshake to `host:port`.
///
/// Resolves `host` via `getaddrinfo` (AF_UNSPEC accepts both IPv4
/// and IPv6), iterates the candidate addresses connecting to the
/// first that responds; each address receives the configured timeout
/// independently (non-blocking + `select`). The same value is then applied as
/// the recv/send timeout for individual socket operations before the
/// socket, then runs the TLS handshake. On any failure the socket
/// is closed and NULL is returned. Initialises Winsock once on
/// Windows. The host name is forwarded into the config for SNI and
/// hostname verification regardless of the caller-provided `hostname`.
/// @param host DNS name or numeric address used for TCP and default TLS
///        identity.
/// @param port Nonzero destination TCP port.
/// @param config Optional caller configuration copied for this connection.
/// @return Connected `rt_tls_session_t*` on success, NULL on
///         resolution / connect / handshake failure.
rt_tls_session_t *rt_tls_connect(const char *host, uint16_t port, const rt_tls_config_t *config) {
    rt_net_init_wsa();

    if (!host || !host[0] || port == 0) {
        tls_set_last_error_msg("TLS: invalid connect target");
        return NULL;
    }

    rt_tls_config_t cfg;
    if (config) {
        cfg = *config;
    } else {
        rt_tls_config_init(&cfg);
    }
    tls_set_last_error_msg(NULL);
    if (!cfg.hostname)
        cfg.hostname = host;
    if (cfg.timeout_ms <= 0)
        cfg.timeout_ms = 30000;

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);

    struct addrinfo *res = NULL;
    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) {
        tls_set_last_error_msg("TLS: host resolution failed");
        return NULL;
    }

    // One monotonic deadline covers every address attempt AND the handshake,
    // so ConnectFor(t) honors t as an overall connection budget rather than
    // applying t independently per address (VDOC-179). cfg.timeout_ms is > 0
    // here (normalized above).
    int64_t deadline_us = rt_clock_ticks_us() + (int64_t)cfg.timeout_ms * 1000;

    socket_t sock = INVALID_SOCK;
    for (struct addrinfo *p = res; p; p = p->ai_next) {
        int64_t remaining_ms = (deadline_us - rt_clock_ticks_us()) / 1000;
        if (remaining_ms <= 0)
            break; // overall connection deadline exhausted
        if (remaining_ms > INT_MAX)
            remaining_ms = INT_MAX;

        sock = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sock == INVALID_SOCK)
            continue;
        suppress_sigpipe(sock);

        int connected = 0;
        if (!rt_socket_set_nonblocking(sock, true)) {
            CLOSE_SOCKET(sock);
            sock = INVALID_SOCK;
            continue;
        }
        if (connect(sock, p->ai_addr, (int)p->ai_addrlen) == 0) {
            connected = 1;
        } else {
            int err = GET_LAST_ERROR();
            if (rt_socket_error_is_in_progress(err)) {
                int ready = wait_socket(sock, (int)remaining_ms, true);
                if (ready > 0) {
                    int so_error = 0;
                    connected = rt_socket_pending_error(sock, &so_error) && so_error == 0;
                }
            }
        }

        if (connected && !rt_socket_set_nonblocking(sock, false))
            connected = 0;

        if (connected)
            break;

        CLOSE_SOCKET(sock);
        sock = INVALID_SOCK;
    }

    freeaddrinfo(res);
    if (sock == INVALID_SOCK) {
        tls_set_last_error_msg("TLS: TCP connect failed");
        return NULL;
    }

    // Bound the handshake by the remaining connection budget (minimum 1 ms so a
    // just-in-time connect still gets to attempt the handshake).
    int64_t handshake_ms = (deadline_us - rt_clock_ticks_us()) / 1000;
    if (handshake_ms < 1)
        handshake_ms = 1;
    if (handshake_ms > INT_MAX)
        handshake_ms = INT_MAX;
    if (!set_socket_timeout(sock, (int)handshake_ms, true) ||
        !set_socket_timeout(sock, (int)handshake_ms, false)) {
        CLOSE_SOCKET(sock);
        tls_set_last_error_msg("TLS: failed to configure handshake timeout");
        return NULL;
    }

    rt_tls_session_t *session = rt_tls_new((intptr_t)sock, &cfg);
    if (!session) {
        CLOSE_SOCKET(sock);
        tls_set_last_error_msg("TLS: failed to allocate session");
        return NULL;
    }
    session->timeout_ms = (int)handshake_ms;

    // Perform handshake
    if (rt_tls_handshake(session) != RT_TLS_OK) {
        tls_set_last_error_msg(rt_tls_get_error(session));
        rt_tls_close(session);
        return NULL;
    }

    // Post-connect I/O uses the configured timeout as a per-operation budget,
    // independent of the (now-consumed) connection deadline.
    session->timeout_ms = cfg.timeout_ms;
    if (!set_socket_timeout(sock, cfg.timeout_ms, true) ||
        !set_socket_timeout(sock, cfg.timeout_ms, false)) {
        tls_set_last_error_msg("TLS: failed to configure I/O timeout");
        rt_tls_close(session);
        return NULL;
    }

    tls_set_last_error_msg(NULL);
    return session;
}
