//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/text/rt_keyderive.c
// Purpose: Implements PBKDF2-SHA256 (RFC 8018) and scrypt-SHA256 (RFC 7914)
//          for the Zanna.Crypto.KeyDerive class.
//
// Key invariants:
//   - Minimum PBKDF2 iteration count is RT_PBKDF2_MIN_ITERATIONS (100000);
//     requests below this threshold are rejected by public wrappers.
//   - PBKDF2 and scrypt work factors are capped to avoid CPU/memory DoS.
//   - Salt must be non-empty; a NULL or empty salt causes a trap.
//   - Output key length is specified in bytes and limited to 1 through 1024.
//   - HMAC-SHA256 block size is 64 bytes; key padding follows RFC 2104.
//   - Each algorithm has a Bytes-returning API and a lowercase-hex String API.
//
// Ownership/Lifetime:
//   - Returned Bytes and rt_string keys are fresh allocations owned by callers.
//   - Input password strings and salt Bytes objects are borrowed for the call.
//
// Links: src/runtime/text/rt_keyderive.h (public API),
//        src/runtime/text/rt_hash.h (SHA256 used as PRF base),
//        src/runtime/text/rt_password.h (higher-level password hashing)
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_keyderive.c
 * @brief Implements bounded PBKDF2-HMAC-SHA256 and scrypt derivation.
 * @details Public wrappers validate password and salt objects, enforce
 *          iteration, work, memory, and output-length policy, invoke raw
 *          derivation over borrowed byte spans, scrub temporary key material,
 *          and return caller-owned Bytes or lowercase hexadecimal Strings.
 */

#include "rt_keyderive.h"
#include "rt_keyderive_internal.h"

#include "rt_bytes.h"
#include "rt_codec.h"
#include "rt_crypto_module.h"
#include "rt_hash.h"
#include "rt_internal.h"
#include "rt_string.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/// @brief Extract a borrowed byte pointer + length from an `rt_string` password.
/// @details Used by every public PBKDF2/scrypt entry point that accepts a
///          string password. A NULL or invalid string traps so callers do
///          not accidentally derive the key for the empty password. A real
///          zero-length string is still accepted and represented by a stable
///          empty buffer. The returned pointer is borrowed; caller must not
///          free it.
/// @param password Borrowed runtime password string.
/// @param len Non-null output receiving the password byte length.
/// @param ok Optional output set to one only for valid input.
/// @return Borrowed password bytes, or a stable empty buffer after a trap.
static const uint8_t *pbkdf2_string_bytes(rt_string password, size_t *len, int *ok) {
    if (ok)
        *ok = 0;
    if (!password) {
        rt_trap("KeyDerive: password must not be null");
        *len = 0;
        return (const uint8_t *)"";
    }
    if (!rt_string_is_handle((const void *)password)) {
        rt_trap("KeyDerive: invalid password string handle");
        *len = 0;
        return (const uint8_t *)"";
    }
    int64_t len64 = rt_str_len(password);
    if (len64 < 0) {
        rt_trap("KeyDerive: invalid password length");
        *len = 0;
        return (const uint8_t *)"";
    }
    if (len64 == 0) {
        *len = 0;
        if (ok)
            *ok = 1;
        return (const uint8_t *)"";
    }

    const char *pwd_cstr = rt_string_cstr(password);
    if (!pwd_cstr) {
        rt_trap("KeyDerive: password data is null");
        *len = 0;
        return (const uint8_t *)"";
    }

    *len = (size_t)len64;
    if (ok)
        *ok = 1;
    return (const uint8_t *)pwd_cstr;
}

/// @brief Borrow a non-empty salt buffer from a Bytes object.
/// @details KDF wrappers only need read-only salt bytes for the duration of the
///          call. Borrowing avoids copying potentially large salts while still
///          validating that the input is present, non-empty, and representable
///          as a `size_t` span.
/// @param salt Bytes object containing salt bytes.
/// @param len Receives salt length on success; set to zero on failure.
/// @param context Prefix used in trap messages, e.g. "PBKDF2" or "scrypt".
/// @param ok Optional output set to one only for a valid non-empty salt.
/// @return Borrowed salt byte pointer, or a stable empty pointer after reporting a trap.
static const uint8_t *keyderive_salt_bytes(void *salt, size_t *len, const char *context, int *ok) {
    if (len)
        *len = 0;
    if (ok)
        *ok = 0;
    const char *name = context ? context : "KeyDerive";
    if (salt && !rt_bytes_is_bytes(salt)) {
        char msg[96];
        snprintf(msg, sizeof(msg), "%s: salt must be a Bytes object", name);
        rt_trap(msg);
        return (const uint8_t *)"";
    }
    int64_t len64 = salt ? rt_bytes_len(salt) : 0;
    if (len64 <= 0) {
        char msg[96];
        snprintf(msg, sizeof(msg), "%s: salt must not be empty", name);
        rt_trap(msg);
        return (const uint8_t *)"";
    }
    if ((uint64_t)len64 > (uint64_t)SIZE_MAX) {
        char msg[96];
        snprintf(msg, sizeof(msg), "%s: salt length overflow", name);
        rt_trap(msg);
        return (const uint8_t *)"";
    }
    const uint8_t *data = rt_bytes_data_const(salt);
    if (!data) {
        char msg[96];
        snprintf(msg, sizeof(msg), "%s: salt data is null", name);
        rt_trap(msg);
        return (const uint8_t *)"";
    }
    if (len)
        *len = (size_t)len64;
    if (ok)
        *ok = 1;
    return data;
}

