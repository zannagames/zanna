//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/network/rt_crypto_module.h
// Purpose: Zero-dependency cryptographic module boundary, approved-mode state,
//          self-tests, policy checks, and DRBG-backed random generation.
//
// Key invariants:
//   - A process-global lock serializes initialization, self-tests, mode changes,
//     policy snapshots, and HMAC-DRBG generation.
//   - Any self-test or startup-entropy failure permanently pins the module in
//     ERROR and denies every service.
//   - APPROVED mode permits only the enumerated FIPS-aligned service subset.
//
// Ownership/Lifetime:
//   - Module state and status literals have process lifetime.
//   - Random output is written into caller-owned buffers; the managed status
//     wrapper returns a runtime-owned string reference.
//
// Links: src/runtime/network/rt_crypto_module.c (implementation),
//        src/runtime/network/rt_crypto.c (primitive and entropy dispatch).
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Declares the global cryptographic policy, self-test, and DRBG boundary.
 * @details Defines compatibility and approved modes, lifecycle and service
 * identifiers, synchronized state queries and transitions, policy checks,
 * diagnostic status, and approved-mode random generation.
 */

#pragma once

#include "rt_string.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Crypto module operating mode.
/// @details COMPAT exposes the full primitive surface (legacy + experimental);
///          APPROVED restricts callers to the FIPS-aligned subset (AES-GCM,
///          SHA-2 family, HMAC-SHA2, HKDF, PBKDF2, HMAC-DRBG, ECDSA-P256,
///          RSA-PSS). The current public X25519-only TLS profile is rejected
///          rather than rerouted in APPROVED mode.
typedef enum rt_crypto_module_mode {
    RT_CRYPTO_MODULE_MODE_COMPAT = 0,   ///< Default: every primitive available.
    RT_CRYPTO_MODULE_MODE_APPROVED = 1, ///< Approved mode: legacy primitives refuse to run.
} rt_crypto_module_mode_t;

/// @brief Lifecycle state of the crypto module singleton.
typedef enum rt_crypto_module_state {
    RT_CRYPTO_MODULE_STATE_UNINITIALIZED = 0, ///< @c rt_crypto_module_init has not run yet.
    RT_CRYPTO_MODULE_STATE_SELF_TESTING = 1,  ///< Power-up self-tests in progress.
    RT_CRYPTO_MODULE_STATE_READY = 2,         ///< Self-tests passed; module is usable.
    RT_CRYPTO_MODULE_STATE_ERROR = 3,         ///< Self-test failure; module is locked down.
} rt_crypto_module_state_t;

/// @brief Catalogue of cryptographic services the module gates by mode.
/// @details Service identifiers are passed to @ref rt_crypto_module_service_allowed
///          so callers can pre-flight a primitive request before running it.
///          In APPROVED mode only the "approved" entries return non-zero;
///          legacy entries (MD5, SHA-1, ChaCha20-Poly1305, X25519, scrypt,
///          CRC32, SipHash) refuse.
typedef enum rt_crypto_module_service {
    RT_CRYPTO_SERVICE_AES_GCM = 0,                ///< AES-128/256-GCM AEAD (approved).
    RT_CRYPTO_SERVICE_SHA2 = 1,                   ///< SHA-256/384/512 (approved).
    RT_CRYPTO_SERVICE_HMAC_SHA2 = 2,              ///< HMAC over SHA-256/384/512 (approved).
    RT_CRYPTO_SERVICE_HKDF_SHA2 = 3,              ///< HKDF over SHA-256/384 (approved).
    RT_CRYPTO_SERVICE_PBKDF2_SHA256 = 4,          ///< PBKDF2 with HMAC-SHA-256 (approved).
    RT_CRYPTO_SERVICE_DRBG = 5,                   ///< HMAC-DRBG entropy source (approved).
    RT_CRYPTO_SERVICE_ECDSA_P256 = 6,             ///< ECDSA over NIST P-256 (approved).
    RT_CRYPTO_SERVICE_RSA_PSS = 7,                ///< RSA-PSS signatures (approved).
    RT_CRYPTO_SERVICE_TLS13_APPROVED_PROFILE = 8, ///< TLS 1.3 with approved-only suites.
    RT_CRYPTO_SERVICE_CHACHA20_POLY1305 = 9,      ///< ChaCha20-Poly1305 (legacy in APPROVED).
    RT_CRYPTO_SERVICE_X25519 = 10,                ///< X25519 key agreement (legacy in APPROVED).
    RT_CRYPTO_SERVICE_SCRYPT = 11,                ///< scrypt KDF (legacy in APPROVED).
    RT_CRYPTO_SERVICE_MD5 = 12,                   ///< MD5 (legacy, never approved).
    RT_CRYPTO_SERVICE_SHA1 = 13,                  ///< SHA-1 (legacy, never approved).
    RT_CRYPTO_SERVICE_CRC32 = 14,                 ///< CRC-32 (non-cryptographic).
    RT_CRYPTO_SERVICE_SIPHASH = 15,               ///< SipHash hash (non-cryptographic).
} rt_crypto_module_service_t;

