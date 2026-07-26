//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/network/rt_tls_verify_common.c
// Purpose: Platform-neutral TLS certificate verification helpers: DER/TLV parsing,
//   certificate-list iteration, subject/SAN extraction, hostname (incl.
//   wildcard/IP) matching, critical-extension checks, and CertificateVerify
//   message construction. Shared by the Windows and POSIX verification TUs.
//
// Links: rt_tls_verify_internal.h, rt_tls_verify_win.c, rt_tls_verify_posix.c, rt_tls_internal.h
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_tls_verify_common.c
 * @brief Implements platform-neutral X.509 and TLS verification helpers.
 * @details Shared routines parse bounded TLS certificate lists and DER
 *          structures, inspect certificate extensions, match DNS names and IP
 *          subject alternatives, validate critical policy, and construct the
 *          TLS 1.3 CertificateVerify transcript input used by platform trust
 *          adapters.
 */

#include "rt_tls_verify_internal.h"

/// @brief Maximum DER certificate-list bytes retained for verification.
/// @details TLS handshake framing already caps messages at 16 MiB, but keeping
///          a smaller certificate-list limit avoids allocating attacker-sized
///          chains before platform trust evaluation.
#define RT_TLS_MAX_CERT_CHAIN_BYTES (1024u * 1024u)

//=============================================================================
// Certificate Validation — CS-1, CS-2, CS-3
//=============================================================================

// ---------------------------------------------------------------------------
// B.2 — TLS 1.3 Certificate message parser
// ---------------------------------------------------------------------------

/// @brief Advance through one entry of a TLS 1.3 certificate_list.
/// @details Each entry is `[3-byte length][DER cert][2-byte ext length][extensions]`.
///          Bounds checks fire in three layers (each protecting the next):
///          1. The 5-byte header itself must fit (cert-length + ext-length
///             prefixes, before reading either).
///          2. The DER cert body must fit, *and* leave room for the 2-byte
///             extensions length.
///          3. The extensions body must fit within the list.
///          A zero `der_len` is rejected — TLS 1.3 forbids empty certificate
///          entries, and treating an empty entry as valid would let an attacker
///          push the parser past structural integrity for the entries that
///          follow. Updates `*pos` to point past the entry and writes the DER
///          pointer + length out-parameters on success.
/// @param list TLS certificate_list entry bytes.
/// @param list_len Number of bytes in @p list.
/// @param pos In/out cursor offset.
/// @param cert_der Destination for a borrowed certificate pointer.
/// @param cert_len Destination for certificate byte length.
/// @return 0 on success; -1 on any structural error (length underflow,
///         extensions past end of list, zero-length cert, null arguments).
int tls_next_certificate_entry(
    const uint8_t *list, size_t list_len, size_t *pos, const uint8_t **cert_der, size_t *cert_len) {
    if (!list || !pos || !cert_der || !cert_len || *pos > list_len || list_len - *pos < 5)
        return -1;

    size_t entry_pos = *pos;
    size_t der_len = ((size_t)list[entry_pos] << 16) | ((size_t)list[entry_pos + 1] << 8) |
                     (size_t)list[entry_pos + 2];
    entry_pos += 3;
    if (der_len == 0 || entry_pos > list_len || der_len > list_len - entry_pos ||
        list_len - entry_pos - der_len < 2)
        return -1;

    *cert_der = list + entry_pos;
    *cert_len = der_len;
    entry_pos += der_len;

    size_t ext_len = ((size_t)list[entry_pos] << 8) | (size_t)list[entry_pos + 1];
    entry_pos += 2;
    if (entry_pos > list_len || ext_len > list_len - entry_pos)
        return -1;

    *pos = entry_pos + ext_len;
    return 0;
}

/// @brief Parse the TLS 1.3 Certificate handshake message, store the leaf
///        certificate DER, and retain the full certificate_list entries.
///
/// Certificate message format (RFC 8446 §4.4.2):
///   1 byte:  certificate_request_context length
///   N bytes: context (ignored by client)
///   3 bytes: certificate_list total length
///   For each entry:
///     3 bytes: cert_data length
///     N bytes: DER-encoded certificate
///     2 bytes: extensions length
///     N bytes: certificate extensions (ignored)
/// @param session Client session receiving owned chain and leaf copies.
/// @param data Certificate handshake body.
/// @param len Number of body bytes.
/// @return @ref RT_TLS_OK on success or a negative handshake/memory status.
int tls_parse_certificate_msg(rt_tls_session_t *session, const uint8_t *data, size_t len) {
    if (!data || len < 4) {
        session->error = "TLS: Certificate message too short";
        return RT_TLS_ERROR_HANDSHAKE;
    }

    size_t pos = 0;

    // Skip certificate_request_context
    uint8_t ctx_len = data[pos++];
    if ((size_t)ctx_len > len - pos) {
        session->error = "TLS: Certificate context overflows message";
        return RT_TLS_ERROR_HANDSHAKE;
    }
    pos += ctx_len;

    // Read certificate_list length (3-byte big-endian)
    if (len - pos < 3) {
        session->error = "TLS: Certificate list length missing";
        return RT_TLS_ERROR_HANDSHAKE;
    }
    size_t list_len = ((size_t)data[pos] << 16) | ((size_t)data[pos + 1] << 8) | data[pos + 2];
    pos += 3;

    if (list_len > len - pos) {
        session->error = "TLS: Certificate list overflows message";
        return RT_TLS_ERROR_HANDSHAKE;
    }
    if (list_len != len - pos) {
        session->error = "TLS: Certificate message has trailing data";
        return RT_TLS_ERROR_HANDSHAKE;
    }

    if (list_len < 5) {
        session->error = "TLS: Certificate list too short for one entry";
        return RT_TLS_ERROR_HANDSHAKE;
    }
    if (list_len > RT_TLS_MAX_CERT_CHAIN_BYTES) {
        session->error = "TLS: Certificate chain too large";
        return RT_TLS_ERROR_HANDSHAKE;
    }

    const uint8_t *list = data + pos;
    size_t list_pos = 0;
    const uint8_t *leaf_der = NULL;
    size_t leaf_len = 0;
    size_t cert_count = 0;

    while (list_pos < list_len) {
        const uint8_t *cert_der = NULL;
        size_t cert_len = 0;
        if (tls_next_certificate_entry(list, list_len, &list_pos, &cert_der, &cert_len) != 0) {
            session->error = "TLS: malformed certificate chain entry";
            return RT_TLS_ERROR_HANDSHAKE;
        }
        if (cert_count == 0) {
            leaf_der = cert_der;
            leaf_len = cert_len;
        }
        cert_count++;
    }

    if (!leaf_der || leaf_len == 0 || cert_count == 0) {
        session->error = "TLS: Certificate message contained no leaf certificate";
        return RT_TLS_ERROR_HANDSHAKE;
    }

    uint8_t *list_copy = (uint8_t *)malloc(list_len);
    if (!list_copy) {
        session->error = "TLS: memory allocation failed for certificate chain";
        return RT_TLS_ERROR_MEMORY;
    }
    memcpy(list_copy, list, list_len);
    uint8_t *leaf_copy = NULL;
    if (leaf_len > sizeof(session->server_cert_der)) {
        leaf_copy = (uint8_t *)malloc(leaf_len);
        if (!leaf_copy) {
            free(list_copy);
            session->error = "TLS: memory allocation failed for leaf certificate";
            return RT_TLS_ERROR_MEMORY;
        }
        memcpy(leaf_copy, leaf_der, leaf_len);
    }

    if (session->server_cert_list) {
        free(session->server_cert_list);
        session->server_cert_list = NULL;
        session->server_cert_list_len = 0;
        session->server_cert_count = 0;
    }
    if (session->server_cert_der_heap) {
        free(session->server_cert_der_heap);
        session->server_cert_der_heap = NULL;
        session->server_cert_der_len = 0;
    }

    if (leaf_copy) {
        session->server_cert_der_heap = leaf_copy;
    } else {
        memset(session->server_cert_der, 0, sizeof(session->server_cert_der));
        memcpy(session->server_cert_der, leaf_der, leaf_len);
    }
    session->server_cert_der_len = leaf_len;
    session->server_cert_list = list_copy;
    session->server_cert_list_len = list_len;
    session->server_cert_count = cert_count;
    return RT_TLS_OK;
}

