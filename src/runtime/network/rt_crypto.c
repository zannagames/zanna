//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/network/rt_crypto.c
// Purpose: Cryptographic primitives for TLS support: SHA-256/384/512, HMAC-SHA256,
//          HKDF (RFC 5869), ChaCha20-Poly1305 AEAD (RFC 8439), AES-128-GCM
//          and AES-256-GCM (NIST SP 800-38D), plus secure entropy dispatch.
//          X25519 is implemented separately in rt_x25519.c.
//
// Key invariants:
//   - All key material is zeroed before return using rt_secure_zero to prevent
//     stack-residue leaks even when the compiler would otherwise elide the writes.
//   - AEAD tag verification is always constant-time (XOR accumulator).
//   - RT_CHACHA20_MAX_BYTES and RT_AES_GCM_MAX_BYTES enforce per-key size limits.
//   - Random byte generation traps rather than falling back to a predictable source.
//
// Ownership/Lifetime:
//   - Pure functions operating on caller-provided buffers; no heap allocation.
//
// Links: rt_crypto.h, rt_x25519.c, rt_tls.c
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Implements zero-dependency cryptographic primitives used by TLS.
 * @details Provides SHA-2, HMAC, HKDF, ChaCha20-Poly1305, AES-GCM, constant-
 * time authentication checks, secure wiping, and policy-aware entropy with
 * bounded per-key and derivation limits over caller-owned buffers.
 */

#include "rt_crypto.h"
#include "rt_crypto_module.h"
#include "rt_entropy_platform.h"
#include "rt_trap.h"

#include "rt_crypto_internal.h"
#include <stdlib.h>
#include <string.h>

/** RFC 5869 maximum SHA-256 output-keying-material length. */
#define RT_HKDF_MAX_OKM_LEN (255u * 32u)
/** RFC 8439 maximum bytes encrypted under one ChaCha20 key and nonce. */
#define RT_CHACHA20_MAX_BYTES (((UINT64_C(1) << 32) - 1u) * 64u)
/** GCM payload bound derived from the 32-bit counter space. */
#define RT_AES_GCM_MAX_BYTES (((UINT64_C(1) << 32) - 2u) * 16u)

/// @brief Secure memory zeroing that the compiler cannot optimize away.
/// @details Uses volatile pointer writes to prevent dead-store elimination.
///          Each byte write is guaranteed to occur even if the buffer is not read afterward.
/// @param ptr Writable memory to clear.
/// @param len Number of bytes to overwrite with zero.
void rt_secure_zero(void *ptr, size_t len) {
    volatile unsigned char *p = (volatile unsigned char *)ptr;
    while (len--)
        *p++ = 0;
}

/// @brief Validate a buffer pointer against its declared length.
/// @details Many crypto entry points accept `(NULL, 0)` as a legal no-op but
///          must trap when `data == NULL` with a positive length — the latter
///          would otherwise UB inside @c memcpy. The helper returns NULL after
///          trapping on an invalid non-empty input so callers can stop local
///          control flow even when a test trap hook returns.
/// @param data Caller buffer (may be NULL when @p len is 0).
/// @param len Declared length in bytes.
/// @param what Trap message identifying the failing call site.
/// @return @p data when non-NULL, else a static empty-string sentinel.
static const uint8_t *rt_crypto_checked_input(const void *data, size_t len, const char *what) {
    if (!data && len > 0) {
        rt_trap(what);
        return NULL;
    }
    return data ? (const uint8_t *)data : (const uint8_t *)"";
}

/// @brief Store a 64-bit value as 8 little-endian bytes.
/// @details Used to encode the AAD and ciphertext length fields in AEAD constructions.
/// @param out Eight-byte output buffer.
/// @param value Integer value to encode.
static void store64_le(uint8_t out[8], uint64_t value) {
    for (int i = 0; i < 8; i++)
        out[i] = (uint8_t)(value >> (8 * i));
}

/// @brief Load a 32-bit little-endian word from an unaligned byte buffer.
/// @param in Four input bytes in least-significant-first order.
/// @return Decoded 32-bit value.
static uint32_t load32_le(const uint8_t in[4]) {
    return ((uint32_t)in[0]) | ((uint32_t)in[1] << 8) | ((uint32_t)in[2] << 16) |
           ((uint32_t)in[3] << 24);
}

/// @brief Return non-zero if both lengths can be safely multiplied by 8 to get bit counts.
/// @details The GCM length block encodes AAD and ciphertext lengths in bits as
///          64-bit big-endian values; overflow would silently produce a wrong tag.
/// @param aad_len Additional-authenticated-data length in bytes.
/// @param text_len Plaintext or ciphertext length in bytes.
/// @return Non-zero when both byte-to-bit conversions fit in `uint64_t`.
static int gcm_lengths_valid(size_t aad_len, size_t text_len) {
    return (uint64_t)aad_len <= UINT64_MAX / 8 && (uint64_t)text_len <= UINT64_MAX / 8;
}

/// @brief Validate the TLS 1.3 HKDF label payload length before encoding it.
/// @details TLS 1.3 encodes the label and context as one-byte lengths inside
///          `HkdfLabel`. This helper performs all additions in subtract form,
///          avoiding size_t wrap even if a future caller passes a non-literal
///          label. It also verifies the caller-provided stack buffer is large
///          enough for the two-byte output length, label length, label bytes,
///          context length, and context bytes.
/// @param prefix_len Length of the fixed `"tls13 "` prefix.
/// @param label_len Length of the caller label.
/// @param context_len Length of the transcript/context hash.
/// @param buffer_cap Capacity of the local label encoding buffer.
/// @return 1 when the label can be encoded; 0 on length overflow or protocol limit failure.
static int tls13_hkdf_label_lengths_valid(size_t prefix_len,
                                          size_t label_len,
                                          size_t context_len,
                                          size_t buffer_cap) {
    if (label_len > 255u || prefix_len > 255u - label_len || context_len > 255u)
        return 0;
    size_t label_total = prefix_len + label_len;
    size_t needed = 2u;
    if (needed > buffer_cap || 1u > buffer_cap - needed)
        return 0;
    needed += 1u;
    if (label_total > buffer_cap - needed)
        return 0;
    needed += label_total;
    if (1u > buffer_cap - needed)
        return 0;
    needed += 1u;
    return context_len <= buffer_cap - needed;
}

//=============================================================================
// SHA-256
//=============================================================================

static const uint32_t sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

#define ROTR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x) (ROTR32(x, 2) ^ ROTR32(x, 13) ^ ROTR32(x, 22))
#define EP1(x) (ROTR32(x, 6) ^ ROTR32(x, 11) ^ ROTR32(x, 25))
#define SIG0(x) (ROTR32(x, 7) ^ ROTR32(x, 18) ^ ((x) >> 3))
#define SIG1(x) (ROTR32(x, 17) ^ ROTR32(x, 19) ^ ((x) >> 10))

