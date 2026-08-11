//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/runtime/RTTlsCertTests.cpp
// Purpose: Unit tests for TLS X.509 certificate parsing and hostname
//          verification (CS-1/CS-2/CS-3 internal functions).
//
// Coverage:
//   - tls_match_hostname:       RFC 6125 exact and wildcard matching
//   - tls_extract_san_names:    SubjectAltName DNS name extraction from DER
//   - tls_extract_cn:           CommonName extraction from Subject DER
//   - tls_parse_certificate_msg: TLS Certificate message parsing
//   - tls_verify_hostname:      End-to-end hostname verification via session
//   - tls_verify_cert_verify:    Strict CertificateVerify message framing
//
// Test certs generated with one openssl invocation, wrapped here for width:
//   openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256
//     -nodes -keyout key.pem -out cert.pem -days 365
//     -subj "/CN=example.com"
//     -addext "subjectAltName=DNS:example.com,DNS:*.example.com,DNS:www.example.com"
//===----------------------------------------------------------------------===//

#include "common/PlatformCapabilities.hpp"
#include "rt_tls.h"
#include "rt_tls_internal.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

// ---------------------------------------------------------------------------
// Test certificate DER data
//
// Cert 1: CN=example.com, SAN: example.com, *.example.com, www.example.com
//         EC P-256, self-signed, valid 2026-02-23 to 2027-02-23
// ---------------------------------------------------------------------------
static const uint8_t TEST_CERT_WITH_SAN[] = {
    0x30, 0x82, 0x01, 0xbb, 0x30, 0x82, 0x01, 0x61, 0xa0, 0x03, 0x02, 0x01, 0x02, 0x02, 0x14, 0x74,
    0xe1, 0xb8, 0x2b, 0xbc, 0x2a, 0x64, 0x15, 0xe2, 0x1e, 0x1c, 0xa2, 0x0e, 0x2c, 0x63, 0xe9, 0x0c,
    0xfd, 0xb5, 0x0f, 0x30, 0x0a, 0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x02, 0x30,
    0x16, 0x31, 0x14, 0x30, 0x12, 0x06, 0x03, 0x55, 0x04, 0x03, 0x0c, 0x0b, 0x65, 0x78, 0x61, 0x6d,
    0x70, 0x6c, 0x65, 0x2e, 0x63, 0x6f, 0x6d, 0x30, 0x1e, 0x17, 0x0d, 0x32, 0x36, 0x30, 0x32, 0x32,
    0x33, 0x31, 0x35, 0x31, 0x33, 0x34, 0x39, 0x5a, 0x17, 0x0d, 0x32, 0x37, 0x30, 0x32, 0x32, 0x33,
    0x31, 0x35, 0x31, 0x33, 0x34, 0x39, 0x5a, 0x30, 0x16, 0x31, 0x14, 0x30, 0x12, 0x06, 0x03, 0x55,
    0x04, 0x03, 0x0c, 0x0b, 0x65, 0x78, 0x61, 0x6d, 0x70, 0x6c, 0x65, 0x2e, 0x63, 0x6f, 0x6d, 0x30,
    0x59, 0x30, 0x13, 0x06, 0x07, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01, 0x06, 0x08, 0x2a, 0x86,
    0x48, 0xce, 0x3d, 0x03, 0x01, 0x07, 0x03, 0x42, 0x00, 0x04, 0x7e, 0x15, 0x4c, 0x10, 0x71, 0x81,
    0x45, 0x29, 0xb6, 0x70, 0x71, 0x22, 0x0a, 0x5c, 0x15, 0x28, 0xb8, 0xa8, 0xb6, 0xf8, 0x85, 0xe8,
    0x5a, 0xcc, 0x95, 0x75, 0x07, 0xa4, 0x5c, 0x99, 0xdc, 0x01, 0x66, 0x8a, 0x9f, 0x99, 0xc3, 0x09,
    0x31, 0x95, 0x24, 0xaa, 0x69, 0x10, 0xe4, 0x78, 0x1b, 0x58, 0x7b, 0xbc, 0x35, 0x8a, 0x55, 0x77,
    0x07, 0x49, 0x7e, 0x06, 0xb1, 0x4d, 0x1a, 0xd0, 0xaa, 0x27, 0xa3, 0x81, 0x8c, 0x30, 0x81, 0x89,
    0x30, 0x1d, 0x06, 0x03, 0x55, 0x1d, 0x0e, 0x04, 0x16, 0x04, 0x14, 0xa8, 0xa0, 0x68, 0x42, 0xc4,
    0xb7, 0x52, 0xd5, 0x44, 0xa2, 0x4c, 0x09, 0xd6, 0xa4, 0x6a, 0x54, 0x99, 0x18, 0x31, 0x50, 0x30,
    0x1f, 0x06, 0x03, 0x55, 0x1d, 0x23, 0x04, 0x18, 0x30, 0x16, 0x80, 0x14, 0xa8, 0xa0, 0x68, 0x42,
    0xc4, 0xb7, 0x52, 0xd5, 0x44, 0xa2, 0x4c, 0x09, 0xd6, 0xa4, 0x6a, 0x54, 0x99, 0x18, 0x31, 0x50,
    0x30, 0x0f, 0x06, 0x03, 0x55, 0x1d, 0x13, 0x01, 0x01, 0xff, 0x04, 0x05, 0x30, 0x03, 0x01, 0x01,
    0xff, 0x30, 0x36, 0x06, 0x03, 0x55, 0x1d, 0x11, 0x04, 0x2f, 0x30, 0x2d, 0x82, 0x0b, 0x65, 0x78,
    0x61, 0x6d, 0x70, 0x6c, 0x65, 0x2e, 0x63, 0x6f, 0x6d, 0x82, 0x0d, 0x2a, 0x2e, 0x65, 0x78, 0x61,
    0x6d, 0x70, 0x6c, 0x65, 0x2e, 0x63, 0x6f, 0x6d, 0x82, 0x0f, 0x77, 0x77, 0x77, 0x2e, 0x65, 0x78,
    0x61, 0x6d, 0x70, 0x6c, 0x65, 0x2e, 0x63, 0x6f, 0x6d, 0x30, 0x0a, 0x06, 0x08, 0x2a, 0x86, 0x48,
    0xce, 0x3d, 0x04, 0x03, 0x02, 0x03, 0x48, 0x00, 0x30, 0x45, 0x02, 0x20, 0x7e, 0x71, 0x54, 0x66,
    0x70, 0xef, 0xfb, 0x88, 0x43, 0xbd, 0xd9, 0x86, 0x6f, 0x2d, 0xeb, 0x82, 0x41, 0x2a, 0x34, 0xb0,
    0xd2, 0xd1, 0x0b, 0xab, 0x1d, 0x22, 0xc9, 0xe4, 0xb6, 0x22, 0xa2, 0xe2, 0x02, 0x21, 0x00, 0xd2,
    0x2c, 0xdb, 0xe9, 0x11, 0x5f, 0x70, 0xdc, 0x5f, 0xed, 0xd7, 0xe4, 0xc6, 0x7d, 0x43, 0xdf, 0x54,
    0xfd, 0xc9, 0x8f, 0x83, 0x4a, 0x03, 0x80, 0xa4, 0xd0, 0xe2, 0x05, 0x9d, 0x73, 0xa3, 0xef};
