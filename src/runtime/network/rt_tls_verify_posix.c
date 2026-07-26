//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/network/rt_tls_verify_posix.c
// Purpose: In-tree PEM-bundle TLS certificate-chain and CertificateVerify validation for macOS
//   and Linux. Compiled on non-_WIN32 platforms; the Windows CryptoAPI path lives in
//   rt_tls_verify_win.c.
//
// Links: rt_tls_verify_internal.h (shared helpers), rt_tls_verify_win.c, rt_tls_internal.h
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_tls_verify_posix.c
 * @brief Implements native PEM-bundle certificate verification on POSIX.
 * @details The verifier discovers or loads a bounded trust bundle, parses
 *          candidate roots and intermediates, builds and validates certificate
 *          chains with in-tree cryptography, enforces X.509 constraints and
 *          hostname policy, and verifies TLS 1.3 CertificateVerify signatures.
 */

#include "rt_tls_verify_internal.h"

#if !defined(_WIN32)

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/// @brief Maximum PEM trust bundle size accepted by the POSIX verifier.
/// @details System trust bundles are expected to be bounded text files. The
///          cap prevents a configured CA path from driving unbounded allocation
///          before any certificate parsing has happened.
#define TLS_TRUST_BUNDLE_MAX_BYTES (8u * 1024u * 1024u)

/// @brief Whether process credentials make environment trust overrides unsafe.
/// @details If the runtime is executing with elevated effective credentials,
///          trust roots must not be redirected by untrusted inherited
///          environment variables.
/// @return Nonzero when ZANNA_TLS_CA_FILE should be ignored.
static RT_TLS_MAYBE_UNUSED int tls_posix_ignore_env_trust_override(void) {
    return geteuid() != getuid() || getegid() != getgid();
}

/// @brief Find a PEM CA bundle file by probing standard OS paths.
/// @details The ZANNA_TLS_CA_FILE environment variable can point at an
///          application-managed CA bundle. When unset or unusable, standard
///          distro bundle paths are probed in order.
/// @return Path string literal on success, NULL if none found.
static RT_TLS_MAYBE_UNUSED const char *find_ca_bundle(void) {
    static const char *bundles[] = {"/etc/ssl/certs/ca-certificates.crt",
                                    "/etc/pki/tls/certs/ca-bundle.crt",
                                    "/etc/ssl/ca-bundle.pem",
                                    "/etc/ssl/cert.pem",
                                    NULL};
    const char *override =
        tls_posix_ignore_env_trust_override() ? NULL : getenv("ZANNA_TLS_CA_FILE");
    if (override && override[0]) {
        FILE *f = fopen(override, "rb");
        if (f) {
            fclose(f);
            return override;
        }
    }
    for (int i = 0; bundles[i]; i++) {
        FILE *f = fopen(bundles[i], "rb");
        if (f) {
            fclose(f);
            return bundles[i];
        }
    }
    return NULL;
}

/// @brief Decode a Base64 PEM body (between header/footer markers) into DER bytes.
/// @details ASCII whitespace is ignored and decoding stops at the first
///          padding character. Any other non-Base64 character rejects the
///          input.
/// @param[in] pem_b64 Base64 text to decode; it need not be null-terminated.
/// @param[in] b64_len Number of bytes available at @p pem_b64.
/// @param[out] out_der Caller-provided buffer that receives decoded DER bytes.
/// @param[in] max_der Capacity of @p out_der in bytes.
/// @return Number of DER bytes written; 0 on output-buffer overflow or invalid input.
static RT_TLS_MAYBE_UNUSED size_t pem_decode_cert(const char *pem_b64,
                                                  size_t b64_len,
                                                  uint8_t *out_der,
                                                  size_t max_der) {
    static const int8_t b64tab[256] = {
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 62,
        -1, -1, -1, 63, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, -1, -1, -1, -1, -1, -1, -1, 0,
        1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22,
        23, 24, 25, -1, -1, -1, -1, -1, -1, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38,
        39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
    size_t out_len = 0;
    uint32_t acc = 0;
    int bits = 0;

    for (size_t i = 0; i < b64_len; i++) {
        unsigned char c = (unsigned char)pem_b64[i];
        if (c == '\r' || c == '\n' || c == ' ' || c == '\t')
            continue;
        if (c == '=')
            break;
        if (b64tab[c] < 0)
            return 0;
        acc = (acc << 6) | (uint32_t)b64tab[c];
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (out_len >= max_der)
                return 0;
            out_der[out_len++] = (uint8_t)((acc >> bits) & 0xFF);
        }
    }
    return out_len;
}

/// @brief Compare two DER-encoded X.509 Name structures for byte-exact equality.
/// @param[in] a_der First complete DER Name TLV.
/// @param[in] a_len Length of @p a_der in bytes.
/// @param[in] b_der Second complete DER Name TLV.
/// @param[in] b_len Length of @p b_der in bytes.
/// @return 1 if equal, 0 otherwise.
static RT_TLS_MAYBE_UNUSED int der_names_equal(const uint8_t *a_der,
                                               size_t a_len,
                                               const uint8_t *b_der,
                                               size_t b_len) {
    return a_len == b_len && memcmp(a_der, b_der, a_len) == 0;
}

/// @brief Return a pointer into cert_der at the DER-encoded Issuer field, and write its total
///        TLV length (header + value) into *issuer_len.
/// @param[in] cert_der Complete DER-encoded X.509 certificate.
/// @param[in] cert_len Length of @p cert_der in bytes.
/// @param[out] issuer_len Receives the complete Issuer Name TLV length.
/// @return Non-null pointer into cert_der on success, NULL if the certificate is malformed.
static RT_TLS_MAYBE_UNUSED const uint8_t *cert_get_issuer(const uint8_t *cert_der,
                                                          size_t cert_len,
                                                          size_t *issuer_len) {
    uint8_t t;
    size_t vl, hl, cert_hl, tbs_hl;
    const uint8_t *tbs = NULL;
    size_t tbs_len = 0;
    size_t pos = 0;

    if (!issuer_len || der_read_tlv(cert_der, cert_len, &t, &vl, &cert_hl) != 0 || t != 0x30)
        return NULL;
    if (der_read_tlv(cert_der + cert_hl, vl, &t, &tbs_len, &tbs_hl) != 0 || t != 0x30)
        return NULL;
    tbs = cert_der + cert_hl + tbs_hl;

    if (der_read_tlv(tbs + pos, tbs_len - pos, &t, &vl, &hl) != 0)
        return NULL;
    if (t == 0xA0)
        pos += hl + vl;

    for (int i = 0; i < 2; i++) {
        if (der_read_tlv(tbs + pos, tbs_len - pos, &t, &vl, &hl) != 0)
            return NULL;
        pos += hl + vl;
    }
    if (der_read_tlv(tbs + pos, tbs_len - pos, &t, &vl, &hl) != 0 || t != 0x30)
        return NULL;
    *issuer_len = hl + vl;
    return tbs + pos;
}

/// @brief Parse a 2-digit decimal ASCII number from a DER timestamp into @p *out.
/// @details DER UTCTime / GeneralizedTime fields are sequences of ASCII
///          decimal digits ("YYMMDD..." or "YYYYMMDD..."). This helper
///          parses one 2-digit field with strict validation — any non-digit
///          byte rejects the timestamp.
/// @param[in] data Pointer to at least two ASCII bytes.
/// @param[out] out Receives the parsed value from 0 through 99.
/// @return 1 on success, 0 on any non-digit byte.
static int der_decimal_2(const uint8_t *data, int *out) {
    if (data[0] < '0' || data[0] > '9' || data[1] < '0' || data[1] > '9')
        return 0;
    *out = (data[0] - '0') * 10 + (data[1] - '0');
    return 1;
}