/// @brief Apply one 64-byte block to the SHA-256 compression function.
///
/// Implements the FIPS 180-4 §6.2 message schedule plus the 64-round
/// inner loop. Operates on a private set of working variables and
/// folds them back into `ctx->state` at the end.
/// @param ctx Initialized SHA-256 context to mutate.
/// @param data Complete 64-byte message block.
static void sha256_transform(rt_sha256_ctx *ctx, const uint8_t data[64]) {
    uint32_t a, b, c, d, e, f, g, h, t1, t2, w[64];

    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)data[i * 4] << 24) | ((uint32_t)data[i * 4 + 1] << 16) |
               ((uint32_t)data[i * 4 + 2] << 8) | data[i * 4 + 3];
    }
    for (int i = 16; i < 64; i++) {
        w[i] = SIG1(w[i - 2]) + w[i - 7] + SIG0(w[i - 15]) + w[i - 16];
    }

    a = ctx->state[0];
    b = ctx->state[1];
    c = ctx->state[2];
    d = ctx->state[3];
    e = ctx->state[4];
    f = ctx->state[5];
    g = ctx->state[6];
    h = ctx->state[7];

    for (int i = 0; i < 64; i++) {
        t1 = h + EP1(e) + CH(e, f, g) + sha256_k[i] + w[i];
        t2 = EP0(a) + MAJ(a, b, c);
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

/// @brief Initialise a SHA-256 context with FIPS 180-4 IV constants.
/// @param ctx Context whose state, count, and buffered position are reset.
void rt_sha256_init(rt_sha256_ctx *ctx) {
    ctx->state[0] = 0x6a09e667;
    ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372;
    ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f;
    ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab;
    ctx->state[7] = 0x5be0cd19;
    ctx->count = 0;
}

/// @brief Feed `len` bytes into the rolling SHA-256 state.
///
/// Buffers partial blocks; calls `sha256_transform` whenever 64
/// bytes have accumulated. Tracks the cumulative bit count so the
/// MD-strengthening step in `final()` can append the correct length.
/// @param ctx Initialized SHA-256 context to update.
/// @param data Input bytes, or NULL only when @p len is zero.
/// @param len Number of input bytes.
void rt_sha256_update(rt_sha256_ctx *ctx, const void *data, size_t len) {
    if (!ctx) {
        rt_trap("SHA256: context is null");
        return;
    }
    if (len == 0)
        return;
    const uint8_t *ptr = rt_crypto_checked_input(data, len, "SHA256: input buffer is null");
    if (!ptr)
        return;
    size_t idx = (ctx->count / 8) % 64;

    if (len > (UINT64_MAX - ctx->count) / 8) {
        rt_trap("SHA256: input too large");
        return;
    }
    ctx->count += len * 8;

    while (len > 0) {
        size_t copy = 64 - idx;
        if (copy > len)
            copy = len;
        memcpy(ctx->buffer + idx, ptr, copy);
        idx += copy;
        ptr += copy;
        len -= copy;

        if (idx == 64) {
            sha256_transform(ctx, ctx->buffer);
            idx = 0;
        }
    }
}

/// @brief Append MD padding + 64-bit length and emit the 32-byte digest.
///
/// Padding rule (FIPS 180-4 §5.1.1): single `0x80` byte, zeros to
/// length ≡ 56 (mod 64), then the message bit-count in big-endian
/// 64-bit form. The final state words are serialised big-endian
/// into `digest`.
/// @param ctx Incremental SHA-256 context to finalize.
/// @param digest Output buffer receiving exactly 32 digest bytes.
void rt_sha256_final(rt_sha256_ctx *ctx, uint8_t digest[32]) {
    if (!ctx) {
        rt_trap("SHA256: context is null");
        return;
    }
    if (!digest) {
        rt_trap("SHA256: digest output is null");
        return;
    }
    uint64_t bits = ctx->count;
    size_t idx = (ctx->count / 8) % 64;

    ctx->buffer[idx++] = 0x80;
    if (idx > 56) {
        memset(ctx->buffer + idx, 0, 64 - idx);
        sha256_transform(ctx, ctx->buffer);
        idx = 0;
    }
    memset(ctx->buffer + idx, 0, 56 - idx);
    ctx->buffer[56] = (uint8_t)(bits >> 56);
    ctx->buffer[57] = (uint8_t)(bits >> 48);
    ctx->buffer[58] = (uint8_t)(bits >> 40);
    ctx->buffer[59] = (uint8_t)(bits >> 32);
    ctx->buffer[60] = (uint8_t)(bits >> 24);
    ctx->buffer[61] = (uint8_t)(bits >> 16);
    ctx->buffer[62] = (uint8_t)(bits >> 8);
    ctx->buffer[63] = (uint8_t)bits;
    sha256_transform(ctx, ctx->buffer);

    for (int i = 0; i < 8; i++) {
        digest[i * 4] = (ctx->state[i] >> 24) & 0xFF;
        digest[i * 4 + 1] = (ctx->state[i] >> 16) & 0xFF;
        digest[i * 4 + 2] = (ctx->state[i] >> 8) & 0xFF;
        digest[i * 4 + 3] = ctx->state[i] & 0xFF;
    }
}

/// @brief One-shot SHA-256: init → update → final into a fresh context.
/// @param data Input bytes, or NULL only when @p len is zero.
/// @param len Number of input bytes.
/// @param digest Output buffer receiving exactly 32 digest bytes.
void rt_sha256(const void *data, size_t len, uint8_t digest[32]) {
    rt_sha256_ctx ctx;
    if (!digest) {
        rt_trap("SHA256: digest output is null");
        return;
    }
    const uint8_t *ptr = rt_crypto_checked_input(data, len, "SHA256: input buffer is null");
    if (!ptr)
        return;
    if ((uint64_t)len > UINT64_MAX / 8) {
        rt_trap("SHA256: input too large");
        return;
    }
    rt_sha256_init(&ctx);
    rt_sha256_update(&ctx, ptr, len);
    rt_sha256_final(&ctx, digest);
}

//=============================================================================
// SHA-384 / SHA-512
//=============================================================================

typedef struct {
    uint64_t state[8];
    uint64_t count_hi;
    uint64_t count_lo;
    uint8_t buffer[128];
} rt_sha512_ctx_internal;

static const uint64_t sha512_k[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
    0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL};

#define ROTR64(x, n) (((x) >> (n)) | ((x) << (64 - (n))))
#define EP0_64(x) (ROTR64((x), 28) ^ ROTR64((x), 34) ^ ROTR64((x), 39))
#define EP1_64(x) (ROTR64((x), 14) ^ ROTR64((x), 18) ^ ROTR64((x), 41))
#define SIG0_64(x) (ROTR64((x), 1) ^ ROTR64((x), 8) ^ ((x) >> 7))
#define SIG1_64(x) (ROTR64((x), 19) ^ ROTR64((x), 61) ^ ((x) >> 6))

/// @brief Apply one 128-byte block to the SHA-512/384 compression function.
///
/// Mirrors sha256_transform but operates on 64-bit words, 80 rounds, and the
/// FIPS 180-4 SHA-512 message schedule (SIG0_64/SIG1_64 macros). Used by both
/// SHA-384 and SHA-512 since they share the same compression function.
/// @param ctx Initialized SHA-512-family context to mutate.
/// @param data Complete 128-byte message block.
static void sha512_transform(rt_sha512_ctx_internal *ctx, const uint8_t data[128]) {
    uint64_t a, b, c, d, e, f, g, h, t1, t2, w[80];

    for (int i = 0; i < 16; i++) {
        w[i] = ((uint64_t)data[i * 8 + 0] << 56) | ((uint64_t)data[i * 8 + 1] << 48) |
               ((uint64_t)data[i * 8 + 2] << 40) | ((uint64_t)data[i * 8 + 3] << 32) |
               ((uint64_t)data[i * 8 + 4] << 24) | ((uint64_t)data[i * 8 + 5] << 16) |
               ((uint64_t)data[i * 8 + 6] << 8) | (uint64_t)data[i * 8 + 7];
    }
    for (int i = 16; i < 80; i++)
        w[i] = SIG1_64(w[i - 2]) + w[i - 7] + SIG0_64(w[i - 15]) + w[i - 16];

    a = ctx->state[0];
    b = ctx->state[1];
    c = ctx->state[2];
    d = ctx->state[3];
    e = ctx->state[4];
    f = ctx->state[5];
    g = ctx->state[6];
    h = ctx->state[7];

    for (int i = 0; i < 80; i++) {
        t1 = h + EP1_64(e) + CH(e, f, g) + sha512_k[i] + w[i];
        t2 = EP0_64(a) + MAJ(a, b, c);
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

/// @brief Initialise SHA-512 or SHA-384 context with the correct FIPS 180-4 IV.
/// @param ctx Context whose state and 128-bit bit count are reset.
/// @param is_sha384 Non-zero to use SHA-384 IVs; zero for SHA-512.
static void sha512_family_init(rt_sha512_ctx_internal *ctx, int is_sha384) {
    if (is_sha384) {
        ctx->state[0] = 0xcbbb9d5dc1059ed8ULL;
        ctx->state[1] = 0x629a292a367cd507ULL;
        ctx->state[2] = 0x9159015a3070dd17ULL;
        ctx->state[3] = 0x152fecd8f70e5939ULL;
        ctx->state[4] = 0x67332667ffc00b31ULL;
        ctx->state[5] = 0x8eb44a8768581511ULL;
        ctx->state[6] = 0xdb0c2e0d64f98fa7ULL;
        ctx->state[7] = 0x47b5481dbefa4fa4ULL;
    } else {
        ctx->state[0] = 0x6a09e667f3bcc908ULL;
        ctx->state[1] = 0xbb67ae8584caa73bULL;
        ctx->state[2] = 0x3c6ef372fe94f82bULL;
        ctx->state[3] = 0xa54ff53a5f1d36f1ULL;
        ctx->state[4] = 0x510e527fade682d1ULL;
        ctx->state[5] = 0x9b05688c2b3e6c1fULL;
        ctx->state[6] = 0x1f83d9abfb41bd6bULL;
        ctx->state[7] = 0x5be0cd19137e2179ULL;
    }
    ctx->count_hi = 0;
    ctx->count_lo = 0;
}

/// @brief Feed bytes into a SHA-512/384 rolling context.
///
/// Equivalent to rt_sha256_update but with a 128-byte block and a
/// 128-bit count (count_hi/count_lo) to handle the >2^64 bit-length
/// that SHA-512 supports in theory. In practice `count_hi` carries
/// overflow from count_lo so the implementation handles files > 2 EiB.
/// @param ctx Initialized SHA-512-family context to update.
/// @param data Input bytes, or NULL only when @p len is zero.
/// @param len Number of input bytes.
static void sha512_family_update(rt_sha512_ctx_internal *ctx, const void *data, size_t len) {
    if (!ctx) {
        rt_trap("SHA512: context is null");
        return;
    }
    if (len == 0)
        return;
    const uint8_t *ptr = rt_crypto_checked_input(data, len, "SHA512: input buffer is null");
    if (!ptr)
        return;
    size_t idx = (size_t)((ctx->count_lo >> 3) & 127u);
    uint64_t prev_lo = ctx->count_lo;

    ctx->count_lo += ((uint64_t)len << 3);
    if (ctx->count_lo < prev_lo)
        ctx->count_hi++;
    ctx->count_hi += (uint64_t)(len >> 61);

    while (len > 0) {
        size_t copy = 128 - idx;
        if (copy > len)
            copy = len;
        memcpy(ctx->buffer + idx, ptr, copy);
        idx += copy;
        ptr += copy;
        len -= copy;
        if (idx == 128) {
            sha512_transform(ctx, ctx->buffer);
            idx = 0;
        }
    }
}

/// @brief Finalise a SHA-512/384 context, writing `digest_len` bytes to `digest`.
/// @param digest_len 48 for SHA-384, 64 for SHA-512.
///
/// Appends the 0x80 byte + zero padding + 128-bit big-endian length and
/// processes the remaining blocks. The outer caller truncates the state
/// by passing the appropriate digest_len; the state words are serialised
/// big-endian, byte-by-byte.
/// @param ctx Incremental SHA-512-family context to finalize.
/// @param digest Output buffer receiving @p digest_len bytes.
static void sha512_family_final(rt_sha512_ctx_internal *ctx, uint8_t *digest, size_t digest_len) {
    if (!ctx) {
        rt_trap("SHA512: context is null");
        return;
    }
    if (!digest) {
        rt_trap("SHA512: digest output is null");
        return;
    }
    uint64_t bits_hi = ctx->count_hi;
    uint64_t bits_lo = ctx->count_lo;
    size_t idx = (size_t)((ctx->count_lo >> 3) & 127u);

    ctx->buffer[idx++] = 0x80;
    if (idx > 112) {
        memset(ctx->buffer + idx, 0, 128 - idx);
        sha512_transform(ctx, ctx->buffer);
        idx = 0;
    }
    memset(ctx->buffer + idx, 0, 112 - idx);
    ctx->buffer[112] = (uint8_t)(bits_hi >> 56);
    ctx->buffer[113] = (uint8_t)(bits_hi >> 48);
    ctx->buffer[114] = (uint8_t)(bits_hi >> 40);
    ctx->buffer[115] = (uint8_t)(bits_hi >> 32);
    ctx->buffer[116] = (uint8_t)(bits_hi >> 24);
    ctx->buffer[117] = (uint8_t)(bits_hi >> 16);
    ctx->buffer[118] = (uint8_t)(bits_hi >> 8);
    ctx->buffer[119] = (uint8_t)bits_hi;
    ctx->buffer[120] = (uint8_t)(bits_lo >> 56);
    ctx->buffer[121] = (uint8_t)(bits_lo >> 48);
    ctx->buffer[122] = (uint8_t)(bits_lo >> 40);
    ctx->buffer[123] = (uint8_t)(bits_lo >> 32);
    ctx->buffer[124] = (uint8_t)(bits_lo >> 24);
    ctx->buffer[125] = (uint8_t)(bits_lo >> 16);
    ctx->buffer[126] = (uint8_t)(bits_lo >> 8);
    ctx->buffer[127] = (uint8_t)bits_lo;
    sha512_transform(ctx, ctx->buffer);

    for (size_t i = 0, pos = 0; i < 8 && pos < digest_len; i++) {
        for (int shift = 56; shift >= 0 && pos < digest_len; shift -= 8)
            digest[pos++] = (uint8_t)(ctx->state[i] >> shift);
    }
}

/// @brief One-shot SHA-384: init → update → final → 48-byte digest.
/// @param data Input bytes, or NULL only when @p len is zero.
/// @param len Number of input bytes.
/// @param digest Output buffer receiving exactly 48 digest bytes.
void rt_sha384(const void *data, size_t len, uint8_t digest[48]) {
    rt_sha512_ctx_internal ctx;
    if (!digest) {
        rt_trap("SHA384: digest output is null");
        return;
    }
    const uint8_t *ptr = rt_crypto_checked_input(data, len, "SHA384: input buffer is null");
    if (!ptr)
        return;
    sha512_family_init(&ctx, 1);
    sha512_family_update(&ctx, ptr, len);
    sha512_family_final(&ctx, digest, 48);
}

/// @brief One-shot SHA-512: init → update → final → 64-byte digest.
/// @param data Input bytes, or NULL only when @p len is zero.
/// @param len Number of input bytes.
/// @param digest Output buffer receiving exactly 64 digest bytes.
void rt_sha512(const void *data, size_t len, uint8_t digest[64]) {
    rt_sha512_ctx_internal ctx;
    if (!digest) {
        rt_trap("SHA512: digest output is null");
        return;
    }
    const uint8_t *ptr = rt_crypto_checked_input(data, len, "SHA512: input buffer is null");
    if (!ptr)
        return;
    sha512_family_init(&ctx, 0);
    sha512_family_update(&ctx, ptr, len);
    sha512_family_final(&ctx, digest, 64);
}

//=============================================================================
// HMAC-SHA256
//=============================================================================

/// @brief HMAC-SHA256 (RFC 2104) with secret key and message → 32-byte MAC.
///
/// Pads/truncates the key to one SHA-256 block (64 bytes), forms
/// the inner pad (XOR 0x36) and outer pad (XOR 0x5C), and computes
/// `H((K ⊕ opad) || H((K ⊕ ipad) || data))`. All intermediate
/// key-derived material is cleared before return to discourage
/// post-call leakage.
/// @param key Secret key bytes, or NULL only when @p key_len is zero.
/// @param key_len Number of key bytes.
/// @param data Message bytes, or NULL only when @p data_len is zero.
/// @param data_len Number of message bytes.
/// @param mac Output buffer receiving exactly 32 MAC bytes.
void rt_hmac_sha256(
    const uint8_t *key, size_t key_len, const void *data, size_t data_len, uint8_t mac[32]) {
    uint8_t k[64], ipad[64], opad[64];

    if (!mac) {
        rt_trap("HMAC-SHA256: output buffer is null");
        return;
    }
    if (!key && key_len > 0) {
        rt_trap("HMAC-SHA256: key buffer is null");
        return;
    }
    if (!data && data_len > 0) {
        rt_trap("HMAC-SHA256: input buffer is null");
        return;
    }
    if (!key)
        key = (const uint8_t *)"";
    if (!data)
        data = "";

    if (key_len > 64) {
        rt_sha256(key, key_len, k);
        key_len = 32;
    } else {
        memcpy(k, key, key_len);
    }
    memset(k + key_len, 0, 64 - key_len);

    for (int i = 0; i < 64; i++) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }

    rt_sha256_ctx ctx;
    rt_sha256_init(&ctx);
    rt_sha256_update(&ctx, ipad, 64);
    rt_sha256_update(&ctx, data, data_len);
    rt_sha256_final(&ctx, mac);

    rt_sha256_init(&ctx);
    rt_sha256_update(&ctx, opad, 64);
    rt_sha256_update(&ctx, mac, 32);
    rt_sha256_final(&ctx, mac);

    rt_secure_zero(k, sizeof(k));
    rt_secure_zero(ipad, sizeof(ipad));
    rt_secure_zero(opad, sizeof(opad));
}

/// @brief Shared HMAC-SHA-384 / HMAC-SHA-512 implementation.
/// @details Mirrors @ref rt_hmac_sha256 but operates on 128-byte blocks and
///          uses the SHA-512/384 compression function selected by
///          @p is_sha384. The inner hash is truncated to @p mac_len bytes
///          (matching the chosen variant) before being fed into the outer
///          hash. All intermediate key-derived material is scrubbed before
///          return.
/// @param key Pointer to the HMAC key.
/// @param key_len Length of @p key in bytes.
/// @param data Pointer to the input data.
/// @param data_len Length of @p data in bytes.
/// @param mac Output buffer (@p mac_len bytes).
/// @param mac_len 48 for SHA-384, 64 for SHA-512.
/// @param is_sha384 Non-zero to use SHA-384 IVs, zero for SHA-512.
static void hmac_sha512_family(const uint8_t *key,
                               size_t key_len,
                               const void *data,
                               size_t data_len,
                               uint8_t *mac,
                               size_t mac_len,
                               int is_sha384) {
    uint8_t k[128];
    uint8_t ipad[128];
    uint8_t opad[128];
    uint8_t inner[64];

    if (!mac) {
        rt_trap("HMAC-SHA2: output buffer is null");
        return;
    }
    if (!key && key_len > 0) {
        rt_trap("HMAC-SHA2: key buffer is null");
        return;
    }
    if (!data && data_len > 0) {
        rt_trap("HMAC-SHA2: input buffer is null");
        return;
    }
    if (!key)
        key = (const uint8_t *)"";
    if (!data)
        data = "";

    if (key_len > sizeof(k)) {
        if (is_sha384) {
            rt_sha384(key, key_len, k);
            key_len = 48;
        } else {
            rt_sha512(key, key_len, k);
            key_len = 64;
        }
    } else if (key_len > 0) {
        memcpy(k, key, key_len);
    }
    memset(k + key_len, 0, sizeof(k) - key_len);

    for (size_t i = 0; i < sizeof(k); i++) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }

    rt_sha512_ctx_internal ctx;
    sha512_family_init(&ctx, is_sha384);
    sha512_family_update(&ctx, ipad, sizeof(ipad));
    sha512_family_update(&ctx, data, data_len);
    sha512_family_final(&ctx, inner, mac_len);

    sha512_family_init(&ctx, is_sha384);
    sha512_family_update(&ctx, opad, sizeof(opad));
    sha512_family_update(&ctx, inner, mac_len);
    sha512_family_final(&ctx, mac, mac_len);

    rt_secure_zero(k, sizeof(k));
    rt_secure_zero(ipad, sizeof(ipad));
    rt_secure_zero(opad, sizeof(opad));
    rt_secure_zero(inner, sizeof(inner));
    rt_secure_zero(&ctx, sizeof(ctx));
}

/// @brief HMAC-SHA-384 wrapper over @ref hmac_sha512_family.
/// @param key Secret key bytes, or NULL only when @p key_len is zero.
/// @param key_len Number of key bytes.
/// @param data Message bytes, or NULL only when @p data_len is zero.
/// @param data_len Number of message bytes.
/// @param mac Output buffer receiving exactly 48 MAC bytes.
void rt_hmac_sha384(
    const uint8_t *key, size_t key_len, const void *data, size_t data_len, uint8_t mac[48]) {
    hmac_sha512_family(key, key_len, data, data_len, mac, 48, 1);
}

/// @brief HMAC-SHA-512 wrapper over @ref hmac_sha512_family.
/// @param key Secret key bytes, or NULL only when @p key_len is zero.
/// @param key_len Number of key bytes.
/// @param data Message bytes, or NULL only when @p data_len is zero.
/// @param data_len Number of message bytes.
/// @param mac Output buffer receiving exactly 64 MAC bytes.
void rt_hmac_sha512(
    const uint8_t *key, size_t key_len, const void *data, size_t data_len, uint8_t mac[64]) {
    hmac_sha512_family(key, key_len, data, data_len, mac, 64, 0);
}

//=============================================================================
// HKDF-SHA256 (RFC 5869)
//=============================================================================

/// @brief HKDF-Extract step (RFC 5869 §2.2): produce a 32-byte PRK.
///
/// PRK = HMAC-SHA256(salt, IKM). When the caller passes a NULL or
/// empty salt, the spec mandates substituting `HashLen` zero bytes
/// — TLS 1.3 relies on this for the early-secret derivation.
/// @param salt Optional salt bytes; NULL is valid only with zero length.
/// @param salt_len Number of salt bytes.
/// @param ikm Input keying material, or NULL only when @p ikm_len is zero.
/// @param ikm_len Number of input-key-material bytes.
/// @param prk Output buffer receiving the 32-byte pseudorandom key.
void rt_hkdf_extract(
    const uint8_t *salt, size_t salt_len, const uint8_t *ikm, size_t ikm_len, uint8_t prk[32]) {
    if (!prk) {
        rt_trap("HKDF-Extract: output buffer is null");
        return;
    }
    if (!salt && salt_len > 0) {
        rt_trap("HKDF-Extract: salt buffer is null");
        return;
    }
    if (!ikm && ikm_len > 0) {
        rt_trap("HKDF-Extract: input key material buffer is null");
        return;
    }
    if (salt == NULL || salt_len == 0) {
        uint8_t zero_salt[32] = {0};
        rt_hmac_sha256(zero_salt, 32, ikm, ikm_len, prk);
        rt_secure_zero(zero_salt, sizeof(zero_salt));
    } else {
        rt_hmac_sha256(salt, salt_len, ikm, ikm_len, prk);
    }
}

/// @brief HKDF-Expand step (RFC 5869 §2.3): stretch a PRK to OKM of `okm_len` bytes.
///
/// Iterates `T(n) = HMAC(PRK, T(n-1) || info || n)` for
/// `n = 1, 2, …` and concatenates the truncated outputs. Each
/// iteration's intermediate HMAC pads are scrubbed before
/// returning. The spec caps OKM at `255 * HashLen`; this
/// implementation enforces `RT_HKDF_MAX_OKM_LEN`.
/// @param prk 32-byte pseudorandom key from HKDF-Extract.
/// @param info Optional application context, or NULL only when @p info_len is zero.
/// @param info_len Number of context bytes.
/// @param okm Output keying-material buffer, or NULL only for zero output length.
/// @param okm_len Requested output length in bytes.
/// @return 0 on success, -1 if the requested size exceeds the cap.
int rt_hkdf_expand(
    const uint8_t prk[32], const uint8_t *info, size_t info_len, uint8_t *okm, size_t okm_len) {
    uint8_t t[32] = {0};
    size_t t_len = 0;
    uint8_t counter = 1;
    size_t pos = 0;

    if (!prk || (!okm && okm_len > 0))
        return -1;
    if (!info && info_len > 0)
        return -1;
    if (okm_len > RT_HKDF_MAX_OKM_LEN)
        return -1;

    while (pos < okm_len) {
        rt_sha256_ctx ctx;
        uint8_t temp[32];

        // HMAC(PRK, T(n-1) || info || counter)
        uint8_t k[64], ipad[64], opad[64];
        memcpy(k, prk, 32);
        memset(k + 32, 0, 32);

        for (int i = 0; i < 64; i++) {
            ipad[i] = k[i] ^ 0x36;
            opad[i] = k[i] ^ 0x5c;
        }

        rt_sha256_init(&ctx);
        rt_sha256_update(&ctx, ipad, 64);
        if (t_len > 0)
            rt_sha256_update(&ctx, t, t_len);
        if (info_len > 0)
            rt_sha256_update(&ctx, info, info_len);
        rt_sha256_update(&ctx, &counter, 1);
        rt_sha256_final(&ctx, temp);

        rt_sha256_init(&ctx);
        rt_sha256_update(&ctx, opad, 64);
        rt_sha256_update(&ctx, temp, 32);
        rt_sha256_final(&ctx, t);
        t_len = 32;

        size_t copy = okm_len - pos;
        if (copy > 32)
            copy = 32;
        memcpy(okm + pos, t, copy);
        pos += copy;
        counter++;

        rt_secure_zero(k, sizeof(k));
        rt_secure_zero(ipad, sizeof(ipad));
        rt_secure_zero(opad, sizeof(opad));
        rt_secure_zero(temp, sizeof(temp));
    }
    rt_secure_zero(t, sizeof(t));
    return 0;
}

/// @brief TLS 1.3 HKDF-Expand-Label (RFC 8446 §7.1).
///
/// Wraps `rt_hkdf_expand` with the standardised info-block encoding:
/// `length(2) || "tls13 " + label(prefixed-1) || context(prefixed-1)`.
/// All TLS 1.3 traffic key derivations route through this function.
/// @param secret 32-byte secret used as the HKDF pseudorandom key.
/// @param label NUL-terminated label without the `"tls13 "` prefix.
/// @param context Optional context bytes, or NULL only when @p context_len is zero.
/// @param context_len Number of context bytes.
/// @param out Output keying-material buffer, or NULL only for zero output length.
/// @param out_len Requested output length encoded in the two-byte TLS field.
/// @return 0 on success, -1 if the resulting OKM length would
///         exceed the HKDF cap.
int rt_hkdf_expand_label(const uint8_t secret[32],
                         const char *label,
                         const uint8_t *context,
                         size_t context_len,
                         uint8_t *out,
                         size_t out_len) {
    // TLS 1.3 HkdfLabel structure
    uint8_t hkdf_label[512];
    size_t pos = 0;

    if (!secret || !label || (!out && out_len > 0))
        return -1;
    if (!context && context_len > 0)
        return -1;
    if (out_len > RT_HKDF_MAX_OKM_LEN || out_len > UINT16_MAX)
        return -1;

    // Length (2 bytes, big-endian)
    hkdf_label[pos++] = (out_len >> 8) & 0xFF;
    hkdf_label[pos++] = out_len & 0xFF;

    // Label = "tls13 " + label
    const char *prefix = "tls13 ";
    size_t prefix_len = 6;
    size_t label_len = strlen(label);

    if (!tls13_hkdf_label_lengths_valid(prefix_len, label_len, context_len, sizeof(hkdf_label)))
        return -1;

    hkdf_label[pos++] = (uint8_t)(prefix_len + label_len);
    memcpy(hkdf_label + pos, prefix, prefix_len);
    pos += prefix_len;
    memcpy(hkdf_label + pos, label, label_len);
    pos += label_len;

    // Context
    hkdf_label[pos++] = (uint8_t)context_len;
    if (context_len > 0) {
        memcpy(hkdf_label + pos, context, context_len);
        pos += context_len;
    }

    return rt_hkdf_expand(secret, hkdf_label, pos, out, out_len);
}

#define RT_HKDF_SHA384_MAX_OKM_LEN (255u * 48u)

/// @brief HKDF-Extract over HMAC-SHA-384, producing a 48-byte PRK.
/// @details Mirrors @ref rt_hkdf_extract but uses the SHA-384 hash and a
///          48-byte zero-salt default. Used by the TLS 1.3 SHA-384
///          cipher-suite family.
/// @param salt Optional salt bytes; NULL is valid only with zero length.
/// @param salt_len Number of salt bytes.
/// @param ikm Input keying material, or NULL only when @p ikm_len is zero.
/// @param ikm_len Number of input-key-material bytes.
/// @param prk Output buffer receiving the 48-byte pseudorandom key.
void rt_hkdf_extract_sha384(
    const uint8_t *salt, size_t salt_len, const uint8_t *ikm, size_t ikm_len, uint8_t prk[48]) {
    if (!prk) {
        rt_trap("HKDF-SHA384-Extract: output buffer is null");
        return;
    }
    if (!salt && salt_len > 0) {
        rt_trap("HKDF-SHA384-Extract: salt buffer is null");
        return;
    }
    if (!ikm && ikm_len > 0) {
        rt_trap("HKDF-SHA384-Extract: input key material buffer is null");
        return;
    }
    if (salt == NULL || salt_len == 0) {
        uint8_t zero_salt[48] = {0};
        rt_hmac_sha384(zero_salt, sizeof(zero_salt), ikm, ikm_len, prk);
        rt_secure_zero(zero_salt, sizeof(zero_salt));
    } else {
        rt_hmac_sha384(salt, salt_len, ikm, ikm_len, prk);
    }
}

/// @brief HKDF-Expand over HMAC-SHA-384 (RFC 5869 §2.3, hash = SHA-384).
/// @details Iterates @c T(n) = HMAC-SHA-384(PRK, T(n-1) || info || n)
///          and copies up to 48 bytes per iteration into @p okm.
///          Allocates a temporary scratch buffer for the concatenated
///          input rather than streaming the HMAC update — keeps the
///          implementation small at the cost of one malloc/free per
///          iteration. Cap is @c 255 * 48 = 12240 bytes.
/// @param prk 48-byte pseudorandom key from SHA-384 HKDF-Extract.
/// @param info Optional application context, or NULL only when @p info_len is zero.
/// @param info_len Number of context bytes.
/// @param okm Output keying-material buffer, or NULL only for zero output length.
/// @param okm_len Requested output length in bytes.
/// @return 0 on success; -1 on bad argument, OOM, or oversize @p okm_len.
int rt_hkdf_expand_sha384(
    const uint8_t prk[48], const uint8_t *info, size_t info_len, uint8_t *okm, size_t okm_len) {
    uint8_t t[48] = {0};
    size_t t_len = 0;
    uint8_t counter = 1;
    size_t pos = 0;
    int rc = -1;

    if (!prk || (!okm && okm_len > 0))
        return -1;
    if (!info && info_len > 0)
        return -1;
    if (okm_len > RT_HKDF_SHA384_MAX_OKM_LEN)
        return -1;

    while (pos < okm_len) {
        if (t_len > SIZE_MAX - info_len - 1)
            goto done;
        size_t msg_len = t_len + info_len + 1;
        uint8_t *msg = (uint8_t *)malloc(msg_len > 0 ? msg_len : 1);
        if (!msg)
            goto done;
        size_t msg_pos = 0;
        if (t_len > 0) {
            memcpy(msg + msg_pos, t, t_len);
            msg_pos += t_len;
        }
        if (info_len > 0) {
            memcpy(msg + msg_pos, info, info_len);
            msg_pos += info_len;
        }
        msg[msg_pos++] = counter;
        rt_hmac_sha384(prk, 48, msg, msg_pos, t);
        rt_secure_zero(msg, msg_len);
        free(msg);
        t_len = 48;

        size_t copy = okm_len - pos;
        if (copy > 48)
            copy = 48;
        memcpy(okm + pos, t, copy);
        pos += copy;
        counter++;
    }
    rc = 0;
done:
    rt_secure_zero(t, sizeof(t));
    return rc;
}

/// @brief TLS 1.3 HKDF-Expand-Label using HMAC-SHA-384.
/// @details Mirrors @ref rt_hkdf_expand_label but encodes the label list
///          for the SHA-384 PRK path. The 48-byte secret + 6-byte
///          "tls13 " prefix + label + context all fit inside the
///          stack-resident 512-byte scratch buffer.
/// @param secret 48-byte secret used as the HKDF pseudorandom key.
/// @param label NUL-terminated label without the `"tls13 "` prefix.
/// @param context Optional context bytes, or NULL only when @p context_len is zero.
/// @param context_len Number of context bytes.
/// @param out Output keying-material buffer, or NULL only for zero output length.
/// @param out_len Requested output length encoded in the two-byte TLS field.
/// @return 0 on success; -1 on bad input or oversize encoding.
int rt_hkdf_expand_label_sha384(const uint8_t secret[48],
                                const char *label,
                                const uint8_t *context,
                                size_t context_len,
                                uint8_t *out,
                                size_t out_len) {
    uint8_t hkdf_label[512];
    size_t pos = 0;

    if (!secret || !label || (!out && out_len > 0))
        return -1;
    if (!context && context_len > 0)
        return -1;
    if (out_len > RT_HKDF_SHA384_MAX_OKM_LEN || out_len > UINT16_MAX)
        return -1;

    hkdf_label[pos++] = (uint8_t)(out_len >> 8);
    hkdf_label[pos++] = (uint8_t)out_len;

    const char *prefix = "tls13 ";
    size_t prefix_len = 6;
    size_t label_len = strlen(label);
    if (!tls13_hkdf_label_lengths_valid(prefix_len, label_len, context_len, sizeof(hkdf_label)))
        return -1;

    hkdf_label[pos++] = (uint8_t)(prefix_len + label_len);
    memcpy(hkdf_label + pos, prefix, prefix_len);
    pos += prefix_len;
    memcpy(hkdf_label + pos, label, label_len);
    pos += label_len;
    hkdf_label[pos++] = (uint8_t)context_len;
    if (context_len > 0) {
        memcpy(hkdf_label + pos, context, context_len);
        pos += context_len;
    }

    return rt_hkdf_expand_sha384(secret, hkdf_label, pos, out, out_len);
}

//=============================================================================
// ChaCha20
//=============================================================================

#define ROTL32(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

#define QUARTERROUND(a, b, c, d)                                                                   \
    do {                                                                                           \
        a += b;                                                                                    \
        d ^= a;                                                                                    \
        d = ROTL32(d, 16);                                                                         \
        c += d;                                                                                    \
        b ^= c;                                                                                    \
        b = ROTL32(b, 12);                                                                         \
        a += b;                                                                                    \
        d ^= a;                                                                                    \
        d = ROTL32(d, 8);                                                                          \
        c += d;                                                                                    \
        b ^= c;                                                                                    \
        b = ROTL32(b, 7);                                                                          \
    } while (0)

/// @brief Compute one 64-byte ChaCha20 keystream block from `state` (RFC 8439).
///
/// 20 rounds = 10 column-round pairs of `QUARTERROUND`s, then add
/// the original state back to thwart the trivial inverse, and
/// serialise little-endian.
/// @param state Sixteen-word input state; not modified.
/// @param out Output buffer receiving 64 keystream bytes.
static void chacha20_block(const uint32_t state[16], uint8_t out[64]) {
    uint32_t x[16];
    memcpy(x, state, 64);

    for (int i = 0; i < 10; i++) {
        // Column rounds
        QUARTERROUND(x[0], x[4], x[8], x[12]);
        QUARTERROUND(x[1], x[5], x[9], x[13]);
        QUARTERROUND(x[2], x[6], x[10], x[14]);
        QUARTERROUND(x[3], x[7], x[11], x[15]);
        // Diagonal rounds
        QUARTERROUND(x[0], x[5], x[10], x[15]);
        QUARTERROUND(x[1], x[6], x[11], x[12]);
        QUARTERROUND(x[2], x[7], x[8], x[13]);
        QUARTERROUND(x[3], x[4], x[9], x[14]);
    }

    for (int i = 0; i < 16; i++)
        x[i] += state[i];

    // Output little-endian
    for (int i = 0; i < 16; i++) {
        out[i * 4 + 0] = x[i] & 0xFF;
        out[i * 4 + 1] = (x[i] >> 8) & 0xFF;
        out[i * 4 + 2] = (x[i] >> 16) & 0xFF;
        out[i * 4 + 3] = (x[i] >> 24) & 0xFF;
    }
}

/// @brief Lay out the ChaCha20 starting state per RFC 8439 §2.3.
///
///   words[0..3]  = "expand 32-byte k" sigma constants
///   words[4..11] = 256-bit key (little-endian)
///   words[12]    = block counter
///   words[13..15] = 96-bit nonce (little-endian)
/// @param state Sixteen-word output state.
/// @param key 32-byte ChaCha20 key.
/// @param nonce 12-byte IETF nonce.
/// @param counter Initial 32-bit block counter.
static void chacha20_init(uint32_t state[16],
                          const uint8_t key[32],
                          const uint8_t nonce[12],
                          uint32_t counter) {
    // "expand 32-byte k"
    state[0] = 0x61707865;
    state[1] = 0x3320646e;
    state[2] = 0x79622d32;
    state[3] = 0x6b206574;

    // Key (little-endian)
    for (int i = 0; i < 8; i++) {
        state[4 + i] = ((uint32_t)key[i * 4 + 0]) | ((uint32_t)key[i * 4 + 1] << 8) |
                       ((uint32_t)key[i * 4 + 2] << 16) | ((uint32_t)key[i * 4 + 3] << 24);
    }

    // Counter
    state[12] = counter;

    // Nonce (little-endian)
    for (int i = 0; i < 3; i++) {
        state[13 + i] = ((uint32_t)nonce[i * 4 + 0]) | ((uint32_t)nonce[i * 4 + 1] << 8) |
                        ((uint32_t)nonce[i * 4 + 2] << 16) | ((uint32_t)nonce[i * 4 + 3] << 24);
    }
}

/// @brief XOR the ChaCha20 keystream into `in` to produce `out` (encrypt = decrypt).
///
/// Generates one 64-byte keystream block per iteration, advancing
/// the block counter. Aborts the loop if the counter wraps to zero
/// (256 GiB with the same key+nonce) so we never reuse keystream.
/// @param key 32-byte ChaCha20 key.
/// @param nonce 12-byte IETF nonce.
/// @param counter Initial block counter.
/// @param in Input bytes.
/// @param out Output buffer; may alias @p in.
/// @param len Number of bytes to transform.
/// @return 0 on success; -1 if more data remains after counter wrap.
static int chacha20_crypt(const uint8_t key[32],
                          const uint8_t nonce[12],
                          uint32_t counter,
                          const uint8_t *in,
                          uint8_t *out,
                          size_t len) {
    uint32_t state[16];
    uint8_t keystream[64];

    chacha20_init(state, key, nonce, counter);

    while (len > 0) {
        chacha20_block(state, keystream);
        size_t use = len > 64 ? 64 : len;
        for (size_t i = 0; i < use; i++)
            out[i] = in[i] ^ keystream[i];
        in += use;
        out += use;
        len -= use;
        // Increment counter; abort if it wraps to prevent keystream reuse
        if (++state[12] == 0 && len > 0)
            return -1;
    }
    return 0;
}

//=============================================================================
// Poly1305
//=============================================================================

typedef struct {
    uint32_t r[5];
    uint32_t h[5];
    uint32_t pad[4];
    uint8_t buffer[16];
    size_t buffer_len;
} poly1305_ctx;

/// @brief Initialise a Poly1305 MAC context from a 32-byte one-time key.
///
/// Splits the key into the multiplier `r` (clamped per RFC 8439
/// §2.5.1 to ensure modular arithmetic stays bounded) and the
/// final additive `pad`. The accumulator `h` starts at zero.
/// @param ctx Poly1305 context to initialize.
/// @param key 32-byte one-time authentication key.
static void poly1305_init(poly1305_ctx *ctx, const uint8_t key[32]) {
    // r (first 16 bytes, clamped)
    uint32_t t0 = load32_le(key);
    uint32_t t1 = load32_le(key + 4);
    uint32_t t2 = load32_le(key + 8);
    uint32_t t3 = load32_le(key + 12);
    ctx->r[0] = t0 & 0x3ffffff;
    ctx->r[1] = ((t0 >> 26) | (t1 << 6)) & 0x3ffff03;
    ctx->r[2] = ((t1 >> 20) | (t2 << 12)) & 0x3ffc0ff;
    ctx->r[3] = ((t2 >> 14) | (t3 << 18)) & 0x3f03fff;
    ctx->r[4] = (t3 >> 8) & 0x00fffff;

    // h = 0
    ctx->h[0] = ctx->h[1] = ctx->h[2] = ctx->h[3] = ctx->h[4] = 0;

    // pad (last 16 bytes)
    ctx->pad[0] = load32_le(key + 16);
    ctx->pad[1] = load32_le(key + 20);
    ctx->pad[2] = load32_le(key + 24);
    ctx->pad[3] = load32_le(key + 28);

    ctx->buffer_len = 0;
}

/// @brief Absorb whole 16-byte blocks into the Poly1305 accumulator.
///
/// Implements the inner loop `h = (h + m_i) * r mod 2^130 - 5`
/// using a 26-bit-limb representation that lets us carry with
/// 64-bit intermediates. The `final` flag suppresses the implicit
/// 0x01 high bit on the last (padded) block; the public
/// `poly1305_final` sets `final=1` for the partial tail.
/// @param ctx Initialized Poly1305 context to update.
/// @param data Input containing at least @p len complete-block bytes.
/// @param len Number of bytes to absorb; only complete 16-byte blocks are consumed.
/// @param final Non-zero to suppress the implicit high bit for a padded tail.
static void poly1305_blocks(poly1305_ctx *ctx, const uint8_t *data, size_t len, int final) {
    uint32_t r0 = ctx->r[0], r1 = ctx->r[1], r2 = ctx->r[2], r3 = ctx->r[3], r4 = ctx->r[4];
    uint32_t s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;
    uint32_t h0 = ctx->h[0], h1 = ctx->h[1], h2 = ctx->h[2], h3 = ctx->h[3], h4 = ctx->h[4];
    uint32_t hibit = final ? 0 : (1 << 24);

    while (len >= 16) {
        // h += m[i]
        h0 += ((uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) |
               ((uint32_t)data[3] << 24)) &
              0x3ffffff;
        h1 += (((uint32_t)data[3] >> 2) | ((uint32_t)data[4] << 6) | ((uint32_t)data[5] << 14) |
               ((uint32_t)data[6] << 22)) &
              0x3ffffff;
        h2 += (((uint32_t)data[6] >> 4) | ((uint32_t)data[7] << 4) | ((uint32_t)data[8] << 12) |
               ((uint32_t)data[9] << 20)) &
              0x3ffffff;
        h3 += (((uint32_t)data[9] >> 6) | ((uint32_t)data[10] << 2) | ((uint32_t)data[11] << 10) |
               ((uint32_t)data[12] << 18)) &
              0x3ffffff;
        h4 += (((uint32_t)data[12] >> 8) | ((uint32_t)data[13]) | ((uint32_t)data[14] << 8) |
               ((uint32_t)data[15] << 16)) |
              hibit;

        // h *= r
        uint64_t d0 = (uint64_t)h0 * r0 + (uint64_t)h1 * s4 + (uint64_t)h2 * s3 +
                      (uint64_t)h3 * s2 + (uint64_t)h4 * s1;
        uint64_t d1 = (uint64_t)h0 * r1 + (uint64_t)h1 * r0 + (uint64_t)h2 * s4 +
                      (uint64_t)h3 * s3 + (uint64_t)h4 * s2;
        uint64_t d2 = (uint64_t)h0 * r2 + (uint64_t)h1 * r1 + (uint64_t)h2 * r0 +
                      (uint64_t)h3 * s4 + (uint64_t)h4 * s3;
        uint64_t d3 = (uint64_t)h0 * r3 + (uint64_t)h1 * r2 + (uint64_t)h2 * r1 +
                      (uint64_t)h3 * r0 + (uint64_t)h4 * s4;
        uint64_t d4 = (uint64_t)h0 * r4 + (uint64_t)h1 * r3 + (uint64_t)h2 * r2 +
                      (uint64_t)h3 * r1 + (uint64_t)h4 * r0;

        // Carry
        uint32_t c;
        c = (uint32_t)(d0 >> 26);
        h0 = (uint32_t)d0 & 0x3ffffff;
        d1 += c;
        c = (uint32_t)(d1 >> 26);
        h1 = (uint32_t)d1 & 0x3ffffff;
        d2 += c;
        c = (uint32_t)(d2 >> 26);
        h2 = (uint32_t)d2 & 0x3ffffff;
        d3 += c;
        c = (uint32_t)(d3 >> 26);
        h3 = (uint32_t)d3 & 0x3ffffff;
        d4 += c;
        c = (uint32_t)(d4 >> 26);
        h4 = (uint32_t)d4 & 0x3ffffff;
        h0 += c * 5;
        c = h0 >> 26;
        h0 &= 0x3ffffff;
        h1 += c;

        data += 16;
        len -= 16;
    }

    ctx->h[0] = h0;
    ctx->h[1] = h1;
    ctx->h[2] = h2;
    ctx->h[3] = h3;
    ctx->h[4] = h4;
}

/// @brief Stream `len` bytes through Poly1305, buffering partial 16-byte blocks.
/// @param ctx Initialized Poly1305 context to update.
/// @param data Input bytes, or NULL only when @p len is zero.
/// @param len Number of bytes to absorb.
static void poly1305_update(poly1305_ctx *ctx, const void *data, size_t len) {
    if (len == 0)
        return;
    if (!data) {
        rt_trap("Poly1305: input buffer is null");
        return;
    }
    const uint8_t *ptr = (const uint8_t *)data;

    if (ctx->buffer_len > 0) {
        size_t need = 16 - ctx->buffer_len;
        if (len < need) {
            memcpy(ctx->buffer + ctx->buffer_len, ptr, len);
            ctx->buffer_len += len;
            return;
        }
        memcpy(ctx->buffer + ctx->buffer_len, ptr, need);
        poly1305_blocks(ctx, ctx->buffer, 16, 0);
        ptr += need;
        len -= need;
        ctx->buffer_len = 0;
    }

    if (len >= 16) {
        size_t blocks = len & ~(size_t)15;
        poly1305_blocks(ctx, ptr, blocks, 0);
        ptr += blocks;
        len -= blocks;
    }

    if (len > 0) {
        memcpy(ctx->buffer, ptr, len);
        ctx->buffer_len = len;
    }
}

/// @brief Pad, freeze the modular reduction, add the secret pad, emit a 16-byte tag.
///
/// Three-step finalisation per RFC 8439 §2.5:
///   1. Append `0x01` and zero-pad the partial block, then absorb.
///   2. Fully reduce `h` mod 2^130 - 5 in constant time using the
///      "freeze" trick (compute `h - p` and conditionally select).
///   3. Add `pad` (a one-time 128-bit value derived from the
///      ChaCha20 keystream) and serialise little-endian.
/// @param ctx Poly1305 context to finalize.
/// @param tag Output buffer receiving the 16-byte authentication tag.
static void poly1305_final(poly1305_ctx *ctx, uint8_t tag[16]) {
    // Process remaining bytes
    if (ctx->buffer_len > 0) {
        ctx->buffer[ctx->buffer_len++] = 1;
        while (ctx->buffer_len < 16)
            ctx->buffer[ctx->buffer_len++] = 0;
        poly1305_blocks(ctx, ctx->buffer, 16, 1);
    }

    // Freeze h
    uint32_t h0 = ctx->h[0], h1 = ctx->h[1], h2 = ctx->h[2], h3 = ctx->h[3], h4 = ctx->h[4];

    uint32_t c = h1 >> 26;
    h1 &= 0x3ffffff;
    h2 += c;
    c = h2 >> 26;
    h2 &= 0x3ffffff;
    h3 += c;
    c = h3 >> 26;
    h3 &= 0x3ffffff;
    h4 += c;
    c = h4 >> 26;
    h4 &= 0x3ffffff;
    h0 += c * 5;
    c = h0 >> 26;
    h0 &= 0x3ffffff;
    h1 += c;

    // Compute h - p
    uint32_t g0 = h0 + 5;
    c = g0 >> 26;
    g0 &= 0x3ffffff;
    uint32_t g1 = h1 + c;
    c = g1 >> 26;
    g1 &= 0x3ffffff;
    uint32_t g2 = h2 + c;
    c = g2 >> 26;
    g2 &= 0x3ffffff;
    uint32_t g3 = h3 + c;
    c = g3 >> 26;
    g3 &= 0x3ffffff;
    uint32_t g4 = h4 + c - (1 << 26);

    // Select h or h-p based on carry
    uint32_t mask = (g4 >> 31) - 1;
    g0 &= mask;
    g1 &= mask;
    g2 &= mask;
    g3 &= mask;
    g4 &= mask;
    mask = ~mask;
    h0 = (h0 & mask) | g0;
    h1 = (h1 & mask) | g1;
    h2 = (h2 & mask) | g2;
    h3 = (h3 & mask) | g3;
    h4 = (h4 & mask) | g4;

    // h = h + pad
    uint64_t f;
    f = (uint64_t)h0 + (h1 << 26) + ctx->pad[0];
    tag[0] = f & 0xFF;
    tag[1] = (f >> 8) & 0xFF;
    tag[2] = (f >> 16) & 0xFF;
    tag[3] = (f >> 24) & 0xFF;
    f = (f >> 32) + (uint64_t)(h1 >> 6) + (h2 << 20) + ctx->pad[1];
    tag[4] = f & 0xFF;
    tag[5] = (f >> 8) & 0xFF;
    tag[6] = (f >> 16) & 0xFF;
    tag[7] = (f >> 24) & 0xFF;
    f = (f >> 32) + (uint64_t)(h2 >> 12) + (h3 << 14) + ctx->pad[2];
    tag[8] = f & 0xFF;
    tag[9] = (f >> 8) & 0xFF;
    tag[10] = (f >> 16) & 0xFF;
    tag[11] = (f >> 24) & 0xFF;
    f = (f >> 32) + (uint64_t)(h3 >> 18) + (h4 << 8) + ctx->pad[3];
    tag[12] = f & 0xFF;
    tag[13] = (f >> 8) & 0xFF;
    tag[14] = (f >> 16) & 0xFF;
    tag[15] = (f >> 24) & 0xFF;
}

//=============================================================================
// ChaCha20-Poly1305 AEAD
//=============================================================================

/// @brief Absorb zero-padding to bring `len`'s contribution to a 16-byte boundary.
///
/// Required by the AEAD MAC construction (RFC 8439 §2.8): each
/// component (AAD, ciphertext) is padded to a 16-byte multiple
/// before the next is absorbed so an attacker can't slide bytes
/// between fields.
/// @param ctx Poly1305 context receiving the zero padding.
/// @param len Unpadded component length in bytes.
static void pad16(poly1305_ctx *ctx, size_t len) {
    size_t pad = (16 - (len & 15)) & 15;
    uint8_t zeros[16] = {0};
    if (pad > 0)
        poly1305_update(ctx, zeros, pad);
}

/// @brief AEAD encrypt (RFC 8439 §2.8.1) with ChaCha20 + Poly1305.
///
/// Generates the one-time Poly1305 key from ChaCha20 block 0,
/// encrypts the plaintext starting at block 1, then computes a
/// 16-byte tag over `aad ‖ pad16 ‖ ct ‖ pad16 ‖ aad_len_le64 ‖
/// ct_len_le64`. The tag is appended to `ciphertext` so the output
/// length is `plaintext_len + 16`.
/// @param key 32-byte encryption key.
/// @param nonce Unique 12-byte nonce for this key.
/// @param aad Additional authenticated data, or NULL only when @p aad_len is zero.
/// @param aad_len Number of AAD bytes.
/// @param plaintext Input bytes, or NULL only when @p plaintext_len is zero.
/// @param plaintext_len Number of plaintext bytes.
/// @param ciphertext Output buffer; must have room for `plaintext_len + 16` bytes.
/// @return Total bytes written, or 0 for invalid input, counter exhaustion, or
///         a plaintext length beyond the safety cap.
size_t rt_chacha20_poly1305_encrypt(const uint8_t key[32],
                                    const uint8_t nonce[12],
                                    const void *aad,
                                    size_t aad_len,
                                    const void *plaintext,
                                    size_t plaintext_len,
                                    uint8_t *ciphertext) {
    if (!key || !nonce || !ciphertext)
        return 0;
    if ((!aad && aad_len > 0) || (!plaintext && plaintext_len > 0))
        return 0;
    if (plaintext_len > SIZE_MAX - 16)
        return 0;
    if ((uint64_t)plaintext_len > RT_CHACHA20_MAX_BYTES)
        return 0;

    // Generate Poly1305 key (block 0)
    uint8_t poly_key[64] = {0};
    uint8_t zeros[64] = {0};
    if (chacha20_crypt(key, nonce, 0, zeros, poly_key, 64) != 0) {
        rt_secure_zero(poly_key, sizeof(poly_key));
        return 0;
    }

    // Encrypt plaintext (starting at block 1)
    if (chacha20_crypt(key, nonce, 1, (const uint8_t *)plaintext, ciphertext, plaintext_len) != 0) {
        rt_secure_zero(poly_key, sizeof(poly_key));
        return 0;
    }

    // Compute tag
    poly1305_ctx poly;
    poly1305_init(&poly, poly_key);
    poly1305_update(&poly, aad, aad_len);
    pad16(&poly, aad_len);
    poly1305_update(&poly, ciphertext, plaintext_len);
    pad16(&poly, plaintext_len);

    // Lengths (little-endian)
    uint8_t lens[16];
    store64_le(lens, (uint64_t)aad_len);
    store64_le(lens + 8, (uint64_t)plaintext_len);
    poly1305_update(&poly, lens, 16);

    poly1305_final(&poly, ciphertext + plaintext_len);

    // Zero key material to prevent stack residue
    rt_secure_zero(&poly, sizeof(poly));
    rt_secure_zero(lens, sizeof(lens));
    rt_secure_zero(poly_key, sizeof(poly_key));

    return plaintext_len + 16;
}

/// @brief AEAD decrypt for ChaCha20-Poly1305 (RFC 8439 §2.8.2).
///
/// Verifies the trailing 16-byte tag against the recomputed
/// Poly1305 MAC in constant time. Only on tag match is the
/// ciphertext stripped of the tag and decrypted into `plaintext`.
/// On mismatch nothing is written to `plaintext` and -1 is returned.
/// @param key 32-byte decryption key.
/// @param nonce 12-byte nonce used for encryption.
/// @param aad Additional authenticated data, or NULL only when @p aad_len is zero.
/// @param aad_len Number of AAD bytes.
/// @param ciphertext Ciphertext bytes followed by the 16-byte tag.
/// @param ciphertext_len Includes the 16-byte tag.
/// @param plaintext Output buffer for `ciphertext_len - 16` bytes; may be NULL
///                  only for an empty plaintext.
/// @return Plaintext length on success, -1 on tag failure / too-short input.
long rt_chacha20_poly1305_decrypt(const uint8_t key[32],
                                  const uint8_t nonce[12],
                                  const void *aad,
                                  size_t aad_len,
                                  const void *ciphertext,
                                  size_t ciphertext_len,
                                  uint8_t *plaintext) {
    if (!key || !nonce || !ciphertext)
        return -1;
    if ((!aad && aad_len > 0) || (!plaintext && ciphertext_len > 16))
        return -1;
    if (ciphertext_len < 16)
        return -1;

    size_t data_len = ciphertext_len - 16;
    if ((uint64_t)data_len > RT_CHACHA20_MAX_BYTES)
        return -1;
    const uint8_t *tag = (const uint8_t *)ciphertext + data_len;

    // Generate Poly1305 key
    uint8_t poly_key[64] = {0};
    uint8_t zeros[64] = {0};
    if (chacha20_crypt(key, nonce, 0, zeros, poly_key, 64) != 0) {
        rt_secure_zero(poly_key, sizeof(poly_key));
        return -1;
    }

    // Verify tag
    poly1305_ctx poly;
    poly1305_init(&poly, poly_key);
    poly1305_update(&poly, aad, aad_len);
    pad16(&poly, aad_len);
    poly1305_update(&poly, ciphertext, data_len);
    pad16(&poly, data_len);

    uint8_t lens[16];
    store64_le(lens, (uint64_t)aad_len);
    store64_le(lens + 8, (uint64_t)data_len);
    poly1305_update(&poly, lens, 16);

    uint8_t computed_tag[16];
    poly1305_final(&poly, computed_tag);
    rt_secure_zero(&poly, sizeof(poly));
    rt_secure_zero(lens, sizeof(lens));

    // Constant-time comparison
    uint8_t diff = 0;
    for (int i = 0; i < 16; i++)
        diff |= computed_tag[i] ^ tag[i];
    rt_secure_zero(computed_tag, sizeof(computed_tag));
    if (diff != 0) {
        rt_secure_zero(&diff, sizeof(diff));
        rt_secure_zero(poly_key, sizeof(poly_key));
        return -1;
    }
    rt_secure_zero(&diff, sizeof(diff));

    // Decrypt
    if (chacha20_crypt(key, nonce, 1, (const uint8_t *)ciphertext, plaintext, data_len) != 0) {
        rt_secure_zero(poly_key, sizeof(poly_key));
        return -1;
    }

    // Zero key material to prevent stack residue
    rt_secure_zero(poly_key, sizeof(poly_key));

    return (long)data_len;
}

//=============================================================================
// AES-128 Block Cipher (FIPS 197)
//=============================================================================

static const uint8_t aes_sbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16};