// ---------------------------------------------------------------------------
// B.3 — ASN.1 DER helpers and X.509 hostname extraction
// ---------------------------------------------------------------------------

/// @brief Read one ASN.1 TLV (tag-length-value) header.
/// @param buf     Buffer to parse.
/// @param buf_len Remaining bytes in buf.
/// @param tag     Output: the tag byte.
/// @param val_len Output: the length of the value field.
/// @param hdr_len Output: the total header length (tag + length octets).
/// @return 0 on success, -1 on error.
int der_read_tlv(
    const uint8_t *buf, size_t buf_len, uint8_t *tag, size_t *val_len, size_t *hdr_len) {
    if (!buf || !tag || !val_len || !hdr_len || buf_len < 2)
        return -1;

    *tag = buf[0];

    uint8_t l0 = buf[1];
    if (l0 < 0x80) {
        *val_len = l0;
        *hdr_len = 2;
    } else {
        size_t num_len_bytes = l0 & 0x7F;
        if (num_len_bytes == 0 || num_len_bytes > sizeof(size_t) || 2 + num_len_bytes > buf_len ||
            buf[2] == 0x00)
            return -1;

        size_t v = 0;
        for (size_t i = 0; i < num_len_bytes; i++)
            v = (v << 8) | buf[2 + i];
        if (v < 128)
            return -1;

        *val_len = v;
        *hdr_len = 2 + num_len_bytes;
    }

    if (*hdr_len > buf_len || *val_len > buf_len - *hdr_len)
        return -1;

    return 0;
}

/// @brief Validate absent or canonical DER NULL algorithm parameters.
/// @param params Remaining AlgorithmIdentifier parameter bytes.
/// @param params_len Number of bytes in @p params.
/// @return One for absent parameters or one empty NULL TLV; otherwise zero.
RT_TLS_MAYBE_UNUSED int der_params_absent_or_null(const uint8_t *params, size_t params_len) {
    uint8_t tag;
    size_t vl;
    size_t hl;
    if (params_len == 0)
        return 1;
    if (der_read_tlv(params, params_len, &tag, &vl, &hl) != 0 || tag != 0x05)
        return 0;
    return vl == 0 && hl + vl == params_len;
}

/// @brief Compare a DER OID payload with expected value bytes.
/// @param buf Candidate OID value bytes.
/// @param buf_len Number of candidate bytes.
/// @param oid_val Expected OID value bytes.
/// @param oid_val_len Number of expected bytes.
/// @return One for exact equality; otherwise zero.
static int oid_matches(const uint8_t *buf,
                       size_t buf_len,
                       const uint8_t *oid_val,
                       size_t oid_val_len) {
    return buf_len == oid_val_len && memcmp(buf, oid_val, oid_val_len) == 0;
}

// Known OID encoded values (value bytes only, after the OID tag+length)
static const uint8_t OID_COMMON_NAME[] = {0x55, 0x04, 0x03};            // 2.5.4.3
static const uint8_t OID_SUBJECT_ALT_NAME[] = {0x55, 0x1d, 0x11};       // 2.5.29.17
static const uint8_t OID_X509_KEY_USAGE[] = {0x55, 0x1d, 0x0f};         // 2.5.29.15
static const uint8_t OID_X509_BASIC_CONSTRAINTS[] = {0x55, 0x1d, 0x13}; // 2.5.29.19
static const uint8_t OID_X509_EXT_KEY_USAGE[] = {0x55, 0x1d, 0x25};     // 2.5.29.37

/// @brief Return a pointer into cert_der at the DER-encoded Subject field, and write its total
///        TLV length (header + value) into *subject_len.
/// @param cert_der Complete certificate DER bytes.
/// @param cert_len Number of certificate bytes.
/// @param subject_len Destination for the Subject TLV length.
/// @return Non-null pointer into cert_der on success, NULL if the certificate is malformed.
RT_TLS_MAYBE_UNUSED const uint8_t *cert_get_subject(const uint8_t *cert_der,
                                                    size_t cert_len,
                                                    size_t *subject_len) {
    uint8_t t;
    size_t vl, hl, cert_hl, tbs_hl;
    const uint8_t *tbs = NULL;
    size_t tbs_len = 0;
    size_t pos = 0;

    if (!subject_len || der_read_tlv(cert_der, cert_len, &t, &vl, &cert_hl) != 0 || t != 0x30)
        return NULL;
    if (der_read_tlv(cert_der + cert_hl, vl, &t, &tbs_len, &tbs_hl) != 0 || t != 0x30)
        return NULL;
    tbs = cert_der + cert_hl + tbs_hl;

    if (der_read_tlv(tbs + pos, tbs_len - pos, &t, &vl, &hl) != 0)
        return NULL;
    if (t == 0xA0)
        pos += hl + vl;

    for (int i = 0; i < 4; i++) {
        if (der_read_tlv(tbs + pos, tbs_len - pos, &t, &vl, &hl) != 0)
            return NULL;
        pos += hl + vl;
    }
    if (der_read_tlv(tbs + pos, tbs_len - pos, &t, &vl, &hl) != 0 || t != 0x30)
        return NULL;
    *subject_len = hl + vl;
    return tbs + pos;
}