static const size_t TEST_CERT_WITH_SAN_LEN = 447;

// ---------------------------------------------------------------------------
// Cert 2: CN=cn-only.example.com, NO SubjectAltName extension
//         EC P-256, self-signed, valid 2026-02-23 to 2027-02-23
// ---------------------------------------------------------------------------
static const uint8_t TEST_CERT_CN_ONLY[] = {
    0x30, 0x82, 0x01, 0x91, 0x30, 0x82, 0x01, 0x37, 0xa0, 0x03, 0x02, 0x01, 0x02, 0x02, 0x14, 0x75,
    0xa0, 0x2a, 0xee, 0x50, 0x19, 0x58, 0xa4, 0x34, 0x3d, 0x32, 0x9a, 0xf2, 0x2f, 0x1f, 0xe1, 0x9d,
    0xf4, 0x79, 0xe2, 0x30, 0x0a, 0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x02, 0x30,
    0x1e, 0x31, 0x1c, 0x30, 0x1a, 0x06, 0x03, 0x55, 0x04, 0x03, 0x0c, 0x13, 0x63, 0x6e, 0x2d, 0x6f,
    0x6e, 0x6c, 0x79, 0x2e, 0x65, 0x78, 0x61, 0x6d, 0x70, 0x6c, 0x65, 0x2e, 0x63, 0x6f, 0x6d, 0x30,
    0x1e, 0x17, 0x0d, 0x32, 0x36, 0x30, 0x32, 0x32, 0x33, 0x31, 0x35, 0x31, 0x33, 0x35, 0x38, 0x5a,
    0x17, 0x0d, 0x32, 0x37, 0x30, 0x32, 0x32, 0x33, 0x31, 0x35, 0x31, 0x33, 0x35, 0x38, 0x5a, 0x30,
    0x1e, 0x31, 0x1c, 0x30, 0x1a, 0x06, 0x03, 0x55, 0x04, 0x03, 0x0c, 0x13, 0x63, 0x6e, 0x2d, 0x6f,
    0x6e, 0x6c, 0x79, 0x2e, 0x65, 0x78, 0x61, 0x6d, 0x70, 0x6c, 0x65, 0x2e, 0x63, 0x6f, 0x6d, 0x30,
    0x59, 0x30, 0x13, 0x06, 0x07, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01, 0x06, 0x08, 0x2a, 0x86,
    0x48, 0xce, 0x3d, 0x03, 0x01, 0x07, 0x03, 0x42, 0x00, 0x04, 0x72, 0xe5, 0xf5, 0x22, 0x99, 0xd3,
    0xd8, 0xfd, 0x68, 0xe9, 0xcd, 0xd8, 0x6b, 0xe7, 0x2e, 0x2b, 0xab, 0x0c, 0x08, 0x2a, 0xe3, 0x0a,
    0xd4, 0x77, 0x2f, 0x57, 0x5b, 0x26, 0x4d, 0x58, 0x24, 0xa8, 0xfd, 0x73, 0x59, 0xb9, 0x0a, 0x78,
    0xa1, 0x03, 0x2b, 0x8d, 0xfc, 0x2c, 0x81, 0xb7, 0xd7, 0x6c, 0x79, 0x06, 0xf7, 0x18, 0x1d, 0x3c,
    0x78, 0xa2, 0x26, 0x0f, 0xc4, 0x06, 0xc8, 0x56, 0x36, 0x7f, 0xa3, 0x53, 0x30, 0x51, 0x30, 0x1d,
    0x06, 0x03, 0x55, 0x1d, 0x0e, 0x04, 0x16, 0x04, 0x14, 0x59, 0x22, 0x5a, 0x45, 0x05, 0x7f, 0x5d,
    0x4a, 0x22, 0x23, 0xf8, 0x7b, 0x17, 0x95, 0xab, 0x6d, 0xb6, 0x49, 0x1b, 0x16, 0x30, 0x1f, 0x06,
    0x03, 0x55, 0x1d, 0x23, 0x04, 0x18, 0x30, 0x16, 0x80, 0x14, 0x59, 0x22, 0x5a, 0x45, 0x05, 0x7f,
    0x5d, 0x4a, 0x22, 0x23, 0xf8, 0x7b, 0x17, 0x95, 0xab, 0x6d, 0xb6, 0x49, 0x1b, 0x16, 0x30, 0x0f,
    0x06, 0x03, 0x55, 0x1d, 0x13, 0x01, 0x01, 0xff, 0x04, 0x05, 0x30, 0x03, 0x01, 0x01, 0xff, 0x30,
    0x0a, 0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x02, 0x03, 0x48, 0x00, 0x30, 0x45,
    0x02, 0x20, 0x4d, 0xfd, 0x78, 0xc9, 0x11, 0x39, 0x3c, 0xe7, 0xb9, 0xf1, 0x74, 0xa6, 0xc2, 0xc1,
    0xc4, 0x82, 0xc5, 0xde, 0xef, 0xec, 0x08, 0x2e, 0xfb, 0x32, 0xbb, 0x70, 0x07, 0xac, 0x13, 0xcb,
    0x22, 0x26, 0x02, 0x21, 0x00, 0xf6, 0x19, 0x73, 0x8d, 0x13, 0x53, 0x29, 0xdd, 0x5f, 0xd8, 0x7f,
    0x61, 0x0b, 0x6c, 0x88, 0xe6, 0x86, 0x30, 0xba, 0x23, 0xe8, 0xdb, 0x4c, 0x1b, 0x30, 0x42, 0x1b,
    0xd9, 0x8e, 0x14, 0x24, 0xc8};