static const uint8_t aes_rcon[10] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36};

/// @brief Expand AES-128 key (16 bytes) into 11 round keys (176 bytes).
/// @param key 16-byte cipher key.
/// @param rk Output buffer receiving 176 bytes of round-key material.
static void aes128_key_expand(const uint8_t key[16], uint8_t rk[176]) {
    memcpy(rk, key, 16);
    for (int i = 4; i < 44; i++) {
        uint8_t tmp[4];
        memcpy(tmp, rk + (i - 1) * 4, 4);
        if (i % 4 == 0) {
            uint8_t t = tmp[0];
            tmp[0] = aes_sbox[tmp[1]] ^ aes_rcon[i / 4 - 1];
            tmp[1] = aes_sbox[tmp[2]];
            tmp[2] = aes_sbox[tmp[3]];
            tmp[3] = aes_sbox[t];
        }
        for (int j = 0; j < 4; j++)
            rk[i * 4 + j] = rk[(i - 4) * 4 + j] ^ tmp[j];
    }
}

/// @brief Expand AES-256 key (32 bytes) into 15 round keys (240 bytes).
/// @param key 32-byte cipher key.
/// @param rk Output buffer receiving 240 bytes of round-key material.
static void aes256_key_expand(const uint8_t key[32], uint8_t rk[240]) {
    memcpy(rk, key, 32);
    for (int i = 8; i < 60; i++) {
        uint8_t tmp[4];
        memcpy(tmp, rk + (i - 1) * 4, 4);
        if (i % 8 == 0) {
            uint8_t t = tmp[0];
            tmp[0] = aes_sbox[tmp[1]] ^ aes_rcon[i / 8 - 1];
            tmp[1] = aes_sbox[tmp[2]];
            tmp[2] = aes_sbox[tmp[3]];
            tmp[3] = aes_sbox[t];
        } else if (i % 8 == 4) {
            tmp[0] = aes_sbox[tmp[0]];
            tmp[1] = aes_sbox[tmp[1]];
            tmp[2] = aes_sbox[tmp[2]];
            tmp[3] = aes_sbox[tmp[3]];
        }
        for (int j = 0; j < 4; j++)
            rk[i * 4 + j] = rk[(i - 8) * 4 + j] ^ tmp[j];
    }
}