/// @brief Validate a DNS name byte sequence for TLS hostname matching.
/// @details Enforces RFC 1035 / RFC 6125 hostname rules:
///          - Length 1..255 bytes total.
///          - Each label 1..63 bytes (no consecutive dots).
///          - Only ASCII letters, digits, and hyphen; no NUL, control bytes,
///            or non-ASCII (IDNA hostnames must be passed in punycode form).
///          - Wildcard `*` only valid when @p allow_wildcard is true, only
///            in the leftmost label, and only when followed by `.` plus a
///            non-empty suffix (so `*.example.com` is OK but `*.com` is
///            rejected by tls_wildcard_suffix_allowed downstream).
/// @param name DNS presentation bytes.
/// @param len Number of bytes in @p name.
/// @param allow_wildcard Nonzero to permit a complete leftmost wildcard label.
/// @return 1 if the name is well-formed, 0 otherwise.
static int tls_dns_name_bytes_valid(const uint8_t *name, size_t len, int allow_wildcard) {
    if (!name || len == 0 || len >= 256)
        return 0;
    size_t label_len = 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t c = name[i];
        if (c == 0 || c < 0x20 || c >= 0x7f)
            return 0;
        if (c == '.') {
            if (label_len == 0 && i != len - 1)
                return 0;
            label_len = 0;
            continue;
        }
        if (c == '*') {
            if (!allow_wildcard || i != 0 || len < 3 || name[1] != '.')
                return 0;
        } else if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                     c == '-')) {
            return 0;
        }
        label_len++;
        if (label_len > 63)
            return 0;
    }
    return 1;
}

/// @brief NUL-terminated convenience wrapper around tls_dns_name_bytes_valid.
/// @details Returns 0 for NULL. Otherwise calls strlen and delegates to the
///          byte-buffer validator. Used by call sites that already have a
///          C-string-shaped hostname.
/// @param name NUL-terminated DNS name.
/// @param allow_wildcard Nonzero to permit a complete leftmost wildcard label.
/// @return One for a valid DNS presentation name; otherwise zero.
static int tls_dns_name_valid(const char *name, int allow_wildcard) {
    return name && tls_dns_name_bytes_valid((const uint8_t *)name, strlen(name), allow_wildcard);
}

/// @brief Check that a wildcard certificate's matching suffix is "long enough" to be safe.
/// @details Wildcards like `*.com` are dangerous — they would match every
///          .com domain. We reject any single-label suffix and a curated
///          set of public-suffix-list-ish multi-label suffixes (.co.uk,
///          .com.au, etc.). This is intentionally conservative: callers
///          deploying with private CAs whose policy permits broader
///          wildcards must validate at the application layer rather than
///          relying on this check to be lenient.
/// @param suffix DNS suffix following the wildcard label.
/// @return 1 if the suffix is acceptably specific, 0 if too broad.
static int tls_wildcard_suffix_allowed(const char *suffix) {
    static const char *const blocked_suffixes[] = {
        "com",    "net",    "org",   "edu",    "gov",   "mil",    "int",    "uk",
        "co.uk",  "org.uk", "ac.uk", "gov.uk", "au",    "com.au", "net.au", "org.au",
        "edu.au", "gov.au", "jp",    "co.jp",  "ne.jp", "or.jp",  "de",     "fr",
        "it",     "es",     "nl",    "br",     "ca",    "io",     "dev",    "app"};
    static const char *const public_sld_labels[] = {
        "ac", "co", "com", "edu", "gov", "mil", "net", "ne", "or", "org"};

    if (!suffix || !tls_dns_name_valid(suffix, 0))
        return 0;
    if (!strchr(suffix, '.'))
        return 0;
    for (size_t i = 0; i < sizeof(blocked_suffixes) / sizeof(blocked_suffixes[0]); i++) {
        if (strcasecmp(suffix, blocked_suffixes[i]) == 0)
            return 0;
    }
    const char *first_dot = strchr(suffix, '.');
    const char *second_dot = first_dot ? strchr(first_dot + 1, '.') : NULL;
    if (first_dot && !second_dot && strlen(first_dot + 1) == 2) {
        size_t first_label_len = (size_t)(first_dot - suffix);
        for (size_t i = 0; i < sizeof(public_sld_labels) / sizeof(public_sld_labels[0]); i++) {
            if (strlen(public_sld_labels[i]) == first_label_len &&
                strncasecmp(suffix, public_sld_labels[i], first_label_len) == 0) {
                return 0;
            }
        }
    }
    return 1;
}

/// @brief Test whether the verifier knows how to honor this critical extension OID.
/// @details Per RFC 5280 §4.2, a certificate that contains a critical
///          extension the verifier doesn't understand MUST be rejected.
///          We currently support: subjectAltName, keyUsage, basicConstraints,
///          and extendedKeyUsage. Anything else marked critical causes the
///          chain to fail.
/// @param oid DER OID value bytes.
/// @param oid_len Number of OID bytes.
/// @return 1 if the verifier handles this OID, 0 otherwise.
static int cert_critical_extension_supported(const uint8_t *oid, size_t oid_len) {
    return oid_matches(oid, oid_len, OID_SUBJECT_ALT_NAME, sizeof(OID_SUBJECT_ALT_NAME)) ||
           oid_matches(oid, oid_len, OID_X509_KEY_USAGE, sizeof(OID_X509_KEY_USAGE)) ||
           oid_matches(
               oid, oid_len, OID_X509_BASIC_CONSTRAINTS, sizeof(OID_X509_BASIC_CONSTRAINTS)) ||
           oid_matches(oid, oid_len, OID_X509_EXT_KEY_USAGE, sizeof(OID_X509_EXT_KEY_USAGE));
}

