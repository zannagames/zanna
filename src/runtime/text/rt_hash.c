//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE in the project root for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/text/rt_hash.c
// Purpose: Implements the Zanna.Crypto.Hash runtime — MD5, SHA-1, SHA-256,
//          matching HMAC variants, CRC32, keyed SipHash-2-4, and fixed-time
//          equality helpers for digest/MAC comparisons.
//
// Key invariants:
//   - MD5 and SHA-1 are included for legacy compatibility only; they are
//     cryptographically broken and must not be used for security.
//   - All public hash functions produce lowercase hex-encoded output.
//   - CRC32 lookup-table initialization uses atomic once-state and is safe for
//     concurrent first use.
//   - Input may be a string or rt_bytes; both paths produce identical digests.
//   - Digest and MAC working state is stack-allocated; only returned runtime
//     strings allocate result storage.
//   - The HMAC helpers (hmac_hash_*) dispatch on `hmac_hash_alg_t` so the
//     PBKDF2 and scrypt KDF code can switch hash algorithms without each
//     KDF having to know the per-hash context layout.
//   - Constant-time equality routes all early-exit paths through the same
//     length-mismatch branch so the timing channel reveals only "lengths
//     differ" — not "first differing byte position".
//
// Ownership/Lifetime:
//   - Returned hex strings are fresh rt_string allocations owned by the caller.
//   - Input strings and bytes buffers are borrowed for the duration of the call.
//
// Links: src/runtime/text/rt_hash.h (public API),
//        src/runtime/text/rt_codec.h (hex encoding used to format output),
//        src/runtime/text/rt_keyderive.c (consumer of HMAC dispatch)
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_hash.c
 * @brief Implements digest, HMAC, CRC32, SipHash, and fixed-time comparison APIs.
 * @details The module provides legacy MD5 and SHA-1, SHA-256, matching HMAC
 *          primitives, noncryptographic CRC32, process-keyed SipHash-2-4, and
 *          length-aware equality. Approved-mode policy gates legacy services,
 *          and sensitive keyed working state is scrubbed after use.
 */

#include "rt_hash.h"

#include "rt_bytes.h"
#include "rt_codec.h"
#include "rt_crc32.h"
#include "rt_crypto_module.h"
#include "rt_internal.h"
#include "rt_string.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/// @brief Erase a byte range through volatile stores.
/// @details Volatile access prevents the compiler from eliminating the writes
///          as dead stores when clearing key material or intermediate state.
/// @param ptr Writable start of the range to clear.
/// @param len Number of bytes to overwrite with zero.
static void hash_secure_zero(void *ptr, size_t len) {
    volatile uint8_t *p = (volatile uint8_t *)ptr;
    while (len-- > 0)
        *p++ = 0;
}

/// @brief Check whether a crypto service is allowed by the active module policy.
/// @details In APPROVED mode the policy gate returns 0 for legacy services
///          (MD5, SHA-1, CRC32, and the legacy SipHash-named fast-hash gate).
///          This helper traps with @p message
///          and returns 0 so public entry points can stop when trap recovery
///          returns control to the runtime.
/// @param service Crypto-module service identifier being requested.
/// @param message Trap message to report when the service is disabled.
/// @return Non-zero when the service may execute; zero after reporting a
///         policy violation.
static int hash_require_service(rt_crypto_module_service_t service, const char *message) {
    if (!rt_crypto_module_service_allowed(service)) {
        rt_trap(message);
        return 0;
    }
    return 1;
}

/// @brief Extract a raw byte pointer and byte count from an rt_string.
/// @details Returns an empty buffer for real zero-length strings, but traps on
///          null or invalid string objects so they are not silently hashed as
///          empty. Failure leaves @p ok false and reports zero length.
/// @param str Borrowed runtime string to inspect.
/// @param len Non-null output receiving the byte length.
/// @param ok Optional output set to one only for a valid string.
/// @return Borrowed immutable string bytes, or a stable empty buffer on failure.
static const uint8_t *hash_string_bytes(rt_string str, size_t *len, int *ok) {
    if (ok)
        *ok = 0;
    if (!len) {
        rt_trap("Hash: internal length pointer is null");
        return (const uint8_t *)"";
    }
    if (!str) {
        rt_trap("Hash: string must not be null");
        *len = 0;
        return (const uint8_t *)"";
    }
    if (!rt_string_is_handle((const void *)str)) {
        rt_trap("Hash: invalid string handle");
        *len = 0;
        return (const uint8_t *)"";
    }
    int64_t len64 = rt_str_len(str);
    if (len64 < 0) {
        rt_trap("Hash: invalid string length");
        *len = 0;
        return (const uint8_t *)"";
    }
    if (len64 == 0) {
        *len = 0;
        if (ok)
            *ok = 1;
        return (const uint8_t *)"";
    }

    const char *cstr = rt_string_cstr(str);
    if (!cstr) {
        rt_trap("Hash: string data is null");
        *len = 0;
        return (const uint8_t *)"";
    }

    *len = (size_t)len64;
    if (ok)
        *ok = 1;
    return (const uint8_t *)cstr;
}

/// @brief Borrow the immutable payload and length from a Bytes object.
/// @details NULL is treated as an empty byte array for compatibility with the
///          public Bytes hash APIs. Non-empty Bytes objects must expose a
///          non-NULL backing pointer. Invalid lengths or missing backing data
///          trap with @p context and return an empty buffer if trap recovery is
///          active.
/// @param bytes Candidate Bytes object, or NULL for empty input.
/// @param len Receives the byte length on success, or zero on failure.
/// @param context Public API name used in trap messages.
/// @param ok Optional output set to one only for valid input.
/// @return Borrowed immutable byte pointer valid for the duration of the call.
static const uint8_t *hash_bytes_data(void *bytes, size_t *len, const char *context, int *ok) {
    const char *api = context ? context : "Hash.Bytes";
    if (ok)
        *ok = 0;
    if (!len) {
        rt_trap("Hash.Bytes: internal length pointer is null");
        return (const uint8_t *)"";
    }

    if (bytes && !rt_bytes_is_bytes(bytes)) {
        rt_trap(api);
        *len = 0;
        return (const uint8_t *)"";
    }

    int64_t len64 = bytes ? rt_bytes_len(bytes) : 0;
    if (len64 < 0 || (uint64_t)len64 > (uint64_t)SIZE_MAX) {
        rt_trap(api);
        *len = 0;
        return (const uint8_t *)"";
    }

    *len = (size_t)len64;
    if (*len == 0) {
        if (ok)
            *ok = 1;
        return (const uint8_t *)"";
    }

    const uint8_t *data = rt_bytes_data_const(bytes);
    if (!data) {
        rt_trap(api);
        *len = 0;
        return (const uint8_t *)"";
    }
    if (ok)
        *ok = 1;
    return data;
}

//=============================================================================
// MD5 Implementation (RFC 1321)
//=============================================================================

/// Incremental MD5 state following RFC 1321.
typedef struct {
    uint32_t state[4]; ///< Four chaining words.
    uint64_t bit_count; ///< Number of message bits consumed.
    uint8_t buffer[64]; ///< Partial 512-bit input block.
} MD5_CTX;

#define MD5_F(x, y, z) (((x) & (y)) | ((~x) & (z)))
#define MD5_G(x, y, z) (((x) & (z)) | ((y) & (~z)))
#define MD5_H(x, y, z) ((x) ^ (y) ^ (z))
#define MD5_I(x, y, z) ((y) ^ ((x) | (~z)))

#define MD5_ROTATE_LEFT(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

#define MD5_FF(a, b, c, d, x, s, ac)                                                               \
    {                                                                                              \
        (a) += MD5_F((b), (c), (d)) + (x) + (uint32_t)(ac);                                        \
        (a) = MD5_ROTATE_LEFT((a), (s));                                                           \
        (a) += (b);                                                                                \
    }
#define MD5_GG(a, b, c, d, x, s, ac)                                                               \
    {                                                                                              \
        (a) += MD5_G((b), (c), (d)) + (x) + (uint32_t)(ac);                                        \
        (a) = MD5_ROTATE_LEFT((a), (s));                                                           \
        (a) += (b);                                                                                \
    }
#define MD5_HH(a, b, c, d, x, s, ac)                                                               \
    {                                                                                              \
        (a) += MD5_H((b), (c), (d)) + (x) + (uint32_t)(ac);                                        \
        (a) = MD5_ROTATE_LEFT((a), (s));                                                           \
        (a) += (b);                                                                                \
    }
