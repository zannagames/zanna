//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/network/rt_tls_certs.c
// Purpose: Certificate and private-key parsing for the TLS runtime — PEM/DER
//          decoding, SEC1/PKCS#8 EC and RSA key extraction, and certificate
//          public-key-type detection. Split out of rt_tls.c; shares the DER
//          TLV/OID helpers via rt_tls_internal.h.
//
// Key invariants:
//   - Parsers are bounds-checked against the input length and reject malformed
//     or unsupported (e.g. encrypted) key material rather than trusting it.
//   - Key-type detection drives the handshake's signature-algorithm selection.
//
// Ownership/Lifetime:
//   - Returns parsed key/cert material into caller-owned buffers/structs; the
//     text-file reader returns a malloc'd buffer the caller frees.
//
// Links: src/runtime/network/rt_tls.c (TLS protocol engine),
//        src/runtime/network/rt_tls_internal.h (shared DER/cert decls)
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_tls_certs.c
 * @brief Implements TLS certificate and private-key loading and parsing.
 * @details The bounded loaders decode PEM or DER certificate chains and
 *          SEC1, PKCS#8, or PKCS#1 private keys, validate strict TLV and OID
 *          structure, and identify supported public-key types for server
 *          signature selection. Parsed material is returned through explicitly
 *          owned caller storage.
 */

#include "rt_crypto.h"
#include "rt_crypto_module.h"
#include "rt_ecdsa_p256.h"
#include "rt_file_stdio.h"
#include "rt_internal.h"
#include "rt_object.h"
#include "rt_rsa.h"
#include "rt_tls_internal.h"
#include "rt_tls_server_internal.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/// @brief Maximum text certificate/key file size accepted by TLS loaders.
/// @details PEM bundles, certificate chains, and private keys should be small.
///          A fixed cap prevents accidental or maliciously huge files from
///          forcing unbounded allocations through ftell()/malloc().
#define TLS_TEXT_FILE_MAX_BYTES (4u * 1024u * 1024u)

/// @brief Read a bounded text file into a NUL-terminated native buffer.
/// @details UTF-8 path opening is platform-adapted. Files larger than 4 MiB,
///          seek/read failures, and allocation failure return NULL with all
///          partial resources released.
/// @param path Nonempty UTF-8 filesystem path.
/// @param len_out Optional destination for the byte length excluding NUL.
/// @return Caller-owned heap buffer, or NULL on failure.
char *tls_read_text_file(const char *path, size_t *len_out) {
    FILE *f = NULL;
    char *buf = NULL;
    long len = 0;

    if (len_out)
        *len_out = 0;
    if (!path || !*path)
        return NULL;

    f = rt_file_stdio_open_utf8(path, "rb");
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0)
        goto fail;
    len = ftell(f);
    if (len < 0 || fseek(f, 0, SEEK_SET) != 0)
        goto fail;
    if ((unsigned long)len > TLS_TEXT_FILE_MAX_BYTES)
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