/// @brief Walk the TBSCertificate.extensions sequence and reject if any critical extension is
/// unrecognized.
/// @details Implements the "MUST reject" rule from RFC 5280 §4.2: parses
///          the certificate's extensions list, and for each entry marked
///          critical that isn't in cert_critical_extension_supported, the
///          chain fails. Returns 1 if any unsupported critical extension
///          is found (caller should reject the cert), 0 if all critical
///          extensions are recognized.
/// @param cert_der Complete certificate DER bytes.
/// @param cert_len Number of certificate bytes.
/// @return 1 if the cert should be rejected, 0 if safe to continue.
int cert_has_unsupported_critical_extension(const uint8_t *cert_der, size_t cert_len) {
    uint8_t tag;
    size_t vl, hl;
    if (der_read_tlv(cert_der, cert_len, &tag, &vl, &hl) != 0 || tag != 0x30)
        return 1;
    const uint8_t *p = cert_der + hl;
    size_t rem = vl;
    if (der_read_tlv(p, rem, &tag, &vl, &hl) != 0 || tag != 0x30)
        return 1;
    const uint8_t *tbs = p + hl;
    size_t tbs_rem = vl;

    if (der_read_tlv(tbs, tbs_rem, &tag, &vl, &hl) != 0)
        return 1;
    if (tag == 0xA0) {
        tbs += hl + vl;
        tbs_rem -= hl + vl;
    }

    for (int i = 0; i < 6; i++) {
        if (der_read_tlv(tbs, tbs_rem, &tag, &vl, &hl) != 0)
            return 1;
        tbs += hl + vl;
        tbs_rem -= hl + vl;
    }

    while (tbs_rem > 0) {
        if (der_read_tlv(tbs, tbs_rem, &tag, &vl, &hl) != 0)
            return 1;
        if (tag == 0xA3) {
            uint8_t seq_tag;
            size_t seq_len, seq_hl;
            if (der_read_tlv(tbs + hl, vl, &seq_tag, &seq_len, &seq_hl) != 0 || seq_tag != 0x30)
                return 1;
            const uint8_t *exts = tbs + hl + seq_hl;
            size_t exts_rem = seq_len;
            while (exts_rem > 0) {
                uint8_t ext_tag;
                size_t ext_len, ext_hl;
                if (der_read_tlv(exts, exts_rem, &ext_tag, &ext_len, &ext_hl) != 0 ||
                    ext_tag != 0x30)
                    return 1;
                const uint8_t *ext = exts + ext_hl;
                size_t ext_rem = ext_len;
                uint8_t oid_tag;
                size_t oid_len, oid_hl;
                if (der_read_tlv(ext, ext_rem, &oid_tag, &oid_len, &oid_hl) != 0 || oid_tag != 0x06)
                    return 1;
                const uint8_t *oid = ext + oid_hl;
                ext += oid_hl + oid_len;
                ext_rem -= oid_hl + oid_len;

                int critical = 0;
                uint8_t next_tag;
                size_t next_len, next_hl;
                if (der_read_tlv(ext, ext_rem, &next_tag, &next_len, &next_hl) != 0)
                    return 1;
                if (next_tag == 0x01) {
                    if (next_len != 1)
                        return 1;
                    critical = ext[next_hl] != 0;
                    ext += next_hl + next_len;
                    ext_rem -= next_hl + next_len;
                    if (der_read_tlv(ext, ext_rem, &next_tag, &next_len, &next_hl) != 0)
                        return 1;
                }
                if (next_tag != 0x04)
                    return 1;
                if (critical && !cert_critical_extension_supported(oid, oid_len))
                    return 1;

                exts += ext_hl + ext_len;
                exts_rem -= ext_hl + ext_len;
            }
            return 0;
        }
        tbs += hl + vl;
        tbs_rem -= hl + vl;
    }
    return 0;
}

/// @brief Extract DNS names from SubjectAltName extension value.
/// @param ext_val Points to the value bytes of the SAN extension (after the OCTET STRING wrapper).
/// @param ext_len Length of ext_val.
/// @param san_out Pre-allocated array for DNS name strings.
/// @param max_names Maximum number of names to return.
/// @param count Output: number of names found.
static void extract_san_from_ext_value(
    const uint8_t *ext_val, size_t ext_len, char san_out[][256], int max_names, int *count) {
    *count = 0;

    // SubjectAltName is an OCTET STRING containing a GeneralNames SEQUENCE
    uint8_t t;
    size_t vl, hl;
    if (der_read_tlv(ext_val, ext_len, &t, &vl, &hl) != 0 || t != 0x04 /* OCTET STRING */)
        return;

    const uint8_t *inner = ext_val + hl;
    size_t inner_len = vl;

    // GeneralNames SEQUENCE
    if (der_read_tlv(inner, inner_len, &t, &vl, &hl) != 0 || t != 0x30 /* SEQUENCE */)
        return;

    const uint8_t *names = inner + hl;
    size_t names_len = vl;
    size_t pos = 0;

    while (pos < names_len && *count < max_names) {
        if (der_read_tlv(names + pos, names_len - pos, &t, &vl, &hl) != 0)
            break;

        // dNSName is context tag [2] = 0x82
        if (t == 0x82 && tls_dns_name_bytes_valid(names + pos + hl, vl, 1)) {
            memcpy(san_out[*count], names + pos + hl, vl);
            san_out[*count][vl] = '\0';
            (*count)++;
        }

        pos += hl + vl;
    }
}

/// @brief Detect IPv4 / IPv6 literal hostnames and copy into binary form.
///
/// Returns 1 if the hostname is an IP literal (with the binary
/// representation in `ip_out` and its length 4 or 16), 0 otherwise.
/// Used so SAN matching can compare against `iPAddress` entries
/// instead of (incorrectly) trying DNS-name matching.
/// @param hostname NUL-terminated candidate host.
/// @param ip_out Destination 16-byte address buffer.
/// @param ip_len Destination receiving four or sixteen.
/// @return One for a parsed IPv4/IPv6 literal; otherwise zero.
static int tls_hostname_is_ip_literal(const char *hostname, uint8_t ip_out[16], size_t *ip_len) {
    struct in_addr ipv4;
    struct in6_addr ipv6;

    if (!hostname || !ip_out || !ip_len)
        return 0;

    if (inet_pton(AF_INET, hostname, &ipv4) == 1) {
        memcpy(ip_out, &ipv4, 4);
        *ip_len = 4;
        return 1;
    }

    if (inet_pton(AF_INET6, hostname, &ipv6) == 1) {
        memcpy(ip_out, &ipv6, 16);
        *ip_len = 16;
        return 1;
    }

    return 0;
}

/// @brief Walk a SAN extension value looking for an `iPAddress` entry that matches.
///
/// SAN entries are tagged: 0x82 = dNSName, 0x87 = iPAddress. This
/// helper iterates entries, sets `*saw_ip_san=1` if any IP-typed entry
/// is present, and returns 1 if any entry's bytes equal `expected_ip`.
/// @param ext_val DER-wrapped SAN extension value.
/// @param ext_len Number of extension bytes.
/// @param expected_ip Binary IPv4 or IPv6 address.
/// @param expected_ip_len Four or sixteen bytes.
/// @param saw_ip_san Optional destination indicating any IP SAN was present.
/// @return One for an exact IP SAN match; otherwise zero.
static int san_ext_has_ip_match(const uint8_t *ext_val,
                                size_t ext_len,
                                const uint8_t *expected_ip,
                                size_t expected_ip_len,
                                int *saw_ip_san) {
    uint8_t t;
    size_t vl, hl;
    if (saw_ip_san)
        *saw_ip_san = 0;

    if (der_read_tlv(ext_val, ext_len, &t, &vl, &hl) != 0 || t != 0x04)
        return 0;

    const uint8_t *inner = ext_val + hl;
    size_t inner_len = vl;
    if (der_read_tlv(inner, inner_len, &t, &vl, &hl) != 0 || t != 0x30)
        return 0;

    const uint8_t *names = inner + hl;
    size_t names_len = vl;
    size_t pos = 0;
    while (pos < names_len) {
        if (der_read_tlv(names + pos, names_len - pos, &t, &vl, &hl) != 0)
            break;
        if (t == 0x87) {
            if (saw_ip_san)
                *saw_ip_san = 1;
            if (vl == expected_ip_len &&
                memcmp(names + pos + hl, expected_ip, expected_ip_len) == 0)
                return 1;
        }
        pos += hl + vl;
    }
    return 0;
}

