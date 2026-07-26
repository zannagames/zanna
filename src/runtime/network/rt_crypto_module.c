//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/network/rt_crypto_module.c
// Purpose: Implementation of the FIPS-aligned crypto module boundary: mode
//          state machine (COMPAT/APPROVED), embedded HMAC-DRBG, power-up
//          self-tests, and policy gating per primitive service id.
// Key invariants:
//   - All public entry points serialise behind the @c g_module_lock
//     atomic flag so mode flips and DRBG generates cannot interleave.
//   - APPROVED-mode random bytes come exclusively from the DRBG. The DRBG
//     is reseeded from OS entropy every 2^48 requests.
//   - Self-tests run on first @c init and again when transitioning into
//     APPROVED mode. A self-test failure pins the module in @c ERROR
//     state for the lifetime of the process.
// Ownership/Lifetime:
//   - Process-global singleton; no per-call allocation.
// Links: rt_crypto_module.h, rt_crypto.c
//
//===----------------------------------------------------------------------===//

#include "rt_crypto_module.h"

#include "rt_crypto.h"
#include "rt_entropy_platform.h"
#include "rt_platform.h"
#include "rt_string.h"
#include "rt_trap.h"

#include <stdlib.h>
#include <string.h>

#if RT_PLATFORM_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sched.h>
#endif

/// @brief Process-global HMAC-SHA-256 DRBG state.
typedef struct rt_hmac_drbg_state {
    uint8_t k[32];
    uint8_t v[32];
    uint64_t reseed_counter;
    int ready;
} rt_hmac_drbg_state_t;

#define RT_HMAC_DRBG_SEED_LEN 48
#define RT_HMAC_DRBG_MAX_REQUEST 65536u
#define RT_HMAC_DRBG_RESEED_INTERVAL (UINT64_C(1) << 48)

static rt_crypto_module_mode_t g_mode = RT_CRYPTO_MODULE_MODE_COMPAT;
static rt_crypto_module_state_t g_state = RT_CRYPTO_MODULE_STATE_UNINITIALIZED;
static const char *g_status = "uninitialized";
static rt_hmac_drbg_state_t g_drbg;
static volatile int g_module_lock = 0;

/// @brief Yield the processor while waiting for the crypto module lock.
/// @details Keeps the module spinlock lightweight under no contention while
///          avoiding a hot busy-wait loop when another thread is performing
///          DRBG work or mode transitions.
static void module_lock_yield(void) {
#if RT_PLATFORM_WINDOWS
    SwitchToThread();
#else
    (void)sched_yield();
#endif
}

/// @brief Spin-acquire the module-wide lock.
/// @details Test-and-set spinlock with cooperative yielding under contention.
///          Uncontended acquisition remains a single atomic operation; contended
///          acquisition avoids monopolizing a CPU core.
static void module_lock(void) {
    while (__atomic_test_and_set(&g_module_lock, __ATOMIC_ACQUIRE)) {
        module_lock_yield();
    }
}

/// @brief Release the module-wide lock.
static void module_unlock(void) {
    __atomic_clear(&g_module_lock, __ATOMIC_RELEASE);
}

/// @brief Zero @p len bytes at @p ptr in a way the compiler cannot elide.
/// @details Used to scrub key material, DRBG state, and intermediate
///          self-test buffers before they fall out of scope.
/// @param ptr Writable memory to clear.
/// @param len Number of bytes to overwrite with zero.
static void module_secure_zero(void *ptr, size_t len) {
    volatile uint8_t *p = (volatile uint8_t *)ptr;
    while (len-- > 0)
        *p++ = 0;
}

/// @brief Pull @p len bytes of OS entropy into @p buf.
/// @details Delegates to the shared entropy adapter. The caller traps when
///          this returns failure so the DRBG is never seeded from predictable
///          data.
/// @param buf Output buffer to fill.
/// @param len Number of entropy bytes requested.
/// @return 0 on success, -1 on entropy failure.
static int module_os_entropy(uint8_t *buf, size_t len) {
    return rt_entropy_platform_random_bytes(buf, len);
}