/// SHA256 output size in bytes.
#define SHA256_DIGEST_LEN 32

/// @brief Optimization-resistant zero-fill for sensitive PBKDF2 intermediates.
/// @details `volatile` pointer write defeats dead-store elimination so
///          the compiler can't optimize this loop away. Used to wipe
///          every transient buffer (`U`, `T`, `salt || INT(i)`, the
///          fully-derived key once it's been copied to a Bytes
///          object) before functions return.
/// @param ptr Writable sensitive-memory range.
/// @param len Number of bytes to overwrite with zero.
static void keyderive_secure_zero(void *ptr, size_t len) {
    volatile uint8_t *p = (volatile uint8_t *)ptr;
    while (len-- > 0)
        *p++ = 0;
}

/// @brief PBKDF2-HMAC-SHA256 implementation (RFC 2898 § 5.2 / RFC 8018).
/// @details Derives `out_len` bytes from `(password, salt)` by iterating HMAC-
///          SHA256 `iterations` times per output block. The output is
///          structured as a concatenation of fixed-size blocks:
///
///          ```
///          DK = T(1) || T(2) || ... || T(L)        where L = ceil(out_len / 32)
///          T(i) = U(1) XOR U(2) XOR ... XOR U(c)   c = iterations
///          U(1) = HMAC-SHA256(password, salt || INT32_BE(i))
///          U(j) = HMAC-SHA256(password, U(j-1))    for j > 1
///          ```
///
///          The XOR-accumulation across iterations is the cost amplification:
///          an attacker brute-forcing the password must evaluate HMAC-SHA256
///          `c` times per guess, making the per-guess cost linear in
///          `iterations`. This raw function accepts low counts for algorithms
///          such as scrypt that internally require PBKDF2 with c=1. Public
///          wrappers enforce deployment policy.
///
///          The block-index `INT32_BE(i)` is appended to the salt buffer in
///          place across the outer loop (allocated once, reused with the
///          last 4 bytes overwritten per block). All intermediate state
///          (`U`, `T`, the `salt || INT(i)` buffer) is zeroed via
///          `keyderive_secure_zero` before the function returns so the
///          derived key material doesn't linger in heap memory after the
///          caller copies it out.
/// @param password Password bytes (any length).
/// @param password_len Length of `password`.
/// @param salt Salt bytes (any length, including 0).
/// @param salt_len Length of `salt`.
/// @param iterations Number of HMAC iterations per output block (cost
///        parameter — higher means slower per guess).
/// @param out Output buffer for derived key material.
/// @param out_len Number of bytes to produce.
void rt_keyderive_pbkdf2_sha256_raw(const uint8_t *password,
                                    size_t password_len,
                                    const uint8_t *salt,
                                    size_t salt_len,
                                    uint32_t iterations,
                                    uint8_t *out,
                                    size_t out_len) {
    if (!password && password_len > 0) {
        rt_trap("PBKDF2: invalid password buffer");
        return;
    }
    if (!salt && salt_len > 0) {
        rt_trap("PBKDF2: invalid salt buffer");
        return;
    }
    if (iterations == 0) {
        rt_trap("PBKDF2: iterations must be greater than 0");
        return;
    }
    if (iterations > RT_PBKDF2_MAX_ITERATIONS) {
        rt_trap("PBKDF2: iterations exceed policy maximum");
        return;
    }
    if (!out || out_len == 0 || out_len > RT_SCRYPT_MAX_MEMORY) {
        rt_trap("PBKDF2: invalid output length");
        return;
    }

    // Number of blocks needed
    uint32_t block_count = (uint32_t)((out_len + SHA256_DIGEST_LEN - 1) / SHA256_DIGEST_LEN);

    // Allocate buffer for salt || INT(i)
    if (salt_len > SIZE_MAX - 4) {
        rt_trap("PBKDF2: salt too large");
        return;
    }
    size_t salt_int_len = salt_len + 4;
    uint8_t *salt_int = (uint8_t *)malloc(salt_int_len);
    if (!salt_int) {
        rt_trap("PBKDF2: memory allocation failed");
        return;
    }

    if (salt_len > 0)
        memcpy(salt_int, salt, salt_len);

    size_t bytes_written = 0;

    for (uint32_t block_num = 1; block_num <= block_count; block_num++) {
        // Append block number as big-endian 32-bit integer
        salt_int[salt_len] = (uint8_t)(block_num >> 24);
        salt_int[salt_len + 1] = (uint8_t)(block_num >> 16);
        salt_int[salt_len + 2] = (uint8_t)(block_num >> 8);
        salt_int[salt_len + 3] = (uint8_t)(block_num);

        // U1 = PRF(Password, Salt || INT(i))
        uint8_t U[SHA256_DIGEST_LEN];
        uint8_t T[SHA256_DIGEST_LEN];

        rt_hash_hmac_sha256_raw(password, password_len, salt_int, salt_int_len, U);
        memcpy(T, U, SHA256_DIGEST_LEN);

        // U2 through Uc
        for (uint32_t iter = 2; iter <= iterations; iter++) {
            rt_hash_hmac_sha256_raw(password, password_len, U, SHA256_DIGEST_LEN, U);
            for (int j = 0; j < SHA256_DIGEST_LEN; j++) {
                T[j] ^= U[j];
            }
        }

        // Copy T to output (may be partial for last block)
        size_t bytes_to_copy = out_len - bytes_written;
        if (bytes_to_copy > SHA256_DIGEST_LEN)
            bytes_to_copy = SHA256_DIGEST_LEN;

        memcpy(out + bytes_written, T, bytes_to_copy);
        bytes_written += bytes_to_copy;

        keyderive_secure_zero(U, sizeof(U));
        keyderive_secure_zero(T, sizeof(T));
    }

    keyderive_secure_zero(salt_int, salt_int_len);
    free(salt_int);
}