/// @brief Initialise the crypto module singleton.
/// @details Idempotent — repeated calls are no-ops once the module is READY.
///          Runs the embedded self-tests on first call; on failure the
///          module transitions to ERROR and every subsequent service
///          predicate returns 0.
/// @return 1 on success, 0 when self-tests fail.
int rt_crypto_module_init(void);

/// @brief Re-run the module's self-tests.
/// @details Forces a fresh self-test pass even when the module is READY.
///          Used by approved-mode startup and by host applications that
///          want to confirm a known-answer baseline before sensitive work.
/// @return 1 on success; 0 leaves the module in ERROR.
int rt_crypto_module_self_test(void);

/// @brief Switch between COMPAT and APPROVED mode.
/// @details Switching to APPROVED initializes the module if necessary and
///          then runs the self-tests again. A module pinned in ERROR rejects
///          every transition. Non-APPROVED values follow the compatibility
///          policy path.
/// @param mode Requested operating mode.
/// @return 1 on success; 0 when the transition is illegal.
int rt_crypto_module_set_mode(rt_crypto_module_mode_t mode);

/// @brief Return the module's current operating mode.
/// @return Synchronized snapshot of the process-global mode.
rt_crypto_module_mode_t rt_crypto_module_get_mode(void);

/// @brief Return the module's lifecycle state.
/// @return Synchronized snapshot of the process-global lifecycle state.
rt_crypto_module_state_t rt_crypto_module_get_state(void);

/// @brief Predicate: is the module currently in APPROVED mode?
/// @return Non-zero in APPROVED, zero otherwise.
int rt_crypto_module_is_approved_mode(void);

/// @brief Pre-flight check whether @p service is callable right now.
/// @details Returns non-zero for services in COMPAT unless the module is pinned
///          in ERROR. In APPROVED, returns zero for legacy/non-approved services
///          so callers can fail or select an allowed operation.
/// @param service Service identifier to check.
/// @return Non-zero when current state and mode allow @p service; otherwise zero.
int rt_crypto_module_service_allowed(rt_crypto_module_service_t service);

/// @brief Return a static human-readable status banner for diagnostics.
/// @details The string lives in static storage and is safe to embed in
///          log messages without copying.
/// @return Process-lifetime status literal.
const char *rt_crypto_module_status_cstr(void);

/// @brief Fill @p buf with @p len cryptographically-secure random bytes.
/// @details In COMPAT delegates to the OS RNG (`getrandom`,
///          `arc4random_buf`, `BCryptGenRandom`). In APPROVED routes through the
///          embedded HMAC-DRBG under the module's instantiate/reseed policy.
/// @param buf Caller-owned output buffer, or NULL only when @p len is zero.
/// @param len Number of random bytes requested; large requests are internally chunked.
void rt_crypto_module_random_bytes(uint8_t *buf, size_t len);

/// @brief Zia-callable wrapper: switch the module to APPROVED mode.
/// @return 1 on success, 0 on failure (illegal state or self-test fail).
int8_t rt_crypto_module_enable_approved_mode(void);

/// @brief Zia-callable wrapper: switch the module back to COMPAT mode.
/// @details Always succeeds when the module is READY; no-op otherwise.
void rt_crypto_module_disable_approved_mode(void);

/// @brief Zia-callable predicate: is APPROVED mode active?
/// @return 1 if yes, 0 otherwise.
int8_t rt_crypto_module_is_approved_mode_zanna(void);

/// @brief Zia-callable status banner returning a managed @c rt_string.
/// @details Equivalent payload to @ref rt_crypto_module_status_cstr
///          but boxed for direct return into Zia code.
/// @return Managed runtime string containing the current status banner.
rt_string rt_crypto_module_status_text(void);

#ifdef __cplusplus
}
#endif
