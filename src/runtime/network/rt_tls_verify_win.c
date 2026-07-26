//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/network/rt_tls_verify_win.c
// Purpose: Windows TLS certificate verification via the CryptoAPI trust store: builds
//   and validates the certificate chain (CertGetCertificateChain) and verifies
//   the TLS 1.3 CertificateVerify signature with BCrypt. Compiled only on
//   _WIN32; the native path lives in rt_tls_verify_posix.c.
//
// Key invariants:
//   - CertificateVerify hashes use the digest size selected by the TLS signature scheme.
//   - Imported certificate and CNG key handles are released on every return path.
// Ownership/Lifetime:
//   - Certificate contexts and key handles are borrowed only within each verification call.
//
// Links: rt_tls_verify_internal.h (shared helpers), rt_tls_verify_posix.c, rt_tls_internal.h
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_tls_verify_win.c
 * @brief Implements Windows trust-chain and CertificateVerify validation.
 * @details The Windows adapter imports server certificates and optional
 *          caller-provided trust anchors into CryptoAPI stores, evaluates the
 *          chain and name policy, and uses CNG keys to verify the negotiated
 *          TLS 1.3 signature. Every native context, store, and key handle is
 *          released on all return paths.
 */

#include "rt_tls_verify_internal.h"

#if defined(_WIN32)
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>

enum {
    TLS_MAX_CUSTOM_CA_FILE_BYTES = 16 * 1024 * 1024,
    TLS_MAX_CUSTOM_CA_CERTIFICATES = 1024,
};

/// @brief Open one runtime UTF-8 path through the Unicode Windows CRT boundary.
/// @details The path is decoded with strict UTF-8 validation and opened in
///          binary-read mode through _wfopen(). Temporary UTF-16 storage is
///          released before return.
/// @param[in] path Null-terminated UTF-8 filesystem path.
/// @return Open stream on success, or NULL for an invalid path, conversion
///         failure, allocation failure, or open failure. The caller owns the stream.
static FILE *tls_open_file_utf8_win(const char *path) {
    wchar_t *wide_path = NULL;
    FILE *file = NULL;
    int descriptor = -1;
    int required;

    if (!path || !*path)
        return NULL;
    required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    if (required <= 0 || (size_t)required > SIZE_MAX / sizeof(wchar_t))
        return NULL;
    wide_path = (wchar_t *)malloc((size_t)required * sizeof(wchar_t));
    if (!wide_path)
        return NULL;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide_path, required) ==
            required &&
        _wsopen_s(&descriptor, wide_path, _O_RDONLY | _O_BINARY, _SH_DENYWR, _S_IREAD) == 0) {
        file = _fdopen(descriptor, "rb");
        if (!file) {
            int saved_errno = errno;
            (void)_close(descriptor);
            errno = saved_errno;
        }
    }
    free(wide_path);
    return file;
}