/// @brief Top-level X.509 walker: find the SAN extension and run the IP match.
///
/// Drills through Certificate → TBSCertificate → Extensions context-tag
/// 3 ([3] EXPLICIT) → individual Extension SEQUENCEs → SAN OID match
/// → SAN value. Uses iterative DER parsing (no recursion) to bound
/// stack use on adversarial inputs.
/// @param der Complete certificate DER bytes.
/// @param der_len Number of certificate bytes.
/// @param expected_ip Binary IPv4 or IPv6 address.
/// @param expected_ip_len Four or sixteen bytes.
/// @param saw_ip_san Optional destination indicating any IP SAN was present.
/// @return One for an exact IP SAN match; otherwise zero.
static int tls_cert_has_matching_ip_san(const uint8_t *der,
                                        size_t der_len,
                                        const uint8_t *expected_ip,
                                        size_t expected_ip_len,
                                        int *saw_ip_san) {
    uint8_t t;
    size_t vl, hl;
    if (saw_ip_san)
        *saw_ip_san = 0;

    if (der_read_tlv(der, der_len, &t, &vl, &hl) != 0 || t != 0x30)
        return 0;

    const uint8_t *cert_val = der + hl;
    if (der_read_tlv(cert_val, vl, &t, &vl, &hl) != 0 || t != 0x30)
        return 0;

    const uint8_t *tbs_val = cert_val + hl;
    size_t tbs_len = vl;
    size_t pos = 0;

    while (pos < tbs_len) {
        if (der_read_tlv(tbs_val + pos, tbs_len - pos, &t, &vl, &hl) != 0)
            break;
        if (t == 0xA3) {
            const uint8_t *exts_wrap = tbs_val + pos + hl;
            uint8_t t2;
            size_t vl2, hl2;
            if (der_read_tlv(exts_wrap, vl, &t2, &vl2, &hl2) != 0 || t2 != 0x30)
                break;

            const uint8_t *exts = exts_wrap + hl2;
            size_t exts_len = vl2;
            size_t ep = 0;
            while (ep < exts_len) {
                uint8_t t3;
                size_t vl3, hl3;
                if (der_read_tlv(exts + ep, exts_len - ep, &t3, &vl3, &hl3) != 0)
                    break;
                if (t3 == 0x30) {
                    const uint8_t *ext = exts + ep + hl3;
                    uint8_t t4;
                    size_t vl4, hl4;
                    if (der_read_tlv(ext, vl3, &t4, &vl4, &hl4) == 0 && t4 == 0x06 &&
                        oid_matches(
                            ext + hl4, vl4, OID_SUBJECT_ALT_NAME, sizeof(OID_SUBJECT_ALT_NAME))) {
                        size_t after_oid = hl4 + vl4;
                        if (after_oid < vl3) {
                            uint8_t nt;
                            size_t nvl, nhl;
                            if (der_read_tlv(ext + after_oid, vl3 - after_oid, &nt, &nvl, &nhl) ==
                                0) {
                                if (nt == 0x01)
                                    after_oid += nhl + nvl;
                                if (after_oid < vl3) {
                                    return san_ext_has_ip_match(ext + after_oid,
                                                                vl3 - after_oid,
                                                                expected_ip,
                                                                expected_ip_len,
                                                                saw_ip_san);
                                }
                            }
                        }
                    }
                }
                ep += hl3 + vl3;
            }
            break;
        }
        pos += hl + vl;
    }

    return 0;
}

/// @brief Extract SubjectAltName dNSName entries from a certificate DER.
/// @details Walks the ASN.1 tree: Certificate → TBSCertificate → Extensions
///          ([3] EXPLICIT) → looking for the SubjectAltName extension
///          (OID 2.5.29.17). Within SAN, only the dNSName variant ([2] IMPLICIT
///          IA5String) is extracted; iPAddress and other GeneralName variants
///          are skipped. Each name is copied null-terminated into `san_out[i]`,
///          truncated to 255 bytes (SAN entries longer than that are skipped
///          silently — a name that doesn't fit in the buffer can't be safely
///          compared against a hostname anyway).
///
///          SAN is the canonical hostname list per RFC 6125 § 6.4; modern
///          certificate authorities are required to populate it. CN-based
///          matching is fallback only and should not be consulted when SAN
///          is present (caller's responsibility — `tls_verify_hostname` enforces
///          this ordering).
/// @param der Pointer to the certificate's DER bytes.
/// @param der_len Length of the DER buffer.
/// @param san_out Output array of fixed-size 256-byte name buffers.
/// @param max_names Capacity of `san_out`.
/// @return Number of dNSName entries written (0 if SAN extension absent or
///         no dNSName variants present).
int tls_extract_san_names(const uint8_t *der, size_t der_len, char san_out[][256], int max_names) {
    int count = 0;

    // Certificate SEQUENCE
    uint8_t t;
    size_t vl, hl;
    if (der_read_tlv(der, der_len, &t, &vl, &hl) != 0 || t != 0x30)
        return 0;

    // TBSCertificate SEQUENCE (first child of Certificate)
    const uint8_t *cert_val = der + hl;
    if (der_read_tlv(cert_val, vl, &t, &vl, &hl) != 0 || t != 0x30)
        return 0;

    // Walk TBSCertificate to find extensions [3] EXPLICIT context tag
    const uint8_t *tbs_val = cert_val + hl;
    size_t tbs_len = vl;
    size_t pos = 0;

    while (pos < tbs_len) {
        if (der_read_tlv(tbs_val + pos, tbs_len - pos, &t, &vl, &hl) != 0)
            break;

        // Extensions is [3] EXPLICIT = 0xA3
        if (t == 0xA3) {
            // Inside [3]: one SEQUENCE of Extensions
            const uint8_t *exts_wrap = tbs_val + pos + hl;
            uint8_t t2;
            size_t vl2, hl2;
            if (der_read_tlv(exts_wrap, vl, &t2, &vl2, &hl2) != 0 || t2 != 0x30)
                break;

            const uint8_t *exts = exts_wrap + hl2;
            size_t exts_len = vl2;
            size_t ep = 0;

            while (ep < exts_len && count < max_names) {
                // Each Extension is a SEQUENCE { OID, [BOOL], OCTET STRING }
                uint8_t t3;
                size_t vl3, hl3;
                if (der_read_tlv(exts + ep, exts_len - ep, &t3, &vl3, &hl3) != 0)
                    break;

                if (t3 == 0x30) {
                    const uint8_t *ext = exts + ep + hl3;
                    uint8_t t4;
                    size_t vl4, hl4;

                    // OID
                    if (der_read_tlv(ext, vl3, &t4, &vl4, &hl4) == 0 && t4 == 0x06) {
                        if (oid_matches(ext + hl4,
                                        vl4,
                                        OID_SUBJECT_ALT_NAME,
                                        sizeof(OID_SUBJECT_ALT_NAME))) {
                            // Skip optional BOOLEAN (critical flag)
                            size_t after_oid = hl4 + vl4;
                            if (after_oid < vl3) {
                                uint8_t nt;
                                size_t nvl, nhl;
                                if (der_read_tlv(
                                        ext + after_oid, vl3 - after_oid, &nt, &nvl, &nhl) == 0) {
                                    if (nt == 0x01) // BOOLEAN (critical flag) — skip
                                        after_oid += nhl + nvl;
                                    // Now should be at OCTET STRING
                                    if (after_oid < vl3)
                                        extract_san_from_ext_value(ext + after_oid,
                                                                   vl3 - after_oid,
                                                                   san_out,
                                                                   max_names,
                                                                   &count);
                                }
                            }
                        }
                    }
                }

                ep += hl3 + vl3;
            }
            break; // Extensions found and processed
        }

        pos += hl + vl;
    }

    return count;
}