/// @brief Decode the Base64 body of a PEM block into a DER byte buffer.
///        Skips whitespace and stops at '=' padding characters or non-Base64 bytes.
///        Returns the number of DER bytes written; returns 0 if out_der overflows max_der.
/// @param pem_b64 Base64 text bytes between PEM markers.
/// @param b64_len Number of input bytes.
/// @param out_der Caller-provided DER destination.
/// @param max_der Destination capacity.
/// @return Number of decoded bytes, or zero for invalid input or overflow.
size_t tls_pem_base64_decode(const char *pem_b64,
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

/// @brief Locate one PEM block delimited by begin_marker / end_marker in the pem string.
///        Sets *body_out and *body_len_out to the Base64 text between the markers.
///        Sets *next_out to the character after the end marker for chained iteration.
///        Returns 1 if a block was found, 0 otherwise.
/// @param pem NUL-terminated PEM document or remaining suffix.
/// @param begin_marker NUL-terminated begin delimiter.
/// @param end_marker NUL-terminated end delimiter.
/// @param body_out Destination for a borrowed body pointer.
/// @param body_len_out Destination for body byte length.
/// @param next_out Optional destination for the suffix after the end marker.
/// @return One when a complete block is located; otherwise zero.
int tls_find_pem_block(const char *pem,
                       const char *begin_marker,
                       const char *end_marker,
                       const char **body_out,
                       size_t *body_len_out,
                       const char **next_out) {
    const char *begin = NULL;
    const char *body = NULL;
    const char *end = NULL;
    if (!pem || !begin_marker || !end_marker || !body_out || !body_len_out)
        return 0;
    begin = strstr(pem, begin_marker);
    if (!begin)
        return 0;
    body = strchr(begin, '\n');
    if (!body)
        return 0;
    body++;
    end = strstr(body, end_marker);
    if (!end)
        return 0;
    *body_out = body;
    *body_len_out = (size_t)(end - body);
    if (next_out)
        *next_out = end + strlen(end_marker);
    return 1;
}

/// @brief Copy up to 32 DER integer bytes into a right-aligned 32-byte big-endian buffer.
///        Strips leading zero padding bytes (required for DER positive-integer encoding).
///        Returns 1 on success; 0 if the remaining value exceeds 32 bytes.
/// @param data DER integer or OCTET STRING content bytes.
/// @param len Number of input bytes.
/// @param out Destination 32-byte right-aligned value.
/// @param out_len Destination for significant byte length.
/// @return One on success; zero for empty, invalid, or oversized input.
static int tls_copy_der_octets(const uint8_t *data, size_t len, uint8_t out[32], size_t *out_len) {
    size_t skip = 0;
    if (len == 0 || !out || !out_len)
        return 0;
    while (skip + 1 < len && data[skip] == 0x00)
        skip++;
    len -= skip;
    data += skip;
    if (len > 32)
        return 0;
    memset(out, 0, 32);
    memcpy(out + (32 - len), data, len);
    *out_len = len;
    return 1;
}

/// @brief Extract the 32-byte scalar from a SEC 1 (RFC 5915) DER-encoded EC private key.
///        SEC 1 structure: SEQUENCE { INTEGER (version), OCTET STRING (key), ... }.
///        Returns 1 on success, 0 on parse failure or key too long.
/// @param der Complete SEC 1 ECPrivateKey DER bytes.
/// @param der_len Number of DER bytes.
/// @param out_priv Destination 32-byte right-aligned private scalar.
/// @return One on success; zero for malformed or oversized input.
int tls_parse_sec1_ec_private_key(const uint8_t *der, size_t der_len, uint8_t out_priv[32]) {
    uint8_t tag;
    size_t vl, hl;
    const uint8_t *p = der;
    size_t rem = der_len;
    size_t scalar_len = 0;

    if (tls_der_read_tlv(p, rem, &tag, &vl, &hl) != 0 || tag != 0x30)
        return 0;
    p += hl;
    rem = vl;
    if (tls_der_read_tlv(p, rem, &tag, &vl, &hl) != 0 || tag != 0x02)
        return 0;
    p += hl + vl;
    rem -= hl + vl;
    if (tls_der_read_tlv(p, rem, &tag, &vl, &hl) != 0 || tag != 0x04)
        return 0;
    return tls_copy_der_octets(p + hl, vl, out_priv, &scalar_len);
}

/// @brief Extract the 32-byte scalar from a PKCS#8 (RFC 5958) DER-encoded EC private key.
///        Verifies the AlgorithmIdentifier contains id-ecPublicKey + prime256v1 OIDs before
///        delegating the inner SEC 1 ECPrivateKey to tls_parse_sec1_ec_private_key.
///        Returns 1 on success, 0 on parse failure or OID mismatch.
/// @param der Complete PKCS#8 PrivateKeyInfo DER bytes.
/// @param der_len Number of DER bytes.
/// @param out_priv Destination 32-byte P-256 private scalar.
/// @return One on success; zero for malformed input or unsupported algorithm.
int tls_parse_pkcs8_ec_private_key(const uint8_t *der, size_t der_len, uint8_t out_priv[32]) {
    static const uint8_t OID_EC_PUBLIC_KEY[] = {0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x02, 0x01};
    static const uint8_t OID_PRIME256V1[] = {0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07};
    uint8_t tag;
    size_t vl, hl;
    const uint8_t *p = der;
    size_t rem = der_len;
    const uint8_t *alg = NULL;
    size_t alg_len = 0;

    if (tls_der_read_tlv(p, rem, &tag, &vl, &hl) != 0 || tag != 0x30)
        return 0;
    p += hl;
    rem = vl;
    if (tls_der_read_tlv(p, rem, &tag, &vl, &hl) != 0 || tag != 0x02)
        return 0;
    p += hl + vl;
    rem -= hl + vl;
    if (tls_der_read_tlv(p, rem, &tag, &vl, &hl) != 0 || tag != 0x30)
        return 0;
    alg = p + hl;
    alg_len = vl;
    p += hl + vl;
    rem -= hl + vl;

    if (tls_der_read_tlv(alg, alg_len, &tag, &vl, &hl) != 0 || tag != 0x06 ||
        !tls_oid_matches(alg + hl, vl, OID_EC_PUBLIC_KEY, sizeof(OID_EC_PUBLIC_KEY))) {
        return 0;
    }
    alg += hl + vl;
    alg_len -= hl + vl;
    if (tls_der_read_tlv(alg, alg_len, &tag, &vl, &hl) != 0 || tag != 0x06 ||
        !tls_oid_matches(alg + hl, vl, OID_PRIME256V1, sizeof(OID_PRIME256V1))) {
        return 0;
    }

    if (tls_der_read_tlv(p, rem, &tag, &vl, &hl) != 0 || tag != 0x04)
        return 0;
    return tls_parse_sec1_ec_private_key(p + hl, vl, out_priv);
}

/// @brief Extract the uncompressed P-256 public key (x, y) from a DER-encoded X.509 certificate.
///        Navigates TBSCertificate → SubjectPublicKeyInfo, checks the ec-public-key +
///        prime256v1 OIDs, then reads the 04-prefixed 65-byte uncompressed point.
///        Returns 1 on success, 0 on parse failure or wrong key type.
/// @param cert_der Complete X.509 certificate DER bytes.
/// @param cert_len Number of certificate bytes.
/// @param x_out Destination 32-byte affine x coordinate.
/// @param y_out Destination 32-byte affine y coordinate.
/// @return One on success; zero for malformed input or a non-P-256 key.
int tls_extract_cert_ec_pubkey(const uint8_t *cert_der,
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

    if (tls_der_read_tlv(p, rem, &tag, &vl, &hl) != 0 || tag != 0x30)
        return 0;
    p += hl;
    rem = vl;
    if (tls_der_read_tlv(p, rem, &tag, &vl, &hl) != 0 || tag != 0x30)
        return 0;
    tbs = p + hl;
    tbs_rem = vl;

    if (tls_der_read_tlv(tbs, tbs_rem, &tag, &vl, &hl) != 0)
        return 0;
    if (tag == 0xA0) {
        tbs += hl + vl;
        tbs_rem -= hl + vl;
    }

    for (int i = 0; i < 5; i++) {
        if (tls_der_read_tlv(tbs, tbs_rem, &tag, &vl, &hl) != 0)
            return 0;
        tbs += hl + vl;
        tbs_rem -= hl + vl;
    }

    if (tls_der_read_tlv(tbs, tbs_rem, &tag, &vl, &hl) != 0 || tag != 0x30)
        return 0;
    spki = tbs + hl;
    spki_rem = vl;

    if (tls_der_read_tlv(spki, spki_rem, &tag, &vl, &hl) != 0 || tag != 0x30)
        return 0;
    algo = spki + hl;
    algo_rem = vl;
    spki += hl + vl;
    spki_rem -= hl + vl;

    if (tls_der_read_tlv(algo, algo_rem, &tag, &vl, &hl) != 0 || tag != 0x06 ||
        !tls_oid_matches(algo + hl, vl, OID_EC_PUBLIC_KEY, sizeof(OID_EC_PUBLIC_KEY))) {
        return 0;
    }
    algo += hl + vl;
    algo_rem -= hl + vl;
    if (tls_der_read_tlv(algo, algo_rem, &tag, &vl, &hl) != 0 || tag != 0x06 ||
        !tls_oid_matches(algo + hl, vl, OID_PRIME256V1, sizeof(OID_PRIME256V1))) {
        return 0;
    }

    if (tls_der_read_tlv(spki, spki_rem, &tag, &vl, &hl) != 0 || tag != 0x03 || vl < 66)
        return 0;
    bits = spki + hl;
    if (bits[0] != 0x00 || bits[1] != 0x04)
        return 0;
    memcpy(x_out, bits + 2, 32);
    memcpy(y_out, bits + 34, 32);
    return 1;
}

/// @brief Extract an RSA public key from a DER-encoded X.509 certificate into out.
///        Navigates TBSCertificate → SubjectPublicKeyInfo, verifies the rsaEncryption OID,
///        then parses the RSAPublicKey (modulus + exponent) via rt_rsa_key_from_der.
///        Returns 1 on success, 0 on parse failure or wrong key type.
/// @param cert_der Complete X.509 certificate DER bytes.
/// @param cert_len Number of certificate bytes.
/// @param out Initialized RSA key receiving public components.
/// @return One on success; zero for malformed input or a non-RSA key.
int tls_extract_cert_rsa_pubkey(const uint8_t *cert_der, size_t cert_len, rt_rsa_key_t *out) {
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

    if (!cert_der || cert_len == 0 || !out)
        return 0;
    rt_rsa_key_init(out);

    if (tls_der_read_tlv(p, rem, &tag, &vl, &hl) != 0 || tag != 0x30)
        return 0;
    p += hl;
    rem = vl;
    if (tls_der_read_tlv(p, rem, &tag, &vl, &hl) != 0 || tag != 0x30)
        return 0;
    tbs = p + hl;
    tbs_rem = vl;

    if (tls_der_read_tlv(tbs, tbs_rem, &tag, &vl, &hl) != 0)
        return 0;
    if (tag == 0xA0) {
        tbs += hl + vl;
        tbs_rem -= hl + vl;
    }

    for (int i = 0; i < 5; i++) {
        if (tls_der_read_tlv(tbs, tbs_rem, &tag, &vl, &hl) != 0)
            return 0;
        tbs += hl + vl;
        tbs_rem -= hl + vl;
    }

    if (tls_der_read_tlv(tbs, tbs_rem, &tag, &vl, &hl) != 0 || tag != 0x30)
        return 0;
    spki = tbs + hl;
    spki_rem = vl;
    if (tls_der_read_tlv(spki, spki_rem, &tag, &vl, &hl) != 0 || tag != 0x30)
        return 0;
    algo = spki + hl;
    algo_rem = vl;
    spki += hl + vl;
    spki_rem -= hl + vl;

    if (tls_der_read_tlv(algo, algo_rem, &tag, &vl, &hl) != 0 || tag != 0x06 ||
        !tls_oid_matches(algo + hl, vl, OID_RSA_ENCRYPTION, sizeof(OID_RSA_ENCRYPTION))) {
        return 0;
    }
    if (tls_der_read_tlv(spki, spki_rem, &tag, &vl, &hl) != 0 || tag != 0x03 || vl < 2)
        return 0;
    bits = spki + hl;
    if (bits[0] != 0x00)
        return 0;
    return rt_rsa_parse_public_key_pkcs1(bits + 1, vl - 1, out);
}

/// @brief Return TLS_SERVER_KEY_ECDSA_P256 or TLS_SERVER_KEY_RSA_PSS_SHA256 by inspecting
///        the SubjectPublicKeyInfo algorithm OID in the leaf certificate.
///        Returns TLS_SERVER_KEY_NONE if the key type is not recognised.
/// @param cert_der Complete X.509 certificate DER bytes.
/// @param cert_len Number of certificate bytes.
/// @return Supported server key type, or @c TLS_SERVER_KEY_NONE.
int tls_extract_cert_key_type(const uint8_t *cert_der, size_t cert_len) {
    static const uint8_t OID_EC_PUBLIC_KEY[] = {0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x02, 0x01};
    static const uint8_t OID_PRIME256V1[] = {0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07};
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

    if (tls_der_read_tlv(p, rem, &tag, &vl, &hl) != 0 || tag != 0x30)
        return TLS_SERVER_KEY_NONE;
    p += hl;
    rem = vl;
    if (tls_der_read_tlv(p, rem, &tag, &vl, &hl) != 0 || tag != 0x30)
        return TLS_SERVER_KEY_NONE;
    tbs = p + hl;
    tbs_rem = vl;

    if (tls_der_read_tlv(tbs, tbs_rem, &tag, &vl, &hl) != 0)
        return TLS_SERVER_KEY_NONE;
    if (tag == 0xA0) {
        tbs += hl + vl;
        tbs_rem -= hl + vl;
    }

    for (int i = 0; i < 5; i++) {
        if (tls_der_read_tlv(tbs, tbs_rem, &tag, &vl, &hl) != 0)
            return TLS_SERVER_KEY_NONE;
        tbs += hl + vl;
        tbs_rem -= hl + vl;
    }

    if (tls_der_read_tlv(tbs, tbs_rem, &tag, &vl, &hl) != 0 || tag != 0x30)
        return TLS_SERVER_KEY_NONE;
    spki = tbs + hl;
    spki_rem = vl;

    if (tls_der_read_tlv(spki, spki_rem, &tag, &vl, &hl) != 0 || tag != 0x30)
        return TLS_SERVER_KEY_NONE;
    algo = spki + hl;
    algo_rem = vl;

    if (tls_der_read_tlv(algo, algo_rem, &tag, &vl, &hl) != 0 || tag != 0x06)
        return TLS_SERVER_KEY_NONE;
    if (tls_oid_matches(algo + hl, vl, OID_RSA_ENCRYPTION, sizeof(OID_RSA_ENCRYPTION)))
        return TLS_SERVER_KEY_RSA_PSS_SHA256;
    if (!tls_oid_matches(algo + hl, vl, OID_EC_PUBLIC_KEY, sizeof(OID_EC_PUBLIC_KEY)))
        return TLS_SERVER_KEY_NONE;

    algo += hl + vl;
    algo_rem -= hl + vl;
    if (tls_der_read_tlv(algo, algo_rem, &tag, &vl, &hl) != 0 || tag != 0x06)
        return TLS_SERVER_KEY_NONE;
    if (tls_oid_matches(algo + hl, vl, OID_PRIME256V1, sizeof(OID_PRIME256V1)))
        return TLS_SERVER_KEY_ECDSA_P256;
    return TLS_SERVER_KEY_NONE;
}

/// @brief Reset the transcript hash to the empty-handshake state.
///
/// TLS 1.3 keys are derived from a running SHA-256 hash over every
/// handshake message exchanged so far. This primes the rolling
/// context and seeds `transcript_hash` with `SHA-256("")` so that
/// HKDF helpers can be called even before the first message arrives.