/// @brief HMAC-DRBG Update (NIST SP 800-90A §10.1.2.2).
/// @details Mixes optional @p seed material into the (K, V) state. The
///          update is two passes: the first XORs in 0x00 and runs HMAC
///          to produce the new K; the second pass uses 0x01 only when
///          @p seed_len > 0, matching the NIST construction.
/// @param st Mutable DRBG state.
/// @param seed Optional seed or additional-input bytes.
/// @param seed_len Number of seed bytes; values above 64 trap.
static void drbg_update(rt_hmac_drbg_state_t *st, const uint8_t *seed, size_t seed_len) {
    uint8_t material[32 + 1 + 64];
    if (seed_len > 64) {
        rt_trap("Crypto.Module: DRBG seed material too large");
        return;
    }

    memcpy(material, st->v, 32);
    material[32] = 0x00;
    if (seed_len > 0)
        memcpy(material + 33, seed, seed_len);
    rt_hmac_sha256(st->k, sizeof(st->k), material, 33 + seed_len, st->k);
    rt_hmac_sha256(st->k, sizeof(st->k), st->v, sizeof(st->v), st->v);

    if (seed_len > 0) {
        memcpy(material, st->v, 32);
        material[32] = 0x01;
        memcpy(material + 33, seed, seed_len);
        rt_hmac_sha256(st->k, sizeof(st->k), material, 33 + seed_len, st->k);
        rt_hmac_sha256(st->k, sizeof(st->k), st->v, sizeof(st->v), st->v);
    }

    module_secure_zero(material, sizeof(material));
}

/// @brief HMAC-DRBG Instantiate (NIST SP 800-90A §10.1.2.3).
/// @details Initialises K = 0^256, V = 0x01^256, then runs Update with the
///          supplied seed material. Sets the reseed counter to 1 and
///          marks the state ready for generation.
/// @param st DRBG state to initialize.
/// @param seed Seed material passed to the update construction.
/// @param seed_len Number of seed bytes.
static void drbg_instantiate(rt_hmac_drbg_state_t *st, const uint8_t *seed, size_t seed_len) {
    memset(st->k, 0, sizeof(st->k));
    memset(st->v, 1, sizeof(st->v));
    st->reseed_counter = 1;
    st->ready = 1;
    drbg_update(st, seed, seed_len);
}

/// @brief Pull fresh entropy from the OS and reseed the DRBG.
/// @details Called automatically by @ref drbg_generate when the request
///          counter exceeds @c RT_HMAC_DRBG_RESEED_INTERVAL. Returns 0
///          when OS entropy is unavailable so the caller can trap
///          rather than continue with stale state.
/// @param st Instantiated DRBG state to reseed.
/// @return 1 on success, 0 on OS entropy failure.
static int drbg_reseed_from_os(rt_hmac_drbg_state_t *st) {
    uint8_t seed[RT_HMAC_DRBG_SEED_LEN];
    if (module_os_entropy(seed, sizeof(seed)) != 0) {
        module_secure_zero(seed, sizeof(seed));
        return 0;
    }
    drbg_update(st, seed, sizeof(seed));
    st->reseed_counter = 1;
    module_secure_zero(seed, sizeof(seed));
    return 1;
}