/// @brief Check whether a DER-encoded certificate carries a Subject Alternative Name extension.
/// @details Walks the TBSCertificate extensions sequence looking for the
///          subjectAltName OID (2.5.29.17). Used by hostname-matching code
///          to enforce RFC 6125's "if SAN is present, ignore CN" rule —
///          presence of SAN means CN-based matching MUST NOT be used.
/// @param der Complete certificate DER bytes.
/// @param der_len Number of certificate bytes.
/// @return 1 if a SAN extension is present, 0 if not (or on parse failure).
int tls_cert_has_san_extension(const uint8_t *der, size_t der_len) {
    uint8_t t;
    size_t vl, hl;
    if (der_read_tlv(der, der_len, &t, &vl, &hl) != 0 || t != 0x30)
        return 0;

    const uint8_t *cert_val = der + hl;
    if (der_read_tlv(cert_val, vl, &t, &vl, &hl) != 0 || t != 0x30)
        return 0;

    const uint8_t *tbs_val = cert_val + hl;
    size_t tbs_len = vl;
    size_t pos = 0;

    while (pos < tbs_len) {
        if (der_read_tlv(tbs_val + pos, tbs_len - pos, &t, &vl, &hl) != 0)
            return 0;
        if (t == 0xA3) {
            const uint8_t *exts_wrap = tbs_val + pos + hl;
            uint8_t t2;
            size_t vl2, hl2;
            if (der_read_tlv(exts_wrap, vl, &t2, &vl2, &hl2) != 0 || t2 != 0x30)
                return 0;

            const uint8_t *exts = exts_wrap + hl2;
            size_t exts_len = vl2;
            size_t ep = 0;
            while (ep < exts_len) {
                uint8_t t3;
                size_t vl3, hl3;
                if (der_read_tlv(exts + ep, exts_len - ep, &t3, &vl3, &hl3) != 0)
                    return 0;
                if (t3 == 0x30) {
                    const uint8_t *ext = exts + ep + hl3;
                    uint8_t t4;
                    size_t vl4, hl4;
                    if (der_read_tlv(ext, vl3, &t4, &vl4, &hl4) == 0 && t4 == 0x06 &&
                        oid_matches(
                            ext + hl4, vl4, OID_SUBJECT_ALT_NAME, sizeof(OID_SUBJECT_ALT_NAME))) {
                        return 1;
                    }
                }
                ep += hl3 + vl3;
            }
            return 0;
        }
        pos += hl + vl;
    }

    return 0;
}

/// @brief Extract a valid DNS CommonName from one DER Name.
/// @param name_der Complete Subject Name TLV.
/// @param name_len Number of Name bytes.
/// @param cn_out Destination 256-byte NUL-terminated buffer.
/// @return One when a valid CN is copied; otherwise zero.
static int tls_extract_cn_from_name(const uint8_t *name_der, size_t name_len, char cn_out[256]) {
    uint8_t t;
    size_t vl, hl;
    if (!name_der || !cn_out || der_read_tlv(name_der, name_len, &t, &vl, &hl) != 0 || t != 0x30) {
        return 0;
    }

    const uint8_t *seq_val = name_der + hl;
    size_t seq_len = vl;
    size_t sp = 0;

    while (sp < seq_len) {
        uint8_t ts;
        size_t vls, hls;
        if (der_read_tlv(seq_val + sp, seq_len - sp, &ts, &vls, &hls) != 0)
            return 0;

        if (ts == 0x31) {
            const uint8_t *set_val = seq_val + sp + hls;
            size_t set_pos = 0;
            while (set_pos < vls) {
                uint8_t ta;
                size_t vla, hla;
                if (der_read_tlv(set_val + set_pos, vls - set_pos, &ta, &vla, &hla) != 0)
                    return 0;
                if (ta == 0x30) {
                    const uint8_t *atv = set_val + set_pos + hla;
                    uint8_t to;
                    size_t vlo, hlo;
                    if (der_read_tlv(atv, vla, &to, &vlo, &hlo) == 0 && to == 0x06 &&
                        oid_matches(atv + hlo, vlo, OID_COMMON_NAME, sizeof(OID_COMMON_NAME))) {
                        size_t after_oid = hlo + vlo;
                        if (after_oid >= vla)
                            return 0;
                        const uint8_t *val_start = atv + after_oid;
                        size_t val_remaining = vla - after_oid;
                        uint8_t tv;
                        size_t vlv, hlv;
                        if (der_read_tlv(val_start, val_remaining, &tv, &vlv, &hlv) == 0 &&
                            tls_dns_name_bytes_valid(val_start + hlv, vlv, 1)) {
                            memcpy(cn_out, val_start + hlv, vlv);
                            cn_out[vlv] = '\0';
                            return 1;
                        }
                    }
                }
                set_pos += hla + vla;
            }
        }

        sp += hls + vls;
    }

    return 0;
}