static const size_t TEST_CERT_CN_ONLY_LEN = 405;

static const char *TEST_RSA_ROOT_CERT_PEM = R"PEM(-----BEGIN CERTIFICATE-----
MIIDEzCCAfugAwIBAgICEAEwDQYJKoZIhvcNAQELBQAwGjEYMBYGA1UEAwwPVmlw
ZXIgVGVzdCBSb290MB4XDTI0MDEwMTAwMDAwMFoXDTM4MDEwMTAwMDAwMFowGjEY
MBYGA1UEAwwPVmlwZXIgVGVzdCBSb290MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8A
MIIBCgKCAQEAsCvt6uYssvJ6Qlsn8LQAmJ0YUbN2gOYwyj2RV3s8UejGMQPleNIf
eRvoeD/WZuG25Vf35YJ6djd0Vw1Mgt0Uk6M8C8oORoGQ6XRsGlNBOqzDiZbuDqxH
KZ1kP3C69p7Ey5cOmk+tgo3vVJgwFAGPhR2f4540UvJ+rAe255wbs9IJ5uqFkKHI
ccb+lrNZZaFPFGBSmI1Czm6ggPx3RHByOtSBepqB6VZzv05rzDV1WHFCGhlyBClJ
BfG29kPuj5n03MUPxfw8NAMahPFszFPoA71oxan4qzffWyKP7FubAUTzxArI8sf5
3otkyWp1d6snlyHlAI7kAb4vdIrwqZRcMQIDAQABo2MwYTAdBgNVHQ4EFgQUZsaT
+usuVwXWs0SWZvbAykvJFP8wHwYDVR0jBBgwFoAUZsaT+usuVwXWs0SWZvbAykvJ
FP8wDwYDVR0TAQH/BAUwAwEB/zAOBgNVHQ8BAf8EBAMCAQYwDQYJKoZIhvcNAQEL
BQADggEBAI+1UE7O2TCevcQfQJZSPGJw8vNGkzCv9tMh4qhV+zfmtjLkmefvQ+0M
FJr/adXHYGE0WjwQ8VK4pfmd6gs26/PCEDREy0JpSXxrkyEiyei5WZvrNBvGQL4m
+XAngI5eTX7UwL7YBP/z8oUHQZOWXGsRyCNFpKi5zZKMUoR8aHuc1f9JDkyNbeon
by85P8iOkGoFFWGMSSI8+lQqPLAYITEUfzqWsyU/T9yXuGzaeOh+lLnq/sOygumJ
crDCbvYzu1M8VSpiGtQ9tiFSqIyQruau7RM69GFicRtKfWOOnHEafY5cZ+V9Dder
RXrTo5++oZs1QvYkMwOG9dD4/fGdRTY=
-----END CERTIFICATE-----
)PEM";

static const char *TEST_RSA_LEAF_CERT_PEM = R"PEM(-----BEGIN CERTIFICATE-----
MIIDPTCCAiWgAwIBAgICIAEwDQYJKoZIhvcNAQELBQAwGjEYMBYGA1UEAwwPVmlw
ZXIgVGVzdCBSb290MB4XDTI0MDEwMTAwMDAwMFoXDTM4MDEwMTAwMDAwMFowFDES
MBAGA1UEAwwJbG9jYWxob3N0MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKC
AQEAhKKdI7etjnO2Jnf/6gT9VrQEFCRmU73zic7zvHXkpyg/5zWy+cz7kdV87Ol6
h+5mvl63sspIpCeQ4DbQenlzhyMyDwO+YCZhkZp1Pu6Tq/XoixFL30JHL8uWyvd+
IJDY/Ihn163GdFHJ5aoiljUeZu9xEsYz8qqTR3hwDBpQpeTL3Bd4qIfVUeD5vazF
nEAjOzQpGv3yTmZVn8p5vPxkwusjOHwhXSrIDjAw/PoffsdHXGutjDxZMBdwviEd
VShtoVWN6L5SQZK/y01P4FXN+YpgAcBNUA7vovJO76iaPeCQR2vnj3R/rxq/vvug
sO7JA04QImdhyZ4qbZoLUHJ69wIDAQABo4GSMIGPMBoGA1UdEQQTMBGCCWxvY2Fs
aG9zdIcEfwAAATAMBgNVHRMBAf8EAjAAMA4GA1UdDwEB/wQEAwIFoDATBgNVHSUE
DDAKBggrBgEFBQcDATAdBgNVHQ4EFgQUUrU52T/pEly3nsAxjlpTVltClS0wHwYD
VR0jBBgwFoAUZsaT+usuVwXWs0SWZvbAykvJFP8wDQYJKoZIhvcNAQELBQADggEB
AA1wTY/oNMXbAONfawVKoz+biuEFiSwsy4XHKyZmhsFpOcjGlotWl14hjJp/zRBG
6B4PZwYT7D43/1C6wC3q5AOD+kjcrGik4Ef5WFggSZJUl43Ln51TNm5yhWhCT6nZ
0IVv2vKWRmoy3JMRG4hDfAT3Z+SaiwEZPpXnIvClXOgIl+DAuC+8h8CMRrJQ4mVN
aPCWTHi1eZAIiIcJ7Z55yWyWBmH7wx+y1YtYnDXH5ZeXSsm2EMJJtOeX/MPVStpT
cx+Oj/wMKAolQHbKLlO3Y2cBkuJ/cRM252X3QJfC+wk0T6n5MfF8MBlNj/z2fure
KwmzLhH+CCh6NMsSird2hjw=
-----END CERTIFICATE-----
)PEM";

static int pem_base64_value(char c) {
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;
    return -1;
}