/// @brief HMAC-DRBG Generate (NIST SP 800-90A §10.1.2.5).
/// @details Iterates V = HMAC-SHA-256(K, V) and copies the rolling V
///          into @p out until @p out_len bytes are produced. Each
///          invocation finishes with a "post-update" K/V mix using
///          empty additional input. Refuses requests larger than
///          @c RT_HMAC_DRBG_MAX_REQUEST and triggers automatic reseed
///          when the request counter exceeds the NIST interval.
/// @param st Instantiated DRBG state.
/// @param out Output buffer, or NULL only when @p out_len is zero.
/// @param out_len Requested bytes, capped at 65,536 per invocation.
static void drbg_generate(rt_hmac_drbg_state_t *st, uint8_t *out, size_t out_len) {
    if (!st->ready) {
        rt_trap("Crypto.Module: DRBG is not instantiated");
        rt_abort("Crypto.Module: DRBG is not instantiated");
    }
    if (!out && out_len > 0) {
        rt_trap("Crypto.Module: random output buffer is null");
        return;
    }
    if (out_len > RT_HMAC_DRBG_MAX_REQUEST) {
        rt_trap("Crypto.Module: DRBG request too large");
        return;
    }
    if (st->reseed_counter > RT_HMAC_DRBG_RESEED_INTERVAL && !drbg_reseed_from_os(st)) {
        rt_trap("Crypto.Module: DRBG reseed failed");
        rt_abort("Crypto.Module: DRBG reseed failed");
    }
    size_t pos = 0;
    while (pos < out_len) {
        rt_hmac_sha256(st->k, sizeof(st->k), st->v, sizeof(st->v), st->v);
        size_t copy = out_len - pos;
        if (copy > sizeof(st->v))
            copy = sizeof(st->v);
        memcpy(out + pos, st->v, copy);
        pos += copy;
    }
    drbg_update(st, NULL, 0);
    st->reseed_counter++;
}

/// @brief Power-up known-answer test for SHA-256/384/512.
/// @details Hashes the canonical "abc" test vector and compares against
///          the NIST-published expected digests. Used by the boot-time
///          self-test sequence; failure pins the module in @c ERROR.
/// @return 1 on pass, 0 on any mismatch.
static int self_test_sha2(void) {
    static const uint8_t sha256_exp[32] = {0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
                                           0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
                                           0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
                                           0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};
    static const uint8_t sha384_exp[48] = {
        0xcb, 0x00, 0x75, 0x3f, 0x45, 0xa3, 0x5e, 0x8b, 0xb5, 0xa0, 0x3d, 0x69,
        0x9a, 0xc6, 0x50, 0x07, 0x27, 0x2c, 0x32, 0xab, 0x0e, 0xde, 0xd1, 0x63,
        0x1a, 0x8b, 0x60, 0x5a, 0x43, 0xff, 0x5b, 0xed, 0x80, 0x86, 0x07, 0x2b,
        0xa1, 0xe7, 0xcc, 0x23, 0x58, 0xba, 0xec, 0xa1, 0x34, 0xc8, 0x25, 0xa7};
    static const uint8_t sha512_exp[64] = {
        0xdd, 0xaf, 0x35, 0xa1, 0x93, 0x61, 0x7a, 0xba, 0xcc, 0x41, 0x73, 0x49, 0xae,
        0x20, 0x41, 0x31, 0x12, 0xe6, 0xfa, 0x4e, 0x89, 0xa9, 0x7e, 0xa2, 0x0a, 0x9e,
        0xee, 0xe6, 0x4b, 0x55, 0xd3, 0x9a, 0x21, 0x92, 0x99, 0x2a, 0x27, 0x4f, 0xc1,
        0xa8, 0x36, 0xba, 0x3c, 0x23, 0xa3, 0xfe, 0xeb, 0xbd, 0x45, 0x4d, 0x44, 0x23,
        0x64, 0x3c, 0xe8, 0x0e, 0x2a, 0x9a, 0xc9, 0x4f, 0xa5, 0x4c, 0xa4, 0x9f};
    uint8_t out[64];
    rt_sha256("abc", 3, out);
    if (memcmp(out, sha256_exp, 32) != 0)
        return 0;
    rt_sha384("abc", 3, out);
    if (memcmp(out, sha384_exp, 48) != 0)
        return 0;
    rt_sha512("abc", 3, out);
    return memcmp(out, sha512_exp, 64) == 0;
}