/// @brief xtime: multiply by 2 in GF(2^8) with AES reducing polynomial.
/// @param x Field element to multiply.
/// @return Product `x * 2` reduced by the AES polynomial.
static uint8_t aes_xtime(uint8_t x) {
    return (uint8_t)((x << 1) ^ ((x >> 7) * 0x1b));
}

/// @brief AES encrypt one 16-byte block with an expanded key.
/// @param rk Expanded round-key bytes.
/// @param rounds AES round count: 10 for AES-128 or 14 for AES-256.
/// @param in Sixteen-byte plaintext block.
/// @param out Sixteen-byte ciphertext block; may alias @p in.
static void aes_encrypt_block_generic(const uint8_t *rk,
                                      int rounds,
                                      const uint8_t in[16],
                                      uint8_t out[16]) {
    uint8_t s[16];
    memcpy(s, in, 16);

    // AddRoundKey (round 0)
    for (int i = 0; i < 16; i++)
        s[i] ^= rk[i];

    // Middle rounds: SubBytes, ShiftRows, MixColumns, AddRoundKey
    for (int round = 1; round < rounds; round++) {
        uint8_t t[16];
        // SubBytes
        for (int i = 0; i < 16; i++)
            t[i] = aes_sbox[s[i]];
        // ShiftRows
        s[0] = t[0];
        s[1] = t[5];
        s[2] = t[10];
        s[3] = t[15];
        s[4] = t[4];
        s[5] = t[9];
        s[6] = t[14];
        s[7] = t[3];
        s[8] = t[8];
        s[9] = t[13];
        s[10] = t[2];
        s[11] = t[7];
        s[12] = t[12];
        s[13] = t[1];
        s[14] = t[6];
        s[15] = t[11];
        // MixColumns
        for (int c = 0; c < 4; c++) {
            int off = c * 4;
            uint8_t a0 = s[off], a1 = s[off + 1], a2 = s[off + 2], a3 = s[off + 3];
            uint8_t x0 = aes_xtime(a0), x1 = aes_xtime(a1);
            uint8_t x2 = aes_xtime(a2), x3 = aes_xtime(a3);
            s[off] = x0 ^ x1 ^ a1 ^ a2 ^ a3;
            s[off + 1] = a0 ^ x1 ^ x2 ^ a2 ^ a3;
            s[off + 2] = a0 ^ a1 ^ x2 ^ x3 ^ a3;
            s[off + 3] = x0 ^ a0 ^ a1 ^ a2 ^ x3;
        }
        // AddRoundKey
        for (int i = 0; i < 16; i++)
            s[i] ^= rk[round * 16 + i];
    }

    // Final round: SubBytes, ShiftRows, AddRoundKey (no MixColumns)
    {
        uint8_t t[16];
        for (int i = 0; i < 16; i++)
            t[i] = aes_sbox[s[i]];
        s[0] = t[0];
        s[1] = t[5];
        s[2] = t[10];
        s[3] = t[15];
        s[4] = t[4];
        s[5] = t[9];
        s[6] = t[14];
        s[7] = t[3];
        s[8] = t[8];
        s[9] = t[13];
        s[10] = t[2];
        s[11] = t[7];
        s[12] = t[12];
        s[13] = t[1];
        s[14] = t[6];
        s[15] = t[11];
        for (int i = 0; i < 16; i++)
            s[i] ^= rk[rounds * 16 + i];
    }

    memcpy(out, s, 16);
}

