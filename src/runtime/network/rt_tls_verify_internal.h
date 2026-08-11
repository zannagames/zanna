//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/network/rt_tls_verify_internal.h
// Purpose: Shared includes, macros, and cert/ASN.1 helper declarations for the
//   TLS certificate-verification runtime, whose implementation is split into
//   rt_tls_verify_common.c (platform-neutral DER/SAN/hostname parsing),
//   rt_tls_verify_win.c (CryptoAPI chain + CertificateVerify), and
//   rt_tls_verify_posix.c (manual PEM-bundle verification on macOS and Linux).
//
// Key invariants:
//   - The seven cert/ASN.1 helpers below are defined once in the common TU and
//     called from the platform TUs; cert_allows_tls_server_auth is implemented
//     per-platform and is therefore declared only inside those TUs.
//   - On Windows, winsock2.h (which pulls windows.h) must precede wincrypt.h.
//
// Links: rt_tls_verify_common.c, rt_tls_verify_win.c, rt_tls_verify_posix.c,
//        rt_tls_internal.h
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_tls_verify_internal.h
 * @brief Declares shared certificate-parsing and verification infrastructure.
 * @details This private interface centralizes cryptographic includes, platform
 *          trust-store types, verification limits, X.509 usage checks, DER and
 *          SAN helpers, hostname matching, and CertificateVerify construction
 *          for the POSIX and Windows verification translation units.
 */

#pragma once

#include "rt_crypto.h"
#include "rt_ecdsa_p256.h"
#include "rt_network_time.inc"
#include "rt_rsa.h"
#include "rt_tls_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#define strcasecmp _stricmp
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <strings.h>
#endif

// Platform-specific trust-store and crypto APIs (CS-1/CS-2/CS-3)
#if defined(_WIN32)
#ifndef _WINDOWS_
#error "windows.h must be included before wincrypt.h"
#endif
#include <bcrypt.h>
#include <wincrypt.h>
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "bcrypt.lib")
#endif

/// @brief Suppress unused-function diagnostics for helpers compiled into only some platform paths.
#if defined(__GNUC__) || defined(__clang__)
#define RT_TLS_MAYBE_UNUSED __attribute__((unused))
#else
#define RT_TLS_MAYBE_UNUSED
#endif

/// @brief Maximum number of handshake intermediates accepted by either trust adapter.
#define TLS_MAX_INTERMEDIATE_CERTS 16

// `cert_allows_tls_server_auth` is intentionally not declared here. Each
// platform translation unit defines its own static version above its first use;
// a static declaration in this shared header would also reach the common TU,
// which never defines it, and compilers reject that as an undefined static.

/// @brief Advance through one entry of a TLS 1.3 certificate_list.
/// @details Parses the certificate and per-entry extension length fields,
///          returns a borrowed view of the DER certificate, and advances the
///          cursor past the complete entry.
/// @param[in] list Encoded certificate_list entry bytes.
/// @param[in] list_len Number of bytes available at @p list.
/// @param[in,out] pos Current byte offset; advanced past the entry on success.
/// @param[out] cert_der Receives a borrowed pointer into @p list.
/// @param[out] cert_len Receives the certificate length in bytes.
/// @return 0 on success, or -1 for null arguments, truncated fields, an empty
///         certificate, or out-of-bounds lengths.
int tls_next_certificate_entry(const uint8_t *list,
                               size_t list_len,
                               size_t *pos,
                               const uint8_t **cert_der,
                               size_t *cert_len);

/// @brief Parse one canonical ASN.1 DER tag-and-length header.
/// @details Indefinite lengths, non-minimal long-form lengths, oversized
///          length fields, and values extending beyond the supplied buffer are rejected.
/// @param[in] buf DER bytes beginning at a TLV header.
/// @param[in] buf_len Number of bytes available at @p buf.
/// @param[out] tag Receives the single-byte ASN.1 tag.
/// @param[out] val_len Receives the value length in bytes.
/// @param[out] hdr_len Receives the tag-plus-length header size in bytes.
/// @return 0 on success, or -1 when the input is null, truncated, or non-canonical.
int der_read_tlv(const uint8_t *buf, size_t buf_len, uint8_t *tag, size_t *val_len, size_t *hdr_len);

/// @brief Validate absent or canonical DER NULL AlgorithmIdentifier parameters.
/// @param[in] params Remaining parameter bytes after an algorithm OID.
/// @param[in] params_len Number of bytes available at @p params.
/// @return 1 for absent parameters or exactly one empty NULL TLV; otherwise 0.
int der_params_absent_or_null(const uint8_t *params, size_t params_len);

/// @brief Locate the DER-encoded Subject Name in an X.509 certificate.
/// @param[in] cert_der Complete DER-encoded X.509 certificate.
/// @param[in] cert_len Length of @p cert_der in bytes.
/// @param[out] subject_len Receives the complete Subject Name TLV length.
/// @return Borrowed pointer into @p cert_der on success, or NULL when malformed.
const uint8_t *cert_get_subject(const uint8_t *cert_der, size_t cert_len, size_t *subject_len);

/// @brief Detect critical X.509 extensions unsupported by the runtime verifier.
/// @details Malformed certificate or extension structures fail closed and are
///          reported as unsupported.
/// @param[in] cert_der Complete DER-encoded X.509 certificate.
/// @param[in] cert_len Length of @p cert_der in bytes.
/// @return 1 when the certificate must be rejected; 0 when all critical
///         extensions are recognized.
int cert_has_unsupported_critical_extension(const uint8_t *cert_der, size_t cert_len);

/// @brief Construct the RFC 8446 server CertificateVerify signed content.
/// @param[in] transcript_hash 32-byte SHA-256 handshake transcript hash.
/// @param[out] out_content Buffer that receives the 130-byte context-prefixed message.
void build_cert_verify_message(const uint8_t transcript_hash[32], uint8_t out_content[130]);

/// @brief Return the digest size selected by a supported TLS SignatureScheme.
/// @param[in] sig_scheme TLS SignatureScheme code.
/// @return 32, 48, or 64 for supported SHA-256, SHA-384, or SHA-512 schemes;
///         otherwise 0.
size_t sig_scheme_hash_len(uint16_t sig_scheme);