/// @brief Extract the CommonName (CN) attribute from the certificate Subject.
/// @details Walks the ASN.1 tree: Certificate → TBSCertificate → looking for
///          a SEQUENCE whose children include a SET containing an
///          AttributeTypeAndValue with OID 2.5.4.3 (commonName). The first
///          matching CN value is copied null-terminated into `cn_out`,
///          truncated at 255 bytes.
///
///          **CN-based hostname matching is a legacy fallback** per RFC 6125
///          § 6.4.4 and is deprecated in favour of SubjectAltName dNSName
///          entries. Callers should only consult the CN when no SAN extension
///          is present — `tls_verify_hostname` enforces that ordering. CAs
///          that comply with the CA/Browser Forum Baseline Requirements no
///          longer issue certificates that rely on CN for hostname identity,
///          but this function is retained for compatibility with older
///          internal CAs and self-signed certificates.
/// @param der Pointer to the certificate's DER bytes.
/// @param der_len Length of the DER buffer.
/// @param cn_out Output 256-byte buffer for the CN string.
/// @return 1 if a CN was found and written, 0 if no CN is present or it
///         would not fit in the buffer.
int tls_extract_cn(const uint8_t *der, size_t der_len, char cn_out[256]) {
    size_t subject_len = 0;
    const uint8_t *subject = cert_get_subject(der, der_len, &subject_len);
    if (!subject)
        return 0;
    return tls_extract_cn_from_name(subject, subject_len, cn_out);
}

/// @brief Match a hostname against a certificate name pattern (RFC 6125 § 6.4).
/// @details Two cases are supported:
///          - **Exact match**: `"example.com"` vs `"example.com"` (case-insensitive
///            via `strcasecmp`).
///          - **Single-label wildcard**: `"*.example.com"` vs `"foo.example.com"`.
///            The `*` must be the entire leftmost label (so `"foo*.example.com"`
///            and `"*foo.example.com"` are intentionally rejected — partial-label
///            wildcards are a CA/Browser Forum prohibition because they enable
///            attacks like a cert for `"*.com"`). The wildcard label matches
///            exactly one DNS label and never matches `.` itself, so
///            `"*.example.com"` does not match `"a.b.example.com"`.
///
///          Returns 0 on any structural mismatch, including null inputs and
///          the case where the hostname has no `.` (a hostname with no dots
///          can't match a `*.X` pattern by construction).
/// @param pattern Pattern from a certificate dNSName or CN.
/// @param hostname User-supplied hostname being verified.
/// @return 1 on match, 0 otherwise.
int tls_match_hostname(const char *pattern, const char *hostname) {
    if (!pattern || !hostname)
        return 0;
    if (!tls_dns_name_valid(pattern, 1) || !tls_dns_name_valid(hostname, 0))
        return 0;

    if (pattern[0] == '*' && pattern[1] == '.') {
        // Wildcard: *.example.com
        const char *suffix = pattern + 2; // "example.com"
        if (!tls_wildcard_suffix_allowed(suffix))
            return 0;
        const char *dot = strchr(hostname, '.');
        if (!dot)
            return 0; // hostname has no dot — can't match *.X

        // hostname suffix after first dot must match pattern suffix
        const char *host_suffix = dot + 1;
        if (strcasecmp(host_suffix, suffix) != 0)
            return 0;

        // The wildcard label must not contain a dot (one label only)
        // Already guaranteed since we took the first dot.
        return 1;
    }

    return strcasecmp(pattern, hostname) == 0 ? 1 : 0;
}

/// @brief Walk one SAN extension and check every dNSName against @p hostname.
/// @details This is separate from tls_extract_san_names because verification
///          must not be capped by the fixed-size public extraction buffer.
/// @param ext_val DER-wrapped SAN extension value.
/// @param ext_len Number of extension bytes.
/// @param hostname Valid DNS hostname to match.
/// @param saw_dns_san Optional destination indicating any valid DNS SAN.
/// @return One for a matching dNSName; otherwise zero.
static int san_ext_has_dns_match(const uint8_t *ext_val,
                                 size_t ext_len,
                                 const char *hostname,
                                 int *saw_dns_san) {
    uint8_t t;
    size_t vl, hl;
    if (saw_dns_san)
        *saw_dns_san = 0;

    if (der_read_tlv(ext_val, ext_len, &t, &vl, &hl) != 0 || t != 0x04)
        return 0;

    const uint8_t *inner = ext_val + hl;
    size_t inner_len = vl;
    if (der_read_tlv(inner, inner_len, &t, &vl, &hl) != 0 || t != 0x30)
        return 0;

    const uint8_t *names = inner + hl;
    size_t names_len = vl;
    size_t pos = 0;
    while (pos < names_len) {
        if (der_read_tlv(names + pos, names_len - pos, &t, &vl, &hl) != 0)
            break;
        if (t == 0x82 && tls_dns_name_bytes_valid(names + pos + hl, vl, 1)) {
            char name[256];
            if (saw_dns_san)
                *saw_dns_san = 1;
            memcpy(name, names + pos + hl, vl);
            name[vl] = '\0';
            if (tls_match_hostname(name, hostname))
                return 1;
        }
        pos += hl + vl;
    }
    return 0;
}

/// @brief Scan all DNS SubjectAltNames for a hostname match.
/// @param der Complete certificate DER bytes.
/// @param der_len Number of certificate bytes.
/// @param hostname Valid DNS hostname to match.
/// @param saw_dns_san Optional destination indicating any valid DNS SAN.
/// @return One for a matching dNSName; otherwise zero.
static int tls_cert_has_matching_dns_san(const uint8_t *der,
                                         size_t der_len,
                                         const char *hostname,
                                         int *saw_dns_san) {
    int saw_any = 0;
    uint8_t t;
    size_t vl, hl;
    if (saw_dns_san)
        *saw_dns_san = 0;

    if (der_read_tlv(der, der_len, &t, &vl, &hl) != 0 || t != 0x30)
        return 0;

    const uint8_t *cert_val = der + hl;
    if (der_read_tlv(cert_val, vl, &t, &vl, &hl) != 0 || t != 0x30)
        return 0;

    const uint8_t *tbs_val = cert_val + hl;
    size_t tbs_len = vl;
    size_t pos = 0;

    while (pos < tbs_len) {
        if (der_read_tlv(tbs_val + pos, tbs_len - pos, &t, &vl, &hl) != 0)
            break;

        if (t == 0xA3) {
            const uint8_t *exts_wrap = tbs_val + pos + hl;
            uint8_t t2;
            size_t vl2, hl2;
            if (der_read_tlv(exts_wrap, vl, &t2, &vl2, &hl2) != 0 || t2 != 0x30)
                break;

            const uint8_t *exts = exts_wrap + hl2;
            size_t exts_len = vl2;
            size_t ep = 0;
            while (ep < exts_len) {
                uint8_t t3;
                size_t vl3, hl3;
                if (der_read_tlv(exts + ep, exts_len - ep, &t3, &vl3, &hl3) != 0)
                    break;

                if (t3 == 0x30) {
                    const uint8_t *ext = exts + ep + hl3;
                    uint8_t t4;
                    size_t vl4, hl4;
                    if (der_read_tlv(ext, vl3, &t4, &vl4, &hl4) == 0 && t4 == 0x06 &&
                        oid_matches(
                            ext + hl4, vl4, OID_SUBJECT_ALT_NAME, sizeof(OID_SUBJECT_ALT_NAME))) {
                        size_t after_oid = hl4 + vl4;
                        if (after_oid < vl3) {
                            uint8_t nt;
                            size_t nvl, nhl;
                            if (der_read_tlv(ext + after_oid, vl3 - after_oid, &nt, &nvl, &nhl) ==
                                0) {
                                if (nt == 0x01)
                                    after_oid += nhl + nvl;
                                if (after_oid < vl3) {
                                    int saw_this_ext = 0;
                                    if (san_ext_has_dns_match(ext + after_oid,
                                                              vl3 - after_oid,
                                                              hostname,
                                                              &saw_this_ext))
                                        return 1;
                                    if (saw_this_ext)
                                        saw_any = 1;
                                }
                            }
                        }
                    }
                }

                ep += hl3 + vl3;
            }
        }

        pos += hl + vl;
    }

    if (saw_dns_san)
        *saw_dns_san = saw_any;
    return 0;
}