/// @brief Power-up known-answer test for HMAC-SHA-256 and HKDF.
/// @details Uses the RFC 4231 HMAC-SHA-256 test case 1 and the RFC 5869
///          HKDF Appendix A.1 test vector. A divergence indicates the
///          MAC or the expand step has regressed.
/// @return 1 when both known-answer vectors match; otherwise 0.
static int self_test_hmac_hkdf(void) {
    uint8_t key[20];
    uint8_t mac[48];
    memset(key, 0x0b, sizeof(key));
    static const uint8_t hmac_exp[32] = {0xb0, 0x34, 0x4c, 0x61, 0xd8, 0xdb, 0x38, 0x53,
                                         0x5c, 0xa8, 0xaf, 0xce, 0xaf, 0x0b, 0xf1, 0x2b,
                                         0x88, 0x1d, 0xc2, 0x00, 0xc9, 0x83, 0x3d, 0xa7,
                                         0x26, 0xe9, 0x37, 0x6c, 0x2e, 0x32, 0xcf, 0xf7};
    rt_hmac_sha256(key, sizeof(key), "Hi There", 8, mac);
    if (memcmp(mac, hmac_exp, 32) != 0)
        return 0;

    static const uint8_t prk[32] = {0x07, 0x77, 0x09, 0x36, 0x2c, 0x2e, 0x32, 0xdf,
                                    0x0d, 0xdc, 0x3f, 0x0d, 0xc4, 0x7b, 0xba, 0x63,
                                    0x90, 0xb6, 0xc7, 0x3b, 0xb5, 0x0f, 0x9c, 0x31,
                                    0x22, 0xec, 0x84, 0x4a, 0xd7, 0xc2, 0xb3, 0xe5};
    static const uint8_t info[10] = {0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9};
    static const uint8_t okm_exp[42] = {
        0x3c, 0xb2, 0x5f, 0x25, 0xfa, 0xac, 0xd5, 0x7a, 0x90, 0x43, 0x4f, 0x64, 0xd0, 0x36,
        0x2f, 0x2a, 0x2d, 0x2d, 0x0a, 0x90, 0xcf, 0x1a, 0x5a, 0x4c, 0x5d, 0xb0, 0x2d, 0x56,
        0xec, 0xc4, 0xc5, 0xbf, 0x34, 0x00, 0x72, 0x08, 0xd5, 0xb8, 0x87, 0x18, 0x58, 0x65};
    uint8_t okm[42];
    if (rt_hkdf_expand(prk, info, sizeof(info), okm, sizeof(okm)) != 0)
        return 0;
    return memcmp(okm, okm_exp, sizeof(okm)) == 0;
}

/// @brief Power-up known-answer test for AES-128-GCM and AES-256-GCM.
/// @details Uses the GCM "test case 1" all-zero key/nonce/plaintext
///          vectors from NIST SP 800-38D Annex B. The decrypt path is
///          also exercised so a one-sided implementation regression
///          would still surface.
/// @return 1 when AES-128 and AES-256 encryption/decryption vectors match;
///         otherwise 0.
static int self_test_aes_gcm(void) {
    uint8_t nonce[12] = {0};
    uint8_t pt[16] = {0};
    uint8_t ct[32];
    uint8_t dec[16];
    uint8_t key128[16] = {0};
    uint8_t key256[32] = {0};
    static const uint8_t aes128_exp[32] = {0x03, 0x88, 0xda, 0xce, 0x60, 0xb6, 0xa3, 0x92,
                                           0xf3, 0x28, 0xc2, 0xb9, 0x71, 0xb2, 0xfe, 0x78,
                                           0xab, 0x6e, 0x47, 0xd4, 0x2c, 0xec, 0x13, 0xbd,
                                           0xf5, 0x3a, 0x67, 0xb2, 0x12, 0x57, 0xbd, 0xdf};
    static const uint8_t aes256_exp[32] = {0xce, 0xa7, 0x40, 0x3d, 0x4d, 0x60, 0x6b, 0x6e,
                                           0x07, 0x4e, 0xc5, 0xd3, 0xba, 0xf3, 0x9d, 0x18,
                                           0xd0, 0xd1, 0xc8, 0xa7, 0x99, 0x99, 0x6b, 0xf0,
                                           0x26, 0x5b, 0x98, 0xb5, 0xd4, 0x8a, 0xb9, 0x19};
    if (rt_aes128_gcm_encrypt(key128, nonce, NULL, 0, pt, sizeof(pt), ct) != sizeof(ct))
        return 0;
    if (memcmp(ct, aes128_exp, sizeof(ct)) != 0)
        return 0;
    if (rt_aes128_gcm_decrypt(key128, nonce, NULL, 0, ct, sizeof(ct), dec) != 16)
        return 0;
    if (memcmp(dec, pt, sizeof(pt)) != 0)
        return 0;
    if (rt_aes256_gcm_encrypt(key256, nonce, NULL, 0, pt, sizeof(pt), ct) != sizeof(ct))
        return 0;
    if (memcmp(ct, aes256_exp, sizeof(ct)) != 0)
        return 0;
    return rt_aes256_gcm_decrypt(key256, nonce, NULL, 0, ct, sizeof(ct), dec) == 16 &&
           memcmp(dec, pt, sizeof(pt)) == 0;
}