static std::vector<uint8_t> decode_pem_certificate(const char *pem) {
    const char *begin_marker = "-----BEGIN CERTIFICATE-----";
    const char *end_marker = "-----END CERTIFICATE-----";
    const char *begin = strstr(pem, begin_marker);
    const char *end = strstr(pem, end_marker);
    std::vector<uint8_t> decoded;
    int quartet[4];
    int quartet_len = 0;

    if (!begin || !end || end <= begin)
        return decoded;
    begin = strchr(begin, '\n');
    if (!begin)
        return decoded;
    begin++;

    for (const char *p = begin; p < end; ++p) {
        if (*p == '\r' || *p == '\n' || *p == ' ' || *p == '\t')
            continue;
        quartet[quartet_len++] = (*p == '=') ? -2 : pem_base64_value(*p);
        if (quartet[quartet_len - 1] < 0 && quartet[quartet_len - 1] != -2)
            return std::vector<uint8_t>();
        if (quartet_len == 4) {
            uint32_t block = 0;
            if (quartet[0] < 0 || quartet[1] < 0)
                return std::vector<uint8_t>();
            block |= (uint32_t)quartet[0] << 18;
            block |= (uint32_t)quartet[1] << 12;
            if (quartet[2] >= 0)
                block |= (uint32_t)quartet[2] << 6;
            if (quartet[3] >= 0)
                block |= (uint32_t)quartet[3];
            decoded.push_back((uint8_t)((block >> 16) & 0xFF));
            if (quartet[2] != -2)
                decoded.push_back((uint8_t)((block >> 8) & 0xFF));
            if (quartet[3] != -2)
                decoded.push_back((uint8_t)(block & 0xFF));
            quartet_len = 0;
        }
    }

    if (quartet_len != 0)
        return std::vector<uint8_t>();
    return decoded;
}

static void append_der_len(std::vector<uint8_t> &out, size_t len) {
    if (len < 128) {
        out.push_back((uint8_t)len);
        return;
    }
    uint8_t tmp[sizeof(size_t)];
    size_t count = 0;
    while (len > 0) {
        tmp[count++] = (uint8_t)(len & 0xff);
        len >>= 8;
    }
    out.push_back((uint8_t)(0x80 | count));
    while (count > 0)
        out.push_back(tmp[--count]);
}

static std::vector<uint8_t> der_tlv(uint8_t tag, const std::vector<uint8_t> &value) {
    std::vector<uint8_t> out;
    out.push_back(tag);
    append_der_len(out, value.size());
    out.insert(out.end(), value.begin(), value.end());
    return out;
}

static std::vector<uint8_t> der_dns_name(const std::string &name) {
    std::vector<uint8_t> value(name.begin(), name.end());
    return der_tlv(0x82, value);
}

static std::vector<uint8_t> make_minimal_cert_with_dns_sans(size_t filler_count,
                                                            const std::string &target_name) {
    std::vector<uint8_t> names;
    for (size_t i = 0; i < filler_count; ++i) {
        std::string name = "filler" + std::to_string(i) + ".example.com";
        std::vector<uint8_t> entry = der_dns_name(name);
        names.insert(names.end(), entry.begin(), entry.end());
    }
    std::vector<uint8_t> target = der_dns_name(target_name);
    names.insert(names.end(), target.begin(), target.end());

    std::vector<uint8_t> san_sequence = der_tlv(0x30, names);
    std::vector<uint8_t> san_octet = der_tlv(0x04, san_sequence);
    std::vector<uint8_t> san_extension_value = {0x06, 0x03, 0x55, 0x1d, 0x11};
    san_extension_value.insert(san_extension_value.end(), san_octet.begin(), san_octet.end());
    std::vector<uint8_t> san_extension = der_tlv(0x30, san_extension_value);
    std::vector<uint8_t> extensions = der_tlv(0x30, san_extension);
    std::vector<uint8_t> explicit_extensions = der_tlv(0xa3, extensions);
    std::vector<uint8_t> tbs = der_tlv(0x30, explicit_extensions);
    return der_tlv(0x30, tbs);
}

static std::filesystem::path filesystem_path_from_utf8(std::string_view text) {
    return std::filesystem::path(
        std::u8string(reinterpret_cast<const char8_t *>(text.data()),
                      reinterpret_cast<const char8_t *>(text.data()) + text.size()));
}

static std::string filesystem_path_to_utf8(const std::filesystem::path &path) {
    const std::u8string encoded = path.u8string();
    return std::string(reinterpret_cast<const char *>(encoded.data()), encoded.size());
}

struct temp_text_file_t {
    std::string path;

    temp_text_file_t() = default;
    temp_text_file_t(const temp_text_file_t &) = delete;
    temp_text_file_t &operator=(const temp_text_file_t &) = delete;

    temp_text_file_t(temp_text_file_t &&other) noexcept : path(std::move(other.path)) {
        other.path.clear();
    }

    temp_text_file_t &operator=(temp_text_file_t &&other) noexcept {
        if (this == &other)
            return *this;
        std::error_code ec;
        if (!path.empty())
            std::filesystem::remove(filesystem_path_from_utf8(path), ec);
        path = std::move(other.path);
        other.path.clear();
        return *this;
    }

    ~temp_text_file_t() {
        std::error_code ec;
        if (!path.empty())
            std::filesystem::remove(filesystem_path_from_utf8(path), ec);
    }
};

static temp_text_file_t write_temp_text_file(const char *suffix, const char *content) {
    temp_text_file_t file;
    std::filesystem::path dir = std::filesystem::temp_directory_path();
    auto id = (unsigned long long)std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path native_path =
        dir / filesystem_path_from_utf8("zanna_tls_cert_" + std::to_string(id) + suffix);
    file.path = filesystem_path_to_utf8(native_path);
    std::ofstream out(native_path, std::ios::binary | std::ios::trunc);
    if (!out)
        file.path.clear();
    else
        out << content;
    return file;
}

// ---------------------------------------------------------------------------
// Helper: build a minimal TLS 1.3 Certificate message wrapping raw DER bytes.
//
// Structure per RFC 8446 §4.4.2:
//   1 byte:  context_len (0)
//   3 bytes: cert_list total length
//     3 bytes: cert_data_len
//     N bytes: DER
//     2 bytes: extensions_len (0)
// ---------------------------------------------------------------------------
static void build_tls_cert_msg(const uint8_t *der, size_t der_len, uint8_t *out, size_t *out_len) {
    size_t extensions_len = 0;
    size_t entry_len = 3 + der_len + 2; // cert_len_field + DER + ext_len
    size_t list_len = entry_len;

    size_t pos = 0;
    out[pos++] = 0x00; // context_len

    // cert_list_len (3 bytes, big-endian)
    out[pos++] = (uint8_t)((list_len >> 16) & 0xFF);
    out[pos++] = (uint8_t)((list_len >> 8) & 0xFF);
    out[pos++] = (uint8_t)(list_len & 0xFF);

    // cert_data_len (3 bytes, big-endian)
    out[pos++] = (uint8_t)((der_len >> 16) & 0xFF);
    out[pos++] = (uint8_t)((der_len >> 8) & 0xFF);
    out[pos++] = (uint8_t)(der_len & 0xFF);

    // DER bytes
    memcpy(out + pos, der, der_len);
    pos += der_len;

    // extensions_len (2 bytes, big-endian)
    out[pos++] = (uint8_t)((extensions_len >> 8) & 0xFF);
    out[pos++] = (uint8_t)(extensions_len & 0xFF);

    *out_len = pos;
}