/// @brief AES-128 encrypt one 16-byte block.
/// @param rk Expanded 176-byte AES-128 round-key schedule.
/// @param in Sixteen-byte plaintext block.
/// @param out Sixteen-byte ciphertext block; may alias @p in.
static void aes128_encrypt_block(const uint8_t rk[176], const uint8_t in[16], uint8_t out[16]) {
    aes_encrypt_block_generic(rk, 10, in, out);
}

/// @brief AES-256 encrypt one 16-byte block.
/// @param rk Expanded 240-byte AES-256 round-key schedule.
/// @param in Sixteen-byte plaintext block.
/// @param out Sixteen-byte ciphertext block; may alias @p in.
static void aes256_encrypt_block(const uint8_t rk[240], const uint8_t in[16], uint8_t out[16]) {
    aes_encrypt_block_generic(rk, 14, in, out);
}

//=============================================================================
// AES-128-GCM AEAD (NIST SP 800-38D)
//=============================================================================

/// @brief Increment the rightmost 32 bits of a 128-bit counter (big-endian).
/// @param counter Mutable 16-byte counter block.
static void gcm_inc32(uint8_t counter[16]) {
    for (int i = 15; i >= 12; i--) {
        if (++counter[i] != 0)
            break;
    }
}

/// @brief GF(2^128) multiplication for GHASH.
/// @details Uses the reducing polynomial `x^128 + x^7 + x^2 + x + 1`,
///          represented by `0xE1` in the high byte.
/// @param H Sixteen-byte hash subkey field element.
/// @param X Sixteen-byte multiplicand field element.
/// @param out Output buffer receiving the 16-byte product.
static void ghash_mult(const uint8_t H[16], const uint8_t X[16], uint8_t out[16]) {
    uint8_t V[16], Z[16];
    memcpy(V, H, 16);
    memset(Z, 0, 16);

    for (int i = 0; i < 128; i++) {
        if ((X[i / 8] >> (7 - (i & 7))) & 1) {
            for (int j = 0; j < 16; j++)
                Z[j] ^= V[j];
        }
        // Right shift V by 1, apply reduction if LSB was set
        uint8_t carry = V[15] & 1;
        for (int j = 15; j > 0; j--)
            V[j] = (V[j] >> 1) | (V[j - 1] << 7);
        V[0] >>= 1;
        if (carry)
            V[0] ^= 0xE1; // reduction polynomial high byte
    }

    memcpy(out, Z, 16);
}