/// @brief Power-up known-answer test for HMAC-DRBG instantiation + first generate.
/// @details Instantiates a fresh DRBG from a deterministic 48-byte seed
///          (0x00..0x2f) and compares the first 64 bytes of output to a
///          reference vector computed offline against NIST SP 800-90A.
/// @return 1 when the generated bytes match the reference; otherwise 0.
static int self_test_drbg(void) {
    uint8_t seed[48];
    uint8_t out[64];
    static const uint8_t expected[64] = {
        0x0f, 0xfb, 0x80, 0x87, 0x5a, 0x3e, 0x90, 0x22, 0xa4, 0x94, 0x1a, 0x3f, 0xa1,
        0xb0, 0xd3, 0x61, 0x1d, 0xf1, 0x4e, 0x1c, 0xf6, 0x51, 0xa7, 0x3c, 0xe9, 0x22,
        0x9b, 0x9f, 0x3a, 0xd5, 0x68, 0x87, 0x68, 0x04, 0x28, 0x84, 0x57, 0x10, 0x28,
        0x8e, 0xa4, 0x39, 0x1c, 0xa6, 0xf2, 0x1d, 0xf8, 0xcd, 0x88, 0xb7, 0xb2, 0x7a,
        0x8d, 0xfc, 0x16, 0x55, 0x95, 0x40, 0x73, 0x97, 0x59, 0x48, 0x0c, 0x16};
    for (size_t i = 0; i < sizeof(seed); i++)
        seed[i] = (uint8_t)i;
    rt_hmac_drbg_state_t st;
    drbg_instantiate(&st, seed, sizeof(seed));
    drbg_generate(&st, out, sizeof(out));
    module_secure_zero(&st, sizeof(st));
    module_secure_zero(seed, sizeof(seed));
    return memcmp(out, expected, sizeof(out)) == 0;
}

/// @brief Run the full self-test battery (caller already holds the lock).
/// @details Stops at the first failure and records a human-readable
///          banner in @c g_status so callers can surface the exact
///          subsystem that failed.
/// @return 1 when every known-answer test passes; otherwise 0.
static int rt_crypto_module_self_test_unlocked(void) {
    if (!self_test_sha2()) {
        g_status = "self-test failed: SHA-2";
        return 0;
    }
    if (!self_test_hmac_hkdf()) {
        g_status = "self-test failed: HMAC/HKDF";
        return 0;
    }
    if (!self_test_aes_gcm()) {
        g_status = "self-test failed: AES-GCM";
        return 0;
    }
    if (!self_test_drbg()) {
        g_status = "self-test failed: DRBG";
        return 0;
    }
    return 1;
}

/// @brief Locked wrapper around @ref rt_crypto_module_self_test_unlocked.
/// @return 1 on success; 0 if the module is already in ERROR or a fresh test
///         fails and pins it there.
int rt_crypto_module_self_test(void) {
    int ok;
    module_lock();
    if (g_state == RT_CRYPTO_MODULE_STATE_ERROR) {
        module_unlock();
        return 0;
    }
    ok = rt_crypto_module_self_test_unlocked();
    if (!ok)
        g_state = RT_CRYPTO_MODULE_STATE_ERROR;
    module_unlock();
    return ok;
}