/// @brief Read an entire file into a freshly allocated buffer (Windows).
/// @details Files larger than TLS_MAX_CUSTOM_CA_FILE_BYTES and files that grow
///          after the initial size snapshot are rejected. The caller owns the
///          returned null-terminated buffer and must release it with free().
/// @param[in] path Null-terminated UTF-8 path of the file to read.
/// @param[out] len_out Optional destination for the file size, initialized to zero on failure.
/// @return Pointer to null-terminated buffer on success, NULL on failure.
static char *tls_read_file_bytes_win(const char *path, size_t *len_out) {
    FILE *f = NULL;
    char *buf = NULL;
    __int64 len = 0;

    if (len_out)
        *len_out = 0;
    if (!path || !*path)
        return NULL;

    f = tls_open_file_utf8_win(path);
    if (!f)
        return NULL;
    if (_fseeki64(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    len = _ftelli64(f);
    if (len < 0 || len > TLS_MAX_CUSTOM_CA_FILE_BYTES) {
        fclose(f);
        return NULL;
    }
    if (_fseeki64(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }

    buf = (char *)malloc((size_t)len + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (len > 0 && fread(buf, 1, (size_t)len, f) != (size_t)len) {
        free(buf);
        fclose(f);
        return NULL;
    }
    /* Reject a file that grew after the size snapshot instead of silently trusting a prefix. */
    if (fgetc(f) != EOF || ferror(f)) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    buf[len] = '\0';
    if (len_out)
        *len_out = (size_t)len;
    return buf;
}

/// @brief Load a PEM bundle or DER file and add all certificates to a Windows HCERTSTORE.
/// @details PEM input must contain complete CERTIFICATE blocks and is limited
///          to TLS_MAX_CUSTOM_CA_CERTIFICATES entries. Input without PEM
///          markers is treated as one DER certificate. Certificates already
///          added before a malformed later block remain in @p store.
/// @param[in] store Open destination certificate store; ownership remains with the caller.
/// @param[in] path Null-terminated UTF-8 path to a PEM bundle or DER certificate.
/// @return 1 if at least one certificate was added, 0 otherwise.
static int tls_add_pem_or_der_certs_to_store_win(HCERTSTORE store, const char *path) {
    static const char begin_marker[] = "-----BEGIN CERTIFICATE-----";
    static const char end_marker[] = "-----END CERTIFICATE-----";
    size_t file_len = 0;
    char *file = NULL;
    char *cursor = NULL;
    int added = 0;
    int saw_pem = 0;
    int malformed = 0;

    if (!store || !path || !*path)
        return 0;
    file = tls_read_file_bytes_win(path, &file_len);
    if (!file)
        return 0;
    cursor = file;
    if (strstr(file, begin_marker) && memchr(file, '\0', file_len)) {
        free(file);
        return 0;
    }

    while (cursor && *cursor) {
        char *begin = strstr(cursor, begin_marker);
        char *end = begin ? strstr(begin, end_marker) : NULL;
        DWORD der_len = 0;
        uint8_t *der = NULL;
        size_t pem_len = 0;

        if (!begin)
            break;
        saw_pem = 1;
        if (!end || added >= TLS_MAX_CUSTOM_CA_CERTIFICATES) {
            malformed = 1;
            break;
        }
        end += strlen(end_marker);
        pem_len = (size_t)(end - begin);

        if (pem_len > UINT32_MAX ||
            !CryptStringToBinaryA(
                begin, (DWORD)pem_len, CRYPT_STRING_BASE64HEADER, NULL, &der_len, NULL, NULL) ||
            der_len == 0) {
            malformed = 1;
            break;
        }
        der = (uint8_t *)malloc(der_len);
        if (!der ||
            !CryptStringToBinaryA(
                begin, (DWORD)pem_len, CRYPT_STRING_BASE64HEADER, der, &der_len, NULL, NULL) ||
            !CertAddEncodedCertificateToStore(store,
                                              X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
                                              der,
                                              der_len,
                                              CERT_STORE_ADD_ALWAYS,
                                              NULL)) {
            free(der);
            malformed = 1;
            break;
        }
        free(der);
        added++;

        cursor = end;
    }

    if (!malformed && !saw_pem && file_len > 0 && file_len <= UINT32_MAX &&
        CertAddEncodedCertificateToStore(store,
                                         X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
                                         (const BYTE *)file,
                                         (DWORD)file_len,
                                         CERT_STORE_ADD_ALWAYS,
                                         NULL)) {
        added = 1;
    }

    free(file);
    return !malformed && added > 0;
}

/// @brief Check whether a certificate permits TLS server authentication (Windows).
/// @details When KeyUsage is present, digitalSignature must be asserted. When
///          ExtendedKeyUsage is present, it must include id-kp-serverAuth or
///          anyExtendedKeyUsage. Malformed recognized extensions fail closed.
/// @param[in] cert_der Complete DER-encoded X.509 certificate.
/// @param[in] cert_len Length of @p cert_der in bytes.
/// @return 1 if both checks pass, 0 if any extension explicitly forbids server auth.
static RT_TLS_MAYBE_UNUSED int cert_allows_tls_server_auth(const uint8_t *cert_der,
                                                           size_t cert_len) {
    static const uint8_t OID_KEY_USAGE[] = {0x55, 0x1d, 0x0f};
    static const uint8_t OID_EXTENDED_KEY_USAGE[] = {0x55, 0x1d, 0x25};
    static const uint8_t OID_SERVER_AUTH[] = {0x2b, 0x06, 0x01, 0x05, 0x05, 0x07, 0x03, 0x01};
    static const uint8_t OID_ANY_EXTENDED_KEY_USAGE[] = {0x55, 0x1d, 0x25, 0x00};
    uint8_t tag;
    size_t vl, hl;
    const uint8_t *p = cert_der;
    size_t rem = cert_len;
    const uint8_t *tbs = NULL;
    size_t tbs_rem = 0;
    int key_usage_allows = 1;
    int saw_eku = 0;
    int eku_allows = 1;

    if (der_read_tlv(p, rem, &tag, &vl, &hl) != 0 || tag != 0x30)
        return 0;
    p += hl;
    rem = vl;
    if (der_read_tlv(p, rem, &tag, &vl, &hl) != 0 || tag != 0x30)
        return 0;
    tbs = p + hl;
    tbs_rem = vl;

    if (der_read_tlv(tbs, tbs_rem, &tag, &vl, &hl) != 0)
        return 0;
    if (tag == 0xA0) {
        tbs += hl + vl;
        tbs_rem -= hl + vl;
    }

    for (int i = 0; i < 6; i++) {
        if (der_read_tlv(tbs, tbs_rem, &tag, &vl, &hl) != 0)
            return 0;
        tbs += hl + vl;
        tbs_rem -= hl + vl;
    }

    while (tbs_rem > 0) {
        if (der_read_tlv(tbs, tbs_rem, &tag, &vl, &hl) != 0)
            break;
        if (tag == 0xA3) {
            const uint8_t *exts = tbs + hl;
            size_t exts_rem = vl;
            if (der_read_tlv(exts, exts_rem, &tag, &vl, &hl) != 0 || tag != 0x30)
                return 0;
            exts += hl;
            exts_rem = vl;
            while (exts_rem > 0) {
                const uint8_t *ext = exts;
                size_t ext_rem = 0;
                size_t ext_hdr_len = 0;
                size_t ext_seq_len = 0;
                const uint8_t *extn_value = NULL;
                size_t extn_value_len = 0;
                if (der_read_tlv(ext, exts_rem, &tag, &ext_seq_len, &ext_hdr_len) != 0 ||
                    tag != 0x30)
                    return 0;
                ext += ext_hdr_len;
                ext_rem = ext_seq_len;
                if (der_read_tlv(ext, ext_rem, &tag, &vl, &hl) != 0 || tag != 0x06)
                    return 0;
                {
                    int is_key_usage =
                        (vl == sizeof(OID_KEY_USAGE) &&
                         memcmp(ext + hl, OID_KEY_USAGE, sizeof(OID_KEY_USAGE)) == 0);
                    int is_eku =
                        (vl == sizeof(OID_EXTENDED_KEY_USAGE) &&
                         memcmp(ext + hl, OID_EXTENDED_KEY_USAGE, sizeof(OID_EXTENDED_KEY_USAGE)) ==
                             0);
                    ext += hl + vl;
                    ext_rem -= hl + vl;
                    if (der_read_tlv(ext, ext_rem, &tag, &vl, &hl) == 0 && tag == 0x01) {
                        ext += hl + vl;
                        ext_rem -= hl + vl;
                    }
                    if (der_read_tlv(ext, ext_rem, &tag, &vl, &hl) != 0 || tag != 0x04)
                        return 0;
                    extn_value = ext + hl;
                    extn_value_len = vl;

                    if (is_key_usage) {
                        if (der_read_tlv(extn_value, extn_value_len, &tag, &vl, &hl) == 0 &&
                            tag == 0x03 && vl >= 2 && extn_value[hl] <= 7) {
                            const uint8_t *bits = extn_value + hl;
                            key_usage_allows = (bits[1] & 0x80) != 0;
                        } else
                            key_usage_allows = 0;
                    } else if (is_eku) {
                        const uint8_t *eku = extn_value;
                        size_t eku_len = extn_value_len;
                        saw_eku = 1;
                        eku_allows = 0;
                        if (der_read_tlv(eku, eku_len, &tag, &vl, &hl) == 0 && tag == 0x30) {
                            eku += hl;
                            eku_len = vl;
                            while (eku_len > 0) {
                                if (der_read_tlv(eku, eku_len, &tag, &vl, &hl) != 0 || tag != 0x06)
                                    break;
                                if ((vl == sizeof(OID_SERVER_AUTH) &&
                                     memcmp(eku + hl, OID_SERVER_AUTH, sizeof(OID_SERVER_AUTH)) ==
                                         0) ||
                                    (vl == sizeof(OID_ANY_EXTENDED_KEY_USAGE) &&
                                     memcmp(eku + hl,
                                            OID_ANY_EXTENDED_KEY_USAGE,
                                            sizeof(OID_ANY_EXTENDED_KEY_USAGE)) == 0)) {
                                    eku_allows = 1;
                                    break;
                                }
                                eku += hl + vl;
                                eku_len -= hl + vl;
                            }
                        }
                    }
                }

                exts += ext_hdr_len + ext_seq_len;
                exts_rem -= ext_hdr_len + ext_seq_len;
            }
            break;
        }
        tbs += hl + vl;
        tbs_rem -= hl + vl;
    }

    return key_usage_allows && (!saw_eku || eku_allows);
}

/// @brief Validate the server certificate chain against the Windows system trust store (CryptoAPI).
/// @details If the session CA file is set, an exclusive or restricted chain
///          engine is built from that bundle; otherwise the default ROOT store
///          is used. Handshake intermediates are added as untrusted chain
///          hints. The leaf is also checked for TLS server usage and
///          unsupported critical extensions before CryptoAPI policy
///          validation. All temporary stores, contexts, and engines are
///          released before return.
/// @param[in,out] session TLS session containing the peer chain and trust configuration.
/// @return RT_TLS_OK on success, RT_TLS_ERROR_HANDSHAKE on validation failure.
int tls_verify_chain(rt_tls_session_t *session) {
    if (!session)
        return RT_TLS_ERROR_HANDSHAKE;
    const uint8_t *server_cert_der = tls_session_server_cert_der(session);
    if (!server_cert_der) {
        session->error = "TLS: no certificate to validate";
        return RT_TLS_ERROR_HANDSHAKE;
    }
    if (cert_has_unsupported_critical_extension(server_cert_der, session->server_cert_der_len)) {
        session->error = "TLS: certificate contains unsupported critical extension";
        return RT_TLS_ERROR_HANDSHAKE;
    }
    if (!cert_allows_tls_server_auth(server_cert_der, session->server_cert_der_len)) {
        session->error = "TLS: certificate is not valid for TLS server authentication";
        return RT_TLS_ERROR_HANDSHAKE;
    }

    if (session->server_cert_der_len > UINT32_MAX) {
        session->error = "TLS: certificate is too large for Windows CryptoAPI";
        return RT_TLS_ERROR_HANDSHAKE;
    }
    PCCERT_CONTEXT cert_ctx = CertCreateCertificateContext(X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
                                                           server_cert_der,
                                                           (DWORD)session->server_cert_der_len);

    if (!cert_ctx) {
        session->error = "TLS: could not parse DER certificate (Windows)";
        return RT_TLS_ERROR_HANDSHAKE;
    }

    CERT_CHAIN_PARA chain_para = {0};
    chain_para.cbSize = sizeof(chain_para);

    HCERTSTORE root_store = NULL;
    HCERTCHAINENGINE chain_engine = NULL;
    HCERTSTORE extra_store =
        CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, CERT_STORE_CREATE_NEW_FLAG, NULL);
    if (!extra_store) {
        CertFreeCertificateContext(cert_ctx);
        session->error = "TLS: could not create intermediate certificate store";
        return RT_TLS_ERROR_HANDSHAKE;
    }

    if (session->ca_file[0]) {
        root_store = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, CERT_STORE_CREATE_NEW_FLAG, NULL);
        if (!root_store) {
            CertCloseStore(extra_store, 0);
            CertFreeCertificateContext(cert_ctx);
            session->error = "TLS: could not create custom root certificate store";
            return RT_TLS_ERROR_HANDSHAKE;
        }
        if (!tls_add_pem_or_der_certs_to_store_win(root_store, session->ca_file)) {
            CertCloseStore(root_store, 0);
            CertCloseStore(extra_store, 0);
            CertFreeCertificateContext(cert_ctx);
            session->error = "TLS: could not load custom CA file";
            return RT_TLS_ERROR_HANDSHAKE;
        }

        CERT_CHAIN_ENGINE_CONFIG engine_config = {0};
        engine_config.cbSize = sizeof(engine_config);
#if defined(CERT_CHAIN_EXCLUSIVE_ENABLE_CA_FLAG)
        engine_config.hExclusiveRoot = root_store;
        engine_config.dwExclusiveFlags = CERT_CHAIN_EXCLUSIVE_ENABLE_CA_FLAG;
#else
        engine_config.hRestrictedRoot = root_store;
#endif
        if (!CertCreateCertificateChainEngine(&engine_config, &chain_engine)) {
            if (chain_engine)
                CertFreeCertificateChainEngine(chain_engine);
            CertCloseStore(root_store, 0);
            CertCloseStore(extra_store, 0);
            CertFreeCertificateContext(cert_ctx);
            session->error = "TLS: could not create custom certificate chain engine";
            return RT_TLS_ERROR_HANDSHAKE;
        }
    }

    if (session->server_cert_list && session->server_cert_list_len > 0) {
        size_t list_pos = 0;
        size_t cert_index = 0;
        while (list_pos < session->server_cert_list_len) {
            const uint8_t *cert_der = NULL;
            size_t cert_len = 0;
            if (tls_next_certificate_entry(session->server_cert_list,
                                           session->server_cert_list_len,
                                           &list_pos,
                                           &cert_der,
                                           &cert_len) != 0) {
                if (chain_engine)
                    CertFreeCertificateChainEngine(chain_engine);
                if (root_store)
                    CertCloseStore(root_store, 0);
                CertCloseStore(extra_store, 0);
                CertFreeCertificateContext(cert_ctx);
                session->error = "TLS: malformed certificate chain";
                return RT_TLS_ERROR_HANDSHAKE;
            }
            if (cert_index == 0) {
                cert_index++;
                continue;
            }
            if (cert_index > TLS_MAX_INTERMEDIATE_CERTS) {
                if (chain_engine)
                    CertFreeCertificateChainEngine(chain_engine);
                if (root_store)
                    CertCloseStore(root_store, 0);
                CertCloseStore(extra_store, 0);
                CertFreeCertificateContext(cert_ctx);
                session->error = "TLS: certificate chain has too many intermediates";
                return RT_TLS_ERROR_HANDSHAKE;
            }
            cert_index++;
            if (cert_len > UINT32_MAX) {
                if (chain_engine)
                    CertFreeCertificateChainEngine(chain_engine);
                if (root_store)
                    CertCloseStore(root_store, 0);
                CertCloseStore(extra_store, 0);
                CertFreeCertificateContext(cert_ctx);
                session->error = "TLS: intermediate certificate is too large for Windows CryptoAPI";
                return RT_TLS_ERROR_HANDSHAKE;
            }
            if (!CertAddEncodedCertificateToStore(extra_store,
                                                  X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
                                                  cert_der,
                                                  (DWORD)cert_len,
                                                  CERT_STORE_ADD_ALWAYS,
                                                  NULL)) {
                if (chain_engine)
                    CertFreeCertificateChainEngine(chain_engine);
                if (root_store)
                    CertCloseStore(root_store, 0);
                CertCloseStore(extra_store, 0);
                CertFreeCertificateContext(cert_ctx);
                session->error = "TLS: could not add intermediate certificate to store";
                return RT_TLS_ERROR_HANDSHAKE;
            }
        }
    }

    PCCERT_CHAIN_CONTEXT chain_ctx = NULL;
    BOOL ok = CertGetCertificateChain(
        chain_engine, cert_ctx, NULL, extra_store, &chain_para, 0, NULL, &chain_ctx);
    CertCloseStore(extra_store, 0);

    if (!ok || !chain_ctx) {
        if (chain_ctx)
            CertFreeCertificateChain(chain_ctx);
        if (chain_engine)
            CertFreeCertificateChainEngine(chain_engine);
        if (root_store)
            CertCloseStore(root_store, 0);
        CertFreeCertificateContext(cert_ctx);
        session->error = "TLS: CertGetCertificateChain failed";
        return RT_TLS_ERROR_HANDSHAKE;
    }

    CERT_CHAIN_POLICY_PARA policy_para = {0};
    policy_para.cbSize = sizeof(policy_para);

    CERT_CHAIN_POLICY_STATUS policy_status = {0};
    policy_status.cbSize = sizeof(policy_status);

    ok = CertVerifyCertificateChainPolicy(
        CERT_CHAIN_POLICY_BASE, chain_ctx, &policy_para, &policy_status);

    CertFreeCertificateChain(chain_ctx);
    if (chain_engine)
        CertFreeCertificateChainEngine(chain_engine);
    if (root_store)
        CertCloseStore(root_store, 0);
    CertFreeCertificateContext(cert_ctx);

    if (!ok || policy_status.dwError != 0) {
        session->error = "TLS: certificate chain validation failed (Windows)";
        return RT_TLS_ERROR_HANDSHAKE;
    }

    return RT_TLS_OK;
}

/// @brief Check whether a DER INTEGER is a canonical positive ECDSA scalar.
/// @details Zero, negative encodings, and redundant leading-zero octets are
///          rejected. A single leading zero is accepted only when required to
///          keep the following high bit from indicating a negative value.
/// @param[in] bytes INTEGER value bytes, excluding the DER tag and length.
/// @param[in] len Number of value bytes at @p bytes.
/// @return 1 for a canonical positive encoding; otherwise 0.
static int ecdsa_der_integer_is_canonical(const uint8_t *bytes, size_t len) {
    if (!bytes || len == 0)
        return 0;
    if ((bytes[0] & 0x80u) != 0)
        return 0;
    if (len == 1)
        return bytes[0] != 0x00;
    if (bytes[0] == 0x00)
        return (bytes[1] & 0x80u) != 0;
    return 1;
}

/// @brief Parse a DER-encoded ECDSA signature for CNG, writing raw r || s bytes.
/// @details Both INTEGERs must use canonical positive DER encodings. Required
///          sign-padding is removed and each scalar is left-padded to 32 bytes.
/// @param[in] sig Complete DER-encoded ECDSA signature.
/// @param[in] sig_len Length of @p sig in bytes.
/// @param[out] r_out Receives the normalized 32-byte big-endian R scalar.
/// @param[out] s_out Receives the normalized 32-byte big-endian S scalar.
/// @return 0 on success, -1 on malformed input.
static int parse_ecdsa_sig_der(const uint8_t *sig,
                               size_t sig_len,
                               uint8_t r_out[32],
                               uint8_t s_out[32]) {
    uint8_t tag;
    size_t vl, hl;
    const uint8_t *inner = NULL;
    size_t inner_rem = 0;
    const uint8_t *r_bytes = NULL;
    const uint8_t *s_bytes = NULL;
    size_t r_len = 0;
    size_t s_len = 0;

    if (der_read_tlv(sig, sig_len, &tag, &vl, &hl) != 0 || tag != 0x30 || hl + vl != sig_len)
        return -1;
    inner = sig + hl;
    inner_rem = vl;
    if (der_read_tlv(inner, inner_rem, &tag, &vl, &hl) != 0 || tag != 0x02)
        return -1;
    r_bytes = inner + hl;
    r_len = vl;
    if (!ecdsa_der_integer_is_canonical(r_bytes, r_len))
        return -1;
    inner += hl + vl;
    inner_rem -= hl + vl;
    if (der_read_tlv(inner, inner_rem, &tag, &vl, &hl) != 0 || tag != 0x02)
        return -1;
    s_bytes = inner + hl;
    s_len = vl;
    if (!ecdsa_der_integer_is_canonical(s_bytes, s_len))
        return -1;
    inner += hl + vl;
    inner_rem -= hl + vl;
    if (inner_rem != 0)
        return -1;

    if (r_len > 1 && r_bytes[0] == 0x00) {
        r_bytes++;
        r_len--;
    }
    if (s_len > 1 && s_bytes[0] == 0x00) {
        s_bytes++;
        s_len--;
    }
    if (r_len > 32 || s_len > 32)
        return -1;
    memset(r_out, 0, 32);
    memset(s_out, 0, 32);
    memcpy(r_out + (32 - r_len), r_bytes, r_len);
    memcpy(s_out + (32 - s_len), s_bytes, s_len);
    return 0;
}

/// @brief Build and hash the CertificateVerify content for Windows CNG verification.
/// @details Constructs the 130-byte RFC 8446 server CertificateVerify content,
///          then hashes it with SHA-256, SHA-384, or SHA-512 as selected by
///          @p sig_scheme. Output storage and the optional length are cleared
///          before input validation.
/// @param[in] sig_scheme TLS SignatureScheme value selecting the digest.
/// @param[in] transcript_hash Stored 32-byte handshake transcript hash.
/// @param[out] out Buffer of at least 64 bytes that receives the digest prefix.
/// @param[out] hash_len_out Optional destination for the selected digest size.
/// @return 1 on success, 0 for unknown or unsupported sig_scheme.
static int build_cert_verify_hash_for_scheme_win(uint16_t sig_scheme,
                                                 const uint8_t transcript_hash[32],
                                                 uint8_t out[64],
                                                 size_t *hash_len_out) {
    uint8_t content[130];
    size_t hash_len = sig_scheme_hash_len(sig_scheme);
    if (hash_len_out)
        *hash_len_out = 0;
    if (out)
        memset(out, 0, 64);
    if (!transcript_hash || !out || hash_len == 0)
        return 0;
    if (hash_len_out)
        *hash_len_out = hash_len;
    build_cert_verify_message(transcript_hash, content);
    switch (hash_len) {
        case 32:
            rt_sha256(content, sizeof(content), out);
            return 1;
        case 48:
            rt_sha384(content, sizeof(content), out);
            return 1;
        case 64:
            rt_sha512(content, sizeof(content), out);
            return 1;
        default:
            return 0;
    }
}

/// @brief Windows path: verify CertificateVerify via Windows CNG.
/// @details Builds and hashes the RFC 8446 signed content, imports the leaf
///          certificate's public key, and verifies ECDSA P-256 or RSA-PSS
///          through CNG. All certificate and key handles are released on every
///          path. On failure, @p session receives a stable error string when
///          it is non-null.
/// @param[in,out] session TLS session containing the transcript hash and leaf certificate.
/// @param[in] data CertificateVerify body beginning with scheme and signature length fields.
/// @param[in] len Number of bytes available at @p data.
/// @return RT_TLS_OK on success, or RT_TLS_ERROR_HANDSHAKE on malformed input,
///         unsupported algorithms, import errors, or signature failure.
int tls_verify_cert_verify(rt_tls_session_t *session, const uint8_t *data, size_t len) {
    if (!session)
        return RT_TLS_ERROR_HANDSHAKE;
    if (!data || len < 4) {
        session->error = "TLS: CertificateVerify message too short";
        return RT_TLS_ERROR_HANDSHAKE;
    }

    uint16_t sig_scheme = ((uint16_t)data[0] << 8) | data[1];
    uint16_t sig_len = ((uint16_t)data[2] << 8) | data[3];
    if ((size_t)sig_len > len - 4u) {
        session->error = "TLS: CertificateVerify signature length overflows";
        return RT_TLS_ERROR_HANDSHAKE;
    }
    if ((size_t)sig_len != len - 4u) {
        session->error = "TLS: CertificateVerify signature length mismatch";
        return RT_TLS_ERROR_HANDSHAKE;
    }

    const uint8_t *sig_bytes = data + 4;

    uint8_t content_hash[64];
    size_t hash_len = 0;
    const uint8_t *server_cert_der = tls_session_server_cert_der(session);

    if (!server_cert_der) {
        session->error = "TLS: CertVerify: no certificate stored";
        return RT_TLS_ERROR_HANDSHAKE;
    }
    if (session->server_cert_der_len > UINT32_MAX) {
        session->error = "TLS: CertVerify: certificate is too large for Windows CryptoAPI";
        return RT_TLS_ERROR_HANDSHAKE;
    }
    if (!build_cert_verify_hash_for_scheme_win(
            sig_scheme, session->cert_transcript_hash, content_hash, &hash_len)) {
        session->error = "TLS: CertificateVerify: unsupported scheme (Windows)";
        return RT_TLS_ERROR_HANDSHAKE;
    }

    PCCERT_CONTEXT cert_ctx = CertCreateCertificateContext(X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
                                                           server_cert_der,
                                                           (DWORD)session->server_cert_der_len);

    if (!cert_ctx) {
        session->error = "TLS: CertVerify: could not parse certificate (Windows)";
        return RT_TLS_ERROR_HANDSHAKE;
    }

    if (sig_scheme == 0x0403) {
        BCRYPT_KEY_HANDLE key = NULL;
        uint8_t sig_r[32];
        uint8_t sig_s[32];
        uint8_t raw_sig[64];
        NTSTATUS status;

        if (parse_ecdsa_sig_der(sig_bytes, sig_len, sig_r, sig_s) != 0) {
            CertFreeCertificateContext(cert_ctx);
            session->error = "TLS: CertificateVerify: malformed ECDSA signature (Windows)";
            return RT_TLS_ERROR_HANDSHAKE;
        }
        memcpy(raw_sig, sig_r, sizeof(sig_r));
        memcpy(raw_sig + sizeof(sig_r), sig_s, sizeof(sig_s));

        if (!CryptImportPublicKeyInfoEx2(
                X509_ASN_ENCODING, &cert_ctx->pCertInfo->SubjectPublicKeyInfo, 0, NULL, &key) ||
            !key) {
            if (key)
                BCryptDestroyKey(key);
            CertFreeCertificateContext(cert_ctx);
            session->error = "TLS: CertVerify: CryptImportPublicKeyInfoEx2 failed";
            return RT_TLS_ERROR_HANDSHAKE;
        }

        status = BCryptVerifySignature(
            key, NULL, content_hash, (ULONG)hash_len, raw_sig, (ULONG)sizeof(raw_sig), 0);
        BCryptDestroyKey(key);
        CertFreeCertificateContext(cert_ctx);

        if (!BCRYPT_SUCCESS(status)) {
            session->error = "TLS: CertificateVerify signature failed (Windows)";
            return RT_TLS_ERROR_HANDSHAKE;
        }

        return RT_TLS_OK;
    }

    if (sig_scheme == 0x0804 || sig_scheme == 0x0805 || sig_scheme == 0x0806) {
        BCRYPT_KEY_HANDLE key = NULL;
        LPCWSTR hash_alg = (sig_scheme == 0x0804)   ? BCRYPT_SHA256_ALGORITHM
                           : (sig_scheme == 0x0805) ? BCRYPT_SHA384_ALGORITHM
                                                    : BCRYPT_SHA512_ALGORITHM;
        BCRYPT_PSS_PADDING_INFO padding_info;
        NTSTATUS status;

        if (!CryptImportPublicKeyInfoEx2(
                X509_ASN_ENCODING, &cert_ctx->pCertInfo->SubjectPublicKeyInfo, 0, NULL, &key) ||
            !key) {
            if (key)
                BCryptDestroyKey(key);
            CertFreeCertificateContext(cert_ctx);
            session->error = "TLS: CertVerify: CryptImportPublicKeyInfoEx2 failed";
            return RT_TLS_ERROR_HANDSHAKE;
        }

        padding_info.pszAlgId = hash_alg;
        padding_info.cbSalt = (ULONG)hash_len;
        status = BCryptVerifySignature(key,
                                       &padding_info,
                                       content_hash,
                                       (ULONG)hash_len,
                                       (PUCHAR)sig_bytes,
                                       (ULONG)sig_len,
                                       BCRYPT_PAD_PSS);
        BCryptDestroyKey(key);
        CertFreeCertificateContext(cert_ctx);

        if (!BCRYPT_SUCCESS(status)) {
            session->error = "TLS: CertificateVerify signature failed (Windows)";
            return RT_TLS_ERROR_HANDSHAKE;
        }

        return RT_TLS_OK;
    }

    CertFreeCertificateContext(cert_ctx);
    session->error = "TLS: CertificateVerify: unsupported scheme (Windows)";
    return RT_TLS_ERROR_HANDSHAKE;
}
#endif // defined(_WIN32)