static std::vector<uint8_t> build_tls_cert_msg_multi(
    const std::vector<std::vector<uint8_t>> &certs) {
    std::vector<uint8_t> entries;
    for (const std::vector<uint8_t> &cert : certs) {
        size_t cert_len = cert.size();
        assert(cert_len <= 0xFFFFFFu);
        entries.push_back((uint8_t)((cert_len >> 16) & 0xFF));
        entries.push_back((uint8_t)((cert_len >> 8) & 0xFF));
        entries.push_back((uint8_t)(cert_len & 0xFF));
        entries.insert(entries.end(), cert.begin(), cert.end());
        entries.push_back(0x00);
        entries.push_back(0x00);
    }

    assert(entries.size() <= 0xFFFFFFu);
    std::vector<uint8_t> msg;
    msg.push_back(0x00);
    msg.push_back((uint8_t)((entries.size() >> 16) & 0xFF));
    msg.push_back((uint8_t)((entries.size() >> 8) & 0xFF));
    msg.push_back((uint8_t)(entries.size() & 0xFF));
    msg.insert(msg.end(), entries.begin(), entries.end());
    return msg;
}

// ---------------------------------------------------------------------------
// tls_match_hostname tests
// ---------------------------------------------------------------------------

static void test_hostname_match_exact(void) {
    assert(tls_match_hostname("example.com", "example.com") == 1);
    assert(tls_match_hostname("EXAMPLE.COM", "example.com") == 1); // case-insensitive
    assert(tls_match_hostname("example.com", "EXAMPLE.COM") == 1);
    assert(tls_match_hostname("example.com", "other.com") == 0);
    assert(tls_match_hostname("a.example.com", "example.com") == 0);
    assert(tls_match_hostname("example.com", "a.example.com") == 0);
    assert(tls_match_hostname("", "") == 0);
    assert(tls_match_hostname("example.com", "") == 0);
    assert(tls_match_hostname("", "example.com") == 0);
    printf("  PASS: test_hostname_match_exact\n");
}

static void test_hostname_match_wildcard(void) {
    // Standard single-label wildcard
    assert(tls_match_hostname("*.example.com", "foo.example.com") == 1);
    assert(tls_match_hostname("*.example.com", "bar.example.com") == 1);
    assert(tls_match_hostname("*.example.com", "www.example.com") == 1);

    // Wildcard does NOT cover two labels deep
    assert(tls_match_hostname("*.example.com", "foo.bar.example.com") == 0);

    // Wildcard does NOT match the base domain itself (no label for *)
    assert(tls_match_hostname("*.example.com", "example.com") == 0);

    // Wildcard only in first label
    assert(tls_match_hostname("foo.*.com", "foo.example.com") == 0);
    assert(tls_match_hostname("*.com", "example.com") == 0);
    assert(tls_match_hostname("*.co.uk", "example.co.uk") == 0);

    // Case insensitivity with wildcards
    assert(tls_match_hostname("*.EXAMPLE.COM", "foo.example.com") == 1);
    assert(tls_match_hostname("*.example.com", "FOO.EXAMPLE.COM") == 1);

    printf("  PASS: test_hostname_match_wildcard\n");
}

static void test_hostname_match_no_wildcard_mismatch(void) {
    // Pattern with no wildcard must match exactly
    assert(tls_match_hostname("www.example.com", "example.com") == 0);
    assert(tls_match_hostname("www.example.com", "www.example.com") == 1);
    assert(tls_match_hostname("www.example.com", "ftp.example.com") == 0);

    // Partial matches should not succeed
    assert(tls_match_hostname("example.co", "example.com") == 0);
    assert(tls_match_hostname("example.comm", "example.com") == 0);

    printf("  PASS: test_hostname_match_no_wildcard_mismatch\n");
}

// ---------------------------------------------------------------------------
// tls_extract_san_names tests
// ---------------------------------------------------------------------------

static void test_san_extraction_three_names(void) {
    char san[8][256];
    int count = tls_extract_san_names(TEST_CERT_WITH_SAN, TEST_CERT_WITH_SAN_LEN, san, 8);

    // Cert has 3 SAN DNS names: example.com, *.example.com, www.example.com
    assert(count == 3);
    assert(strcmp(san[0], "example.com") == 0);
    assert(strcmp(san[1], "*.example.com") == 0);
    assert(strcmp(san[2], "www.example.com") == 0);

    printf("  PASS: test_san_extraction_three_names\n");
}

static void test_san_extraction_no_san(void) {
    // CN-only cert has no SubjectAltName extension
    char san[8][256];
    int count = tls_extract_san_names(TEST_CERT_CN_ONLY, TEST_CERT_CN_ONLY_LEN, san, 8);
    assert(count == 0);
    printf("  PASS: test_san_extraction_no_san\n");
}

static void test_san_extension_presence(void) {
    assert(tls_cert_has_san_extension(TEST_CERT_WITH_SAN, TEST_CERT_WITH_SAN_LEN) == 1);
    assert(tls_cert_has_san_extension(TEST_CERT_CN_ONLY, TEST_CERT_CN_ONLY_LEN) == 0);
    printf("  PASS: test_san_extension_presence\n");
}

static void test_san_extraction_cap_respected(void) {
    // Request fewer slots than the cert has — should not overflow
    char san[2][256];
    int count = tls_extract_san_names(TEST_CERT_WITH_SAN, TEST_CERT_WITH_SAN_LEN, san, 2);
    assert(count == 2);
    assert(strcmp(san[0], "example.com") == 0);
    assert(strcmp(san[1], "*.example.com") == 0);
    printf("  PASS: test_san_extraction_cap_respected\n");
}