/// @brief Bring the module to @c READY state.
/// @details Idempotent: returns 1 immediately when already READY, 0
///          when already pinned in ERROR. Otherwise: runs self-tests,
///          pulls a 48-byte seed from the OS, instantiates the DRBG,
///          and transitions to READY.
/// @return 1 when the module is READY; 0 after a self-test or entropy failure.
int rt_crypto_module_init(void) {
    int ok = 0;
    module_lock();
    if (g_state == RT_CRYPTO_MODULE_STATE_READY) {
        module_unlock();
        return 1;
    }
    if (g_state == RT_CRYPTO_MODULE_STATE_ERROR) {
        module_unlock();
        return 0;
    }

    g_state = RT_CRYPTO_MODULE_STATE_SELF_TESTING;
    g_status = "self-testing";
    if (!rt_crypto_module_self_test_unlocked()) {
        g_state = RT_CRYPTO_MODULE_STATE_ERROR;
        module_unlock();
        return 0;
    }

    uint8_t seed[RT_HMAC_DRBG_SEED_LEN];
    if (module_os_entropy(seed, sizeof(seed)) != 0) {
        g_state = RT_CRYPTO_MODULE_STATE_ERROR;
        g_status = "entropy unavailable";
        module_secure_zero(seed, sizeof(seed));
        module_unlock();
        return 0;
    }
    drbg_instantiate(&g_drbg, seed, sizeof(seed));
    module_secure_zero(seed, sizeof(seed));
    g_state = RT_CRYPTO_MODULE_STATE_READY;
    g_status = "ready";
    ok = 1;
    module_unlock();
    return ok;
}

/// @brief Switch the module between COMPAT and APPROVED.
/// @details Transitioning to APPROVED forces an @c init call (which
///          re-runs the self-tests) so a freshly switched module is
///          always known to be in a passing state. COMPAT transitions are
///          accepted from READY but rejected once ERROR pins the module.
/// @param mode Requested mode. APPROVED triggers initialization and a fresh
///             self-test; other values follow the non-approved policy path.
/// @return 1 on success; 0 when self-tests block the APPROVED move.
int rt_crypto_module_set_mode(rt_crypto_module_mode_t mode) {
    if (mode == RT_CRYPTO_MODULE_MODE_APPROVED) {
        if (!rt_crypto_module_init())
            return 0;
    }
    module_lock();
    if (g_state == RT_CRYPTO_MODULE_STATE_ERROR) {
        module_unlock();
        return 0;
    }
    if (mode == RT_CRYPTO_MODULE_MODE_APPROVED) {
        g_state = RT_CRYPTO_MODULE_STATE_SELF_TESTING;
        g_status = "self-testing";
        if (!rt_crypto_module_self_test_unlocked()) {
            g_state = RT_CRYPTO_MODULE_STATE_ERROR;
            module_unlock();
            return 0;
        }
        g_state = RT_CRYPTO_MODULE_STATE_READY;
        g_status = "ready";
    }
    g_mode = mode;
    module_unlock();
    return 1;
}

/// @brief Read the current mode under the module lock.
/// @return Synchronized snapshot of the process-global operating mode.
rt_crypto_module_mode_t rt_crypto_module_get_mode(void) {
    rt_crypto_module_mode_t mode;
    module_lock();
    mode = g_mode;
    module_unlock();
    return mode;
}

/// @brief Read the current lifecycle state under the module lock.
/// @return Synchronized snapshot of the process-global lifecycle state.
rt_crypto_module_state_t rt_crypto_module_get_state(void) {
    rt_crypto_module_state_t state;
    module_lock();
    state = g_state;
    module_unlock();
    return state;
}

/// @brief Convenience predicate: is APPROVED mode active?
/// @return Non-zero only when the stored mode equals APPROVED.
int rt_crypto_module_is_approved_mode(void) {
    return rt_crypto_module_get_mode() == RT_CRYPTO_MODULE_MODE_APPROVED;
}