/// @brief GHASH over padded AAD + ciphertext + length block.
/// @param H Sixteen-byte GCM hash subkey.
/// @param aad Additional authenticated data, or NULL when @p aad_len is zero.
/// @param aad_len Number of AAD bytes.
/// @param ct Ciphertext bytes, or NULL when @p ct_len is zero.
/// @param ct_len Number of ciphertext bytes.
/// @param tag Output buffer receiving the 16-byte GHASH value.
static void ghash_compute(const uint8_t H[16],
                          const uint8_t *aad,
                          size_t aad_len,
                          const uint8_t *ct,
                          size_t ct_len,
                          uint8_t tag[16]) {
    uint8_t X[16];
    memset(X, 0, 16);
    uint8_t block[16];

    // Process AAD in 16-byte blocks
    size_t i;
    for (i = 0; i + 16 <= aad_len; i += 16) {
        for (int j = 0; j < 16; j++)
            block[j] = X[j] ^ aad[i + j];
        ghash_mult(H, block, X);
    }
    if (i < aad_len) {
        memset(block, 0, 16);
        for (size_t j = 0; j < aad_len - i; j++)
            block[j] = X[j] ^ aad[i + j];
        for (size_t j = aad_len - i; j < 16; j++)
            block[j] = X[j];
        ghash_mult(H, block, X);
    }

    // Process ciphertext in 16-byte blocks
    for (i = 0; i + 16 <= ct_len; i += 16) {
        for (int j = 0; j < 16; j++)
            block[j] = X[j] ^ ct[i + j];
        ghash_mult(H, block, X);
    }
    if (i < ct_len) {
        memset(block, 0, 16);
        for (size_t j = 0; j < ct_len - i; j++)
            block[j] = X[j] ^ ct[i + j];
        for (size_t j = ct_len - i; j < 16; j++)
            block[j] = X[j];
        ghash_mult(H, block, X);
    }

    // Length block: aad_len_bits (64) || ct_len_bits (64), big-endian
    uint64_t aad_bits = (uint64_t)aad_len * 8;
    uint64_t ct_bits = (uint64_t)ct_len * 8;
    memset(block, 0, 16);
    for (int j = 0; j < 8; j++) {
        block[j] = (uint8_t)(aad_bits >> (56 - j * 8));
        block[8 + j] = (uint8_t)(ct_bits >> (56 - j * 8));
    }
    for (int j = 0; j < 16; j++)
        block[j] ^= X[j];
    ghash_mult(H, block, tag);
}