static void test_san_extraction_empty_input(void) {
    char san[4][256];
    // Zero-length input should return 0
    int count = tls_extract_san_names(TEST_CERT_WITH_SAN, 0, san, 4);
    assert(count == 0);
    printf("  PASS: test_san_extraction_empty_input\n");
}

// ---------------------------------------------------------------------------
// tls_extract_cn tests
// ---------------------------------------------------------------------------

static void test_cn_extraction_cn_only_cert(void) {
    char cn[256] = {0};
    int rc = tls_extract_cn(TEST_CERT_CN_ONLY, TEST_CERT_CN_ONLY_LEN, cn);
    assert(rc == 1);
    assert(strcmp(cn, "cn-only.example.com") == 0);
    printf("  PASS: test_cn_extraction_cn_only_cert\n");
}

static void test_cn_extraction_san_cert(void) {
    // The SAN cert also has CN=example.com in its Subject
    char cn[256] = {0};
    int rc = tls_extract_cn(TEST_CERT_WITH_SAN, TEST_CERT_WITH_SAN_LEN, cn);
    assert(rc == 1);
    assert(strcmp(cn, "example.com") == 0);
    printf("  PASS: test_cn_extraction_san_cert\n");
}

static void test_cn_extraction_empty_input(void) {
    char cn[256] = {0};
    int rc = tls_extract_cn(TEST_CERT_WITH_SAN, 0, cn);
    assert(rc == 0);
    printf("  PASS: test_cn_extraction_empty_input\n");
}

// ---------------------------------------------------------------------------
// tls_parse_certificate_msg tests
// ---------------------------------------------------------------------------

// tls_parse_certificate_msg is exposed for unit testing through rt_tls_internal.h.

// ---------------------------------------------------------------------------
// End-to-end hostname verification via rt_tls_session
// These tests exercise the full path:
//   tls_parse_certificate_msg → store DER in session
//   tls_verify_hostname       → SAN/CN extraction + matching
//
// We manually populate session->server_cert_der and call a thin wrapper.
// Since rt_tls_session is opaque (only typedef'd in the header), we need
// a way to reach the DER fields.  The tls_extract_san_names / tls_extract_cn
// / tls_match_hostname functions are already exposed, so we test verify_hostname
// by building a minimal Certificate message and feeding it through.
// ---------------------------------------------------------------------------

// Helper: build a TLS Certificate message and call tls_parse_certificate_msg.
static void test_certificate_msg_parse_san_cert(void) {
    // Build a TLS 1.3 Certificate message around TEST_CERT_WITH_SAN
    uint8_t msg[4096];
    size_t msg_len = 0;
    build_tls_cert_msg(TEST_CERT_WITH_SAN, TEST_CERT_WITH_SAN_LEN, msg, &msg_len);

    // The message should be: 1 + 3 + 3 + der_len + 2 bytes
    size_t expected_len = 1 + 3 + 3 + TEST_CERT_WITH_SAN_LEN + 2;
    assert(msg_len == expected_len);

    // context_len must be 0
    assert(msg[0] == 0x00);

    // cert_list_len big-endian = entry_len = 3 + der_len + 2
    size_t entry_len = 3 + TEST_CERT_WITH_SAN_LEN + 2;
    size_t list_len_parsed = ((size_t)msg[1] << 16) | ((size_t)msg[2] << 8) | (size_t)msg[3];
    assert(list_len_parsed == entry_len);

    // cert_data_len big-endian = der_len
    size_t cert_len_parsed = ((size_t)msg[4] << 16) | ((size_t)msg[5] << 8) | (size_t)msg[6];
    assert(cert_len_parsed == TEST_CERT_WITH_SAN_LEN);

    // DER bytes should match exactly
    assert(memcmp(msg + 7, TEST_CERT_WITH_SAN, TEST_CERT_WITH_SAN_LEN) == 0);

    rt_tls_session_t session;
    memset(&session, 0, sizeof(session));
    assert(tls_parse_certificate_msg(&session, msg, msg_len) == RT_TLS_OK);
    assert(session.server_cert_der_len == TEST_CERT_WITH_SAN_LEN);
    assert(session.server_cert_count == 1);
    assert(memcmp(session.server_cert_der, TEST_CERT_WITH_SAN, TEST_CERT_WITH_SAN_LEN) == 0);
    free(session.server_cert_list);

    printf("  PASS: test_certificate_msg_parse_san_cert\n");
}

static void test_certificate_msg_rejects_trailing_data(void) {
    uint8_t msg[4096];
    size_t msg_len = 0;
    build_tls_cert_msg(TEST_CERT_WITH_SAN, TEST_CERT_WITH_SAN_LEN, msg, &msg_len);
    msg[msg_len++] = 0x00;

    rt_tls_session_t session;
    memset(&session, 0, sizeof(session));
    assert(tls_parse_certificate_msg(&session, msg, msg_len) == RT_TLS_ERROR_HANDSHAKE);
    assert(session.server_cert_list == nullptr);

    printf("  PASS: test_certificate_msg_rejects_trailing_data\n");
}

// ---------------------------------------------------------------------------
// Hostname verification end-to-end tests
// These test the SAN-first, CN-fallback matching logic combined with real DER.
// ---------------------------------------------------------------------------

static void test_hostname_verified_exact_san(void) {
    // "example.com" should match SAN entry "example.com"
    char san[8][256];
    int count = tls_extract_san_names(TEST_CERT_WITH_SAN, TEST_CERT_WITH_SAN_LEN, san, 8);
    assert(count == 3);

    int matched = 0;
    for (int i = 0; i < count; i++) {
        if (tls_match_hostname(san[i], "example.com")) {
            matched = 1;
            break;
        }
    }
    assert(matched == 1);
    printf("  PASS: test_hostname_verified_exact_san\n");
}

static void test_hostname_verified_wildcard_san(void) {
    // "foo.example.com" should match SAN entry "*.example.com"
    char san[8][256];
    int count = tls_extract_san_names(TEST_CERT_WITH_SAN, TEST_CERT_WITH_SAN_LEN, san, 8);
    assert(count == 3);

    int matched = 0;
    for (int i = 0; i < count; i++) {
        if (tls_match_hostname(san[i], "foo.example.com")) {
            matched = 1;
            break;
        }
    }
    assert(matched == 1);
    printf("  PASS: test_hostname_verified_wildcard_san\n");
}