/// @brief Load a 32-bit little-endian unsigned int from @p p.
/// @details scrypt's Salsa20/8 core operates on 32-bit words in
///          little-endian order regardless of host byte order; this helper
///          enforces that.
/// @param p Borrowed readable four-byte range.
/// @return Decoded host-order 32-bit value.
static uint32_t load32_le(const uint8_t *p) {
    return ((uint32_t)p[0]) | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/// @brief Store a 32-bit unsigned int into @p p in little-endian byte order.
/// @details Inverse of load32_le. Used to write the Salsa20/8 output back
///          into the caller's byte-oriented block.
/// @param p Writable four-byte destination.
/// @param v Host-order value to encode.
static void store32_le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/// Rotate a 32-bit Salsa word left by a compile-time round distance.
#define SALSA_ROTL32(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

/// @brief In-place Salsa20/8 core (8 rounds = 4 iterations of the column-and-row pair).
/// @details Standard Salsa20/8 used as the inner mixing primitive of
///          scrypt's BlockMix step. Each call mixes a 64-byte block
///          through 4 column-row pairs and finalizes by adding the
///          original input back (the standard Salsa20 ARX construction).
///          All temporary state is zeroed before return.
/// @param block Writable 64-byte block transformed in place.
static void salsa20_8(uint8_t block[64]) {
    uint32_t x[16];
    uint32_t orig[16];

    for (int i = 0; i < 16; i++) {
        x[i] = load32_le(block + (size_t)i * 4);
        orig[i] = x[i];
    }

    for (int i = 0; i < 8; i += 2) {
        x[4] ^= SALSA_ROTL32(x[0] + x[12], 7);
        x[8] ^= SALSA_ROTL32(x[4] + x[0], 9);
        x[12] ^= SALSA_ROTL32(x[8] + x[4], 13);
        x[0] ^= SALSA_ROTL32(x[12] + x[8], 18);
        x[9] ^= SALSA_ROTL32(x[5] + x[1], 7);
        x[13] ^= SALSA_ROTL32(x[9] + x[5], 9);
        x[1] ^= SALSA_ROTL32(x[13] + x[9], 13);
        x[5] ^= SALSA_ROTL32(x[1] + x[13], 18);
        x[14] ^= SALSA_ROTL32(x[10] + x[6], 7);
        x[2] ^= SALSA_ROTL32(x[14] + x[10], 9);
        x[6] ^= SALSA_ROTL32(x[2] + x[14], 13);
        x[10] ^= SALSA_ROTL32(x[6] + x[2], 18);
        x[3] ^= SALSA_ROTL32(x[15] + x[11], 7);
        x[7] ^= SALSA_ROTL32(x[3] + x[15], 9);
        x[11] ^= SALSA_ROTL32(x[7] + x[3], 13);
        x[15] ^= SALSA_ROTL32(x[11] + x[7], 18);

        x[1] ^= SALSA_ROTL32(x[0] + x[3], 7);
        x[2] ^= SALSA_ROTL32(x[1] + x[0], 9);
        x[3] ^= SALSA_ROTL32(x[2] + x[1], 13);
        x[0] ^= SALSA_ROTL32(x[3] + x[2], 18);
        x[6] ^= SALSA_ROTL32(x[5] + x[4], 7);
        x[7] ^= SALSA_ROTL32(x[6] + x[5], 9);
        x[4] ^= SALSA_ROTL32(x[7] + x[6], 13);
        x[5] ^= SALSA_ROTL32(x[4] + x[7], 18);
        x[11] ^= SALSA_ROTL32(x[10] + x[9], 7);
        x[8] ^= SALSA_ROTL32(x[11] + x[10], 9);
        x[9] ^= SALSA_ROTL32(x[8] + x[11], 13);
        x[10] ^= SALSA_ROTL32(x[9] + x[8], 18);
        x[12] ^= SALSA_ROTL32(x[15] + x[14], 7);
        x[13] ^= SALSA_ROTL32(x[12] + x[15], 9);
        x[14] ^= SALSA_ROTL32(x[13] + x[12], 13);
        x[15] ^= SALSA_ROTL32(x[14] + x[13], 18);
    }

    for (int i = 0; i < 16; i++)
        store32_le(block + (size_t)i * 4, x[i] + orig[i]);

    keyderive_secure_zero(x, sizeof(x));
    keyderive_secure_zero(orig, sizeof(orig));
}

#undef SALSA_ROTL32

/// @brief scrypt BlockMix_{Salsa20/8, r} — RFC 7914 § 6.
/// @details Mixes the (128 * r)-byte @p block in place. Treats the block
///          as a sequence of 2*r 64-byte sub-blocks, runs each through
///          Salsa20/8 chained with the previous output, then de-interleaves
///          even and odd indexes to produce the output ordering. @p tmp
///          must be at least block_len bytes — used as scratch.
/// @param block Writable `128 * r`-byte input/output block.
/// @param tmp Writable scratch range of at least `128 * r` bytes.
/// @param r Positive scrypt block-size parameter.
static void scrypt_blockmix(uint8_t *block, uint8_t *tmp, uint32_t r) {
    uint8_t x[64];
    size_t blocks = (size_t)2 * r;

    memcpy(x, block + (blocks - 1) * 64, 64);
    for (size_t i = 0; i < blocks; i++) {
        for (size_t j = 0; j < 64; j++)
            x[j] ^= block[i * 64 + j];
        salsa20_8(x);
        memcpy(tmp + i * 64, x, 64);
    }

    for (uint32_t i = 0; i < r; i++)
        memcpy(block + (size_t)i * 64, tmp + (size_t)(2 * i) * 64, 64);
    for (uint32_t i = 0; i < r; i++)
        memcpy(block + ((size_t)i + r) * 64, tmp + (size_t)(2 * i + 1) * 64, 64);

    keyderive_secure_zero(x, sizeof(x));
}

/// @brief scrypt Integerify(B) — RFC 7914 § 5.
/// @details Reads the last 64-byte sub-block of @p block and returns its
///          low 64 bits in little-endian order. Used by ROMix to derive
///          the random index `j = Integerify(X) mod N`.
/// @param block Borrowed `128 * r`-byte mixed block.
/// @param r Positive scrypt block-size parameter.
/// @return Low 64 bits of the final sub-block in host order.
static uint64_t scrypt_integerify(const uint8_t *block, uint32_t r) {
    const uint8_t *last = block + ((size_t)2 * r - 1) * 64;
    return ((uint64_t)load32_le(last + 4) << 32) | (uint64_t)load32_le(last);
}

/// @brief scrypt ROMix — RFC 7914 § 5; the memory-hard core of scrypt.
/// @details Two phases over a (128 * r * N)-byte working set V:
///          Phase 1 fills V[0..N-1] with successive BlockMix outputs.
///          Phase 2 runs N iterations of `j = Integerify(B) mod N; B ^= V[j]; B = BlockMix(B)`,
///          which forces an attacker to either store all of V (memory-hard)
///          or recompute an expected-value O(N²/2) BlockMix calls (time-hard).
///          Memory cap is enforced via RT_SCRYPT_MAX_MEMORY; the malloc
///          may itself OOM, in which case we trap with a clean error.
///          All sensitive scratch is zeroed before return.
/// @param block Writable `128 * r`-byte block transformed in place.
/// @param n Power-of-two ROMix iteration count.
/// @param r Positive scrypt block-size parameter.
static void scrypt_romix(uint8_t *block, uint64_t n, uint32_t r) {
    size_t block_len = (size_t)128 * r;
    if (n > SIZE_MAX / block_len) {
        rt_trap("scrypt: memory cost is too large");
        return;
    }
    size_t v_len = (size_t)n * block_len;
    if (v_len > RT_SCRYPT_MAX_MEMORY) {
        rt_trap("scrypt: memory cost exceeds policy maximum");
        return;
    }

    uint8_t *v = (uint8_t *)malloc(v_len);
    uint8_t *tmp = (uint8_t *)malloc(block_len);
    if (!v || !tmp) {
        free(v);
        free(tmp);
        rt_trap("scrypt: memory allocation failed");
        return;
    }

    for (uint64_t i = 0; i < n; i++) {
        memcpy(v + (size_t)i * block_len, block, block_len);
        scrypt_blockmix(block, tmp, r);
    }

    for (uint64_t i = 0; i < n; i++) {
        uint64_t j = scrypt_integerify(block, r) & (n - 1);
        const uint8_t *vj = v + (size_t)j * block_len;
        for (size_t k = 0; k < block_len; k++)
            block[k] ^= vj[k];
        scrypt_blockmix(block, tmp, r);
    }

    keyderive_secure_zero(v, v_len);
    keyderive_secure_zero(tmp, block_len);
    free(v);
    free(tmp);
}

/// @brief Validate scrypt cost parameters against runtime caps.
/// @details Enforces all the scrypt-spec constraints: N must be a power of
///          two ≥ 2; r and p must be positive and ≤ RT_SCRYPT_MAX_R/P;
///          out_len must fit RT_SCRYPT_MAX_KEY_LEN; N ≤ 2^RT_SCRYPT_MAX_N_LOG2;
///          the (N * 128 * r) memory product must fit RT_SCRYPT_MAX_MEMORY
///          and not overflow size_t. Returns 1 if every check passes, 0
///          otherwise. Public wrappers expose this via
///          rt_keyderive_scrypt_params_supported so callers can check
///          before committing memory.
/// @param n ROMix work factor, required to be a supported power of two.
/// @param r Block-size parameter.
/// @param p Parallelization parameter.
/// @param out_len Requested derived-key length in bytes.
/// @return `1` when all arithmetic and runtime policy checks pass, otherwise `0`.
static int scrypt_params_valid(uint64_t n, uint32_t r, uint32_t p, size_t out_len) {
    if (n < 2 || (n & (n - 1)) != 0)
        return 0;
    if (r == 0 || r > RT_SCRYPT_MAX_R || p == 0 || p > RT_SCRYPT_MAX_P)
        return 0;
    if (out_len == 0 || out_len > RT_SCRYPT_MAX_KEY_LEN)
        return 0;
    if (n > (UINT64_C(1) << RT_SCRYPT_MAX_N_LOG2))
        return 0;
    if ((uint64_t)r > SIZE_MAX / 128)
        return 0;
    size_t block_len = (size_t)128 * r;
    if (n > SIZE_MAX / block_len)
        return 0;
    if ((size_t)n * block_len > RT_SCRYPT_MAX_MEMORY)
        return 0;
    if ((uint64_t)p > SIZE_MAX / block_len)
        return 0;
    return 1;
}

/// @brief Public predicate: are these scrypt parameters acceptable to this runtime?
/// @details Exposes scrypt_params_valid for callers that want to probe a
///          parameter set (typically loaded from configuration or an
///          encoded password string) without committing the memory the
///          full derivation would consume.
/// @param n ROMix work factor.
/// @param r Block-size parameter.
/// @param p Parallelization parameter.
/// @param out_len Requested derived-key length in bytes.
/// @return Nonzero if the parameters are valid, 0 if any check fails.
int rt_keyderive_scrypt_params_supported(uint64_t n, uint32_t r, uint32_t p, size_t out_len) {
    return scrypt_params_valid(n, r, p, out_len);
}

/// @brief Raw scrypt-SHA256 KDF (RFC 7914) — internal helper, no Zanna-string wrapping.
/// @details Implements the full scrypt construction:
///          1. PBKDF2-HMAC-SHA256(password, salt, 1, p * 128 * r) → B
///          2. For each of p blocks of B: ROMix_r(N) in place
///          3. PBKDF2-HMAC-SHA256(password, B, 1, out_len) → out
///          Traps on invalid buffers, invalid parameter sets (per
///          scrypt_params_valid), or allocation failure. Sensitive scratch
///          is zeroed before return.
/// @param password Borrowed password bytes; may be null only when length is zero.
/// @param password_len Number of password bytes.
/// @param salt Borrowed salt bytes; may be null only when length is zero.
/// @param salt_len Number of salt bytes.
/// @param n Power-of-two ROMix work factor.
/// @param r Block-size parameter.
/// @param p Parallelization parameter.
/// @param out Writable derived-key buffer.
/// @param out_len Number of output bytes to produce.
void rt_keyderive_scrypt_sha256_raw(const uint8_t *password,
                                    size_t password_len,
                                    const uint8_t *salt,
                                    size_t salt_len,
                                    uint64_t n,
                                    uint32_t r,
                                    uint32_t p,
                                    uint8_t *out,
                                    size_t out_len) {
    if ((!password && password_len > 0) || (!salt && salt_len > 0) || !out) {
        rt_trap("scrypt: invalid buffer");
        return;
    }
    if (!scrypt_params_valid(n, r, p, out_len)) {
        rt_trap("scrypt: invalid or unsupported parameters");
        return;
    }

    size_t block_len = (size_t)128 * r;
    size_t b_len = block_len * p;
    uint8_t *b = (uint8_t *)malloc(b_len);
    if (!b) {
        rt_trap("scrypt: memory allocation failed");
        return;
    }

    rt_keyderive_pbkdf2_sha256_raw(password, password_len, salt, salt_len, 1, b, b_len);
    for (uint32_t i = 0; i < p; i++)
        scrypt_romix(b + (size_t)i * block_len, n, r);
    rt_keyderive_pbkdf2_sha256_raw(password, password_len, b, b_len, 1, out, out_len);

    keyderive_secure_zero(b, b_len);
    free(b);
}

/// @brief Derive a key from a password using PBKDF2-SHA256, returning a Bytes object.
/// @details High-level wrapper around rt_keyderive_pbkdf2_sha256_raw that
///          handles Zanna string/bytes conversion. Validates iterations (min 100000)
///          and key length (1–1024 bytes). The derived key is zeroed from the
///          temporary buffer after copying to the Bytes object.
/// @param password   Password string.
/// @param salt       Bytes object containing the salt.
/// @param iterations Number of PBKDF2 iterations (min 100000).
/// @param key_len    Desired output key length in bytes (1–1024).
/// @return Bytes object containing the derived key.
void *rt_keyderive_pbkdf2_sha256(rt_string password,
                                 void *salt,
                                 int64_t iterations,
                                 int64_t key_len) {
    // Validate iterations
    size_t salt_len;
    const uint8_t *salt_data;

    if (iterations < RT_PBKDF2_MIN_ITERATIONS) {
        rt_trap("PBKDF2: iterations are below the policy minimum");
        return NULL;
    }
    if (iterations > UINT32_MAX) {
        rt_trap("PBKDF2: iterations must not exceed 4294967295");
        return NULL;
    }
    if (iterations > RT_PBKDF2_MAX_ITERATIONS) {
        rt_trap("PBKDF2: iterations exceed policy maximum");
        return NULL;
    }
    if (key_len < 1 || key_len > RT_PBKDF2_MAX_KEY_LEN) {
        rt_trap("PBKDF2: key_len must be between 1 and 1024");
        return NULL;
    }

    size_t pwd_len;
    int pwd_ok;
    const uint8_t *pwd = pbkdf2_string_bytes(password, &pwd_len, &pwd_ok);
    if (!pwd_ok)
        return NULL;

    int salt_ok;
    salt_data = keyderive_salt_bytes(salt, &salt_len, "PBKDF2", &salt_ok);
    if (!salt_ok || !salt_data || salt_len == 0)
        return NULL;

    uint8_t *derived_key = (uint8_t *)malloc((size_t)key_len);
    if (!derived_key) {
        rt_trap("PBKDF2: memory allocation failed");
        return NULL;
    }

    rt_keyderive_pbkdf2_sha256_raw(
        pwd, pwd_len, salt_data, salt_len, (uint32_t)iterations, derived_key, (size_t)key_len);

    // Create Bytes object from derived key
    void *result = rt_bytes_from_raw(derived_key, (size_t)key_len);
    keyderive_secure_zero(derived_key, (size_t)key_len);
    free(derived_key);
    return result;
}

/// @brief Derive a key from a password using PBKDF2-SHA256, returning a hex string.
/// @details Same derivation as rt_keyderive_pbkdf2_sha256 but returns the key
///          encoded as a lowercase hexadecimal string (2 chars per byte). Useful
///          for interop with systems that expect text-encoded keys.
/// @param password   Password string.
/// @param salt       Bytes object containing the salt.
/// @param iterations Number of PBKDF2 iterations (min 100000).
/// @param key_len    Desired output key length in bytes (hex output is 2x this).
/// @return Hex-encoded string of the derived key.
rt_string rt_keyderive_pbkdf2_sha256_str(rt_string password,
                                         void *salt,
                                         int64_t iterations,
                                         int64_t key_len) {
    // Validate iterations
    size_t salt_len;
    const uint8_t *salt_data;

    if (iterations < RT_PBKDF2_MIN_ITERATIONS) {
        rt_trap("PBKDF2: iterations are below the policy minimum");
        return rt_str_empty();
    }
    if (iterations > UINT32_MAX) {
        rt_trap("PBKDF2: iterations must not exceed 4294967295");
        return rt_str_empty();
    }
    if (iterations > RT_PBKDF2_MAX_ITERATIONS) {
        rt_trap("PBKDF2: iterations exceed policy maximum");
        return rt_str_empty();
    }
    if (key_len < 1 || key_len > RT_PBKDF2_MAX_KEY_LEN) {
        rt_trap("PBKDF2: key_len must be between 1 and 1024");
        return rt_str_empty();
    }

    size_t pwd_len;
    int pwd_ok;
    const uint8_t *pwd = pbkdf2_string_bytes(password, &pwd_len, &pwd_ok);
    if (!pwd_ok)
        return rt_str_empty();

    int salt_ok;
    salt_data = keyderive_salt_bytes(salt, &salt_len, "PBKDF2", &salt_ok);
    if (!salt_ok || !salt_data || salt_len == 0)
        return rt_str_empty();

    uint8_t *derived_key = (uint8_t *)malloc((size_t)key_len);
    if (!derived_key) {
        rt_trap("PBKDF2: memory allocation failed");
        return rt_str_empty();
    }

    rt_keyderive_pbkdf2_sha256_raw(
        pwd, pwd_len, salt_data, salt_len, (uint32_t)iterations, derived_key, (size_t)key_len);

    // Convert to hex string using shared codec utility
    rt_string result = rt_codec_hex_enc_bytes(derived_key, (size_t)key_len);
    keyderive_secure_zero(derived_key, (size_t)key_len);
    free(derived_key);
    return result;
}

/// @brief Validate the i64 scrypt parameters from a Zia caller and convert to native types.
/// @details The public Zia API takes int64 cost parameters; this helper
///          checks each is non-negative and fits in the underlying type
///          (uint64 for N, uint32 for r/p), then runs scrypt_params_valid
///          to enforce the runtime caps. Traps on any violation so the
///          caller learns about misconfiguration immediately rather than
///          getting silent clamping. On success, fills @p n / @p r / @p p
///          with the validated native-typed values.
/// @param n64 Signed public ROMix work factor.
/// @param r64 Signed public block-size parameter.
/// @param p64 Signed public parallelization parameter.
/// @param key_len Requested output length in bytes.
/// @param n Non-null output receiving validated @p n64.
/// @param r Non-null output receiving validated @p r64.
/// @param p Non-null output receiving validated @p p64.
/// @return 1 when parameters were valid, 0 after a recoverable validation trap.
static int validate_public_scrypt_params(
    int64_t n64, int64_t r64, int64_t p64, int64_t key_len, uint64_t *n, uint32_t *r, uint32_t *p) {
    if (n64 < 2 || r64 < 1 || p64 < 1) {
        rt_trap("scrypt: cost parameters must be positive");
        return 0;
    }
    if (key_len < 1 || key_len > RT_SCRYPT_MAX_KEY_LEN) {
        rt_trap("scrypt: key_len must be between 1 and 1024");
        return 0;
    }
    if (r64 > UINT32_MAX || p64 > UINT32_MAX) {
        rt_trap("scrypt: cost parameters are too large");
        return 0;
    }

    *n = (uint64_t)n64;
    *r = (uint32_t)r64;
    *p = (uint32_t)p64;
    if (!scrypt_params_valid(*n, *r, *p, (size_t)key_len)) {
        rt_trap("scrypt: invalid or unsupported parameters");
        return 0;
    }
    return 1;
}

/// @brief Public Zanna.Crypto.KeyDerive.ScryptSHA256 — derive a key, return as bytes.
/// @details Validates parameters, extracts password and salt, runs the raw
///          scrypt-SHA256 derivation, and wraps the result as an rt_bytes
///          object. Sensitive intermediate buffers are zeroed before return.
/// @param password Password string. Required.
/// @param salt     Bytes object with the salt. Required, must be non-empty.
/// @param n64      scrypt N parameter (must be a power of two ≥ 2).
/// @param r64      scrypt r parameter.
/// @param p64      scrypt p parameter.
/// @param key_len  Output key length in bytes (1..1024).
/// @return Bytes object owning the derived key.
void *rt_keyderive_scrypt_sha256(
    rt_string password, void *salt, int64_t n64, int64_t r64, int64_t p64, int64_t key_len) {
    if (!rt_crypto_module_service_allowed(RT_CRYPTO_SERVICE_SCRYPT)) {
        rt_trap("KeyDerive.ScryptSHA256 is disabled in approved mode");
        return NULL;
    }
    uint64_t n;
    uint32_t r;
    uint32_t p;
    if (!validate_public_scrypt_params(n64, r64, p64, key_len, &n, &r, &p))
        return NULL;

    size_t pwd_len;
    int pwd_ok;
    const uint8_t *pwd = pbkdf2_string_bytes(password, &pwd_len, &pwd_ok);
    if (!pwd_ok)
        return NULL;

    size_t salt_len;
    int salt_ok;
    const uint8_t *salt_data = keyderive_salt_bytes(salt, &salt_len, "scrypt", &salt_ok);
    if (!salt_ok || !salt_data || salt_len == 0)
        return NULL;

    uint8_t *derived_key = (uint8_t *)malloc((size_t)key_len);
    if (!derived_key) {
        rt_trap("scrypt: memory allocation failed");
        return NULL;
    }

    rt_keyderive_scrypt_sha256_raw(
        pwd, pwd_len, salt_data, salt_len, n, r, p, derived_key, (size_t)key_len);

    void *result = rt_bytes_from_raw(derived_key, (size_t)key_len);
    keyderive_secure_zero(derived_key, (size_t)key_len);
    free(derived_key);
    return result;
}

/// @brief Public Zanna.Crypto.KeyDerive.ScryptSHA256Str — derive a key, return as hex string.
/// @details Same as rt_keyderive_scrypt_sha256 but returns the derived
///          bytes hex-encoded as an rt_string for portability (e.g.
///          embedding in JSON config or comparing as a string). All
///          parameter rules (positive cost values, salt non-empty,
///          key_len in 1..1024) are identical. Approved crypto mode disables
///          scrypt and traps.
/// @param password Borrowed non-null password string; empty is permitted.
/// @param salt Borrowed non-empty Bytes salt.
/// @param n64 Power-of-two ROMix work factor.
/// @param r64 Positive block-size parameter.
/// @param p64 Positive parallelization parameter.
/// @param key_len Output length in bytes from 1 through 1024.
/// @return Newly allocated lowercase hexadecimal key owned by the caller, or
///         an empty fallback string after a trapped error.
rt_string rt_keyderive_scrypt_sha256_str(
    rt_string password, void *salt, int64_t n64, int64_t r64, int64_t p64, int64_t key_len) {
    if (!rt_crypto_module_service_allowed(RT_CRYPTO_SERVICE_SCRYPT)) {
        rt_trap("KeyDerive.ScryptSHA256Str is disabled in approved mode");
        return rt_str_empty();
    }
    uint64_t n;
    uint32_t r;
    uint32_t p;
    if (!validate_public_scrypt_params(n64, r64, p64, key_len, &n, &r, &p))
        return rt_str_empty();

    size_t pwd_len;
    int pwd_ok;
    const uint8_t *pwd = pbkdf2_string_bytes(password, &pwd_len, &pwd_ok);
    if (!pwd_ok)
        return rt_str_empty();

    size_t salt_len;
    int salt_ok;
    const uint8_t *salt_data = keyderive_salt_bytes(salt, &salt_len, "scrypt", &salt_ok);
    if (!salt_ok || !salt_data || salt_len == 0)
        return rt_str_empty();

    uint8_t *derived_key = (uint8_t *)malloc((size_t)key_len);
    if (!derived_key) {
        rt_trap("scrypt: memory allocation failed");
        return rt_str_empty();
    }

    rt_keyderive_scrypt_sha256_raw(
        pwd, pwd_len, salt_data, salt_len, n, r, p, derived_key, (size_t)key_len);

    rt_string result = rt_codec_hex_enc_bytes(derived_key, (size_t)key_len);
    keyderive_secure_zero(derived_key, (size_t)key_len);
    free(derived_key);
    return result;
}