/// @brief Policy gate for individual primitives.
/// @details In COMPAT mode every service is allowed unless state is ERROR. In APPROVED only
///          the FIPS-aligned subset (AES-GCM, SHA-2, HMAC-SHA-2,
///          HKDF-SHA-2, PBKDF2-SHA-256, DRBG, ECDSA-P256, RSA-PSS)
///          returns non-zero — legacy services refuse.
/// @param service Service identifier to check against current policy.
/// @return Non-zero when callable; zero in ERROR or when APPROVED policy rejects it.
int rt_crypto_module_service_allowed(rt_crypto_module_service_t service) {
    rt_crypto_module_mode_t mode;
    rt_crypto_module_state_t state;

    module_lock();
    mode = g_mode;
    state = g_state;
    module_unlock();

    if (state == RT_CRYPTO_MODULE_STATE_ERROR)
        return 0;
    if (mode != RT_CRYPTO_MODULE_MODE_APPROVED)
        return 1;
    switch (service) {
        case RT_CRYPTO_SERVICE_AES_GCM:
        case RT_CRYPTO_SERVICE_SHA2:
        case RT_CRYPTO_SERVICE_HMAC_SHA2:
        case RT_CRYPTO_SERVICE_HKDF_SHA2:
        case RT_CRYPTO_SERVICE_PBKDF2_SHA256:
        case RT_CRYPTO_SERVICE_DRBG:
        case RT_CRYPTO_SERVICE_ECDSA_P256:
        case RT_CRYPTO_SERVICE_RSA_PSS:
            return 1;
        default:
            return 0;
    }
}

/// @brief Return a static human-readable status string.
/// @details Reflects the lifecycle state and, when in @c ERROR, the
///          subsystem that pinned the module there.
/// @return Process-lifetime status literal such as `"uninitialized"` or `"ready"`.
const char *rt_crypto_module_status_cstr(void) {
    const char *status;
    module_lock();
    status = (g_state == RT_CRYPTO_MODULE_STATE_UNINITIALIZED) ? "uninitialized"
                                                               : (g_status ? g_status : "unknown");
    module_unlock();
    return status;
}

/// @brief Module-backed random byte generator.
/// @details Ensures the module is initialised (traps with the captured
///          status on failure) and then chunks the request through the
///          DRBG at most @c RT_HMAC_DRBG_MAX_REQUEST bytes per call so
///          a single 64 KiB+ request still respects the NIST per-call
///          ceiling.
/// @param buf Output buffer, or NULL only when @p len is zero.
/// @param len Number of random bytes requested.
void rt_crypto_module_random_bytes(uint8_t *buf, size_t len) {
    if (!buf && len > 0) {
        rt_trap("Crypto.Module: random output buffer is null");
        return;
    }
    if (len == 0)
        return;
    if (!rt_crypto_module_init()) {
        rt_trap(rt_crypto_module_status_cstr());
        rt_abort(rt_crypto_module_status_cstr());
    }
    while (len > 0) {
        size_t chunk = len;
        if (chunk > RT_HMAC_DRBG_MAX_REQUEST)
            chunk = RT_HMAC_DRBG_MAX_REQUEST;
        module_lock();
        drbg_generate(&g_drbg, buf, chunk);
        module_unlock();
        buf += chunk;
        len -= chunk;
    }
}

/// @brief Zia-callable: enable APPROVED mode.
/// @return 1 on success, 0 on failure.
int8_t rt_crypto_module_enable_approved_mode(void) {
    return rt_crypto_module_set_mode(RT_CRYPTO_MODULE_MODE_APPROVED) ? 1 : 0;
}

/// @brief Zia-callable: disable APPROVED mode (return to COMPAT).
void rt_crypto_module_disable_approved_mode(void) {
    (void)rt_crypto_module_set_mode(RT_CRYPTO_MODULE_MODE_COMPAT);
}

/// @brief Zia-callable: is APPROVED mode active? Returns 1 or 0.
/// @return 1 when APPROVED is active; otherwise 0.
int8_t rt_crypto_module_is_approved_mode_zanna(void) {
    return rt_crypto_module_is_approved_mode() ? 1 : 0;
}

/// @brief Zia-callable: status banner as a managed @c rt_string.
/// @return Managed runtime string containing the current status banner.
rt_string rt_crypto_module_status_text(void) {
    return rt_const_cstr(rt_crypto_module_status_cstr());
}