static void test_hostname_verified_mismatch_san(void) {
    // "other.com" should NOT match any SAN
    char san[8][256];
    int count = tls_extract_san_names(TEST_CERT_WITH_SAN, TEST_CERT_WITH_SAN_LEN, san, 8);
    assert(count == 3);

    int matched = 0;
    for (int i = 0; i < count; i++) {
        if (tls_match_hostname(san[i], "other.com")) {
            matched = 1;
            break;
        }
    }
    assert(matched == 0);
    printf("  PASS: test_hostname_verified_mismatch_san\n");
}

static void test_hostname_verified_cn_fallback(void) {
    // CN-only cert: no SAN, CN = "cn-only.example.com"
    char san[8][256];
    int san_count = tls_extract_san_names(TEST_CERT_CN_ONLY, TEST_CERT_CN_ONLY_LEN, san, 8);
    assert(san_count == 0); // no SAN — must fall back to CN

    char cn[256] = {0};
    int cn_found = tls_extract_cn(TEST_CERT_CN_ONLY, TEST_CERT_CN_ONLY_LEN, cn);
    assert(cn_found == 1);

    // "cn-only.example.com" should match CN
    assert(tls_match_hostname(cn, "cn-only.example.com") == 1);

    // "other.com" should not
    assert(tls_match_hostname(cn, "other.com") == 0);

    printf("  PASS: test_hostname_verified_cn_fallback\n");
}

static void test_hostname_rejects_cn_when_san_has_no_dns(void) {
    uint8_t cert[TEST_CERT_WITH_SAN_LEN];
    memcpy(cert, TEST_CERT_WITH_SAN, sizeof(cert));
    int replaced = 0;
    for (size_t i = 0; i + 2 < sizeof(cert); i++) {
        if (cert[i] == 0x82 && ((cert[i + 1] == 0x0b && i + 13 < sizeof(cert) &&
                                 memcmp(cert + i + 2, "example.com", 11) == 0) ||
                                (cert[i + 1] == 0x0d && i + 15 < sizeof(cert) &&
                                 memcmp(cert + i + 2, "*.example.com", 13) == 0) ||
                                (cert[i + 1] == 0x0f && i + 17 < sizeof(cert) &&
                                 memcmp(cert + i + 2, "www.example.com", 15) == 0))) {
            cert[i] = 0x86; // uniformResourceIdentifier GeneralName, not dNSName
            replaced++;
        }
    }
    assert(replaced == 3);
    assert(tls_cert_has_san_extension(cert, sizeof(cert)) == 1);
    char san[4][256];
    int san_count = tls_extract_san_names(cert, sizeof(cert), san, 4);
    assert(san_count == 0);
    (void)san_count;

    rt_tls_session_t session;
    memset(&session, 0, sizeof(session));
    memcpy(session.server_cert_der, cert, sizeof(cert));
    session.server_cert_der_len = sizeof(cert);
    strncpy(session.hostname, "example.com", sizeof(session.hostname) - 1);

    assert(tls_verify_hostname(&session) == RT_TLS_ERROR_HANDSHAKE);
    printf("  PASS: test_hostname_rejects_cn_when_san_has_no_dns\n");
}

static void test_hostname_verified_wildcard_san_two_levels(void) {
    // "foo.bar.example.com" is two labels deep — *.example.com should NOT match
    char san[8][256];
    int count = tls_extract_san_names(TEST_CERT_WITH_SAN, TEST_CERT_WITH_SAN_LEN, san, 8);
    assert(count == 3);

    int matched = 0;
    for (int i = 0; i < count; i++) {
        if (tls_match_hostname(san[i], "foo.bar.example.com")) {
            matched = 1;
            break;
        }
    }
    assert(matched == 0);
    printf("  PASS: test_hostname_verified_wildcard_san_two_levels\n");
}

static void test_hostname_verified_www_san(void) {
    // "www.example.com" appears as an explicit SAN entry
    char san[8][256];
    int count = tls_extract_san_names(TEST_CERT_WITH_SAN, TEST_CERT_WITH_SAN_LEN, san, 8);
    assert(count == 3);

    int matched = 0;
    for (int i = 0; i < count; i++) {
        if (tls_match_hostname(san[i], "www.example.com")) {
            matched = 1;
            break;
        }
    }
    assert(matched == 1);
    printf("  PASS: test_hostname_verified_www_san\n");
}

static void test_hostname_verification_scans_past_san_extraction_cap(void) {
    const std::string target = "target.example.com";
    std::vector<uint8_t> der = make_minimal_cert_with_dns_sans(70, target);
    const int san_cap = 64;

    char san[san_cap][256];
    int count = tls_extract_san_names(der.data(), der.size(), san, san_cap);
    assert(count == san_cap);
    for (int i = 0; i < count; i++)
        assert(strcmp(san[i], target.c_str()) != 0);

    rt_tls_session_t session;
    memset(&session, 0, sizeof(session));
    assert(der.size() <= sizeof(session.server_cert_der));
    memcpy(session.server_cert_der, der.data(), der.size());
    session.server_cert_der_len = der.size();
    strncpy(session.hostname, target.c_str(), sizeof(session.hostname) - 1);

    assert(tls_verify_hostname(&session) == RT_TLS_OK);
    printf("  PASS: test_hostname_verification_scans_past_san_extraction_cap\n");
}

static void test_chain_verification_custom_bundle_rsa(void) {
    std::vector<uint8_t> leaf_der = decode_pem_certificate(TEST_RSA_LEAF_CERT_PEM);
    temp_text_file_t ca_file =
        write_temp_text_file("_rsa_\xe6\xa0\xb9_ca.pem", TEST_RSA_ROOT_CERT_PEM);
    rt_tls_session_t session;

    assert(!leaf_der.empty());
    assert(!ca_file.path.empty());
    assert(leaf_der.size() <= sizeof(session.server_cert_der));

    memset(&session, 0, sizeof(session));
    memcpy(session.server_cert_der, leaf_der.data(), leaf_der.size());
    session.server_cert_der_len = leaf_der.size();
    strncpy(session.ca_file, ca_file.path.c_str(), sizeof(session.ca_file) - 1);

    int verify_rc = tls_verify_chain(&session);
    if (verify_rc != RT_TLS_OK) {
        fprintf(stderr,
                "tls_verify_chain failed: rc=%d error=%s\n",
                verify_rc,
                session.error ? session.error : "(null)");
    }
    assert(verify_rc == RT_TLS_OK);
    printf("  PASS: test_chain_verification_custom_bundle_rsa\n");
}