/// @brief Parse a 4-digit decimal ASCII number from a DER GeneralizedTime year field.
/// @details Composes two der_decimal_2 calls into the high/low pair of a
///          four-digit year. UTCTime uses 2-digit years (parsed via
///          der_decimal_2 with century inference); GeneralizedTime uses
///          this 4-digit form directly.
/// @param[in] data Pointer to at least four ASCII bytes.
/// @param[out] out Receives the parsed value from 0 through 9999.
/// @return 1 on success, 0 on any non-digit byte.
static int der_decimal_4(const uint8_t *data, int *out) {
    int hi, lo;
    if (!der_decimal_2(data, &hi) || !der_decimal_2(data + 2, &lo))
        return 0;
    *out = hi * 100 + lo;
    return 1;
}

/// @brief Test whether @p year is a Gregorian leap year (RFC 5280 § 4.1.2.5).
/// @details Standard rule: divisible by 4, except century years not
///          divisible by 400. Used by der_time_days_in_month to validate
///          February day counts.
/// @param[in] year Four-digit Gregorian calendar year.
/// @return Nonzero when @p year is a leap year; otherwise zero.
static int der_time_is_leap_year(int year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

/// @brief Return the number of days in a given Gregorian month, accounting for leap years.
/// @details Used to validate certificate notBefore / notAfter timestamps —
///          a date like "2026-02-30" must be rejected. Returns 0 for
///          out-of-range month values; 29 for February in leap years; the
///          standard table value otherwise.
/// @param[in] year Gregorian calendar year used for February.
/// @param[in] month One-based month number.
/// @return Number of days in @p month, or 0 when @p month is outside 1 through 12.
static int der_time_days_in_month(int year, int month) {
    static const int days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12)
        return 0;
    if (month == 2 && der_time_is_leap_year(year))
        return 29;
    return days[month - 1];
}

/// @brief Parse a UTC DER UTCTime or GeneralizedTime value into a time_t.
/// @details Only the canonical seconds-and-`Z` forms are accepted: 13-byte
///          UTCTime and 15-byte GeneralizedTime. Calendar fields are checked
///          before conversion to UTC.
/// @param[in] data DER time value bytes, excluding the tag and length.
/// @param[in] len Number of value bytes at @p data.
/// @param[in] tag DER tag 0x17 for UTCTime or 0x18 for GeneralizedTime.
/// @return Parsed time, or (time_t)-1 on format error.
static time_t parse_der_time(const uint8_t *data, size_t len, uint8_t tag) {
    struct tm tm_val;
    int pos;
    int year, month, day, hour, minute, second;

    memset(&tm_val, 0, sizeof(tm_val));
    if (!data)
        return (time_t)-1;
    if (tag == 0x17) {
        int yy;
        if (len != 13 || data[12] != 'Z' || !der_decimal_2(data, &yy))
            return (time_t)-1;
        year = (yy >= 50) ? 1900 + yy : 2000 + yy;
        pos = 2;
    } else if (tag == 0x18) {
        if (len != 15 || data[14] != 'Z' || !der_decimal_4(data, &year))
            return (time_t)-1;
        pos = 4;
    } else {
        return (time_t)-1;
    }

    if (!der_decimal_2(data + pos, &month) || !der_decimal_2(data + pos + 2, &day) ||
        !der_decimal_2(data + pos + 4, &hour) || !der_decimal_2(data + pos + 6, &minute) ||
        !der_decimal_2(data + pos + 8, &second)) {
        return (time_t)-1;
    }
    if (month < 1 || month > 12 || day < 1 || day > der_time_days_in_month(year, month) ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) {
        return (time_t)-1;
    }

    tm_val.tm_year = year - 1900;
    tm_val.tm_mon = month - 1;
    tm_val.tm_mday = day;
    tm_val.tm_hour = hour;
    tm_val.tm_min = minute;
    tm_val.tm_sec = second;
    tm_val.tm_isdst = 0;
    return rt_network_timegm_utc(&tm_val);
}

/// @brief Check that the certificate's Validity window contains the current wall-clock time.
/// @param[in] cert_der Complete DER-encoded X.509 certificate.
/// @param[in] cert_len Length of @p cert_der in bytes.
/// @return 0 if the certificate is currently valid, -1 if expired, not-yet-valid, or malformed.
static RT_TLS_MAYBE_UNUSED int cert_check_expiry(const uint8_t *cert_der, size_t cert_len) {
    uint8_t t;
    size_t vl, hl, cert_hl, tbs_hl;
    const uint8_t *tbs = NULL;
    size_t tbs_len = 0;
    size_t pos = 0;

    if (der_read_tlv(cert_der, cert_len, &t, &vl, &cert_hl) != 0 || t != 0x30) {
        return -1;
    }
    if (der_read_tlv(cert_der + cert_hl, vl, &t, &tbs_len, &tbs_hl) != 0 || t != 0x30) {
        return -1;
    }
    tbs = cert_der + cert_hl + tbs_hl;

    if (der_read_tlv(tbs + pos, tbs_len - pos, &t, &vl, &hl) != 0) {
        return -1;
    }
    if (t == 0xA0)
        pos += hl + vl;

    for (int i = 0; i < 3; i++) {
        if (der_read_tlv(tbs + pos, tbs_len - pos, &t, &vl, &hl) != 0) {
            return -1;
        }
        pos += hl + vl;
    }

    if (der_read_tlv(tbs + pos, tbs_len - pos, &t, &vl, &hl) != 0 || t != 0x30) {
        return -1;
    }
    {
        const uint8_t *validity = tbs + pos + hl;
        size_t validity_len = vl;
        size_t vpos = 0;
        time_t not_before;
        time_t not_after;
        time_t now;
        if (der_read_tlv(validity + vpos, validity_len - vpos, &t, &vl, &hl) != 0) {
            return -1;
        }
        not_before = parse_der_time(validity + vpos + hl, vl, t);
        vpos += hl + vl;
        if (der_read_tlv(validity + vpos, validity_len - vpos, &t, &vl, &hl) != 0) {
            return -1;
        }
        not_after = parse_der_time(validity + vpos + hl, vl, t);
        if (not_before == (time_t)-1 || not_after == (time_t)-1) {
            return -1;
        }
        now = time(NULL);
        if (now == (time_t)-1 || now < not_before || now > not_after) {
            return -1;
        }
        return 0;
    }
}

typedef enum {
    TLS_CERT_SIG_NONE = 0,
    TLS_CERT_SIG_RSA_PKCS1 = 1,
    TLS_CERT_SIG_RSA_PSS = 2,
    TLS_CERT_SIG_ECDSA_P256 = 3,
} tls_cert_sig_kind_t;

typedef struct {
    tls_cert_sig_kind_t kind;
    rt_rsa_hash_t hash_id;
} tls_cert_sig_alg_t;

static RT_TLS_MAYBE_UNUSED size_t hash_len_from_id(rt_rsa_hash_t hash_id);

/// @brief Check whether a certificate is self-signed (Subject == Issuer byte-for-byte).
/// @param[in] cert_der Complete DER-encoded X.509 certificate.
/// @param[in] cert_len Length of @p cert_der in bytes.
/// @return 1 if self-signed, 0 otherwise.
static RT_TLS_MAYBE_UNUSED int cert_is_self_signed(const uint8_t *cert_der, size_t cert_len) {
    size_t subject_len = 0;
    size_t issuer_len = 0;
    const uint8_t *subject = cert_get_subject(cert_der, cert_len, &subject_len);
    const uint8_t *issuer = cert_get_issuer(cert_der, cert_len, &issuer_len);
    return subject && issuer && der_names_equal(subject, subject_len, issuer, issuer_len);
}