#define MD5_II(a, b, c, d, x, s, ac)                                                               \
    {                                                                                              \
        (a) += MD5_I((b), (c), (d)) + (x) + (uint32_t)(ac);                                        \
        (a) = MD5_ROTATE_LEFT((a), (s));                                                           \
        (a) += (b);                                                                                \
    }

// ===========================================================================
// Three hash families implemented for the user-facing `Hash.MD5/SHA1/SHA256`
// APIs: MD5 (RFC 1321), SHA-1 (FIPS 180-4), SHA-256 (FIPS 180-4). Each
// family follows the same shape: `*_init` zeros the state, `*_update`
// streams bytes through, `*_final` emits the digest. None of these
// are suitable for new cryptographic uses (use `rt_crypto` instead);
// they're here for legacy / interoperability reasons (ETags, file
// integrity, etc.).
// ===========================================================================

/// @brief Apply one MD5 compression round to a 64-byte block (RFC 1321 §3.4).
///
/// The four round functions (F, G, H, I) plus per-round constants
/// are unrolled into 64 statements at compile time — this is the
/// canonical "textbook" MD5 implementation.
/// @param state Four chaining words updated in place.
/// @param block Complete 64-byte message block.
static void md5_transform(uint32_t state[4], const uint8_t block[64]) {
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t x[16];

    for (int i = 0; i < 16; i++) {
        x[i] = ((uint32_t)block[i * 4]) | ((uint32_t)block[i * 4 + 1] << 8) |
               ((uint32_t)block[i * 4 + 2] << 16) | ((uint32_t)block[i * 4 + 3] << 24);
    }

    // Round 1
    MD5_FF(a, b, c, d, x[0], 7, 0xd76aa478);
    MD5_FF(d, a, b, c, x[1], 12, 0xe8c7b756);
    MD5_FF(c, d, a, b, x[2], 17, 0x242070db);
    MD5_FF(b, c, d, a, x[3], 22, 0xc1bdceee);
    MD5_FF(a, b, c, d, x[4], 7, 0xf57c0faf);
    MD5_FF(d, a, b, c, x[5], 12, 0x4787c62a);
    MD5_FF(c, d, a, b, x[6], 17, 0xa8304613);
    MD5_FF(b, c, d, a, x[7], 22, 0xfd469501);
    MD5_FF(a, b, c, d, x[8], 7, 0x698098d8);
    MD5_FF(d, a, b, c, x[9], 12, 0x8b44f7af);
    MD5_FF(c, d, a, b, x[10], 17, 0xffff5bb1);
    MD5_FF(b, c, d, a, x[11], 22, 0x895cd7be);
    MD5_FF(a, b, c, d, x[12], 7, 0x6b901122);
    MD5_FF(d, a, b, c, x[13], 12, 0xfd987193);
    MD5_FF(c, d, a, b, x[14], 17, 0xa679438e);
    MD5_FF(b, c, d, a, x[15], 22, 0x49b40821);

    // Round 2
    MD5_GG(a, b, c, d, x[1], 5, 0xf61e2562);
    MD5_GG(d, a, b, c, x[6], 9, 0xc040b340);
    MD5_GG(c, d, a, b, x[11], 14, 0x265e5a51);
    MD5_GG(b, c, d, a, x[0], 20, 0xe9b6c7aa);
    MD5_GG(a, b, c, d, x[5], 5, 0xd62f105d);
    MD5_GG(d, a, b, c, x[10], 9, 0x02441453);
    MD5_GG(c, d, a, b, x[15], 14, 0xd8a1e681);
    MD5_GG(b, c, d, a, x[4], 20, 0xe7d3fbc8);
    MD5_GG(a, b, c, d, x[9], 5, 0x21e1cde6);
    MD5_GG(d, a, b, c, x[14], 9, 0xc33707d6);
    MD5_GG(c, d, a, b, x[3], 14, 0xf4d50d87);
    MD5_GG(b, c, d, a, x[8], 20, 0x455a14ed);
    MD5_GG(a, b, c, d, x[13], 5, 0xa9e3e905);
    MD5_GG(d, a, b, c, x[2], 9, 0xfcefa3f8);
    MD5_GG(c, d, a, b, x[7], 14, 0x676f02d9);
    MD5_GG(b, c, d, a, x[12], 20, 0x8d2a4c8a);

    // Round 3
    MD5_HH(a, b, c, d, x[5], 4, 0xfffa3942);
    MD5_HH(d, a, b, c, x[8], 11, 0x8771f681);
    MD5_HH(c, d, a, b, x[11], 16, 0x6d9d6122);
    MD5_HH(b, c, d, a, x[14], 23, 0xfde5380c);
    MD5_HH(a, b, c, d, x[1], 4, 0xa4beea44);
    MD5_HH(d, a, b, c, x[4], 11, 0x4bdecfa9);
    MD5_HH(c, d, a, b, x[7], 16, 0xf6bb4b60);
    MD5_HH(b, c, d, a, x[10], 23, 0xbebfbc70);
    MD5_HH(a, b, c, d, x[13], 4, 0x289b7ec6);
    MD5_HH(d, a, b, c, x[0], 11, 0xeaa127fa);
    MD5_HH(c, d, a, b, x[3], 16, 0xd4ef3085);
    MD5_HH(b, c, d, a, x[6], 23, 0x04881d05);
    MD5_HH(a, b, c, d, x[9], 4, 0xd9d4d039);
    MD5_HH(d, a, b, c, x[12], 11, 0xe6db99e5);
    MD5_HH(c, d, a, b, x[15], 16, 0x1fa27cf8);
    MD5_HH(b, c, d, a, x[2], 23, 0xc4ac5665);

    // Round 4
    MD5_II(a, b, c, d, x[0], 6, 0xf4292244);
    MD5_II(d, a, b, c, x[7], 10, 0x432aff97);
    MD5_II(c, d, a, b, x[14], 15, 0xab9423a7);
    MD5_II(b, c, d, a, x[5], 21, 0xfc93a039);
    MD5_II(a, b, c, d, x[12], 6, 0x655b59c3);
    MD5_II(d, a, b, c, x[3], 10, 0x8f0ccc92);
    MD5_II(c, d, a, b, x[10], 15, 0xffeff47d);
    MD5_II(b, c, d, a, x[1], 21, 0x85845dd1);
    MD5_II(a, b, c, d, x[8], 6, 0x6fa87e4f);
    MD5_II(d, a, b, c, x[15], 10, 0xfe2ce6e0);
    MD5_II(c, d, a, b, x[6], 15, 0xa3014314);
    MD5_II(b, c, d, a, x[13], 21, 0x4e0811a1);
    MD5_II(a, b, c, d, x[4], 6, 0xf7537e82);
    MD5_II(d, a, b, c, x[11], 10, 0xbd3af235);
    MD5_II(c, d, a, b, x[2], 15, 0x2ad7d2bb);
    MD5_II(b, c, d, a, x[9], 21, 0xeb86d391);

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

/// @brief Initialise an MD5 context with the standard A/B/C/D IV constants.
/// @param ctx Writable context to initialize.
static void md5_init(MD5_CTX *ctx) {
    ctx->bit_count = 0;
    ctx->state[0] = 0x67452301;
    ctx->state[1] = 0xefcdab89;
    ctx->state[2] = 0x98badcfe;
    ctx->state[3] = 0x10325476;
}

/// @brief Stream `len` bytes through the MD5 context, transforming whenever 64 bytes accumulate.
/// @param ctx Initialized context to update.
/// @param data Borrowed input bytes.
/// @param len Number of bytes at @p data.
static void md5_update(MD5_CTX *ctx, const uint8_t *data, size_t len) {
    size_t i, index, partLen;

    index = (size_t)((ctx->bit_count >> 3) & 0x3F);

    if (len > (UINT64_MAX - ctx->bit_count) / 8) {
        rt_trap("MD5: input too large");
        return;
    }
    ctx->bit_count += (uint64_t)len * 8;

    partLen = 64 - index;

    if (len >= partLen) {
        memcpy(&ctx->buffer[index], data, partLen);
        md5_transform(ctx->state, ctx->buffer);

        for (i = partLen; i + 63 < len; i += 64) {
            md5_transform(ctx->state, &data[i]);
        }
        index = 0;
    } else {
        i = 0;
    }

    memcpy(&ctx->buffer[index], &data[i], len - i);
}

/// @brief Append MD-strengthening (0x80 + zeros + 64-bit length) and emit the 16-byte digest.
/// @param digest Writable 16-byte output buffer.
/// @param ctx Initialized context to finalize.
static void md5_final(uint8_t digest[16], MD5_CTX *ctx) {
    static const uint8_t padding[64] = {0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                        0,    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                        0,    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                        0,    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    uint8_t bits[8];
    size_t index, padLen;

    for (int i = 0; i < 4; i++) {
        bits[i] = (uint8_t)(ctx->bit_count >> (i * 8));
        bits[i + 4] = (uint8_t)(ctx->bit_count >> (32 + i * 8));
    }

    index = (size_t)((ctx->bit_count >> 3) & 0x3f);
    padLen = (index < 56) ? (56 - index) : (120 - index);
    md5_update(ctx, padding, padLen);
    md5_update(ctx, bits, 8);

    for (int i = 0; i < 4; i++) {
        digest[i] = (uint8_t)(ctx->state[0] >> (i * 8));
        digest[i + 4] = (uint8_t)(ctx->state[1] >> (i * 8));
        digest[i + 8] = (uint8_t)(ctx->state[2] >> (i * 8));
        digest[i + 12] = (uint8_t)(ctx->state[3] >> (i * 8));
    }
}

/// @brief One-shot MD5: init → update → final. Compute the 16-byte hash of `data`.
/// @param data Borrowed message bytes.
/// @param len Number of message bytes.
/// @param digest Writable 16-byte digest buffer.
static void compute_md5(const uint8_t *data, size_t len, uint8_t digest[16]) {
    MD5_CTX ctx;
    md5_init(&ctx);
    md5_update(&ctx, data, len);
    md5_final(digest, &ctx);
}

//=============================================================================
// SHA1 Implementation (RFC 3174 / FIPS 180-1)
//=============================================================================

/// Incremental SHA-1 state following FIPS 180.
typedef struct {
    uint32_t state[5]; ///< Five chaining words.
    uint64_t bit_count; ///< Number of message bits consumed.
    uint8_t buffer[64]; ///< Partial 512-bit input block.
} SHA1_CTX;

#define SHA1_ROL(value, bits) (((value) << (bits)) | ((value) >> (32 - (bits))))

/// @brief SHA-1 compression function — applies one 64-byte block to the state (FIPS 180-4 §6.1).
///
/// Expands the 16-word block to 80 words via the message schedule,
/// then runs the four 20-step rounds with rotating logical functions.
/// @param state Five chaining words updated in place.
/// @param buffer Complete 64-byte message block.
static void sha1_transform(uint32_t state[5], const uint8_t buffer[64]) {
    uint32_t a, b, c, d, e;
    uint32_t w[80];

    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)buffer[i * 4] << 24) | ((uint32_t)buffer[i * 4 + 1] << 16) |
               ((uint32_t)buffer[i * 4 + 2] << 8) | buffer[i * 4 + 3];
    }
    for (int i = 16; i < 80; i++) {
        w[i] = SHA1_ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    a = state[0];
    b = state[1];
    c = state[2];
    d = state[3];
    e = state[4];

    for (int i = 0; i < 20; i++) {
        uint32_t temp = SHA1_ROL(a, 5) + ((b & c) | ((~b) & d)) + e + w[i] + 0x5A827999;
        e = d;
        d = c;
        c = SHA1_ROL(b, 30);
        b = a;
        a = temp;
    }
    for (int i = 20; i < 40; i++) {
        uint32_t temp = SHA1_ROL(a, 5) + (b ^ c ^ d) + e + w[i] + 0x6ED9EBA1;
        e = d;
        d = c;
        c = SHA1_ROL(b, 30);
        b = a;
        a = temp;
    }
    for (int i = 40; i < 60; i++) {
        uint32_t temp = SHA1_ROL(a, 5) + ((b & c) | (b & d) | (c & d)) + e + w[i] + 0x8F1BBCDC;
        e = d;
        d = c;
        c = SHA1_ROL(b, 30);
        b = a;
        a = temp;
    }
    for (int i = 60; i < 80; i++) {
        uint32_t temp = SHA1_ROL(a, 5) + (b ^ c ^ d) + e + w[i] + 0xCA62C1D6;
        e = d;
        d = c;
        c = SHA1_ROL(b, 30);
        b = a;
        a = temp;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

/// @brief Initialise a SHA-1 context with the FIPS 180-4 IV.
/// @param ctx Writable context to initialize.
static void sha1_init(SHA1_CTX *ctx) {
    ctx->state[0] = 0x67452301;
    ctx->state[1] = 0xEFCDAB89;
    ctx->state[2] = 0x98BADCFE;
    ctx->state[3] = 0x10325476;
    ctx->state[4] = 0xC3D2E1F0;
    ctx->bit_count = 0;
}

/// @brief Stream `len` bytes through the SHA-1 context (transforms whenever 64 bytes accumulate).
/// @param ctx Initialized context to update.
/// @param data Borrowed input bytes.
/// @param len Number of bytes at @p data.
static void sha1_update(SHA1_CTX *ctx, const uint8_t *data, size_t len) {
    size_t i, j;

    j = (ctx->bit_count >> 3) & 63;
    if (len > (UINT64_MAX - ctx->bit_count) / 8) {
        rt_trap("SHA1: input too large");
        return;
    }
    ctx->bit_count += (uint64_t)len * 8;

    if ((j + len) > 63) {
        memcpy(&ctx->buffer[j], data, (i = 64 - j));
        sha1_transform(ctx->state, ctx->buffer);
        for (; i + 63 < len; i += 64) {
            sha1_transform(ctx->state, &data[i]);
        }
        j = 0;
    } else {
        i = 0;
    }
    memcpy(&ctx->buffer[j], &data[i], len - i);
}

/// @brief Pad and emit the 20-byte SHA-1 digest.
/// @param digest Writable 20-byte output buffer.
/// @param ctx Initialized context to finalize.
static void sha1_final(uint8_t digest[20], SHA1_CTX *ctx) {
    uint8_t finalcount[8];
    uint8_t c;

    for (int i = 0; i < 8; i++) {
        finalcount[i] = (uint8_t)(ctx->bit_count >> ((7 - i) * 8));
    }

    c = 0x80;
    sha1_update(ctx, &c, 1);
    while ((ctx->bit_count & 504) != 448) {
        c = 0x00;
        sha1_update(ctx, &c, 1);
    }
    sha1_update(ctx, finalcount, 8);

    for (int i = 0; i < 20; i++) {
        digest[i] = (uint8_t)((ctx->state[i >> 2] >> ((3 - (i & 3)) * 8)) & 255);
    }
}

/// @brief One-shot SHA-1: init → update → final.
/// @param data Borrowed message bytes.
/// @param len Number of message bytes.
/// @param digest Writable 20-byte digest buffer.
static void compute_sha1(const uint8_t *data, size_t len, uint8_t digest[20]) {
    SHA1_CTX ctx;
    sha1_init(&ctx);
    sha1_update(&ctx, data, len);
    sha1_final(digest, &ctx);
}

//=============================================================================
// SHA256 Implementation (RFC 6234 / FIPS 180-4)
//=============================================================================

/// Incremental SHA-256 state following FIPS 180-4.
typedef struct {
    uint32_t state[8]; ///< Eight chaining words.
    uint64_t bitcount; ///< Number of message bits consumed.
    uint8_t buffer[64]; ///< Partial 512-bit input block.
} SHA256_CTX;

/// FIPS 180-4 SHA-256 round constants.
static const uint32_t sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

#define SHA256_ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define SHA256_CH(x, y, z) (((x) & (y)) ^ ((~(x)) & (z)))
#define SHA256_MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define SHA256_EP0(x) (SHA256_ROTR(x, 2) ^ SHA256_ROTR(x, 13) ^ SHA256_ROTR(x, 22))
#define SHA256_EP1(x) (SHA256_ROTR(x, 6) ^ SHA256_ROTR(x, 11) ^ SHA256_ROTR(x, 25))
#define SHA256_SIG0(x) (SHA256_ROTR(x, 7) ^ SHA256_ROTR(x, 18) ^ ((x) >> 3))
#define SHA256_SIG1(x) (SHA256_ROTR(x, 17) ^ SHA256_ROTR(x, 19) ^ ((x) >> 10))

/// @brief SHA-256 compression function — applies one 64-byte block (FIPS 180-4 §6.2).
///
/// Same shape as the SHA-1 transform but with the SHA-256 message
/// schedule (W[16..63] from sigma0/sigma1 of earlier words) and 64
/// rounds with K[0..63] constants.
/// @param ctx Initialized context whose chaining state is updated.
/// @param data Complete 64-byte message block.
static void sha256_transform(SHA256_CTX *ctx, const uint8_t data[64]) {
    uint32_t a, b, c, d, e, f, g, h, t1, t2, m[64];

    for (int i = 0, j = 0; i < 16; ++i, j += 4) {
        m[i] = ((uint32_t)data[j] << 24) | ((uint32_t)data[j + 1] << 16) |
               ((uint32_t)data[j + 2] << 8) | ((uint32_t)data[j + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        m[i] = SHA256_SIG1(m[i - 2]) + m[i - 7] + SHA256_SIG0(m[i - 15]) + m[i - 16];
    }

    a = ctx->state[0];
    b = ctx->state[1];
    c = ctx->state[2];
    d = ctx->state[3];
    e = ctx->state[4];
    f = ctx->state[5];
    g = ctx->state[6];
    h = ctx->state[7];

    for (int i = 0; i < 64; ++i) {
        t1 = h + SHA256_EP1(e) + SHA256_CH(e, f, g) + sha256_k[i] + m[i];
        t2 = SHA256_EP0(a) + SHA256_MAJ(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

/// @brief Initialise a SHA-256 context with the FIPS 180-4 IV constants.
/// @param ctx Writable context to initialize.
static void sha256_init(SHA256_CTX *ctx) {
    ctx->bitcount = 0;
    ctx->state[0] = 0x6a09e667;
    ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372;
    ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f;
    ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab;
    ctx->state[7] = 0x5be0cd19;
}

/// @brief Stream `len` bytes through the SHA-256 context.
/// @param ctx Initialized context to update.
/// @param data Borrowed input bytes.
/// @param len Number of bytes at @p data.
static void sha256_update(SHA256_CTX *ctx, const uint8_t *data, size_t len) {
    size_t idx = (size_t)(ctx->bitcount / 8 % 64);
    if (len > (UINT64_MAX - ctx->bitcount) / 8) {
        rt_trap("SHA256: input too large");
        return;
    }
    ctx->bitcount += (uint64_t)len * 8;

    // Fill remaining buffer space
    size_t fill = 64 - idx;
    if (len >= fill) {
        memcpy(ctx->buffer + idx, data, fill);
        sha256_transform(ctx, ctx->buffer);
        size_t offset = fill;
        // Process complete 64-byte blocks directly from input
        for (; offset + 63 < len; offset += 64)
            sha256_transform(ctx, data + offset);
        idx = 0;
        len -= offset;
        data += offset;
    }
    // Buffer remaining bytes
    memcpy(ctx->buffer + idx, data, len);
}

/// @brief Pad and emit the 32-byte SHA-256 digest.
/// @param hash Writable 32-byte digest buffer.
/// @param ctx Initialized context to finalize.
static void sha256_final(uint8_t hash[32], SHA256_CTX *ctx) {
    size_t i = ctx->bitcount / 8 % 64;

    ctx->buffer[i++] = 0x80;

    if (i > 56) {
        while (i < 64)
            ctx->buffer[i++] = 0x00;
        sha256_transform(ctx, ctx->buffer);
        i = 0;
    }

    while (i < 56)
        ctx->buffer[i++] = 0x00;

    for (int j = 7; j >= 0; --j) {
        ctx->buffer[i++] = (uint8_t)(ctx->bitcount >> (j * 8));
    }
    sha256_transform(ctx, ctx->buffer);

    for (int j = 0; j < 8; ++j) {
        hash[j * 4] = (ctx->state[j] >> 24) & 0xff;
        hash[j * 4 + 1] = (ctx->state[j] >> 16) & 0xff;
        hash[j * 4 + 2] = (ctx->state[j] >> 8) & 0xff;
        hash[j * 4 + 3] = ctx->state[j] & 0xff;
    }
}

/// @brief One-shot SHA-256: init → update → final.
/// @param data Borrowed message bytes.
/// @param len Number of message bytes.
/// @param hash Writable 32-byte digest buffer.
static void compute_sha256(const uint8_t *data, size_t len, uint8_t hash[32]) {
    SHA256_CTX ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, data, len);
    sha256_final(hash, &ctx);
}

//=============================================================================
// Public API
//=============================================================================

/// @brief Computes the MD5 hash of a string.
///
/// Calculates the 128-bit MD5 message digest (RFC 1321) and returns it as
/// a 32-character lowercase hexadecimal string.
///
/// **Security Warning:** MD5 is cryptographically broken. Collisions can be
/// generated in seconds on modern hardware. Do NOT use for:
/// - Password hashing
/// - Digital signatures
/// - Certificate verification
/// - Any security-critical application
///
/// **Acceptable uses:**
/// - File checksums (non-security)
/// - Content deduplication
/// - Cache key generation
/// - Legacy system compatibility
///
/// **Examples:**
/// ```
/// Hash.MD5("")        → "d41d8cd98f00b204e9800998ecf8427e"
/// Hash.MD5("Hello")   → "8b1a9953c4611296a827abf8c47804d7"
/// Hash.MD5("a")       → "0cc175b9c0f1b6a831c399e269772661"
/// ```
///
/// **Usage example:**
/// ```
/// Dim content = ReadFile("document.txt")
/// Dim checksum = Hash.MD5(content)
/// Print "MD5: " & checksum
/// ```
///
/// @param str Borrowed non-null string to hash. Null or invalid handles trap
///            and produce an empty fallback if trap recovery returns.
///
/// @return A 32-character lowercase hex string representing the MD5 hash.
///
/// @note O(n) time complexity where n is input length.
/// @note Always returns exactly 32 characters.
///
/// @see rt_hash_sha256 For a secure alternative
/// @see rt_hash_md5_bytes For hashing Bytes objects
rt_string rt_hash_md5(rt_string str) {
    if (!hash_require_service(RT_CRYPTO_SERVICE_MD5, "Hash.MD5 is disabled in approved mode"))
        return rt_const_cstr("");
    size_t len;
    int ok;
    const uint8_t *data = hash_string_bytes(str, &len, &ok);
    if (!ok)
        return rt_const_cstr("");

    uint8_t digest[16];
    compute_md5(data, len, digest);
    return rt_codec_hex_enc_bytes(digest, 16);
}

/// @brief Computes the MD5 hash of a Bytes object.
///
/// Calculates the 128-bit MD5 message digest of binary data and returns it
/// as a 32-character lowercase hexadecimal string. This function is useful
/// for hashing binary data that may contain null bytes.
///
/// **Security Warning:** MD5 is cryptographically broken. See rt_hash_md5
/// for details on appropriate use cases.
///
/// **Usage example:**
/// ```
/// Dim data = Bytes.New(256)
/// ' ... fill with binary data ...
/// Dim checksum = Hash.MD5Bytes(data)
/// Print "Binary MD5: " & checksum
/// ```
///
/// @param bytes A Bytes object containing the data to hash.
///              NULL is treated as empty input.
///
/// @return A 32-character lowercase hex string representing the MD5 hash.
///
/// @note O(n) time complexity where n is the byte array length.
/// @note Borrows the Bytes backing buffer for hashing; no temporary data copy is made.
///
/// @see rt_hash_md5 For hashing strings
/// @see rt_hash_sha256_bytes For a secure alternative
rt_string rt_hash_md5_bytes(void *bytes) {
    if (!hash_require_service(RT_CRYPTO_SERVICE_MD5, "Hash.MD5Bytes is disabled in approved mode"))
        return rt_const_cstr("");
    size_t len;
    int ok;
    const uint8_t *data =
        hash_bytes_data(bytes, &len, "Hash.MD5Bytes: invalid Bytes object", &ok);
    if (!ok)
        return rt_const_cstr("");
    uint8_t digest[16];
    compute_md5(data, len, digest);
    return rt_codec_hex_enc_bytes(digest, 16);
}

/// @brief Computes the SHA-1 hash of a string.
///
/// Calculates the 160-bit SHA-1 message digest (RFC 3174 / FIPS 180-1) and
/// returns it as a 40-character lowercase hexadecimal string.
///
/// **Security Warning:** SHA-1 is cryptographically broken. Chosen-prefix
/// collisions have been demonstrated (SHAttered attack, 2017). Do NOT use for:
/// - Password hashing
/// - Digital signatures
/// - Certificate verification
/// - Any security-critical application
///
/// **Acceptable uses:**
/// - Git object identifiers (though Git is migrating to SHA-256)
/// - Legacy system compatibility
/// - Non-security checksums
///
/// **Examples:**
/// ```
/// Hash.SHA1("")        → "da39a3ee5e6b4b0d3255bfef95601890afd80709"
/// Hash.SHA1("Hello")   → "f7ff9e8b7bb2e09b70935a5d785e0cc5d9d0abf0"
/// Hash.SHA1("abc")     → "a9993e364706816aba3e25717850c26c9cd0d89d"
/// ```
///
/// **Usage example:**
/// ```
/// Dim content = ReadFile("source.c")
/// Dim hash = Hash.SHA1(content)
/// Print "SHA1: " & hash
/// ```
///
/// @param str Borrowed non-null string to hash. Null or invalid handles trap
///            and produce an empty fallback if trap recovery returns.
///
/// @return A 40-character lowercase hex string representing the SHA-1 hash.
///
/// @note O(n) time complexity where n is input length.
/// @note Always returns exactly 40 characters.
///
/// @see rt_hash_sha256 For a secure alternative
/// @see rt_hash_sha1_bytes For hashing Bytes objects
rt_string rt_hash_sha1(rt_string str) {
    if (!hash_require_service(RT_CRYPTO_SERVICE_SHA1, "Hash.SHA1 is disabled in approved mode"))
        return rt_const_cstr("");
    size_t len;
    int ok;
    const uint8_t *data = hash_string_bytes(str, &len, &ok);
    if (!ok)
        return rt_const_cstr("");

    uint8_t digest[20];
    compute_sha1(data, len, digest);
    return rt_codec_hex_enc_bytes(digest, 20);
}

/// @brief Computes the SHA-1 hash of a Bytes object.
///
/// Calculates the 160-bit SHA-1 message digest of binary data and returns it
/// as a 40-character lowercase hexadecimal string. This function is useful
/// for hashing binary data that may contain null bytes.
///
/// **Security Warning:** SHA-1 is cryptographically broken. See rt_hash_sha1
/// for details on appropriate use cases.
///
/// **Usage example:**
/// ```
/// Dim fileData = File.ReadAllBytes("image.png")
/// Dim hash = Hash.SHA1Bytes(fileData)
/// Print "Image SHA1: " & hash
/// ```
///
/// @param bytes A Bytes object containing the data to hash.
///              NULL is treated as empty input.
///
/// @return A 40-character lowercase hex string representing the SHA-1 hash.
///
/// @note O(n) time complexity where n is the byte array length.
/// @note Borrows the Bytes backing buffer for hashing; no temporary data copy is made.
///
/// @see rt_hash_sha1 For hashing strings
/// @see rt_hash_sha256_bytes For a secure alternative
rt_string rt_hash_sha1_bytes(void *bytes) {
    if (!hash_require_service(RT_CRYPTO_SERVICE_SHA1,
                              "Hash.SHA1Bytes is disabled in approved mode"))
        return rt_const_cstr("");
    size_t len;
    int ok;
    const uint8_t *data =
        hash_bytes_data(bytes, &len, "Hash.SHA1Bytes: invalid Bytes object", &ok);
    if (!ok)
        return rt_const_cstr("");
    uint8_t digest[20];
    compute_sha1(data, len, digest);
    return rt_codec_hex_enc_bytes(digest, 20);
}

/// @brief Computes the SHA-256 hash of a string.
///
/// Calculates the 256-bit SHA-256 message digest (RFC 6234 / FIPS 180-4) and
/// returns it as a 64-character lowercase hexadecimal string. SHA-256 is part
/// of the SHA-2 family and is currently considered cryptographically secure.
///
/// **This is the preferred digest primitive among the algorithms in this
/// module. Use HMAC or a dedicated KDF when keyed or password-hard behavior is
/// required.**
///
/// **Use cases:**
/// - Password hashing (with proper salting and key stretching)
/// - Digital signatures
/// - Certificate verification
/// - Blockchain and cryptocurrency
/// - HMAC constructions
/// - Content integrity verification
/// - Secure token generation
///
/// **Examples:**
/// ```
/// Hash.SHA256("")      → "e3b0c44298fc1c149afbf4c8996fb924..."
/// Hash.SHA256("Hello") → "185f8db32271fe25f561a6fc938b2e26..."
/// Hash.SHA256("abc")   → "ba7816bf8f01cfea414140de5dae2223..."
/// ```
///
/// **Usage example:**
/// ```
/// ' Verify file integrity
/// Dim content = ReadFile("document.txt")
/// Dim computed = Hash.SHA256(content)
/// If computed = expectedHash Then
///     Print "File integrity verified"
/// Else
///     Print "WARNING: File has been modified!"
/// End If
/// ```
///
/// **Password hashing (simplified):**
/// ```
/// ' NOTE: For production, use proper key derivation (PBKDF2, bcrypt, etc.)
/// Dim salt = GenerateRandomBytes(16)
/// Dim saltedPassword = salt & password
/// Dim hash = Hash.SHA256(saltedPassword)
/// ```
///
/// @param str Borrowed non-null string to hash. Null or invalid handles trap
///            and produce an empty fallback if trap recovery returns.
///
/// @return A 64-character lowercase hex string representing the SHA-256 hash.
///
/// @note O(n) time complexity where n is input length.
/// @note Always returns exactly 64 characters.
/// @note Suitable for integrity checks and as a digest primitive; use HMAC/KDF APIs for MACs,
///       passwords, and key derivation.
///
/// @see rt_hash_sha256_bytes For hashing Bytes objects
/// @see rt_hash_md5 For legacy/checksum uses (NOT secure)
rt_string rt_hash_sha256(rt_string str) {
    size_t len;
    int ok;
    const uint8_t *data = hash_string_bytes(str, &len, &ok);
    if (!ok)
        return rt_const_cstr("");

    uint8_t hash[32];
    compute_sha256(data, len, hash);
    return rt_codec_hex_enc_bytes(hash, 32);
}

/// @brief Computes the SHA-256 hash of a Bytes object.
///
/// Calculates the 256-bit SHA-256 message digest of binary data and returns it
/// as a 64-character lowercase hexadecimal string. This is the recommended
/// function for computing a collision-resistant digest of binary data.
///
/// **For authenticated integrity, use the HMAC-SHA-256 entry point rather than
/// an unkeyed digest.**
///
/// **Usage example:**
/// ```
/// ' Hash a binary file
/// Dim fileData = File.ReadAllBytes("document.pdf")
/// Dim hash = Hash.SHA256Bytes(fileData)
/// Print "SHA256: " & hash
///
/// ' Verify download integrity
/// Dim downloadedData = DownloadFile(url)
/// Dim computed = Hash.SHA256Bytes(downloadedData)
/// If computed = expectedChecksum Then
///     Print "Download verified successfully"
/// End If
/// ```
///
/// @param bytes A Bytes object containing the data to hash.
///              NULL is treated as empty input.
///
/// @return A 64-character lowercase hex string representing the SHA-256 hash.
///
/// @note O(n) time complexity where n is the byte array length.
/// @note Borrows the Bytes backing buffer for hashing; no temporary data copy is made.
/// @note Suitable for integrity checks and as a digest primitive; use HMAC/KDF APIs for MACs,
///       passwords, and key derivation.
///
/// @see rt_hash_sha256 For hashing strings
/// @see rt_hash_md5_bytes For legacy/checksum uses (NOT secure)
rt_string rt_hash_sha256_bytes(void *bytes) {
    size_t len;
    int ok;
    const uint8_t *data =
        hash_bytes_data(bytes, &len, "Hash.SHA256Bytes: invalid Bytes object", &ok);
    if (!ok)
        return rt_const_cstr("");
    uint8_t hash[32];
    compute_sha256(data, len, hash);
    return rt_codec_hex_enc_bytes(hash, 32);
}

/// @brief Computes the CRC32 checksum of a string.
///
/// Calculates the 32-bit CRC (Cyclic Redundancy Check) using the IEEE 802.3
/// polynomial (0xEDB88320, bit-reversed). Returns the checksum as an integer.
///
/// **Important:** CRC32 is NOT a cryptographic hash. It is designed for error
/// detection in data transmission, not for security. The same input always
/// produces the same output, but it is trivial to craft inputs that produce
/// a desired CRC32 value.
///
/// **Use cases:**
/// - File integrity checking (detect accidental corruption)
/// - Network packet error detection
/// - ZIP/GZIP file checksums
/// - Quick data comparison (fingerprinting)
/// - Hash table distribution (non-security)
///
/// **NOT suitable for:**
/// - Password hashing
/// - Digital signatures
/// - Any security application
/// - Collision resistance requirements
///
/// **Examples:**
/// ```
/// Hash.CRC32("")        → 0
/// Hash.CRC32("Hello")   → 4157704578
/// Hash.CRC32("123456789") → 3421780262 (standard test vector)
/// ```
///
/// **Usage example:**
/// ```
/// ' Quick file comparison
/// Dim content = ReadFile("data.bin")
/// Dim checksum = Hash.CRC32(content)
/// Print "CRC32: " & checksum
///
/// ' Verify integrity
/// If Hash.CRC32(receivedData) = expectedCRC Then
///     Print "Data received correctly"
/// Else
///     Print "Data corrupted during transfer"
/// End If
/// ```
///
/// @param str Borrowed non-null string to checksum. Null or invalid handles
///            trap and produce zero if trap recovery returns.
///
/// @return The 32-bit CRC32 checksum as an integer (0 to 4294967295).
///
/// @note O(n) time complexity where n is input length.
/// @note Very fast compared to cryptographic hashes.
/// @note Uses IEEE 802.3 polynomial (same as Ethernet, ZIP, PNG, etc.).
///
/// @see rt_hash_crc32_bytes For computing CRC32 of Bytes objects
/// @see rt_hash_sha256 For security-sensitive applications
int64_t rt_hash_crc32(rt_string str) {
    if (!hash_require_service(RT_CRYPTO_SERVICE_CRC32, "Hash.CRC32 is disabled in approved mode"))
        return 0;
    size_t len;
    int ok;
    const uint8_t *data = hash_string_bytes(str, &len, &ok);
    if (!ok)
        return 0;

    return (int64_t)rt_crc32_compute(data, len);
}

/// @brief Computes the CRC32 checksum of a Bytes object.
///
/// Calculates the 32-bit CRC (Cyclic Redundancy Check) of binary data using
/// the IEEE 802.3 polynomial. This is useful for binary data that may contain
/// null bytes.
///
/// **Important:** CRC32 is NOT a cryptographic hash. See rt_hash_crc32 for
/// details on appropriate use cases.
///
/// **Usage example:**
/// ```
/// ' Checksum binary file
/// Dim fileData = File.ReadAllBytes("archive.zip")
/// Dim crc = Hash.CRC32Bytes(fileData)
/// Print "File CRC32: " & crc
///
/// ' Validate network packet
/// Dim packet = ReceivePacket()
/// Dim payloadCRC = Hash.CRC32Bytes(packet.Payload)
/// If payloadCRC = packet.ExpectedCRC Then
///     ProcessPacket(packet)
/// Else
///     RequestRetransmit()
/// End If
/// ```
///
/// @param bytes A Bytes object containing the data to checksum.
///              NULL is treated as empty input.
///
/// @return The 32-bit CRC32 checksum as an integer (0 to 4294967295).
///
/// @note O(n) time complexity where n is the byte array length.
/// @note Very fast compared to cryptographic hashes.
/// @note Borrows the Bytes backing buffer for checksum calculation; no temporary data copy is made.
///
/// @see rt_hash_crc32 For computing CRC32 of strings
/// @see rt_hash_sha256_bytes For security-sensitive applications
int64_t rt_hash_crc32_bytes(void *bytes) {
    if (!hash_require_service(RT_CRYPTO_SERVICE_CRC32,
                              "Hash.CRC32Bytes is disabled in approved mode"))
        return 0;
    size_t len;
    int ok;
    const uint8_t *data =
        hash_bytes_data(bytes, &len, "Hash.CRC32Bytes: invalid Bytes object", &ok);
    if (!ok)
        return 0;
    uint32_t result = rt_crc32_compute(data, len);
    return (int64_t)result;
}

//=============================================================================
// HMAC Implementation (RFC 2104)
//=============================================================================

/// Compression-block size shared by MD5, SHA-1, and SHA-256.
#define HMAC_BLOCK_SIZE 64

/// Hash primitive selected by the generic HMAC dispatch helpers.
typedef enum { HMAC_HASH_MD5, HMAC_HASH_SHA1, HMAC_HASH_SHA256 } hmac_hash_alg_t;

/// Storage large enough for any supported incremental hash context.
typedef union {
    MD5_CTX md5;       ///< Active storage for HMAC-MD5.
    SHA1_CTX sha1;     ///< Active storage for HMAC-SHA-1.
    SHA256_CTX sha256; ///< Active storage for HMAC-SHA-256.
} hmac_hash_ctx_t;

/// @brief Return the digest length in bytes for the given hash algorithm tag.
/// @details MD5 = 16, SHA-1 = 20, SHA-256 = 32. Used by callers that need to
///          allocate output buffers without baking per-algorithm constants
///          into their own code.
/// @param alg Supported hash algorithm selector.
/// @return Digest length in bytes, or zero for an unknown selector.
static size_t hmac_digest_size(hmac_hash_alg_t alg) {
    switch (alg) {
        case HMAC_HASH_MD5:
            return 16;
        case HMAC_HASH_SHA1:
            return 20;
        case HMAC_HASH_SHA256:
            return 32;
    }
    return 0;
}

/// @brief Initialize a hash-algorithm-agnostic context (`hmac_hash_ctx_t`).
/// @details Dispatches to md5_init / sha1_init / sha256_init based on @p alg.
///          The shared union lets KDFs (PBKDF2, scrypt) hold one stack-
///          allocated context type and feed any of three algorithms into it
///          without per-algorithm specialization.
/// @param alg Hash algorithm selector.
/// @param ctx Writable union to initialize for @p alg.
static void hmac_hash_init(hmac_hash_alg_t alg, hmac_hash_ctx_t *ctx) {
    switch (alg) {
        case HMAC_HASH_MD5:
            md5_init(&ctx->md5);
            break;
        case HMAC_HASH_SHA1:
            sha1_init(&ctx->sha1);
            break;
        case HMAC_HASH_SHA256:
            sha256_init(&ctx->sha256);
            break;
    }
}

/// @brief Feed @p len bytes from @p data into the appropriate hash context.
/// @details Dispatches by @p alg to md5_update / sha1_update / sha256_update.
///          NULL @p data with non-zero @p len traps; NULL @p data with zero
///          @p len is normalized to a one-byte empty buffer for safety.
/// @param alg Hash algorithm active in @p ctx.
/// @param ctx Initialized hash context to update.
/// @param data Borrowed input byte range.
/// @param len Number of bytes at @p data.
static void hmac_hash_update(hmac_hash_alg_t alg,
                             hmac_hash_ctx_t *ctx,
                             const uint8_t *data,
                             size_t len) {
    if (!data && len > 0) {
        rt_trap("HMAC: invalid input buffer");
        return;
    }
    if (!data)
        data = (const uint8_t *)"";
    switch (alg) {
        case HMAC_HASH_MD5:
            md5_update(&ctx->md5, data, len);
            break;
        case HMAC_HASH_SHA1:
            sha1_update(&ctx->sha1, data, len);
            break;
        case HMAC_HASH_SHA256:
            sha256_update(&ctx->sha256, data, len);
            break;
    }
}

/// @brief Finalize the hash and write the digest to @p digest.
/// @details Dispatches by @p alg to md5_final / sha1_final / sha256_final.
///          @p digest must have at least hmac_digest_size(alg) bytes capacity.
/// @param alg Hash algorithm active in @p ctx.
/// @param ctx Initialized hash context to finalize.
/// @param digest Writable output with capacity for the selected digest.
static void hmac_hash_final(hmac_hash_alg_t alg, hmac_hash_ctx_t *ctx, uint8_t *digest) {
    switch (alg) {
        case HMAC_HASH_MD5:
            md5_final(digest, &ctx->md5);
            break;
        case HMAC_HASH_SHA1:
            sha1_final(digest, &ctx->sha1);
            break;
        case HMAC_HASH_SHA256:
            sha256_final(digest, &ctx->sha256);
            break;
    }
}

/// @brief One-shot hash: init + update + final + zeroize, single call.
/// @details Convenience wrapper for the common pattern of hashing a single
///          input buffer in one go. Securely zeros the context after final
///          so any sensitive intermediate state doesn't linger on the stack.
/// @param alg Hash algorithm to apply.
/// @param data Borrowed input byte range.
/// @param len Number of bytes at @p data.
/// @param digest Writable output with capacity for the selected digest.
static void hmac_hash_once(hmac_hash_alg_t alg, const uint8_t *data, size_t len, uint8_t *digest) {
    hmac_hash_ctx_t ctx;
    hmac_hash_init(alg, &ctx);
    hmac_hash_update(alg, &ctx, data, len);
    hmac_hash_final(alg, &ctx, digest);
    hash_secure_zero(&ctx, sizeof(ctx));
}

/// @brief Generic HMAC computation with parameterized hash function.
/// @param alg The hash function to use (MD5, SHA1, or SHA256).
/// @param key HMAC key bytes.
/// @param key_len Length of key in bytes.
/// @param data Data to authenticate.
/// @param data_len Length of data in bytes.
/// @param out Output buffer (must be at least digest_size bytes).
/// @details Implements RFC 2104 key normalization plus inner and outer hash
///          passes. All padded keys, hash contexts, and intermediate digest
///          bytes are securely erased before return. Invalid pointers trap.
static void hmac_compute(hmac_hash_alg_t alg,
                         const uint8_t *key,
                         size_t key_len,
                         const uint8_t *data,
                         size_t data_len,
                         uint8_t *out) {
    size_t digest_size = hmac_digest_size(alg);
    if (digest_size == 0 || !out) {
        rt_trap("HMAC: invalid output buffer");
        return;
    }
    if ((!key && key_len > 0) || (!data && data_len > 0)) {
        rt_trap("HMAC: invalid input buffer");
        return;
    }
    if (!key)
        key = (const uint8_t *)"";
    if (!data)
        data = (const uint8_t *)"";

    uint8_t k_padded[HMAC_BLOCK_SIZE];
    uint8_t k_ipad[HMAC_BLOCK_SIZE];
    uint8_t k_opad[HMAC_BLOCK_SIZE];

    // If key is longer than block size, hash it first
    if (key_len > HMAC_BLOCK_SIZE) {
        hmac_hash_once(alg, key, key_len, k_padded);
        memset(k_padded + digest_size, 0, HMAC_BLOCK_SIZE - digest_size);
    } else {
        memcpy(k_padded, key, key_len);
        memset(k_padded + key_len, 0, HMAC_BLOCK_SIZE - key_len);
    }

    // XOR key with ipad and opad
    for (int i = 0; i < HMAC_BLOCK_SIZE; i++) {
        k_ipad[i] = k_padded[i] ^ 0x36;
        k_opad[i] = k_padded[i] ^ 0x5c;
    }

    // Inner hash: H(K xor ipad || data)
    uint8_t inner_hash[32]; // Max digest size
    hmac_hash_ctx_t ctx;
    hmac_hash_init(alg, &ctx);
    hmac_hash_update(alg, &ctx, k_ipad, HMAC_BLOCK_SIZE);
    hmac_hash_update(alg, &ctx, data, data_len);
    hmac_hash_final(alg, &ctx, inner_hash);
    hash_secure_zero(&ctx, sizeof(ctx));

    // Outer hash: H(K xor opad || inner_hash)
    hmac_hash_init(alg, &ctx);
    hmac_hash_update(alg, &ctx, k_opad, HMAC_BLOCK_SIZE);
    hmac_hash_update(alg, &ctx, inner_hash, digest_size);
    hmac_hash_final(alg, &ctx, out);
    hash_secure_zero(&ctx, sizeof(ctx));
    hash_secure_zero(k_padded, sizeof(k_padded));
    hash_secure_zero(k_ipad, sizeof(k_ipad));
    hash_secure_zero(k_opad, sizeof(k_opad));
    hash_secure_zero(inner_hash, sizeof(inner_hash));
}

/// @brief Compute HMAC-MD5 with raw bytes.
/// @param key Borrowed key bytes.
/// @param key_len Number of key bytes.
/// @param data Borrowed message bytes.
/// @param data_len Number of message bytes.
/// @param out Writable 16-byte MAC buffer.
static void hmac_md5_raw(
    const uint8_t *key, size_t key_len, const uint8_t *data, size_t data_len, uint8_t out[16]) {
    hmac_compute(HMAC_HASH_MD5, key, key_len, data, data_len, out);
}

/// @brief Compute HMAC-SHA1 with raw bytes.
/// @param key Borrowed key bytes.
/// @param key_len Number of key bytes.
/// @param data Borrowed message bytes.
/// @param data_len Number of message bytes.
/// @param out Writable 20-byte MAC buffer.
static void hmac_sha1_raw(
    const uint8_t *key, size_t key_len, const uint8_t *data, size_t data_len, uint8_t out[20]) {
    hmac_compute(HMAC_HASH_SHA1, key, key_len, data, data_len, out);
}

/// @brief Compute HMAC-SHA256 with raw bytes (exported for PBKDF2).
/// @param key Borrowed key bytes.
/// @param key_len Number of key bytes.
/// @param data Borrowed message bytes.
/// @param data_len Number of message bytes.
/// @param out Writable 32-byte MAC buffer.
void rt_hash_hmac_sha256_raw(
    const uint8_t *key, size_t key_len, const uint8_t *data, size_t data_len, uint8_t out[32]) {
    hmac_compute(HMAC_HASH_SHA256, key, key_len, data, data_len, out);
}

//=============================================================================
// HMAC Public API
//=============================================================================

/// @brief Compute HMAC-MD5 of string data with string key.
/// @details Invalid or null strings trap. MD5 policy restrictions also trap in
///          approved mode. If trap recovery returns, an empty string is used
///          as the failure sentinel.
/// @param key Borrowed non-null key string.
/// @param data Borrowed non-null message string.
/// @return Newly allocated 32-character lowercase hexadecimal MAC, or an
///         empty fallback string after a trapped error.
rt_string rt_hash_hmac_md5(rt_string key, rt_string data) {
    if (!hash_require_service(RT_CRYPTO_SERVICE_MD5, "Hash.HmacMD5 is disabled in approved mode"))
        return rt_const_cstr("");
    size_t key_len, data_len;
    int key_ok, data_ok;
    const uint8_t *key_data = hash_string_bytes(key, &key_len, &key_ok);
    const uint8_t *msg_data = hash_string_bytes(data, &data_len, &data_ok);
    if (!key_ok || !data_ok)
        return rt_const_cstr("");

    uint8_t digest[16];
    hmac_md5_raw(key_data, key_len, msg_data, data_len, digest);
    return rt_codec_hex_enc_bytes(digest, 16);
}

/// @brief Compute HMAC-MD5 of Bytes data with Bytes key.
/// @details Null Bytes references represent empty buffers. Invalid object
///          types and approved-mode MD5 policy violations trap.
/// @param key Borrowed Bytes key, or null for an empty key.
/// @param data Borrowed Bytes message, or null for empty input.
/// @return Newly allocated 32-character lowercase hexadecimal MAC, or an
///         empty fallback string after a trapped error.
rt_string rt_hash_hmac_md5_bytes(void *key, void *data) {
    if (!hash_require_service(RT_CRYPTO_SERVICE_MD5,
                              "Hash.HmacMD5Bytes is disabled in approved mode"))
        return rt_const_cstr("");
    size_t key_len, data_len;
    int key_ok, data_ok;
    const uint8_t *key_data =
        hash_bytes_data(key, &key_len, "Hash.HmacMD5Bytes: invalid key Bytes object", &key_ok);
    const uint8_t *msg_data =
        hash_bytes_data(data, &data_len, "Hash.HmacMD5Bytes: invalid data Bytes object", &data_ok);
    if (!key_ok || !data_ok)
        return rt_const_cstr("");

    uint8_t digest[16];
    hmac_md5_raw(key_data, key_len, msg_data, data_len, digest);

    return rt_codec_hex_enc_bytes(digest, 16);
}

/// @brief Compute HMAC-SHA1 of string data with string key.
/// @details Invalid or null strings trap. SHA-1 policy restrictions also trap
///          in approved mode.
/// @param key Borrowed non-null key string.
/// @param data Borrowed non-null message string.
/// @return Newly allocated 40-character lowercase hexadecimal MAC, or an
///         empty fallback string after a trapped error.
rt_string rt_hash_hmac_sha1(rt_string key, rt_string data) {
    if (!hash_require_service(RT_CRYPTO_SERVICE_SHA1, "Hash.HmacSHA1 is disabled in approved mode"))
        return rt_const_cstr("");
    size_t key_len, data_len;
    int key_ok, data_ok;
    const uint8_t *key_data = hash_string_bytes(key, &key_len, &key_ok);
    const uint8_t *msg_data = hash_string_bytes(data, &data_len, &data_ok);
    if (!key_ok || !data_ok)
        return rt_const_cstr("");

    uint8_t digest[20];
    hmac_sha1_raw(key_data, key_len, msg_data, data_len, digest);
    return rt_codec_hex_enc_bytes(digest, 20);
}

/// @brief Compute HMAC-SHA1 of Bytes data with Bytes key.
/// @details Null Bytes references represent empty buffers. Invalid object
///          types and approved-mode SHA-1 policy violations trap.
/// @param key Borrowed Bytes key, or null for an empty key.
/// @param data Borrowed Bytes message, or null for empty input.
/// @return Newly allocated 40-character lowercase hexadecimal MAC, or an
///         empty fallback string after a trapped error.
rt_string rt_hash_hmac_sha1_bytes(void *key, void *data) {
    if (!hash_require_service(RT_CRYPTO_SERVICE_SHA1,
                              "Hash.HmacSHA1Bytes is disabled in approved mode"))
        return rt_const_cstr("");
    size_t key_len, data_len;
    int key_ok, data_ok;
    const uint8_t *key_data =
        hash_bytes_data(key, &key_len, "Hash.HmacSHA1Bytes: invalid key Bytes object", &key_ok);
    const uint8_t *msg_data =
        hash_bytes_data(
            data, &data_len, "Hash.HmacSHA1Bytes: invalid data Bytes object", &data_ok);
    if (!key_ok || !data_ok)
        return rt_const_cstr("");

    uint8_t digest[20];
    hmac_sha1_raw(key_data, key_len, msg_data, data_len, digest);

    return rt_codec_hex_enc_bytes(digest, 20);
}

/// @brief Compute HMAC-SHA256 of string data with string key.
/// @details Both arguments must be valid runtime strings; null or invalid
///          handles trap and yield an empty fallback if recovery returns.
/// @param key Borrowed non-null key string.
/// @param data Borrowed non-null message string.
/// @return Newly allocated 64-character lowercase hexadecimal MAC, or an
///         empty fallback string after a trapped error.
rt_string rt_hash_hmac_sha256(rt_string key, rt_string data) {
    size_t key_len, data_len;
    int key_ok, data_ok;
    const uint8_t *key_data = hash_string_bytes(key, &key_len, &key_ok);
    const uint8_t *msg_data = hash_string_bytes(data, &data_len, &data_ok);
    if (!key_ok || !data_ok)
        return rt_const_cstr("");

    uint8_t digest[32];
    rt_hash_hmac_sha256_raw(key_data, key_len, msg_data, data_len, digest);
    return rt_codec_hex_enc_bytes(digest, 32);
}

/// @brief Compute HMAC-SHA256 of Bytes data with Bytes key.
/// @details Null Bytes references represent empty buffers; invalid non-null
///          runtime objects trap.
/// @param key Borrowed Bytes key, or null for an empty key.
/// @param data Borrowed Bytes message, or null for empty input.
/// @return Newly allocated 64-character lowercase hexadecimal MAC, or an
///         empty fallback string after a trapped error.
rt_string rt_hash_hmac_sha256_bytes(void *key, void *data) {
    size_t key_len, data_len;
    int key_ok, data_ok;
    const uint8_t *key_data =
        hash_bytes_data(key, &key_len, "Hash.HmacSHA256Bytes: invalid key Bytes object", &key_ok);
    const uint8_t *msg_data =
        hash_bytes_data(
            data, &data_len, "Hash.HmacSHA256Bytes: invalid data Bytes object", &data_ok);
    if (!key_ok || !data_ok)
        return rt_const_cstr("");

    uint8_t digest[32];
    rt_hash_hmac_sha256_raw(key_data, key_len, msg_data, data_len, digest);

    return rt_codec_hex_enc_bytes(digest, 32);
}

/// @brief Fixed-time byte-buffer equality test.
/// @details Returns 1 if @p a and @p b have identical bytes, 0 otherwise.
///          Length-mismatched inputs short-circuit (the timing side-channel
///          reveals only "lengths differ", not "first differing byte
///          position"). For equal-length inputs the loop OR-folds every
///          byte XOR into a single accumulator, so the running time depends
///          only on the input length — never on where (or how many) bytes
///          differ.
/// @param a Borrowed first byte range.
/// @param a_len Number of bytes at @p a.
/// @param b Borrowed second byte range.
/// @param b_len Number of bytes at @p b.
/// @return 1 if equal, 0 if not (or if lengths differ).
static int8_t fixed_time_eq(const uint8_t *a, size_t a_len, const uint8_t *b, size_t b_len) {
    uint8_t diff = 0;

    if (a_len != b_len)
        return 0;
    for (size_t i = 0; i < a_len; i++)
        diff |= a[i] ^ b[i];

    return diff == 0 ? 1 : 0;
}

/// @brief Public Zanna.Crypto.Hash.ConstantTimeEquals — fixed-time same-length string equality.
/// @details Compares two `rt_string` payloads in time independent of the
///          first differing byte position, so an attacker timing the
///          comparison can't learn how many leading bytes of a guess were
///          correct. Used internally by Password.Verify and exposed for
///          application code comparing MAC tags or session IDs where a
///          standard `==` would leak prefix-equality timing.
/// @param a Borrowed first non-null runtime string.
/// @param b Borrowed second non-null runtime string.
/// @return 1 if the strings are byte-for-byte equal, 0 otherwise.
int8_t rt_hash_constant_time_equals(rt_string a, rt_string b) {
    size_t a_len, b_len;
    int a_ok, b_ok;
    const uint8_t *a_data = hash_string_bytes(a, &a_len, &a_ok);
    const uint8_t *b_data = hash_string_bytes(b, &b_len, &b_ok);
    if (!a_ok || !b_ok)
        return 0;
    return fixed_time_eq(a_data, a_len, b_data, b_len);
}

/// @brief Public Zanna.Crypto.Hash.ConstantTimeEqualsBytes — fixed-time same-length byte equality.
/// @details Same semantics as rt_hash_constant_time_equals but for raw byte
///          arrays. NULL is treated as an empty byte array; invalid non-empty
///          Bytes objects trap instead of being silently compared as empty.
/// @param a Borrowed first Bytes object, or null for an empty buffer.
/// @param b Borrowed second Bytes object, or null for an empty buffer.
/// @return 1 if the buffers are byte-for-byte equal, 0 otherwise.
int8_t rt_hash_constant_time_equals_bytes(void *a, void *b) {
    size_t a_len, b_len;
    int a_ok, b_ok;
    const uint8_t *a_data =
        hash_bytes_data(
            a, &a_len, "Hash.ConstantTimeEqualsBytes: invalid first Bytes object", &a_ok);
    const uint8_t *b_data =
        hash_bytes_data(
            b, &b_len, "Hash.ConstantTimeEqualsBytes: invalid second Bytes object", &b_ok);
    if (!a_ok || !b_ok)
        return 0;
    return fixed_time_eq(a_data, a_len, b_data, b_len);
}

//=============================================================================
// Fast Per-Process-Keyed Hash (SipHash-2-4)
//=============================================================================

#include "rt_hash_util.h"

/// @brief Compute a per-process-keyed SipHash-2-4 value for a string.
/// @details The historical helper name `rt_fnv1a` aliases the runtime's
///          SipHash implementation. Null or invalid strings trap rather than
///          denoting empty input. Approved-mode policy disables this service.
/// @param str Borrowed non-null input string.
/// @return SipHash bit pattern reinterpreted as signed 64-bit, or zero after a
///         policy/input trap. Values are stable only within one process run.
int64_t rt_hash_fast(rt_string str) {
    if (!hash_require_service(RT_CRYPTO_SERVICE_SIPHASH, "Hash.Fast is disabled in approved mode"))
        return 0;
    size_t len;
    int ok;
    const uint8_t *data = hash_string_bytes(str, &len, &ok);
    if (!ok)
        return 0;
    return (int64_t)rt_fnv1a(data, len);
}

/// @brief Compute a per-process-keyed SipHash-2-4 value for a Bytes object.
/// @param bytes Borrowed input Bytes object, or null for an empty buffer.
/// @return SipHash bit pattern reinterpreted as signed 64-bit, or zero after a
///         policy/type trap. Values are stable only within one process run.
int64_t rt_hash_fast_bytes(void *bytes) {
    if (!hash_require_service(RT_CRYPTO_SERVICE_SIPHASH,
                              "Hash.FastBytes is disabled in approved mode"))
        return 0;
    size_t len;
    int ok;
    const uint8_t *data =
        hash_bytes_data(bytes, &len, "Hash.FastBytes: invalid Bytes object", &ok);
    if (!ok)
        return 0;
    if (len == 0)
        return (int64_t)rt_fnv1a("", 0);
    int64_t result = (int64_t)rt_fnv1a(data, len);
    return result;
}

/// @brief Compute keyed SipHash-2-4 over an integer's little-endian bytes.
/// @param value Signed integer whose two's-complement bit pattern is encoded.
/// @return SipHash bit pattern reinterpreted as signed 64-bit, or zero after a
///         policy trap. Values are stable only within one process run.
int64_t rt_hash_fast_int(int64_t value) {
    if (!hash_require_service(RT_CRYPTO_SERVICE_SIPHASH,
                              "Hash.FastInt is disabled in approved mode"))
        return 0;
    uint64_t u = (uint64_t)value;
    uint8_t encoded[8];
    for (size_t i = 0; i < sizeof(encoded); i++)
        encoded[i] = (uint8_t)(u >> (i * 8));
    return (int64_t)rt_fnv1a(encoded, sizeof(encoded));
}