/// @brief AES-128-GCM authenticated encryption (NIST SP 800-38D).
///
/// Pre-counter block J0 is `IV ‖ 0x00000001` (we only support
/// 12-byte IVs, the most common case). Encrypts the plaintext with
/// AES-CTR starting at `J0+1`, then computes the GHASH over
/// `aad ‖ pad || ct ‖ pad || aad_len_bits || ct_len_bits` and
/// XORs it with `AES(J0)` to form the 16-byte tag. The tag is
/// appended to `ciphertext`.
/// @param key 16-byte encryption key.
/// @param nonce Unique 12-byte nonce for this key.
/// @param aad Additional authenticated data, or NULL only when @p aad_len is zero.
/// @param aad_len Number of AAD bytes.
/// @param plaintext Input bytes, or NULL only when @p plaintext_len is zero.
/// @param plaintext_len Number of plaintext bytes.
/// @param ciphertext Output buffer; must have room for `plaintext_len + 16` bytes.
/// @return Total bytes written, or 0 for invalid input, unsafe lengths, or
///         plaintext beyond the GCM counter limit.
size_t rt_aes128_gcm_encrypt(const uint8_t key[16],
                             const uint8_t nonce[12],
                             const void *aad,
                             size_t aad_len,
                             const void *plaintext,
                             size_t plaintext_len,
                             uint8_t *ciphertext) {
    if (!key || !nonce || !ciphertext)
        return 0;
    if ((!aad && aad_len > 0) || (!plaintext && plaintext_len > 0))
        return 0;
    if (plaintext_len > SIZE_MAX - 16)
        return 0;
    if ((uint64_t)plaintext_len > RT_AES_GCM_MAX_BYTES)
        return 0;
    if (!gcm_lengths_valid(aad_len, plaintext_len))
        return 0;

    uint8_t rk[176];
    aes128_key_expand(key, rk);

    // H = AES(K, 0^128) — GHASH subkey
    uint8_t H[16] = {0};
    aes128_encrypt_block(rk, H, H);

    // J0 = nonce || 0x00000001  (initial counter for GCM)
    uint8_t J0[16];
    memcpy(J0, nonce, 12);
    J0[12] = 0;
    J0[13] = 0;
    J0[14] = 0;
    J0[15] = 1;

    // Encrypt plaintext with AES-CTR starting at J0+1
    uint8_t counter[16];
    memcpy(counter, J0, 16);
    gcm_inc32(counter); // counter starts at J0+1

    const uint8_t *pt = (const uint8_t *)plaintext;
    for (size_t i = 0; i < plaintext_len; i += 16) {
        uint8_t keystream[16];
        aes128_encrypt_block(rk, counter, keystream);
        size_t block_len = plaintext_len - i;
        if (block_len > 16)
            block_len = 16;
        for (size_t j = 0; j < block_len; j++)
            ciphertext[i + j] = pt[i + j] ^ keystream[j];
        rt_secure_zero(keystream, sizeof(keystream));
        gcm_inc32(counter);
    }

    // Compute GHASH tag over AAD and ciphertext
    uint8_t ghash_tag[16];
    ghash_compute(H, (const uint8_t *)aad, aad_len, ciphertext, plaintext_len, ghash_tag);

    // Final tag = GHASH XOR AES(K, J0)
    uint8_t enc_j0[16];
    aes128_encrypt_block(rk, J0, enc_j0);
    for (int i = 0; i < 16; i++)
        ciphertext[plaintext_len + i] = ghash_tag[i] ^ enc_j0[i];

    rt_secure_zero(rk, sizeof(rk));
    rt_secure_zero(H, sizeof(H));
    rt_secure_zero(J0, sizeof(J0));
    rt_secure_zero(counter, sizeof(counter));
    rt_secure_zero(ghash_tag, sizeof(ghash_tag));
    rt_secure_zero(enc_j0, sizeof(enc_j0));

    return plaintext_len + 16;
}