static void test_chain_verification_rejects_too_many_intermediates(void) {
    std::vector<uint8_t> leaf_der = decode_pem_certificate(TEST_RSA_LEAF_CERT_PEM);
    std::vector<std::vector<uint8_t>> certs;
    rt_tls_session_t session;

    assert(!leaf_der.empty());
    certs.push_back(leaf_der);
    for (int i = 0; i < 17; ++i)
        certs.push_back(leaf_der);

    std::vector<uint8_t> msg = build_tls_cert_msg_multi(certs);
    memset(&session, 0, sizeof(session));
    assert(tls_parse_certificate_msg(&session, msg.data(), msg.size()) == RT_TLS_OK);

    int verify_rc = tls_verify_chain(&session);
    assert(verify_rc == RT_TLS_ERROR_HANDSHAKE);
    assert(session.error &&
           strcmp(session.error, "TLS: certificate chain has too many intermediates") == 0);
    free(session.server_cert_list);
    printf("  PASS: test_chain_verification_rejects_too_many_intermediates\n");
}

#if ZANNA_HOST_WINDOWS
static void test_windows_chain_verification_rejects_bad_ca_inputs(void) {
    std::vector<uint8_t> leaf_der = decode_pem_certificate(TEST_RSA_LEAF_CERT_PEM);
    const std::string truncated_bundle =
        std::string(TEST_RSA_ROOT_CERT_PEM) + "\n-----BEGIN CERTIFICATE-----\nAAAA\n";
    temp_text_file_t malformed =
        write_temp_text_file("_malformed_ca.pem", truncated_bundle.c_str());
    temp_text_file_t oversized = write_temp_text_file("_oversized_ca.pem", "");
    rt_tls_session_t session;

    assert(!leaf_der.empty());
    assert(!malformed.path.empty());
    assert(!oversized.path.empty());

    memset(&session, 0, sizeof(session));
    memcpy(session.server_cert_der, leaf_der.data(), leaf_der.size());
    session.server_cert_der_len = leaf_der.size();
    strncpy(session.ca_file, malformed.path.c_str(), sizeof(session.ca_file) - 1);
    assert(tls_verify_chain(&session) == RT_TLS_ERROR_HANDSHAKE);
    assert(session.error && strstr(session.error, "could not load custom CA file"));

    {
        std::ofstream out(filesystem_path_from_utf8(oversized.path),
                          std::ios::binary | std::ios::trunc);
        assert(out);
        out.seekp(16 * 1024 * 1024);
        out.put('x');
        assert(out.good());
    }
    memset(&session, 0, sizeof(session));
    memcpy(session.server_cert_der, leaf_der.data(), leaf_der.size());
    session.server_cert_der_len = leaf_der.size();
    strncpy(session.ca_file, oversized.path.c_str(), sizeof(session.ca_file) - 1);
    assert(tls_verify_chain(&session) == RT_TLS_ERROR_HANDSHAKE);
    assert(session.error && strstr(session.error, "could not load custom CA file"));

    assert(tls_verify_chain(nullptr) == RT_TLS_ERROR_HANDSHAKE);
    printf("  PASS: test_windows_chain_verification_rejects_bad_ca_inputs\n");
}
#endif

static void test_certificate_verify_requires_exact_signature_length(void) {
    rt_tls_session_t session;
    const uint8_t trailing_data[] = {0x04, 0x03, 0x00, 0x00, 0xaa};
    const uint8_t truncated_signature[] = {0x04, 0x03, 0x00, 0x02, 0xaa};

    memset(&session, 0, sizeof(session));
    assert(tls_verify_cert_verify(&session, trailing_data, sizeof(trailing_data)) ==
           RT_TLS_ERROR_HANDSHAKE);
    assert(session.error && strstr(session.error, "length mismatch") != nullptr);

    memset(&session, 0, sizeof(session));
    assert(tls_verify_cert_verify(&session, truncated_signature, sizeof(truncated_signature)) ==
           RT_TLS_ERROR_HANDSHAKE);
    assert(session.error && strstr(session.error, "length overflows") != nullptr);

    assert(tls_verify_cert_verify(nullptr, trailing_data, sizeof(trailing_data)) ==
           RT_TLS_ERROR_HANDSHAKE);
    printf("  PASS: test_certificate_verify_requires_exact_signature_length\n");
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(void) {
    printf("=== RTTlsCertTests ===\n");

    printf("-- tls_match_hostname --\n");
    test_hostname_match_exact();
    test_hostname_match_wildcard();
    test_hostname_match_no_wildcard_mismatch();

    printf("-- tls_extract_san_names --\n");
    test_san_extraction_three_names();
    test_san_extraction_no_san();
    test_san_extension_presence();
    test_san_extraction_cap_respected();
    test_san_extraction_empty_input();

    printf("-- tls_extract_cn --\n");
    test_cn_extraction_cn_only_cert();
    test_cn_extraction_san_cert();
    test_cn_extraction_empty_input();

    printf("-- TLS Certificate message structure --\n");
    test_certificate_msg_parse_san_cert();
    test_certificate_msg_rejects_trailing_data();

    printf("-- Hostname verification end-to-end --\n");
    test_hostname_verified_exact_san();
    test_hostname_verified_wildcard_san();
    test_hostname_verified_mismatch_san();
    test_hostname_verified_cn_fallback();
    test_hostname_rejects_cn_when_san_has_no_dns();
    test_hostname_verified_wildcard_san_two_levels();
    test_hostname_verified_www_san();
    test_hostname_verification_scans_past_san_extraction_cap();

    printf("-- Native RSA chain verification --\n");
    test_chain_verification_custom_bundle_rsa();
    test_chain_verification_rejects_too_many_intermediates();
#if ZANNA_HOST_WINDOWS
    test_windows_chain_verification_rejects_bad_ca_inputs();
#endif

    printf("-- CertificateVerify message framing --\n");
    test_certificate_verify_requires_exact_signature_length();

    printf("=== All RTTlsCertTests passed ===\n");
    return 0;
}