/// @brief Check whether a certificate permits TLS server authentication (native macOS/Linux).
/// @details When KeyUsage is present, digitalSignature must be asserted. When
///          ExtendedKeyUsage is present, it must include serverAuth or
///          anyExtendedKeyUsage. Malformed recognized extensions fail closed.
/// @param[in] cert_der Complete DER-encoded X.509 certificate.
/// @param[in] cert_len Length of @p cert_der in bytes.
/// @return 1 if both KeyUsage and ExtendedKeyUsage checks pass, 0 otherwise.
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

/// @brief Check whether a certificate is a CA (BasicConstraints cA=TRUE and KeyUsage keyCertSign).
/// @details BasicConstraints must be present and assert cA. If KeyUsage is
///          present, keyCertSign must also be asserted.
/// @param[in] cert_der Complete DER-encoded X.509 certificate.
/// @param[in] cert_len Length of @p cert_der in bytes.
/// @return 1 if the certificate is a CA with appropriate key usage, 0 otherwise.
static RT_TLS_MAYBE_UNUSED int cert_is_ca(const uint8_t *cert_der, size_t cert_len) {
    static const uint8_t OID_BASIC_CONSTRAINTS[] = {0x55, 0x1d, 0x13};
    static const uint8_t OID_KEY_USAGE[] = {0x55, 0x1d, 0x0f};
    uint8_t tag;
    size_t vl, hl;
    const uint8_t *p = cert_der;
    size_t rem = cert_len;
    const uint8_t *tbs = NULL;
    size_t tbs_rem = 0;
    int saw_basic_constraints = 0;
    int is_ca = 0;
    int key_usage_allows = 1;

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
                    int is_basic =
                        (vl == sizeof(OID_BASIC_CONSTRAINTS) &&
                         memcmp(ext + hl, OID_BASIC_CONSTRAINTS, sizeof(OID_BASIC_CONSTRAINTS)) ==
                             0);
                    int is_key_usage =
                        (vl == sizeof(OID_KEY_USAGE) &&
                         memcmp(ext + hl, OID_KEY_USAGE, sizeof(OID_KEY_USAGE)) == 0);
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

                    if (is_basic) {
                        const uint8_t *bc = extn_value;
                        size_t bc_len = extn_value_len;
                        saw_basic_constraints = 1;
                        if (der_read_tlv(bc, bc_len, &tag, &vl, &hl) == 0 && tag == 0x30) {
                            bc += hl;
                            bc_len = vl;
                            if (bc_len > 0 && der_read_tlv(bc, bc_len, &tag, &vl, &hl) == 0 &&
                                tag == 0x01 && vl == 1 && bc[hl] != 0) {
                                is_ca = 1;
                            }
                        }
                    } else if (is_key_usage) {
                        if (der_read_tlv(extn_value, extn_value_len, &tag, &vl, &hl) == 0 &&
                            tag == 0x03 && vl >= 2) {
                            const uint8_t *bits = extn_value + hl;
                            key_usage_allows = (bits[1] & 0x04) != 0;
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

    return saw_basic_constraints && is_ca && key_usage_allows;
}

/// @brief Extract the RSA public key from a certificate's SubjectPublicKeyInfo and parse it
///        into *out via rt_rsa_parse_public_key_pkcs1.
/// @details The SubjectPublicKeyInfo algorithm must be rsaEncryption with
///          absent or NULL parameters and a byte-aligned BIT STRING. The
///          function initializes @p out; the caller must release populated key
///          material with rt_rsa_key_free().
/// @param[in] cert_der Complete DER-encoded X.509 certificate.
/// @param[in] cert_len Length of @p cert_der in bytes.
/// @param[out] out RSA key object that receives parsed, owned key components.
/// @return 1 on success, 0 if the key is absent, malformed, or not RSA.
static RT_TLS_MAYBE_UNUSED int cert_get_rsa_pubkey(const uint8_t *cert_der,
                                                   size_t cert_len,
                                                   rt_rsa_key_t *out) {
    static const uint8_t OID_RSA_ENCRYPTION[] = {
        0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x01};
    uint8_t tag;
    size_t vl, hl;
    const uint8_t *p = cert_der;
    size_t rem = cert_len;
    const uint8_t *tbs = NULL;
    size_t tbs_rem = 0;
    const uint8_t *spki = NULL;
    size_t spki_rem = 0;
    const uint8_t *algo = NULL;
    size_t algo_rem = 0;
    const uint8_t *bits = NULL;

    if (!cert_der || !out)
        return 0;
    rt_rsa_key_init(out);
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

    for (int i = 0; i < 5; i++) {
        if (der_read_tlv(tbs, tbs_rem, &tag, &vl, &hl) != 0)
            return 0;
        tbs += hl + vl;
        tbs_rem -= hl + vl;
    }

    if (der_read_tlv(tbs, tbs_rem, &tag, &vl, &hl) != 0 || tag != 0x30)
        return 0;
    spki = tbs + hl;
    spki_rem = vl;
    if (der_read_tlv(spki, spki_rem, &tag, &vl, &hl) != 0 || tag != 0x30)
        return 0;
    algo = spki + hl;
    algo_rem = vl;
    spki += hl + vl;
    spki_rem -= hl + vl;
    if (der_read_tlv(algo, algo_rem, &tag, &vl, &hl) != 0 || tag != 0x06 ||
        memcmp(algo + hl, OID_RSA_ENCRYPTION, sizeof(OID_RSA_ENCRYPTION)) != 0 ||
        vl != sizeof(OID_RSA_ENCRYPTION)) {
        return 0;
    }
    algo += hl + vl;
    algo_rem -= hl + vl;
    if (!der_params_absent_or_null(algo, algo_rem))
        return 0;
    if (der_read_tlv(spki, spki_rem, &tag, &vl, &hl) != 0 || tag != 0x03 || vl < 2)
        return 0;
    if (hl + vl != spki_rem)
        return 0;
    bits = spki + hl;
    if (bits[0] != 0x00)
        return 0;
    return rt_rsa_parse_public_key_pkcs1(bits + 1, vl - 1, out);
}

/// @brief Extract an EC P-256 public key from a certificate's SubjectPublicKeyInfo.
/// @details Requires OID 1.2.840.10045.2.1 (id-ecPublicKey) with named curve
///          prime256v1 and an uncompressed point encoded as exactly 32-byte X
///          and Y coordinates.
/// @param[in] cert_der Complete DER-encoded X.509 certificate.
/// @param[in] cert_len Length of @p cert_der in bytes.
/// @param[out] x_out Receives the 32-byte big-endian affine X coordinate.
/// @param[out] y_out Receives the 32-byte big-endian affine Y coordinate.
/// @return 0 on success, -1 if absent, malformed, or not an EC P-256 key.
static RT_TLS_MAYBE_UNUSED int cert_get_ec_pubkey(const uint8_t *cert_der,
                                                  size_t cert_len,
                                                  uint8_t x_out[32],
                                                  uint8_t y_out[32]) {
    static const uint8_t OID_EC_PUBLIC_KEY[] = {0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x02, 0x01};
    static const uint8_t OID_PRIME256V1[] = {0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07};
    uint8_t tag;
    size_t vl, hl;
    const uint8_t *p = cert_der;
    size_t rem = cert_len;
    const uint8_t *tbs = NULL;
    size_t tbs_rem = 0;
    const uint8_t *spki = NULL;
    size_t spki_rem = 0;
    const uint8_t *algo = NULL;
    size_t algo_rem = 0;
    const uint8_t *bits = NULL;

    if (!cert_der || !x_out || !y_out)
        return -1;
    if (der_read_tlv(p, rem, &tag, &vl, &hl) != 0 || tag != 0x30)
        return -1;
    p += hl;
    rem = vl;
    if (der_read_tlv(p, rem, &tag, &vl, &hl) != 0 || tag != 0x30)
        return -1;
    tbs = p + hl;
    tbs_rem = vl;
    if (der_read_tlv(tbs, tbs_rem, &tag, &vl, &hl) != 0)
        return -1;
    if (tag == 0xA0) {
        tbs += hl + vl;
        tbs_rem -= hl + vl;
    }
    for (int i = 0; i < 5; i++) {
        if (der_read_tlv(tbs, tbs_rem, &tag, &vl, &hl) != 0)
            return -1;
        tbs += hl + vl;
        tbs_rem -= hl + vl;
    }
    if (der_read_tlv(tbs, tbs_rem, &tag, &vl, &hl) != 0 || tag != 0x30)
        return -1;
    spki = tbs + hl;
    spki_rem = vl;
    if (der_read_tlv(spki, spki_rem, &tag, &vl, &hl) != 0 || tag != 0x30)
        return -1;
    algo = spki + hl;
    algo_rem = vl;
    spki += hl + vl;
    spki_rem -= hl + vl;
    if (der_read_tlv(algo, algo_rem, &tag, &vl, &hl) != 0 || tag != 0x06 ||
        vl != sizeof(OID_EC_PUBLIC_KEY) ||
        memcmp(algo + hl, OID_EC_PUBLIC_KEY, sizeof(OID_EC_PUBLIC_KEY)) != 0) {
        return -1;
    }
    algo += hl + vl;
    algo_rem -= hl + vl;
    if (der_read_tlv(algo, algo_rem, &tag, &vl, &hl) != 0 || tag != 0x06 ||
        vl != sizeof(OID_PRIME256V1) ||
        memcmp(algo + hl, OID_PRIME256V1, sizeof(OID_PRIME256V1)) != 0) {
        return -1;
    }
    algo += hl + vl;
    algo_rem -= hl + vl;
    if (algo_rem != 0)
        return -1;
    if (der_read_tlv(spki, spki_rem, &tag, &vl, &hl) != 0 || tag != 0x03 || vl != 66)
        return -1;
    if (hl + vl != spki_rem)
        return -1;
    bits = spki + hl;
    if (bits[0] != 0x00 || bits[1] != 0x04)
        return -1;
    memcpy(x_out, bits + 2, 32);
    memcpy(y_out, bits + 34, 32);
    return 0;
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

/// @brief Parse a DER-encoded ECDSA signature (SEQUENCE { INTEGER r, INTEGER s }) and write
///        the r and s scalars as 32-byte big-endian values (zero-padded, leading 0x00 stripped).
/// @param[in] sig Complete DER-encoded ECDSA signature.
/// @param[in] sig_len Length of @p sig in bytes.
/// @param[out] r_out Receives the normalized 32-byte big-endian R scalar.
/// @param[out] s_out Receives the normalized 32-byte big-endian S scalar.
/// @return 0 on success, -1 on malformed input.
static RT_TLS_MAYBE_UNUSED int parse_ecdsa_sig_der(const uint8_t *sig,
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

/// @brief Decompose an X.509 certificate into its three top-level components:
///        TBSCertificate (for hashing), signatureAlgorithm OID, and signatureValue bit-string.
///        All output pointers point into cert_der; no allocation is performed.
/// @param[in] cert_der Complete DER-encoded X.509 certificate.
/// @param[in] cert_len Length of @p cert_der in bytes.
/// @param[out] tbs_der Receives the complete TBSCertificate TLV.
/// @param[out] tbs_len Receives the length of @p tbs_der in bytes.
/// @param[out] alg_der Receives the complete signature AlgorithmIdentifier TLV.
/// @param[out] alg_len Receives the length of @p alg_der in bytes.
/// @param[out] sig_bytes Receives the signature BIT STRING payload after its unused-bit byte.
/// @param[out] sig_len Receives the signature payload length in bytes.
/// @return 1 on success, 0 if the certificate structure is malformed.
static RT_TLS_MAYBE_UNUSED int cert_extract_signature_parts(const uint8_t *cert_der,
                                                            size_t cert_len,
                                                            const uint8_t **tbs_der,
                                                            size_t *tbs_len,
                                                            const uint8_t **alg_der,
                                                            size_t *alg_len,
                                                            const uint8_t **sig_bytes,
                                                            size_t *sig_len) {
    uint8_t tag;
    size_t vl, hl;
    const uint8_t *p = cert_der;
    size_t rem = cert_len;

    if (!cert_der || !tbs_der || !tbs_len || !alg_der || !alg_len || !sig_bytes || !sig_len)
        return 0;
    if (der_read_tlv(p, rem, &tag, &vl, &hl) != 0 || tag != 0x30)
        return 0;
    if (hl + vl != cert_len)
        return 0;
    p += hl;
    rem = vl;

    if (der_read_tlv(p, rem, &tag, &vl, &hl) != 0 || tag != 0x30)
        return 0;
    *tbs_der = p;
    *tbs_len = hl + vl;
    p += hl + vl;
    rem -= hl + vl;

    if (der_read_tlv(p, rem, &tag, &vl, &hl) != 0 || tag != 0x30)
        return 0;
    *alg_der = p;
    *alg_len = hl + vl;
    p += hl + vl;
    rem -= hl + vl;

    if (der_read_tlv(p, rem, &tag, &vl, &hl) != 0 || tag != 0x03 || vl < 1)
        return 0;
    if (p[hl] != 0x00)
        return 0;
    *sig_bytes = p + hl + 1;
    *sig_len = vl - 1;
    p += hl + vl;
    rem -= hl + vl;
    if (rem != 0)
        return 0;
    return 1;
}

/// @brief Map a DER OID value to one of SHA-256, SHA-384, or SHA-512.
/// @param[in] oid DER OBJECT IDENTIFIER value bytes, excluding tag and length.
/// @param[in] oid_len Number of bytes at @p oid.
/// @param[out] hash_id Receives the corresponding runtime RSA hash identifier.
/// @return 1 and sets *hash_id if the OID is recognised, 0 otherwise.
static RT_TLS_MAYBE_UNUSED int cert_parse_hash_oid(const uint8_t *oid,
                                                   size_t oid_len,
                                                   rt_rsa_hash_t *hash_id) {
    static const uint8_t OID_SHA256[] = {0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01};
    static const uint8_t OID_SHA384[] = {0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x02};
    static const uint8_t OID_SHA512[] = {0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x03};

    if (oid_len == sizeof(OID_SHA256) && memcmp(oid, OID_SHA256, sizeof(OID_SHA256)) == 0) {
        *hash_id = RT_RSA_HASH_SHA256;
        return 1;
    }
    if (oid_len == sizeof(OID_SHA384) && memcmp(oid, OID_SHA384, sizeof(OID_SHA384)) == 0) {
        *hash_id = RT_RSA_HASH_SHA384;
        return 1;
    }
    if (oid_len == sizeof(OID_SHA512) && memcmp(oid, OID_SHA512, sizeof(OID_SHA512)) == 0) {
        *hash_id = RT_RSA_HASH_SHA512;
        return 1;
    }
    return 0;
}

/// @brief Parse an RSASSA-PSS AlgorithmIdentifier parameter SEQUENCE (RFC 4055 §3.1).
/// @details Populates @p alg's hash identifier, requires the MGF-1 hash to
///          match it, requires a salt equal to the digest length, and accepts
///          only trailerField 1. Only explicit SHA-256, SHA-384, or SHA-512
///          hash, mask, and salt fields are accepted.
/// @param[in] params_der Complete DER RSASSA-PSS-params SEQUENCE.
/// @param[in] params_len Length of @p params_der in bytes.
/// @param[in,out] alg Signature classification whose hash identifier is populated.
/// @return 1 if the PSS parameters are valid and consistent, 0 otherwise.
static RT_TLS_MAYBE_UNUSED int cert_parse_pss_params(const uint8_t *params_der,
                                                     size_t params_len,
                                                     tls_cert_sig_alg_t *alg) {
    static const uint8_t OID_MGF1[] = {0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x08};
    uint8_t tag;
    size_t vl, hl;
    const uint8_t *p = params_der;
    size_t rem = params_len;
    rt_rsa_hash_t mgf_hash = RT_RSA_HASH_SHA256;
    size_t salt_len = 0;
    int have_hash = 0;
    int have_mgf = 0;
    int have_salt = 0;

    if (!alg || der_read_tlv(p, rem, &tag, &vl, &hl) != 0 || tag != 0x30)
        return 0;
    if (hl + vl != params_len)
        return 0;
    p += hl;
    rem = vl;

    while (rem > 0) {
        if (der_read_tlv(p, rem, &tag, &vl, &hl) != 0)
            return 0;
        uint8_t field_tag = tag;
        const uint8_t *field_value = p + hl;
        size_t field_len = vl;
        size_t field_total_len = hl + vl;
        if (field_tag == 0xA0) {
            const uint8_t *q = field_value;
            size_t q_rem = field_len;
            if (have_hash)
                return 0;
            if (der_read_tlv(q, q_rem, &tag, &vl, &hl) != 0 || tag != 0x30)
                return 0;
            if (hl + vl != q_rem)
                return 0;
            q += hl;
            q_rem = vl;
            if (der_read_tlv(q, q_rem, &tag, &vl, &hl) != 0 || tag != 0x06)
                return 0;
            if (!cert_parse_hash_oid(q + hl, vl, &alg->hash_id))
                return 0;
            q += hl + vl;
            q_rem -= hl + vl;
            if (!der_params_absent_or_null(q, q_rem))
                return 0;
            have_hash = 1;
        } else if (field_tag == 0xA1) {
            const uint8_t *q = field_value;
            size_t q_rem = field_len;
            if (have_mgf)
                return 0;
            if (der_read_tlv(q, q_rem, &tag, &vl, &hl) != 0 || tag != 0x30)
                return 0;
            if (hl + vl != q_rem)
                return 0;
            q += hl;
            q_rem = vl;
            if (der_read_tlv(q, q_rem, &tag, &vl, &hl) != 0 || tag != 0x06 ||
                vl != sizeof(OID_MGF1) || memcmp(q + hl, OID_MGF1, sizeof(OID_MGF1)) != 0) {
                return 0;
            }
            q += hl + vl;
            q_rem -= hl + vl;
            if (der_read_tlv(q, q_rem, &tag, &vl, &hl) != 0 || tag != 0x30)
                return 0;
            if (hl + vl != q_rem)
                return 0;
            q += hl;
            q_rem = vl;
            if (der_read_tlv(q, q_rem, &tag, &vl, &hl) != 0 || tag != 0x06)
                return 0;
            if (!cert_parse_hash_oid(q + hl, vl, &mgf_hash))
                return 0;
            q += hl + vl;
            q_rem -= hl + vl;
            if (!der_params_absent_or_null(q, q_rem))
                return 0;
            have_mgf = 1;
        } else if (field_tag == 0xA2) {
            const uint8_t *q = field_value;
            size_t q_rem = field_len;
            size_t parsed_salt_len = 0;
            if (have_salt)
                return 0;
            if (der_read_tlv(q, q_rem, &tag, &vl, &hl) != 0 || tag != 0x02 || vl == 0 || vl > 4)
                return 0;
            if (hl + vl != q_rem)
                return 0;
            if ((q[hl] & 0x80u) != 0 || (vl > 1 && q[hl] == 0x00 && (q[hl + 1] & 0x80u) == 0))
                return 0;
            for (size_t i = 0; i < vl; i++)
                parsed_salt_len = (parsed_salt_len << 8) | q[hl + i];
            salt_len = parsed_salt_len;
            have_salt = 1;
        } else if (field_tag == 0xA3) {
            const uint8_t *q = field_value;
            size_t q_rem = field_len;
            if (der_read_tlv(q, q_rem, &tag, &vl, &hl) != 0 || tag != 0x02 || vl != 1 || q[hl] != 1)
                return 0;
            if (hl + vl != q_rem)
                return 0;
        } else {
            return 0;
        }
        p += field_total_len;
        rem -= field_total_len;
    }

    return have_hash && have_mgf && have_salt && mgf_hash == alg->hash_id &&
           salt_len == hash_len_from_id(alg->hash_id);
}

/// @brief Parse a certificate signatureAlgorithm SEQUENCE and classify it as one of
///        RSA-PKCS1, RSA-PSS, or ECDSA-P256, filling in alg->kind and alg->hash_id.
///        RSA-PSS additionally validates the parameters via cert_parse_pss_params.
/// @param[in] alg_der Complete DER AlgorithmIdentifier SEQUENCE.
/// @param[in] alg_len Length of @p alg_der in bytes.
/// @param[out] alg Receives the recognized signature family and digest identifier.
/// @return 1 if recognised and parsed, 0 otherwise.
static RT_TLS_MAYBE_UNUSED int cert_parse_signature_algorithm(const uint8_t *alg_der,
                                                              size_t alg_len,
                                                              tls_cert_sig_alg_t *alg) {
    static const uint8_t OID_SHA256_RSA[] = {0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x0B};
    static const uint8_t OID_SHA384_RSA[] = {0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x0C};
    static const uint8_t OID_SHA512_RSA[] = {0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x0D};
    static const uint8_t OID_RSA_PSS[] = {0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x0A};
    static const uint8_t OID_ECDSA_SHA256[] = {0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x04, 0x03, 0x02};
    static const uint8_t OID_ECDSA_SHA384[] = {0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x04, 0x03, 0x03};
    static const uint8_t OID_ECDSA_SHA512[] = {0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x04, 0x03, 0x04};
    uint8_t tag;
    size_t vl, hl;
    const uint8_t *p = alg_der;
    size_t rem = alg_len;

    if (!alg)
        return 0;
    memset(alg, 0, sizeof(*alg));
    if (der_read_tlv(p, rem, &tag, &vl, &hl) != 0 || tag != 0x30)
        return 0;
    if (hl + vl != alg_len)
        return 0;
    p += hl;
    rem = vl;
    if (der_read_tlv(p, rem, &tag, &vl, &hl) != 0 || tag != 0x06)
        return 0;
    const uint8_t *oid = p + hl;
    size_t oid_len = vl;
    p += hl + vl;
    rem -= hl + vl;
    if (oid_len == sizeof(OID_SHA256_RSA) &&
        memcmp(oid, OID_SHA256_RSA, sizeof(OID_SHA256_RSA)) == 0) {
        if (!der_params_absent_or_null(p, rem))
            return 0;
        alg->kind = TLS_CERT_SIG_RSA_PKCS1;
        alg->hash_id = RT_RSA_HASH_SHA256;
        return 1;
    }
    if (oid_len == sizeof(OID_SHA384_RSA) &&
        memcmp(oid, OID_SHA384_RSA, sizeof(OID_SHA384_RSA)) == 0) {
        if (!der_params_absent_or_null(p, rem))
            return 0;
        alg->kind = TLS_CERT_SIG_RSA_PKCS1;
        alg->hash_id = RT_RSA_HASH_SHA384;
        return 1;
    }
    if (oid_len == sizeof(OID_SHA512_RSA) &&
        memcmp(oid, OID_SHA512_RSA, sizeof(OID_SHA512_RSA)) == 0) {
        if (!der_params_absent_or_null(p, rem))
            return 0;
        alg->kind = TLS_CERT_SIG_RSA_PKCS1;
        alg->hash_id = RT_RSA_HASH_SHA512;
        return 1;
    }
    if (oid_len == sizeof(OID_ECDSA_SHA256) &&
        memcmp(oid, OID_ECDSA_SHA256, sizeof(OID_ECDSA_SHA256)) == 0) {
        if (rem != 0)
            return 0;
        alg->kind = TLS_CERT_SIG_ECDSA_P256;
        alg->hash_id = RT_RSA_HASH_SHA256;
        return 1;
    }
    if (oid_len == sizeof(OID_ECDSA_SHA384) &&
        memcmp(oid, OID_ECDSA_SHA384, sizeof(OID_ECDSA_SHA384)) == 0) {
        if (rem != 0)
            return 0;
        alg->kind = TLS_CERT_SIG_ECDSA_P256;
        alg->hash_id = RT_RSA_HASH_SHA384;
        return 1;
    }
    if (oid_len == sizeof(OID_ECDSA_SHA512) &&
        memcmp(oid, OID_ECDSA_SHA512, sizeof(OID_ECDSA_SHA512)) == 0) {
        if (rem != 0)
            return 0;
        alg->kind = TLS_CERT_SIG_ECDSA_P256;
        alg->hash_id = RT_RSA_HASH_SHA512;
        return 1;
    }
    if (oid_len == sizeof(OID_RSA_PSS) && memcmp(oid, OID_RSA_PSS, sizeof(OID_RSA_PSS)) == 0) {
        alg->kind = TLS_CERT_SIG_RSA_PSS;
        if (rem == 0)
            return 0;
        return cert_parse_pss_params(p, rem, alg);
    }
    return 0;
}

/// @brief Return the digest output length in bytes for a given hash identifier.
/// @param[in] hash_id Runtime RSA digest identifier.
/// @return 32, 48, or 64; 0 for unknown IDs.
static RT_TLS_MAYBE_UNUSED size_t hash_len_from_id(rt_rsa_hash_t hash_id) {
    return hash_id == RT_RSA_HASH_SHA256   ? 32
           : hash_id == RT_RSA_HASH_SHA384 ? 48
           : hash_id == RT_RSA_HASH_SHA512 ? 64
                                           : 0;
}

/// @brief Hash data with the algorithm specified by hash_id and write the digest to out (max 64
/// bytes).
/// @param[in] hash_id Digest algorithm to apply.
/// @param[in] data Input byte sequence.
/// @param[in] len Number of input bytes at @p data.
/// @param[out] out Buffer of at least 64 bytes that receives the digest prefix.
/// @return 1 on success, 0 for unknown hash_id.
static RT_TLS_MAYBE_UNUSED int hash_bytes_for_id(rt_rsa_hash_t hash_id,
                                                 const uint8_t *data,
                                                 size_t len,
                                                 uint8_t out[64]) {
    switch (hash_id) {
        case RT_RSA_HASH_SHA256:
            rt_sha256(data, len, out);
            return 1;
        case RT_RSA_HASH_SHA384:
            rt_sha384(data, len, out);
            return 1;
        case RT_RSA_HASH_SHA512:
            rt_sha512(data, len, out);
            return 1;
        default:
            return 0;
    }
}

/// @brief Verify the digital signature on cert_der using the public key from issuer_der.
///        Supports RSA-PKCS1, RSA-PSS, and ECDSA-P256 based on the signatureAlgorithm OID.
/// @param[in] cert_der DER certificate whose TBSCertificate and signature are verified.
/// @param[in] cert_len Length of @p cert_der in bytes.
/// @param[in] issuer_der DER issuer certificate providing the verification key.
/// @param[in] issuer_len Length of @p issuer_der in bytes.
/// @return 1 if the signature is valid, 0 on error or verification failure.
static RT_TLS_MAYBE_UNUSED int verify_cert_signature(const uint8_t *cert_der,
                                                     size_t cert_len,
                                                     const uint8_t *issuer_der,
                                                     size_t issuer_len) {
    const uint8_t *tbs_der = NULL;
    const uint8_t *alg_der = NULL;
    const uint8_t *sig_bytes = NULL;
    size_t tbs_len = 0;
    size_t alg_len = 0;
    size_t sig_len = 0;
    tls_cert_sig_alg_t alg;
    uint8_t digest[64];

    if (!cert_extract_signature_parts(
            cert_der, cert_len, &tbs_der, &tbs_len, &alg_der, &alg_len, &sig_bytes, &sig_len)) {
        return 0;
    }
    if (!cert_parse_signature_algorithm(alg_der, alg_len, &alg))
        return 0;
    if (!hash_bytes_for_id(alg.hash_id, tbs_der, tbs_len, digest))
        return 0;

    if (alg.kind == TLS_CERT_SIG_RSA_PKCS1 || alg.kind == TLS_CERT_SIG_RSA_PSS) {
        rt_rsa_key_t issuer_key;
        int ok;
        rt_rsa_key_init(&issuer_key);
        if (!cert_get_rsa_pubkey(issuer_der, issuer_len, &issuer_key))
            return 0;
        ok = (alg.kind == TLS_CERT_SIG_RSA_PKCS1)
                 ? rt_rsa_pkcs1_v15_verify(&issuer_key,
                                           alg.hash_id,
                                           digest,
                                           hash_len_from_id(alg.hash_id),
                                           sig_bytes,
                                           sig_len)
                 : rt_rsa_pss_verify(&issuer_key,
                                     alg.hash_id,
                                     digest,
                                     hash_len_from_id(alg.hash_id),
                                     sig_bytes,
                                     sig_len);
        rt_rsa_key_free(&issuer_key);
        return ok;
    }

    if (alg.kind == TLS_CERT_SIG_ECDSA_P256) {
        uint8_t pub_x[32];
        uint8_t pub_y[32];
        uint8_t sig_r[32];
        uint8_t sig_s[32];
        uint8_t digest32[32];
        if (cert_get_ec_pubkey(issuer_der, issuer_len, pub_x, pub_y) != 0 ||
            parse_ecdsa_sig_der(sig_bytes, sig_len, sig_r, sig_s) != 0) {
            return 0;
        }
        memcpy(digest32, digest, 32);
        return ecdsa_p256_verify(pub_x, pub_y, digest32, sig_r, sig_s);
    }

    return 0;
}

/// @brief Read an entire file into a freshly allocated, null-terminated buffer (POSIX).
/// @details Files larger than TLS_TRUST_BUNDLE_MAX_BYTES are rejected. The
///          caller owns the returned buffer and must release it with free().
/// @param[in] path Null-terminated path of the file to read.
/// @param[out] len_out Optional destination for the file size, initialized to zero on failure.
/// @return Pointer to buffer on success, NULL on any I/O or allocation error.
static RT_TLS_MAYBE_UNUSED char *tls_read_file_text(const char *path, size_t *len_out) {
    FILE *f = NULL;
    char *buf = NULL;
    long len = 0;
    if (len_out)
        *len_out = 0;
    if (!path || !*path)
        return NULL;
    f = fopen(path, "rb");
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0)
        goto fail;
    len = ftell(f);
    if (len < 0 || fseek(f, 0, SEEK_SET) != 0)
        goto fail;
    if ((unsigned long)len > TLS_TRUST_BUNDLE_MAX_BYTES)
        goto fail;
    buf = (char *)malloc((size_t)len + 1);
    if (!buf)
        goto fail;
    if ((long)fread(buf, 1, (size_t)len, f) != len)
        goto fail;
    buf[len] = '\0';
    fclose(f);
    if (len_out)
        *len_out = (size_t)len;
    return buf;
fail:
    if (f)
        fclose(f);
    free(buf);
    return NULL;
}

/// @brief Iterate through PEM certificates in a bundle.
/// @details On each call, advances @p pos past the next BEGIN/END CERTIFICATE
///          block and returns a borrowed view of the enclosed Base64 text.
/// @param[in] pem Null-terminated PEM bundle text.
/// @param[in] pem_len Number of bytes in @p pem, excluding its terminator.
/// @param[in,out] pos Search offset on entry; advanced past the matched end marker.
/// @param[out] body_out Receives a borrowed pointer to the Base64 body.
/// @param[out] body_len_out Receives the body length in bytes.
/// @return 1 if a certificate block was found, 0 at end-of-bundle or on parse error.
static RT_TLS_MAYBE_UNUSED int pem_next_certificate(
    const char *pem, size_t pem_len, size_t *pos, const char **body_out, size_t *body_len_out) {
    const char *begin_marker = "-----BEGIN CERTIFICATE-----";
    const char *end_marker = "-----END CERTIFICATE-----";
    const char *cursor = pem + *pos;
    const char *end = pem + pem_len;
    const char *begin = strstr(cursor, begin_marker);
    const char *body = NULL;
    const char *finish = NULL;
    if (!begin || begin >= end)
        return 0;
    body = strchr(begin, '\n');
    if (!body)
        return 0;
    body++;
    finish = strstr(body, end_marker);
    if (!finish || finish > end)
        return 0;
    *body_out = body;
    *body_len_out = (size_t)(finish - body);
    *pos = (size_t)(finish - pem) + strlen(end_marker);
    return 1;
}

/// @brief Search a PEM bundle for a certificate whose DER encoding exactly matches cert_der.
/// @param[in] pem Null-terminated PEM trust-bundle text.
/// @param[in] pem_len Number of bundle bytes, excluding the terminator.
/// @param[in] cert_der DER certificate to locate.
/// @param[in] cert_len Length of @p cert_der in bytes.
/// @return 1 if a matching certificate is found, 0 otherwise.
static RT_TLS_MAYBE_UNUSED int bundle_contains_exact_cert(const char *pem,
                                                          size_t pem_len,
                                                          const uint8_t *cert_der,
                                                          size_t cert_len) {
    size_t pos = 0;
    const char *body = NULL;
    size_t body_len = 0;
    while (pem_next_certificate(pem, pem_len, &pos, &body, &body_len)) {
        uint8_t *decoded = (uint8_t *)malloc(body_len);
        size_t decoded_len = 0;
        int match = 0;
        if (!decoded)
            return 0;
        decoded_len = pem_decode_cert(body, body_len, decoded, body_len);
        match = decoded_len == cert_len && memcmp(decoded, cert_der, cert_len) == 0;
        free(decoded);
        if (match)
            return 1;
    }
    return 0;
}

/// @brief Check whether a PEM bundle contains a trusted CA that issued child_der.
///        Matches by Issuer/Subject equality, validates the CA cert's expiry, BasicConstraints,
///        and verifies the child certificate's signature with the CA's public key.
/// @param[in] pem Null-terminated PEM trust-bundle text.
/// @param[in] pem_len Number of bundle bytes, excluding the terminator.
/// @param[in] child_der DER certificate whose issuer is sought.
/// @param[in] child_len Length of @p child_der in bytes.
/// @return 1 if a valid trusted issuer is found, 0 otherwise.
static RT_TLS_MAYBE_UNUSED int bundle_has_trusted_issuer(const char *pem,
                                                         size_t pem_len,
                                                         const uint8_t *child_der,
                                                         size_t child_len) {
    size_t child_issuer_len = 0;
    const uint8_t *child_issuer = cert_get_issuer(child_der, child_len, &child_issuer_len);
    size_t pos = 0;
    const char *body = NULL;
    size_t body_len = 0;

    if (!child_issuer)
        return 0;
    while (pem_next_certificate(pem, pem_len, &pos, &body, &body_len)) {
        uint8_t *decoded = (uint8_t *)malloc(body_len);
        size_t decoded_len = 0;
        size_t subject_len = 0;
        const uint8_t *subject = NULL;
        int ok = 0;
        if (!decoded)
            return 0;
        decoded_len = pem_decode_cert(body, body_len, decoded, body_len);
        subject = cert_get_subject(decoded, decoded_len, &subject_len);
        if (decoded_len > 0 && subject &&
            der_names_equal(subject, subject_len, child_issuer, child_issuer_len)) {
            if (cert_check_expiry(decoded, decoded_len) == 0 && cert_is_ca(decoded, decoded_len) &&
                verify_cert_signature(child_der, child_len, decoded, decoded_len)) {
                ok = 1;
            }
        }
        free(decoded);
        if (ok)
            return 1;
    }
    return 0;
}

/// @brief Validate the server certificate chain against a PEM CA bundle (native macOS/Linux).
/// @details Uses the session CA file when configured and otherwise probes
///          standard system bundle paths. The leaf is checked for TLS server
///          usage and unsupported critical extensions; each chain link is
///          checked for validity, CA authorization, and signature integrity.
///          Intermediates may be supplied out of order and each can be consumed
///          at most once. On failure, @p session receives a stable error string.
/// @param[in,out] session TLS session containing the peer chain and trust configuration.
/// @return RT_TLS_OK on success, RT_TLS_ERROR_HANDSHAKE on validation failure.
int tls_verify_chain(rt_tls_session_t *session) {
    struct cert_ref {
        const uint8_t *der;
        size_t len;
        int used;
    } intermediates[TLS_MAX_INTERMEDIATE_CERTS];

    size_t intermediate_count = 0;
    size_t list_pos = 0;
    const uint8_t *current_der = NULL;
    size_t current_len = 0;
    const uint8_t *server_cert_der = tls_session_server_cert_der(session);
    const char *bundle_path = NULL;
    char *bundle_pem = NULL;
    size_t bundle_len = 0;

    const char *require_revocation = getenv("ZANNA_TLS_REQUIRE_REVOCATION");
    if (require_revocation && require_revocation[0] && strcmp(require_revocation, "0") != 0) {
        session->error = "TLS: mandatory revocation checking is not supported";
        return RT_TLS_ERROR_HANDSHAKE;
    }

    if (!server_cert_der) {
        session->error = "TLS: no certificate to validate";
        return RT_TLS_ERROR_HANDSHAKE;
    }
    if (!cert_allows_tls_server_auth(server_cert_der, session->server_cert_der_len)) {
        session->error = "TLS: certificate is not valid for TLS server authentication";
        return RT_TLS_ERROR_HANDSHAKE;
    }
    if (cert_has_unsupported_critical_extension(server_cert_der, session->server_cert_der_len)) {
        session->error = "TLS: leaf certificate has an unsupported critical extension";
        return RT_TLS_ERROR_HANDSHAKE;
    }

    if (session->server_cert_list && session->server_cert_list_len > 0) {
        size_t cert_index = 0;
        while (list_pos < session->server_cert_list_len) {
            const uint8_t *cert_der = NULL;
            size_t cert_len = 0;
            if (tls_next_certificate_entry(session->server_cert_list,
                                           session->server_cert_list_len,
                                           &list_pos,
                                           &cert_der,
                                           &cert_len) != 0) {
                session->error = "TLS: malformed certificate chain";
                return RT_TLS_ERROR_HANDSHAKE;
            }
            if (cert_index++ == 0)
                continue;
            if (intermediate_count >= TLS_MAX_INTERMEDIATE_CERTS) {
                session->error = "TLS: certificate chain has too many intermediates";
                return RT_TLS_ERROR_HANDSHAKE;
            }
            intermediates[intermediate_count].der = cert_der;
            intermediates[intermediate_count].len = cert_len;
            intermediates[intermediate_count].used = 0;
            intermediate_count++;
        }
    }

    bundle_path = session->ca_file[0] ? session->ca_file : find_ca_bundle();
    if (!bundle_path) {
        session->error = "TLS: no PEM trust bundle is available";
        return RT_TLS_ERROR_HANDSHAKE;
    }
    bundle_pem = tls_read_file_text(bundle_path, &bundle_len);
    if (!bundle_pem) {
        session->error = "TLS: failed to read the configured trust bundle";
        return RT_TLS_ERROR_HANDSHAKE;
    }

    current_der = server_cert_der;
    current_len = session->server_cert_der_len;
    for (size_t depth = 0; depth < intermediate_count + 8; depth++) {
        if (cert_check_expiry(current_der, current_len) != 0) {
            session->error = "TLS: certificate chain validation failed (expired or not yet valid)";
            free(bundle_pem);
            return RT_TLS_ERROR_HANDSHAKE;
        }
        if (bundle_contains_exact_cert(bundle_pem, bundle_len, current_der, current_len)) {
            free(bundle_pem);
            return RT_TLS_OK;
        }

        {
            size_t issuer_len = 0;
            const uint8_t *issuer = cert_get_issuer(current_der, current_len, &issuer_len);
            int advanced = 0;
            if (!issuer)
                break;
            for (size_t i = 0; i < intermediate_count; i++) {
                size_t subject_len = 0;
                const uint8_t *subject = NULL;
                if (intermediates[i].used)
                    continue;
                subject =
                    cert_get_subject(intermediates[i].der, intermediates[i].len, &subject_len);
                if (!subject || !der_names_equal(subject, subject_len, issuer, issuer_len))
                    continue;
                if (cert_has_unsupported_critical_extension(intermediates[i].der,
                                                            intermediates[i].len) ||
                    cert_check_expiry(intermediates[i].der, intermediates[i].len) != 0 ||
                    !cert_is_ca(intermediates[i].der, intermediates[i].len) ||
                    !verify_cert_signature(
                        current_der, current_len, intermediates[i].der, intermediates[i].len)) {
                    continue;
                }
                intermediates[i].used = 1;
                current_der = intermediates[i].der;
                current_len = intermediates[i].len;
                advanced = 1;
                break;
            }
            if (advanced)
                continue;
        }

        if (bundle_has_trusted_issuer(bundle_pem, bundle_len, current_der, current_len)) {
            free(bundle_pem);
            return RT_TLS_OK;
        }
        break;
    }

    free(bundle_pem);
    session->error = "TLS: certificate chain validation failed";
    return RT_TLS_ERROR_HANDSHAKE;
}

/// @brief Verify the TLS 1.3 CertificateVerify message using in-tree ECDSA-P256 or RSA (native).
/// @details Builds the RFC 8446 server CertificateVerify content from the
///          stored transcript hash, hashes it according to the advertised
///          scheme, and verifies ECDSA P-256 or RSA-PSS signatures with the
///          leaf certificate key. On failure, @p session receives a stable
///          error string when it is non-null.
/// @param[in,out] session TLS session containing the transcript hash and leaf certificate.
/// @param[in] data CertificateVerify body beginning with scheme and signature length fields.
/// @param[in] len Number of bytes available at @p data.
/// @return RT_TLS_OK on success, RT_TLS_ERROR_HANDSHAKE on any failure.
int tls_verify_cert_verify(rt_tls_session_t *session, const uint8_t *data, size_t len) {
    uint16_t sig_scheme = 0;
    uint16_t sig_len = 0;
    const uint8_t *sig_bytes = NULL;
    uint8_t cv_message[130];
    uint8_t content_hash[64];
    size_t hash_len = 0;
    const uint8_t *server_cert_der = NULL;

    if (!session)
        return RT_TLS_ERROR_HANDSHAKE;
    if (!data || len < 4) {
        session->error = "TLS: CertificateVerify message too short";
        return RT_TLS_ERROR_HANDSHAKE;
    }
    sig_scheme = ((uint16_t)data[0] << 8) | data[1];
    sig_len = ((uint16_t)data[2] << 8) | data[3];
    if ((size_t)sig_len > len - 4u) {
        session->error = "TLS: CertificateVerify signature length overflows";
        return RT_TLS_ERROR_HANDSHAKE;
    }
    if ((size_t)sig_len != len - 4u) {
        session->error = "TLS: CertificateVerify signature length mismatch";
        return RT_TLS_ERROR_HANDSHAKE;
    }
    sig_bytes = data + 4;
    server_cert_der = tls_session_server_cert_der(session);
    if (!server_cert_der) {
        session->error = "TLS: CertificateVerify: no certificate stored";
        return RT_TLS_ERROR_HANDSHAKE;
    }

    build_cert_verify_message(session->cert_transcript_hash, cv_message);
    hash_len = sig_scheme_hash_len(sig_scheme);
    if (hash_len == 0) {
        session->error = "TLS: CertificateVerify: unsupported signature scheme";
        return RT_TLS_ERROR_HANDSHAKE;
    }

    switch (hash_len) {
        case 32:
            rt_sha256(cv_message, sizeof(cv_message), content_hash);
            break;
        case 48:
            rt_sha384(cv_message, sizeof(cv_message), content_hash);
            break;
        case 64:
            rt_sha512(cv_message, sizeof(cv_message), content_hash);
            break;
        default:
            session->error = "TLS: CertificateVerify: unsupported signature hash";
            return RT_TLS_ERROR_HANDSHAKE;
    }

    if (sig_scheme == 0x0403) {
        uint8_t pub_x[32];
        uint8_t pub_y[32];
        uint8_t sig_r[32];
        uint8_t sig_s[32];

        if (cert_get_ec_pubkey(server_cert_der, session->server_cert_der_len, pub_x, pub_y) != 0) {
            session->error =
                "TLS: CertificateVerify: certificate does not contain a P-256 public key";
            return RT_TLS_ERROR_HANDSHAKE;
        }
        if (parse_ecdsa_sig_der(sig_bytes, sig_len, sig_r, sig_s) != 0 ||
            !ecdsa_p256_verify(pub_x, pub_y, content_hash, sig_r, sig_s)) {
            session->error = "TLS: CertificateVerify signature verification failed";
            return RT_TLS_ERROR_HANDSHAKE;
        }
        return RT_TLS_OK;
    }

    if (sig_scheme == 0x0804 || sig_scheme == 0x0805 || sig_scheme == 0x0806) {
        rt_rsa_key_t rsa_key;
        int verified = 0;
        rt_rsa_key_init(&rsa_key);
        if (!cert_get_rsa_pubkey(server_cert_der, session->server_cert_der_len, &rsa_key)) {
            session->error =
                "TLS: CertificateVerify: certificate does not contain an RSA public key";
            rt_rsa_key_free(&rsa_key);
            return RT_TLS_ERROR_HANDSHAKE;
        }
        verified = rt_rsa_pss_verify(&rsa_key,
                                     sig_scheme == 0x0804   ? RT_RSA_HASH_SHA256
                                     : sig_scheme == 0x0805 ? RT_RSA_HASH_SHA384
                                                            : RT_RSA_HASH_SHA512,
                                     content_hash,
                                     hash_len,
                                     sig_bytes,
                                     sig_len);
        rt_rsa_key_free(&rsa_key);
        if (!verified) {
            session->error = "TLS: CertificateVerify signature verification failed";
            return RT_TLS_ERROR_HANDSHAKE;
        }
        return RT_TLS_OK;
    }

    session->error = "TLS: CertificateVerify: unsupported signature scheme";
    return RT_TLS_ERROR_HANDSHAKE;
}
#endif // !defined(_WIN32)