/// @brief AES-128-GCM authenticated decryption (NIST SP 800-38D).
///
/// Recomputes GHASH and `AES(J0)` to derive the expected tag, then
/// constant-time-compares it against the trailing 16 bytes of
/// `ciphertext`. On a match, performs CTR-mode decryption into
/// `plaintext`. On any failure (short input, tag mismatch),
/// returns -1 and writes nothing.
/// @param key 16-byte decryption key.
/// @param nonce 12-byte nonce used for encryption.
/// @param aad Additional authenticated data, or NULL only when @p aad_len is zero.
/// @param aad_len Number of AAD bytes.
/// @param ciphertext Ciphertext bytes followed by the 16-byte tag.
/// @param ciphertext_len Includes the 16-byte tag.
/// @param plaintext Output buffer for `ciphertext_len - 16` bytes; may be NULL
///                  only for an empty plaintext.
/// @return Plaintext length on success, -1 on failure.
long rt_aes128_gcm_decrypt(const uint8_t key[16],
                           const uint8_t nonce[12],
                           const void *aad,
                           size_t aad_len,
                           const void *ciphertext,
                           size_t ciphertext_len,
                           uint8_t *plaintext) {
    if (!key || !nonce || !ciphertext)
        return -1;
    if ((!aad && aad_len > 0) || (!plaintext && ciphertext_len > 16))
        return -1;
    if (ciphertext_len < 16)
        return -1;

    size_t data_len = ciphertext_len - 16;
    if ((uint64_t)data_len > RT_AES_GCM_MAX_BYTES)
        return -1;
    if (!gcm_lengths_valid(aad_len, data_len))
        return -1;
    const uint8_t *ct = (const uint8_t *)ciphertext;
    const uint8_t *recv_tag = ct + data_len;

    uint8_t rk[176];
    aes128_key_expand(key, rk);

    // H = AES(K, 0^128)
    uint8_t H[16] = {0};
    aes128_encrypt_block(rk, H, H);

    // J0 = nonce || 0x00000001
    uint8_t J0[16];
    memcpy(J0, nonce, 12);
    J0[12] = 0;
    J0[13] = 0;
    J0[14] = 0;
    J0[15] = 1;

    // Recompute GHASH tag and verify (before decryption, per GCM spec)
    uint8_t ghash_tag[16];
    ghash_compute(H, (const uint8_t *)aad, aad_len, ct, data_len, ghash_tag);

    uint8_t enc_j0[16];
    aes128_encrypt_block(rk, J0, enc_j0);
    uint8_t expected_tag[16];
    for (int i = 0; i < 16; i++)
        expected_tag[i] = ghash_tag[i] ^ enc_j0[i];

    // Constant-time tag comparison
    uint8_t diff = 0;
    for (int i = 0; i < 16; i++)
        diff |= expected_tag[i] ^ recv_tag[i];

    if (diff != 0) {
        rt_secure_zero(rk, sizeof(rk));
        rt_secure_zero(H, sizeof(H));
        rt_secure_zero(J0, sizeof(J0));
        rt_secure_zero(ghash_tag, sizeof(ghash_tag));
        rt_secure_zero(enc_j0, sizeof(enc_j0));
        rt_secure_zero(expected_tag, sizeof(expected_tag));
        return -1;
    }

    // Decrypt with AES-CTR starting at J0+1
    uint8_t counter[16];
    memcpy(counter, J0, 16);
    gcm_inc32(counter);

    for (size_t i = 0; i < data_len; i += 16) {
        uint8_t keystream[16];
        aes128_encrypt_block(rk, counter, keystream);
        size_t block_len = data_len - i;
        if (block_len > 16)
            block_len = 16;
        for (size_t j = 0; j < block_len; j++)
            plaintext[i + j] = ct[i + j] ^ keystream[j];
        rt_secure_zero(keystream, sizeof(keystream));
        gcm_inc32(counter);
    }

    rt_secure_zero(rk, sizeof(rk));
    rt_secure_zero(H, sizeof(H));
    rt_secure_zero(J0, sizeof(J0));
    rt_secure_zero(counter, sizeof(counter));
    rt_secure_zero(ghash_tag, sizeof(ghash_tag));
    rt_secure_zero(enc_j0, sizeof(enc_j0));
    rt_secure_zero(expected_tag, sizeof(expected_tag));

    return (long)data_len;
}

/// @brief AES-256-GCM authenticated encryption (NIST SP 800-38D, 256-bit key).
/// @details Mirrors @ref rt_aes128_gcm_encrypt but uses the AES-256
///          key schedule (14 rounds, 240-byte round-key buffer) and
///          calls @ref aes256_encrypt_block for both keystream
///          generation and the GHASH subkey derivation. The 12-byte
///          IV path is the only supported nonce shape.
/// @param key 32-byte encryption key.
/// @param nonce Unique 12-byte nonce for this key.
/// @param aad Additional authenticated data, or NULL only when @p aad_len is zero.
/// @param aad_len Number of AAD bytes.
/// @param plaintext Input bytes, or NULL only when @p plaintext_len is zero.
/// @param plaintext_len Number of plaintext bytes.
/// @param ciphertext Output buffer; must have room for plaintext_len + 16 bytes.
/// @return Total bytes written, or 0 for invalid input, unsafe lengths, or
///         plaintext beyond the GCM counter limit.
size_t rt_aes256_gcm_encrypt(const uint8_t key[32],
                             const uint8_t nonce[12],
                             const void *aad,
                             size_t aad_len,
                             const void *plaintext,
                             size_t plaintext_len,
                             uint8_t *ciphertext) {
    if (!key || !nonce || !ciphertext)
        return 0;
    if ((!aad && aad_len > 0) || (!plaintext && plaintext_len > 0))
        return 0;
    if (plaintext_len > SIZE_MAX - 16)
        return 0;
    if ((uint64_t)plaintext_len > RT_AES_GCM_MAX_BYTES)
        return 0;
    if (!gcm_lengths_valid(aad_len, plaintext_len))
        return 0;

    uint8_t rk[240];
    aes256_key_expand(key, rk);

    uint8_t H[16] = {0};
    aes256_encrypt_block(rk, H, H);

    uint8_t J0[16];
    memcpy(J0, nonce, 12);
    J0[12] = 0;
    J0[13] = 0;
    J0[14] = 0;
    J0[15] = 1;

    uint8_t counter[16];
    memcpy(counter, J0, 16);
    gcm_inc32(counter);

    const uint8_t *pt = (const uint8_t *)plaintext;
    for (size_t i = 0; i < plaintext_len; i += 16) {
        uint8_t keystream[16];
        aes256_encrypt_block(rk, counter, keystream);
        size_t block_len = plaintext_len - i;
        if (block_len > 16)
            block_len = 16;
        for (size_t j = 0; j < block_len; j++)
            ciphertext[i + j] = pt[i + j] ^ keystream[j];
        rt_secure_zero(keystream, sizeof(keystream));
        gcm_inc32(counter);
    }

    uint8_t ghash_tag[16];
    ghash_compute(H, (const uint8_t *)aad, aad_len, ciphertext, plaintext_len, ghash_tag);

    uint8_t enc_j0[16];
    aes256_encrypt_block(rk, J0, enc_j0);
    for (int i = 0; i < 16; i++)
        ciphertext[plaintext_len + i] = ghash_tag[i] ^ enc_j0[i];

    rt_secure_zero(rk, sizeof(rk));
    rt_secure_zero(H, sizeof(H));
    rt_secure_zero(J0, sizeof(J0));
    rt_secure_zero(counter, sizeof(counter));
    rt_secure_zero(ghash_tag, sizeof(ghash_tag));
    rt_secure_zero(enc_j0, sizeof(enc_j0));

    return plaintext_len + 16;
}

/// @brief AES-256-GCM authenticated decryption.
/// @details Recomputes the GHASH tag, compares it against the trailing
///          16 bytes of @p ciphertext in constant time, and only on
///          tag match decrypts the leading data into @p plaintext.
///          Returns -1 on tag failure or too-short input; on success
///          returns the plaintext length (@c ciphertext_len - 16).
/// @param key 32-byte decryption key.
/// @param nonce 12-byte nonce used for encryption.
/// @param aad Additional authenticated data, or NULL only when @p aad_len is zero.
/// @param aad_len Number of AAD bytes.
/// @param ciphertext Ciphertext bytes followed by the 16-byte tag.
/// @param ciphertext_len Total ciphertext length including the tag.
/// @param plaintext Output buffer for `ciphertext_len - 16` bytes; may be NULL
///                  only for an empty plaintext.
/// @return Plaintext length on success; -1 for invalid input, unsafe lengths,
///         counter-limit violation, or authentication failure.
long rt_aes256_gcm_decrypt(const uint8_t key[32],
                           const uint8_t nonce[12],
                           const void *aad,
                           size_t aad_len,
                           const void *ciphertext,
                           size_t ciphertext_len,
                           uint8_t *plaintext) {
    if (!key || !nonce || !ciphertext)
        return -1;
    if ((!aad && aad_len > 0) || (!plaintext && ciphertext_len > 16))
        return -1;
    if (ciphertext_len < 16)
        return -1;

    size_t data_len = ciphertext_len - 16;
    if ((uint64_t)data_len > RT_AES_GCM_MAX_BYTES)
        return -1;
    if (!gcm_lengths_valid(aad_len, data_len))
        return -1;
    const uint8_t *ct = (const uint8_t *)ciphertext;
    const uint8_t *recv_tag = ct + data_len;

    uint8_t rk[240];
    aes256_key_expand(key, rk);

    uint8_t H[16] = {0};
    aes256_encrypt_block(rk, H, H);

    uint8_t J0[16];
    memcpy(J0, nonce, 12);
    J0[12] = 0;
    J0[13] = 0;
    J0[14] = 0;
    J0[15] = 1;

    uint8_t ghash_tag[16];
    ghash_compute(H, (const uint8_t *)aad, aad_len, ct, data_len, ghash_tag);

    uint8_t enc_j0[16];
    aes256_encrypt_block(rk, J0, enc_j0);
    uint8_t expected_tag[16];
    for (int i = 0; i < 16; i++)
        expected_tag[i] = ghash_tag[i] ^ enc_j0[i];

    uint8_t diff = 0;
    for (int i = 0; i < 16; i++)
        diff |= expected_tag[i] ^ recv_tag[i];

    if (diff != 0) {
        rt_secure_zero(rk, sizeof(rk));
        rt_secure_zero(H, sizeof(H));
        rt_secure_zero(J0, sizeof(J0));
        rt_secure_zero(ghash_tag, sizeof(ghash_tag));
        rt_secure_zero(enc_j0, sizeof(enc_j0));
        rt_secure_zero(expected_tag, sizeof(expected_tag));
        return -1;
    }

    uint8_t counter[16];
    memcpy(counter, J0, 16);
    gcm_inc32(counter);

    for (size_t i = 0; i < data_len; i += 16) {
        uint8_t keystream[16];
        aes256_encrypt_block(rk, counter, keystream);
        size_t block_len = data_len - i;
        if (block_len > 16)
            block_len = 16;
        for (size_t j = 0; j < block_len; j++)
            plaintext[i + j] = ct[i + j] ^ keystream[j];
        rt_secure_zero(keystream, sizeof(keystream));
        gcm_inc32(counter);
    }

    rt_secure_zero(rk, sizeof(rk));
    rt_secure_zero(H, sizeof(H));
    rt_secure_zero(J0, sizeof(J0));
    rt_secure_zero(counter, sizeof(counter));
    rt_secure_zero(ghash_tag, sizeof(ghash_tag));
    rt_secure_zero(enc_j0, sizeof(enc_j0));
    rt_secure_zero(expected_tag, sizeof(expected_tag));

    return (long)data_len;
}

//=============================================================================
// Cryptographically Secure Random Bytes
//=============================================================================


/// @brief Fill `buf` with `len` cryptographically secure random bytes.
/// @details Delegates operating-system entropy to rt_entropy_platform_random_bytes().
///          In approved mode the crypto module's DRBG supplies the bytes.
///          On any OS entropy failure this function traps and aborts rather
///          than falling back to predictable data.
/// @param buf Destination buffer. May be NULL only when @p len is zero.
/// @param len Number of random bytes requested.
void rt_crypto_random_bytes(uint8_t *buf, size_t len) {
    if (!buf && len > 0) {
        rt_trap("Crypto: random output buffer is null");
        return;
    }
    if (len == 0)
        return;
    if (rt_crypto_module_is_approved_mode()) {
        rt_crypto_module_random_bytes(buf, len);
        return;
    }
    if (rt_entropy_platform_random_bytes(buf, len) == 0)
        return;
    memset(buf, 0, len);
    rt_trap("Crypto: failed to obtain OS entropy");
    rt_abort("Crypto: failed to obtain OS entropy");
}