// Maximum number of SAN DNS names exposed through tls_extract_san_names.
#define TLS_MAX_SAN_NAMES 64

/// @brief Verify that the session's intended hostname matches the leaf
///        certificate (RFC 6125 § 6).
/// @details The verification ordering is load-bearing for security:
///          1. **IP literals** (e.g., `"192.0.2.1"`, `"::1"`) match exclusively
///             against the certificate's iPAddress SANs. Falling back to dNSName
///             SAN or CN matching for an IP literal would be a bypass —
///             `"192.0.2.1"` should never match a cert issued for the literal
///             string `"192.0.2.1"` as a DNS name.
///          2. **DNS hostnames** scan every dNSName SAN entry before falling
///             back to CommonName.
///          3. **CN fallback** is consulted *only* when no SAN extension is
///             present at all (per RFC 6125 § 6.4.4 — CN must be ignored when
///             SAN is present, even if SAN doesn't list any dNSName entries).
///          On mismatch the function sets `session->error` to a diagnostic
///          message and returns RT_TLS_ERROR_HANDSHAKE so the caller can
///          surface the underlying reason via `rt_tls_last_error()`.
/// @param session Active TLS session with `server_cert_der` populated.
/// @return 0 on match, RT_TLS_ERROR_HANDSHAKE on mismatch or missing data.
int tls_verify_hostname(rt_tls_session_t *session) {
    const uint8_t *server_cert_der = tls_session_server_cert_der(session);
    if (!server_cert_der) {
        session->error = "TLS: no certificate stored for hostname verification";
        return RT_TLS_ERROR_HANDSHAKE;
    }

    const char *host = session->hostname;
    uint8_t ip_literal[16];
    size_t ip_literal_len = 0;

    if (!host || !host[0]) {
        session->error = "TLS: hostname verification requires a hostname";
        return RT_TLS_ERROR_HANDSHAKE;
    }

    if (tls_hostname_is_ip_literal(host, ip_literal, &ip_literal_len)) {
        int saw_ip_san = 0;
        if (tls_cert_has_matching_ip_san(server_cert_der,
                                         session->server_cert_der_len,
                                         ip_literal,
                                         ip_literal_len,
                                         &saw_ip_san)) {
            return RT_TLS_OK;
        }
        session->error = saw_ip_san ? "TLS: certificate IP SAN mismatch"
                                    : "TLS: certificate does not contain an IP SubjectAltName";
        return RT_TLS_ERROR_HANDSHAKE;
    }

    if (!tls_dns_name_valid(host, 0)) {
        session->error = "TLS: invalid hostname for certificate verification";
        return RT_TLS_ERROR_HANDSHAKE;
    }

    // Try SubjectAltName first (RFC 6125 §6.4: SAN takes precedence over CN).
    int saw_dns_san = 0;
    if (tls_cert_has_matching_dns_san(
            server_cert_der, session->server_cert_der_len, host, &saw_dns_san))
        return RT_TLS_OK;
    if (saw_dns_san) {
        session->error = "TLS: certificate hostname mismatch (SAN did not match)";
        return RT_TLS_ERROR_HANDSHAKE;
    }
    if (tls_cert_has_san_extension(server_cert_der, session->server_cert_der_len)) {
        session->error = "TLS: certificate SAN extension does not contain any DNS names";
        return RT_TLS_ERROR_HANDSHAKE;
    }

    // Fall back to CommonName if no SAN
    char cn[256] = {0};
    if (tls_extract_cn(server_cert_der, session->server_cert_der_len, cn)) {
        if (tls_match_hostname(cn, host))
            return RT_TLS_OK;
        session->error = "TLS: certificate hostname mismatch (CN did not match)";
        return RT_TLS_ERROR_HANDSHAKE;
    }

    session->error = "TLS: certificate contains no SAN or CN for hostname verification";
    return RT_TLS_ERROR_HANDSHAKE;
}

// ---------------------------------------------------------------------------
// B.4 — OS trust store chain validation
// ---------------------------------------------------------------------------


// ---------------------------------------------------------------------------
// B.5 — CertificateVerify signature verification
// ---------------------------------------------------------------------------

/// @brief Build the 130-byte CertificateVerify message (RFC 8446 §4.4.3).
/// Content = 64 spaces + "TLS 1.3, server CertificateVerify" + 0x00 + transcript_hash
/// @param transcript_hash 32-byte SHA-256 transcript hash.
/// @param out_content Output buffer (must be at least 130 bytes).
void build_cert_verify_message(const uint8_t transcript_hash[32], uint8_t out_content[130]) {
    static const char context_str[] = "TLS 1.3, server CertificateVerify";
    memset(out_content, 0x20, 64);
    memcpy(out_content + 64, context_str, 33);
    out_content[97] = 0x00;
    memcpy(out_content + 98, transcript_hash, 32);
}

size_t sig_scheme_hash_len(uint16_t sig_scheme);

/// @brief Determine the hash output size for a signature scheme.
/// @param sig_scheme TLS signature scheme code.
/// @return 32 for SHA-256, 48 for SHA-384, 64 for SHA-512, 0 if unknown.
size_t sig_scheme_hash_len(uint16_t sig_scheme) {
    switch (sig_scheme) {
        case 0x0403: /* ecdsa_secp256r1_sha256 */
        case 0x0804: /* rsa_pss_rsae_sha256 */
            return 32;
        case 0x0805: /* rsa_pss_rsae_sha384 */
            return 48;
        case 0x0806: /* rsa_pss_rsae_sha512 */
            return 64;
        default:
            return 0;
    }
}
