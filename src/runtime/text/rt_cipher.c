//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/text/rt_cipher.c
// Purpose: Implements high-level authenticated encryption for Zanna.Crypto.Cipher.
//          Compatibility mode uses ChaCha20-Poly1305; approved mode uses
//          AES-256-GCM. Password and raw-key forms each expose AAD-aware,
//          Result, and Option variants.
//
// Key invariants:
//   - Nonces are 12 bytes drawn entirely from the CSPRNG (full 96-bit random
//     AEAD nonce). Safe per-key message bound is ~2^32 messages (NIST SP
//     800-38D random-nonce guidance).
//   - Password format: [magic(4)][iterations(4)][salt(16)][nonce(12)]
//     [ciphertext][tag(16)]. Raw-key format: [magic(4)][nonce(12)]
//     [ciphertext][tag(16)].
//   - AAD overloads compose the complete format header with application AAD so
//     the active AEAD tag authenticates both wrapper and application context.
//   - Authenticated encryption: any tampering with ciphertext, nonce, magic,
//     or AAD is detected and Decrypt returns NULL (not a trap).
//   - Keys must be exactly 32 bytes (256-bit); wrong size traps.
//   - Decryption verifies the active AEAD tag before returning plaintext.
//   - Nonce allocation is stateless CSPRNG output; all operation state is local.
//
// Ownership/Lifetime:
//   - Returned ciphertext/plaintext rt_bytes buffers are fresh allocations
//     owned by the caller.
//   - Input key, plaintext, and ciphertext buffers are borrowed read-only.
//
// Links: src/runtime/text/rt_cipher.h (public API),
//        src/runtime/text/rt_aes.h (AES-GCM cipher, alternative algorithm),
//        src/runtime/text/rt_keyderive.h (PBKDF2 / scrypt key derivation),
//        src/runtime/text/rt_rand.h (nonce generation)
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_cipher.c
 * @brief Implements the high-level authenticated-encryption facade.
 * @details Password forms derive 256-bit keys with PBKDF2 and random salts;
 *          raw-key forms validate fixed key sizes. Both generate full-width
 *          random nonces, authenticate framing plus optional application AAD,
 *          select ChaCha20-Poly1305 or approved AES-GCM policy, and expose
 *          nullable, Result, and Option decryption outcomes.
 */

#include "rt_cipher.h"
#include "rt_bytes.h"
#include "rt_crypto.h"
#include "rt_crypto_module.h"
#include "rt_internal.h"
#include "rt_keyderive_internal.h"
#include "rt_object.h"
#include "rt_option.h"
#include "rt_platform.h"
#include "rt_result.h"
#include "rt_string.h"

#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// External trap function (defined in rt_io.c)
#include "rt_trap.h"

//=============================================================================
// Internal Constants
//=============================================================================

#define CIPHER_SALT_SIZE 16
#define CIPHER_NONCE_SIZE 12
#define CIPHER_KEY_SIZE 32
#define CIPHER_TAG_SIZE 16
#define CIPHER_PBKDF2_ITERATIONS 300000
#define CIPHER_PW_HEADER_SIZE 36
#define CIPHER_KEY_HEADER_SIZE 16
#define CIPHER_CHACHA20_MAX_BYTES (((UINT64_C(1) << 32) - 1u) * 64u)
#define CIPHER_AES_GCM_MAX_BYTES (((UINT64_C(1) << 32) - 2u) * 16u)

static const uint8_t CIPHER_PW_MAGIC[4] = {'V', 'C', 'P', '2'};
static const uint8_t CIPHER_KEY_MAGIC[4] = {'V', 'C', 'K', '2'};
static const uint8_t CIPHER_PW_APPROVED_MAGIC[4] = {'V', 'C', 'A', '1'};
static const uint8_t CIPHER_KEY_APPROVED_MAGIC[4] = {'V', 'K', 'A', '1'};

// HKDF info string for key derivation
static const char *HKDF_INFO = "zanna-cipher-v1";

/// @brief Decrypt password-protected ciphertext without associated data.
/// @param ciphertext Borrowed encrypted Bytes object.
/// @param password Borrowed password string.
/// @return New plaintext Bytes object, or NULL on authentication or decoding failure.
typedef void *(*cipher_password_decrypt_fn)(void *ciphertext, rt_string password);
/// @brief Decrypt password-protected ciphertext with associated data.
/// @param ciphertext Borrowed encrypted Bytes object.
/// @param password Borrowed password string.
/// @param aad Borrowed associated-data Bytes object.
/// @return New plaintext Bytes object, or NULL on authentication or decoding failure.
typedef void *(*cipher_password_aad_decrypt_fn)(void *ciphertext, rt_string password, void *aad);
/// @brief Decrypt key-protected ciphertext without associated data.
/// @param ciphertext Borrowed encrypted Bytes object.
/// @param key Borrowed key Bytes object.
/// @return New plaintext Bytes object, or NULL on authentication or decoding failure.
typedef void *(*cipher_key_decrypt_fn)(void *ciphertext, void *key);
/// @brief Decrypt key-protected ciphertext with associated data.
/// @param ciphertext Borrowed encrypted Bytes object.
/// @param key Borrowed key Bytes object.
/// @param aad Borrowed associated-data Bytes object.
/// @return New plaintext Bytes object, or NULL on authentication or decoding failure.
typedef void *(*cipher_key_aad_decrypt_fn)(void *ciphertext, void *key, void *aad);

/// @brief Release a temporary runtime object created by a decryptor.
/// @details `Result.Ok` and `Option.Some` retain object payloads. Wrappers that
///          move a freshly allocated plaintext object into those containers
///          call this helper to drop the decryptor's original ownership.
/// @param obj Temporary runtime object reference; NULL is ignored.
static void cipher_release_temp_object(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief Return a string handle for the active trap message or a fallback.
/// @details The returned string is a runtime literal/constant handle suitable
///          for passing to Result.ErrStr without transferring ownership.
/// @param fallback Message selected when no nonempty trap diagnostic is active.
/// @return Borrowed constant runtime string containing the chosen diagnostic.
static rt_string cipher_current_error_message(const char *fallback) {
    const char *err = rt_trap_get_error();
    if (!err || !err[0])
        err = fallback && fallback[0] ? fallback : "Cipher decrypt failed";
    return rt_const_cstr(err);
}

/// @brief Wrap a freshly allocated plaintext object in `Result.Ok`.
/// @details Returns `Err(str)` when @p plaintext is NULL. On success the
///          Result retains @p plaintext and this helper releases the caller's
///          temporary ownership reference.
/// @param plaintext Newly allocated plaintext Bytes object, or NULL.
/// @param null_message Diagnostic used for a NULL plaintext result.
/// @return Caller-owned Result containing plaintext Bytes or an error string.
static void *cipher_plaintext_result(void *plaintext, const char *null_message) {
    if (!plaintext)
        return rt_result_err_str(rt_const_cstr(null_message));
    void *result = rt_result_ok(plaintext);
    cipher_release_temp_object(plaintext);
    return result;
}

/// @brief Wrap a freshly allocated plaintext object in `Option.Some`.
/// @details Returns `None` when @p plaintext is NULL. On success the Option
///          retains @p plaintext and this helper releases the decryptor's
///          temporary ownership reference.
/// @param plaintext Newly allocated plaintext Bytes object, or NULL.
/// @return Caller-owned Option containing plaintext Bytes or None.
static void *cipher_plaintext_option(void *plaintext) {
    if (!plaintext)
        return rt_option_none();
    void *option = rt_option_some(plaintext);
    cipher_release_temp_object(plaintext);
    return option;
}

/// @brief Run a password decryptor and convert traps/NULL into `Result`.
/// @param fn Decryptor invoked inside the temporary trap recovery scope.
/// @param ciphertext Opaque ciphertext argument forwarded to @p fn.
/// @param password Runtime password forwarded to @p fn.
/// @param null_message Diagnostic used when @p fn returns NULL.
/// @param trap_fallback Diagnostic used when a caught trap has no message.
/// @return Caller-owned Result containing plaintext Bytes or diagnostic string.
static void *cipher_password_result(cipher_password_decrypt_fn fn,
                                    void *ciphertext,
                                    rt_string password,
                                    const char *null_message,
                                    const char *trap_fallback) {
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        rt_string message = cipher_current_error_message(trap_fallback);
        rt_trap_clear_recovery();
        return rt_result_err_str(message);
    }
    void *plaintext = fn(ciphertext, password);
    rt_trap_clear_recovery();
    return cipher_plaintext_result(plaintext, null_message);
}

/// @brief Run a password decryptor and convert traps/NULL into `Option`.
/// @param fn Decryptor invoked inside the temporary trap recovery scope.
/// @param ciphertext Opaque ciphertext argument forwarded to @p fn.
/// @param password Runtime password forwarded to @p fn.
/// @return Caller-owned Option containing plaintext Bytes, or None after a trap
///         or NULL decryptor result.
static void *cipher_password_option(cipher_password_decrypt_fn fn,
                                    void *ciphertext,
                                    rt_string password) {
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        rt_trap_clear_recovery();
        return rt_option_none();
    }
    void *plaintext = fn(ciphertext, password);
    rt_trap_clear_recovery();
    return cipher_plaintext_option(plaintext);
}

/// @brief Run a password-plus-AAD decryptor and convert traps/NULL into `Result`.
/// @param fn Decryptor invoked inside the temporary trap recovery scope.
/// @param ciphertext Opaque ciphertext argument forwarded to @p fn.
/// @param password Runtime password forwarded to @p fn.
/// @param aad Optional additional-authenticated-data argument forwarded to @p fn.
/// @param null_message Diagnostic used when @p fn returns NULL.
/// @param trap_fallback Diagnostic used when a caught trap has no message.
/// @return Caller-owned Result containing plaintext Bytes or diagnostic string.
static void *cipher_password_aad_result(cipher_password_aad_decrypt_fn fn,
                                        void *ciphertext,
                                        rt_string password,
                                        void *aad,
                                        const char *null_message,
                                        const char *trap_fallback) {
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        rt_string message = cipher_current_error_message(trap_fallback);
        rt_trap_clear_recovery();
        return rt_result_err_str(message);
    }
    void *plaintext = fn(ciphertext, password, aad);
    rt_trap_clear_recovery();
    return cipher_plaintext_result(plaintext, null_message);
}

/// @brief Run a password-plus-AAD decryptor and convert traps/NULL into `Option`.
/// @param fn Decryptor invoked inside the temporary trap recovery scope.
/// @param ciphertext Opaque ciphertext argument forwarded to @p fn.
/// @param password Runtime password forwarded to @p fn.
/// @param aad Optional additional-authenticated-data argument forwarded to @p fn.
/// @return Caller-owned Option containing plaintext Bytes, or None after any
///         authentication, validation, or trapped failure.
static void *cipher_password_aad_option(cipher_password_aad_decrypt_fn fn,
                                        void *ciphertext,
                                        rt_string password,
                                        void *aad) {
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        rt_trap_clear_recovery();
        return rt_option_none();
    }
    void *plaintext = fn(ciphertext, password, aad);
    rt_trap_clear_recovery();
    return cipher_plaintext_option(plaintext);
}

/// @brief Run a raw-key decryptor and convert traps/NULL into `Result`.
/// @param fn Decryptor invoked inside the temporary trap recovery scope.
/// @param ciphertext Opaque ciphertext argument forwarded to @p fn.
/// @param key Opaque raw-key argument forwarded to @p fn.
/// @param null_message Diagnostic used when @p fn returns NULL.
/// @param trap_fallback Diagnostic used when a caught trap has no message.
/// @return Caller-owned Result containing plaintext Bytes or diagnostic string.
static void *cipher_key_result(cipher_key_decrypt_fn fn,
                               void *ciphertext,
                               void *key,
                               const char *null_message,
                               const char *trap_fallback) {
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        rt_string message = cipher_current_error_message(trap_fallback);
        rt_trap_clear_recovery();
        return rt_result_err_str(message);
    }
    void *plaintext = fn(ciphertext, key);
    rt_trap_clear_recovery();
    return cipher_plaintext_result(plaintext, null_message);
}

/// @brief Run a raw-key decryptor and convert traps/NULL into `Option`.
/// @param fn Decryptor invoked inside the temporary trap recovery scope.
/// @param ciphertext Opaque ciphertext argument forwarded to @p fn.
/// @param key Opaque raw-key argument forwarded to @p fn.
/// @return Caller-owned Option containing plaintext Bytes, or None after a trap
///         or NULL decryptor result.
static void *cipher_key_option(cipher_key_decrypt_fn fn, void *ciphertext, void *key) {
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        rt_trap_clear_recovery();
        return rt_option_none();
    }
    void *plaintext = fn(ciphertext, key);
    rt_trap_clear_recovery();
    return cipher_plaintext_option(plaintext);
}

/// @brief Run a raw-key-plus-AAD decryptor and convert traps/NULL into `Result`.
/// @param fn Decryptor invoked inside the temporary trap recovery scope.
/// @param ciphertext Opaque ciphertext argument forwarded to @p fn.
/// @param key Opaque raw-key argument forwarded to @p fn.
/// @param aad Optional additional-authenticated-data argument forwarded to @p fn.
/// @param null_message Diagnostic used when @p fn returns NULL.
/// @param trap_fallback Diagnostic used when a caught trap has no message.
/// @return Caller-owned Result containing plaintext Bytes or diagnostic string.
static void *cipher_key_aad_result(cipher_key_aad_decrypt_fn fn,
                                   void *ciphertext,
                                   void *key,
                                   void *aad,
                                   const char *null_message,
                                   const char *trap_fallback) {
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        rt_string message = cipher_current_error_message(trap_fallback);
        rt_trap_clear_recovery();
        return rt_result_err_str(message);
    }
    void *plaintext = fn(ciphertext, key, aad);
    rt_trap_clear_recovery();
    return cipher_plaintext_result(plaintext, null_message);
}

/// @brief Run a raw-key-plus-AAD decryptor and convert traps/NULL into `Option`.
/// @param fn Decryptor invoked inside the temporary trap recovery scope.
/// @param ciphertext Opaque ciphertext argument forwarded to @p fn.
/// @param key Opaque raw-key argument forwarded to @p fn.
/// @param aad Optional additional-authenticated-data argument forwarded to @p fn.
/// @return Caller-owned Option containing plaintext Bytes, or None after any
///         authentication, validation, or trapped failure.
static void *cipher_key_aad_option(cipher_key_aad_decrypt_fn fn,
                                   void *ciphertext,
                                   void *key,
                                   void *aad) {
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        rt_trap_clear_recovery();
        return rt_option_none();
    }
    void *plaintext = fn(ciphertext, key, aad);
    rt_trap_clear_recovery();
    return cipher_plaintext_option(plaintext);
}

//=============================================================================
// Internal Bytes Access
//=============================================================================

/// @brief Read the raw byte buffer pointer from a Bytes object handle (NULL-safe).
/// @param obj Candidate Bytes object; NULL is accepted.
/// @return Borrowed mutable data pointer, NULL for a NULL/empty object or after
///         trapping for an invalid runtime type.
static inline uint8_t *bytes_data(void *obj) {
    if (obj && !rt_bytes_is_bytes(obj)) {
        rt_trap("Cipher: invalid Bytes object");
        return NULL;
    }
    return rt_bytes_data(obj);
}

/// @brief Read the byte length from a Bytes object handle (NULL -> 0).
/// @param obj Candidate Bytes object; NULL is accepted.
/// @return Byte length, zero for NULL, or -1 after trapping for an invalid type.
static inline int64_t bytes_len(void *obj) {
    if (obj && !rt_bytes_is_bytes(obj)) {
        rt_trap("Cipher: invalid Bytes object");
        return -1;
    }
    return rt_bytes_len(obj);
}

/// @brief Allocate a runtime Bytes object or raise an operation-specific trap.
/// @param len Requested byte length.
/// @param op Trap diagnostic used when allocation fails.
/// @return New caller-owned Bytes object, or NULL after trapping.
static void *cipher_bytes_new_or_trap(int64_t len, const char *op) {
    void *bytes = rt_bytes_new(len);
    if (!bytes)
        rt_trap(op);
    return bytes;
}

/// @brief Allocate a temporary plaintext buffer before wrapping it as Bytes.
/// @details AEAD decryptors verify tags before writing plaintext, but callers
///          should still avoid creating a runtime Bytes object until after
///          authentication succeeds. This helper allocates a plain heap buffer
///          sized for @p len, using one byte for zero-length plaintext so the
///          pointer remains non-NULL for APIs that permit empty output.
/// @param len Plaintext length in bytes.
/// @param trap_msg Allocation failure trap message.
/// @return Heap buffer to free with `free`, or NULL after reporting a trap.
static uint8_t *cipher_plain_temp_alloc(int64_t len, const char *trap_msg) {
    if (len < 0 || (uint64_t)len > (uint64_t)SIZE_MAX) {
        rt_trap("Cipher.Decrypt: invalid plaintext length");
        return NULL;
    }
    size_t alloc_len = len > 0 ? (size_t)len : 1u;
    uint8_t *buf = (uint8_t *)malloc(alloc_len);
    if (!buf)
        rt_trap(trap_msg ? trap_msg : "Cipher.Decrypt: allocation failed");
    return buf;
}

//=============================================================================
// Key Derivation
//=============================================================================

/// @brief Optimization-resistant zero-fill for sensitive key/nonce buffers.
/// @details Writes through a `volatile` pointer so the compiler can't
///          elide the stores via dead-store elimination. Used after
///          every key-derivation and AEAD operation so transient
///          secrets don't linger in stack frames where a later memory
///          dump (core file, swap, leaked mapping) could expose them.
/// @param ptr Start of the writable sensitive-memory span.
/// @param len Number of bytes to overwrite.
static void cipher_secure_zero(void *ptr, size_t len) {
    volatile uint8_t *p = (volatile uint8_t *)ptr;
    while (len-- > 0)
        *p++ = 0;
}

/// @brief Extract a C string pointer and byte length from an rt_string password.
///        Calls rt_trap with @p op if the password handle is NULL.
///        Returns an empty string and sets *len = 0 for zero-length input.
/// @param password Runtime password string to validate.
/// @param len Required destination for its byte length.
/// @param op Trap diagnostic for a NULL password.
/// @param ok Optional success flag cleared initially and set for a valid view.
/// @return Borrowed password bytes, or an empty static string for valid empty
///         input and all failure fallbacks.
static const char *cipher_password_bytes(rt_string password, size_t *len, const char *op, int *ok) {
    if (len)
        *len = 0;
    if (ok)
        *ok = 0;
    if (!len) {
        rt_trap("Cipher: internal password length pointer is null");
        return "";
    }
    if (!password) {
        rt_trap(op);
        return "";
    }
    if (!rt_string_is_handle((const void *)password)) {
        rt_trap("Cipher: invalid password string handle");
        return "";
    }

    int64_t len64 = rt_str_len(password);
    if (len64 < 0) {
        rt_trap("Cipher: invalid password length");
        return "";
    }
    if (len64 == 0) {
        *len = 0;
        if (ok)
            *ok = 1;
        return "";
    }

    const char *pwd = rt_string_cstr(password);
    if (!pwd) {
        rt_trap("Cipher: password data is null");
        return "";
    }

    *len = (size_t)len64;
    if (ok)
        *ok = 1;
    return pwd;
}

/// @brief Compute and validate the output length for an encryption operation.
///        Traps if input_len is negative, would overflow when added to @p overhead,
///        or exceeds the AEAD single-key stream limit.
/// @param input_len Plaintext length to validate.
/// @param overhead Fixed framing and authentication bytes added to the output.
/// @param max_input_len Algorithm-specific per-message plaintext limit.
/// @param op Trap diagnostic for a negative input length.
/// @return Validated sum of input and overhead, or 0 after trapping.
static int64_t cipher_checked_output_len(int64_t input_len,
                                         int64_t overhead,
                                         uint64_t max_input_len,
                                         const char *op) {
    if (input_len < 0) {
        rt_trap(op);
        return 0;
    }
    if (input_len > INT64_MAX - overhead) {
        rt_trap("Cipher: input is too large");
        return 0;
    }
    if ((uint64_t)input_len > max_input_len) {
        rt_trap("Cipher: input exceeds AEAD size limit");
        return 0;
    }
    return input_len + overhead;
}

/// @brief Derive a 32-byte key using the legacy HKDF-SHA256 scheme.
/// @details HKDF (RFC 5869) is fast and unsuitable for password-based key
///          derivation in isolation — it has no cost parameter, so an
///          attacker brute-forcing the password gets one hash evaluation
///          per guess. This path is **legacy-only**: kept for decrypting
///          ciphertext produced by older versions of the runtime that used
///          HKDF before PBKDF2 was adopted. New encryption goes through
///          `derive_key_pbkdf2` instead.
///          Two-step HKDF: Extract collapses (salt, password) into a 256-bit
///          PRK; Expand stretches the PRK into the 32-byte cipher key,
///          domain-separated by the `HKDF_INFO` string. PRK is zeroed before
///          return so it can't linger in heap memory.
/// @param password Borrowed password bytes.
/// @param password_len Password byte count.
/// @param salt Borrowed legacy salt bytes.
/// @param salt_len Salt byte count.
/// @param key Receives the 32-byte derived key and must later be zeroized.
static void derive_key_legacy(const char *password,
                              size_t password_len,
                              const uint8_t *salt,
                              size_t salt_len,
                              uint8_t key[CIPHER_KEY_SIZE]) {
    uint8_t prk[32];

    // HKDF-Extract: PRK = HMAC-SHA256(salt, password)
    rt_hkdf_extract(salt, salt_len, (const uint8_t *)password, password_len, prk);

    // HKDF-Expand: key = HKDF-Expand(PRK, info, 32)
    if (rt_hkdf_expand(prk, (const uint8_t *)HKDF_INFO, strlen(HKDF_INFO), key, CIPHER_KEY_SIZE) !=
        0) {
        rt_trap("Cipher: legacy HKDF key derivation failed");
    }

    cipher_secure_zero(prk, sizeof(prk));
}

/// @brief Derive a 32-byte key using PBKDF2-HMAC-SHA256 (modern default).
/// @details Iteration count is fixed at `CIPHER_PBKDF2_ITERATIONS` (300,000
///          at the time of writing), chosen to make per-guess brute force
///          expensive on modern hardware while keeping the per-call latency
///          tolerable for an interactive password-based encrypt operation
///          (~50–100 ms on a typical desktop CPU). This is the path all new
///          encryption uses; `derive_key_legacy` is only invoked when
///          decrypting older ciphertexts that carry a legacy version tag.
/// @param password Borrowed password bytes.
/// @param password_len Password byte count.
/// @param salt Borrowed random salt bytes.
/// @param salt_len Salt byte count.
/// @param key Receives the 32-byte derived key and must later be zeroized.
static void derive_key_pbkdf2(const char *password,
                              size_t password_len,
                              const uint8_t *salt,
                              size_t salt_len,
                              uint8_t key[CIPHER_KEY_SIZE]) {
    rt_keyderive_pbkdf2_sha256_raw((const uint8_t *)password,
                                   password_len,
                                   salt,
                                   salt_len,
                                   CIPHER_PBKDF2_ITERATIONS,
                                   key,
                                   CIPHER_KEY_SIZE);
}

/// @brief Write a 32-bit unsigned integer to @p out in big-endian byte order.
/// @details Used by the wire-format header builder to encode parameter
///          fields (PBKDF2 iteration count, version flags) into the
///          ciphertext envelope.
/// @param out Writable four-byte destination.
/// @param v Value to encode.
static void write_be32(uint8_t *out, uint32_t v) {
    out[0] = (uint8_t)(v >> 24);
    out[1] = (uint8_t)(v >> 16);
    out[2] = (uint8_t)(v >> 8);
    out[3] = (uint8_t)v;
}

/// @brief Fill a 96-bit AEAD nonce with full-width CSPRNG output.
/// @details All 12 bytes are drawn from the CSPRNG, so every nonce is
///          independently random per message AND per process (VDOC-172): the
///          previous 4-byte-random + 8-byte-process-counter construction gave
///          only 32 bits of cross-process separation, so two processes/restarts
///          reusing the same raw key and drawing the same 32-bit prefix reused
///          the entire AEAD nonce sequence. A full 96-bit random nonce restores
///          the documented random-nonce space; per NIST SP 800-38D the safe
///          per-key message bound for random nonces is ~2^32 messages (a
///          ~2^-32 collision probability), which the API documents. Encryption
///          is inherently non-deterministic, so this does not affect VM/native
///          determinism for defined programs.
/// @param nonce Destination buffer of exactly @c CIPHER_NONCE_SIZE bytes.
static void cipher_random_nonce(uint8_t nonce[CIPHER_NONCE_SIZE]) {
    rt_crypto_random_bytes(nonce, CIPHER_NONCE_SIZE);
}

/// @brief Read a big-endian 32-bit unsigned integer from @p in.
/// @details Inverse of write_be32. Used during decrypt to recover the
///          encoded iteration count and version flags from the envelope.
/// @param in Borrowed four-byte encoded value.
/// @return Decoded host-order value.
static uint32_t read_be32(const uint8_t *in) {
    return ((uint32_t)in[0] << 24) | ((uint32_t)in[1] << 16) | ((uint32_t)in[2] << 8) |
           (uint32_t)in[3];
}

/// @brief Test whether the first four bytes of @p data match the 4-byte @p magic.
/// @details Used to dispatch decrypt requests to the correct format
///          handler — CIPHER_PW_MAGIC ("VCP2") for password-derived
///          ciphertexts versus CIPHER_KEY_MAGIC ("VCK2") for raw-key
///          ciphertexts. NULL pointer or len < 4 returns 0.
/// @param data Borrowed frame bytes; may be NULL.
/// @param len Number of available bytes.
/// @param magic Four-byte format identifier to compare.
/// @return 1 for an exact prefix match, otherwise 0.
static int has_magic(const uint8_t *data, int64_t len, const uint8_t magic[4]) {
    return data && len >= 4 && memcmp(data, magic, 4) == 0;
}

/// @brief Concatenate the format header with caller-supplied AAD into a single buffer.
/// @details The active AEAD tag must authenticate the wrapper format header
///          (so an attacker can't strip / replace it) AND the application's
///          AAD. We splice them: returned buffer is [header || user_aad];
///          @p aad_out points at it; @p aad_len_out is the total length.
///          When user AAD is empty, returns the header directly without
///          allocating. Caller frees the returned buffer (NULL when no
///          allocation occurred). Traps on bytes_len < 0 or arithmetic
///          overflow.
/// @param header Borrowed framing header bytes.
/// @param header_len Header byte count.
/// @param aad_obj Optional Bytes object containing caller AAD.
/// @param aad_out Required destination for the combined borrowed/allocated span.
/// @param aad_len_out Required destination for combined length; SIZE_MAX marks
///        failure.
/// @return Caller-owned combined allocation when user AAD is nonempty; NULL
///         when the header can be used directly or after a reported failure.
static uint8_t *combine_aad(const uint8_t *header,
                            size_t header_len,
                            void *aad_obj,
                            const uint8_t **aad_out,
                            size_t *aad_len_out) {
    if (aad_out)
        *aad_out = NULL;
    if (aad_len_out)
        *aad_len_out = 0;
    int64_t user_len64 = aad_obj ? bytes_len(aad_obj) : 0;
    if (user_len64 < 0) {
        rt_trap("Cipher: invalid AAD length");
        if (aad_len_out)
            *aad_len_out = SIZE_MAX;
        return NULL;
    }
    size_t user_len = (size_t)user_len64;
    const uint8_t *user = user_len > 0 ? bytes_data(aad_obj) : NULL;
    if (user_len > 0 && !user) {
        rt_trap("Cipher: invalid AAD data");
        if (aad_len_out)
            *aad_len_out = SIZE_MAX;
        return NULL;
    }

    if (user_len == 0) {
        *aad_out = header_len > 0 ? header : NULL;
        *aad_len_out = header_len;
        return NULL;
    }
    if (header_len > SIZE_MAX - user_len) {
        rt_trap("Cipher: AAD too large");
        if (aad_len_out)
            *aad_len_out = SIZE_MAX;
        return NULL;
    }

    uint8_t *combined = (uint8_t *)malloc(header_len + user_len);
    if (!combined) {
        rt_trap("Cipher: memory allocation failed");
        if (aad_len_out)
            *aad_len_out = SIZE_MAX;
        return NULL;
    }
    if (header_len > 0)
        memcpy(combined, header, header_len);
    memcpy(combined + header_len, user, user_len);
    *aad_out = combined;
    *aad_len_out = header_len + user_len;
    return combined;
}

//=============================================================================
// Password-Based Encryption
//=============================================================================

/// @brief Encrypt data using a password with the mode-selected AEAD.
/// @details Derives a 256-bit key from the password using PBKDF2-HMAC-SHA256
///          (`CIPHER_PBKDF2_ITERATIONS`, currently 300,000 iterations) with a
///          fresh random 16-byte salt. Generates a fully random 12-byte nonce.
///          Encrypts and authenticates the plaintext using the active
///          ChaCha20-Poly1305 or AES-256-GCM service and produces a 16-byte tag.
///
///          Wire format: [magic(4) | iterations(4) | salt(16) | nonce(12) |
///          ciphertext | tag(16)].
///
///          The salt is independently random per call, so encrypting the same
///          plaintext with the same password produces independently keyed output.
/// @param plaintext Bytes object containing data to encrypt (traps if NULL).
/// @param password  Password string for key derivation (traps if empty).
/// @return Bytes object containing the encrypted payload.
void *rt_cipher_encrypt(void *plaintext, rt_string password) {
    return rt_cipher_encrypt_aad(plaintext, password, NULL);
}

/// @brief Password-derived authenticated encryption with caller-supplied AAD.
/// @details Implements `Zanna.Crypto.Cipher.EncryptAAD(plaintext, password, aad)`.
///          Generates a fresh 16-byte salt, runs PBKDF2-HMAC-SHA256
///          (CIPHER_PBKDF2_ITERATIONS rounds) to derive a 32-byte key,
///          generates a fresh full-width random nonce, and produces the wire
///          format [magic | iterations | salt | nonce | ciphertext | tag]. The
///          active AEAD tag authenticates [magic||iterations||salt||nonce] plus the
///          application-supplied @p aad, so any tampering with format header
///          OR application context fails verification.
/// @param plaintext Bytes to encrypt. Required.
/// @param password  Password string for PBKDF2 derivation. Empty string traps.
/// @param aad       Optional bytes object carrying application AAD; may be NULL.
/// @return New bytes object owning the framed ciphertext.
void *rt_cipher_encrypt_aad(void *plaintext, rt_string password, void *aad) {
    if (!plaintext) {
        rt_trap("Cipher.Encrypt: plaintext is null");
        return NULL;
    }
    if (!rt_bytes_is_bytes(plaintext)) {
        rt_trap("Cipher.Encrypt: plaintext must be a Bytes object");
        return NULL;
    }

    size_t pwd_len;
    int pwd_ok;
    const char *pwd =
        cipher_password_bytes(password, &pwd_len, "Cipher.Encrypt: password is null", &pwd_ok);
    if (!pwd_ok)
        return NULL;
    if (pwd_len == 0) {
        rt_trap("Cipher.Encrypt: password is empty");
        return NULL;
    }

    uint8_t *plain_data = bytes_data(plaintext);
    int64_t plain_len = bytes_len(plaintext);
    if (plain_len < 0 || (plain_len > 0 && !plain_data))
        return NULL;
    int approved = rt_crypto_module_is_approved_mode();
    int64_t out_len =
        cipher_checked_output_len(plain_len,
                                  CIPHER_PW_HEADER_SIZE + CIPHER_TAG_SIZE,
                                  approved ? CIPHER_AES_GCM_MAX_BYTES : CIPHER_CHACHA20_MAX_BYTES,
                                  "Cipher.Encrypt: plaintext length is invalid");
    if (out_len == 0)
        return NULL;

    // Generate an independent salt and full-width random nonce.
    uint8_t salt[CIPHER_SALT_SIZE];
    uint8_t nonce[CIPHER_NONCE_SIZE];
    rt_crypto_random_bytes(salt, CIPHER_SALT_SIZE);
    cipher_random_nonce(nonce);

    // Derive key from password
    uint8_t key[CIPHER_KEY_SIZE];
    derive_key_pbkdf2(pwd, pwd_len, salt, CIPHER_SALT_SIZE, key);

    void *result = cipher_bytes_new_or_trap(out_len, "Cipher.Encrypt: allocation failed");
    if (!result) {
        cipher_secure_zero(key, sizeof(key));
        return NULL;
    }
    uint8_t *out_data = bytes_data(result);
    if (!out_data) {
        cipher_secure_zero(key, sizeof(key));
        return NULL;
    }

    memcpy(
        out_data, approved ? CIPHER_PW_APPROVED_MAGIC : CIPHER_PW_MAGIC, sizeof(CIPHER_PW_MAGIC));
    write_be32(out_data + 4, CIPHER_PBKDF2_ITERATIONS);
    memcpy(out_data + 8, salt, CIPHER_SALT_SIZE);
    memcpy(out_data + 24, nonce, CIPHER_NONCE_SIZE);

    const uint8_t *aad_data;
    size_t aad_len;
    uint8_t *aad_alloc = combine_aad(out_data, CIPHER_PW_HEADER_SIZE, aad, &aad_data, &aad_len);
    if (aad_len == SIZE_MAX) {
        cipher_secure_zero(key, sizeof(key));
        if (result && rt_obj_release_check0(result))
            rt_obj_free(result);
        return NULL;
    }

    size_t encrypted_len = approved
                               ? rt_aes256_gcm_encrypt(key,
                                                       nonce,
                                                       aad_data,
                                                       aad_len,
                                                       plain_data,
                                                       (size_t)plain_len,
                                                       out_data + CIPHER_PW_HEADER_SIZE)
                               : rt_chacha20_poly1305_encrypt(key,
                                                              nonce,
                                                              aad_data,
                                                              aad_len,
                                                              plain_data,
                                                              (size_t)plain_len,
                                                              out_data + CIPHER_PW_HEADER_SIZE);
    if (aad_alloc)
        free(aad_alloc);
    if (encrypted_len == 0 && plain_len != 0) {
        cipher_secure_zero(key, sizeof(key));
        if (result && rt_obj_release_check0(result))
            rt_obj_free(result);
        rt_trap("Cipher.Encrypt: encryption failed");
        return NULL;
    }

    cipher_secure_zero(key, sizeof(key));

    return result;
}

/// @brief Decrypt data that was encrypted with rt_cipher_encrypt.
/// @details Extracts the salt and nonce from the ciphertext header, re-derives
///          the key using PBKDF2-HMAC-SHA256, then decrypts and verifies the
///          active AEAD tag. Unversioned legacy input is tried with the former
///          PBKDF2 derivation and then the older HKDF-based derivation. A frame
///          whose first four random legacy bytes collide with current magic is
///          classified as current and does not reach the legacy path.
///
///          Current wire format: [magic(4) | iterations(4) | salt(16) |
///          nonce(12) | ciphertext | tag(16)].
/// @param ciphertext Bytes object containing the encrypted payload.
/// @param password   Password string for key derivation.
/// @return Bytes object containing the decrypted plaintext, or NULL on auth failure.
void *rt_cipher_decrypt(void *ciphertext, rt_string password) {
    return rt_cipher_decrypt_aad(ciphertext, password, NULL);
}

/// @brief Decrypt password-encrypted data and return a Result.
/// @details Converts the legacy decryptor's NULL authentication failure into
///          `Err("Cipher.Decrypt: authentication failed")` and captures traps
///          such as malformed ciphertext or empty password as `Err(str)`.
///          Successful plaintext bytes are returned as `Ok(Bytes)`.
/// @param ciphertext Bytes object containing encrypted data.
/// @param password Password string used for key derivation.
/// @return Opaque Zanna.Result containing plaintext bytes or a diagnostic string.
void *rt_cipher_decrypt_result(void *ciphertext, rt_string password) {
    return cipher_password_result(rt_cipher_decrypt,
                                  ciphertext,
                                  password,
                                  "Cipher.Decrypt: authentication failed",
                                  "Cipher.Decrypt failed");
}

/// @brief Attempt password-based decryption and return an Option.
/// @details Converts authentication failure, malformed ciphertext, invalid
///          arguments, and other decryptor traps into `None`.
/// @param ciphertext Bytes object containing encrypted data.
/// @param password Password string used for key derivation.
/// @return Opaque Zanna.Option containing plaintext bytes, or None.
void *rt_cipher_try_decrypt(void *ciphertext, rt_string password) {
    return cipher_password_option(rt_cipher_decrypt, ciphertext, password);
}

/// @brief Password-derived authenticated decryption with AAD verification.
/// @details Inverse of rt_cipher_encrypt_aad. Validates the leading
///          CIPHER_PW_MAGIC, re-derives the 32-byte key from the embedded
///          salt via PBKDF2, then runs the format-selected AEAD verify-and-decrypt
///          with [magic||salt||nonce||@p aad] as the expected AAD. Any
///          mismatch (wrong password, wrong AAD, tampered ciphertext)
///          returns NULL — the caller must treat NULL as authentication
///          failure, not as plaintext.
/// @param ciphertext Framed ciphertext from rt_cipher_encrypt_aad. Required.
/// @param password   Same password used at encrypt time. Empty string traps.
/// @param aad        Same AAD used at encrypt time; may be NULL.
/// @return New bytes object with the decrypted plaintext, or NULL on auth failure.
void *rt_cipher_decrypt_aad(void *ciphertext, rt_string password, void *aad) {
    if (!ciphertext) {
        rt_trap("Cipher.Decrypt: ciphertext is null");
        return NULL;
    }
    if (!rt_bytes_is_bytes(ciphertext)) {
        rt_trap("Cipher.Decrypt: ciphertext must be a Bytes object");
        return NULL;
    }

    size_t pwd_len;
    int pwd_ok;
    const char *pwd =
        cipher_password_bytes(password, &pwd_len, "Cipher.Decrypt: password is null", &pwd_ok);
    if (!pwd_ok)
        return NULL;
    if (pwd_len == 0) {
        rt_trap("Cipher.Decrypt: password is empty");
        return NULL;
    }

    uint8_t *ct_data = bytes_data(ciphertext);
    int64_t ct_len = bytes_len(ciphertext);

    int approved_payload = has_magic(ct_data, ct_len, CIPHER_PW_APPROVED_MAGIC);
    if (rt_crypto_module_is_approved_mode() && !approved_payload) {
        rt_trap("Cipher.Decrypt: non-approved cipher format is disabled in approved mode");
        return NULL;
    }

    // A legacy frame begins with a random salt, so its first four bytes can
    // coincidentally equal current magic (~1 in 2^31). In compatibility mode
    // such a collision must not make a valid legacy payload undecryptable
    // (VDOC-173): if the current-format parse fails for ANY reason, fall
    // through to the legacy attempt below. That fallback is safe — the legacy
    // path is itself AEAD-authenticated, so it only yields plaintext for a
    // genuinely valid legacy ciphertext, and it can never fire in approved
    // mode (which rejected non-approved payloads above). A true current-format
    // ciphertext authenticates on the first try and never reaches the fallback.
    const int allow_legacy_fallback = !approved_payload && !rt_crypto_module_is_approved_mode();
    if (has_magic(ct_data, ct_len, CIPHER_PW_MAGIC) || approved_payload) {
        if (ct_len < CIPHER_PW_HEADER_SIZE + CIPHER_TAG_SIZE) {
            if (allow_legacy_fallback)
                goto try_legacy;
            rt_trap("Cipher.Decrypt: ciphertext too short");
            return NULL;
        }
        uint32_t iterations = read_be32(ct_data + 4);
        if (iterations < RT_PBKDF2_MIN_ITERATIONS || iterations > RT_PBKDF2_MAX_ITERATIONS) {
            if (allow_legacy_fallback)
                goto try_legacy;
            rt_trap("Cipher.Decrypt: unsupported PBKDF2 iteration count");
            return NULL;
        }

        const uint8_t *salt = ct_data + 8;
        const uint8_t *nonce = ct_data + 24;
        const uint8_t *encrypted = ct_data + CIPHER_PW_HEADER_SIZE;
        int64_t encrypted_len = ct_len - CIPHER_PW_HEADER_SIZE;
        int64_t plain_len = encrypted_len - CIPHER_TAG_SIZE;

        uint8_t key[CIPHER_KEY_SIZE];
        rt_keyderive_pbkdf2_sha256_raw((const uint8_t *)pwd,
                                       pwd_len,
                                       salt,
                                       CIPHER_SALT_SIZE,
                                       iterations,
                                       key,
                                       CIPHER_KEY_SIZE);

        uint8_t *plain_data =
            cipher_plain_temp_alloc(plain_len, "Cipher.Decrypt: allocation failed");
        if (!plain_data) {
            cipher_secure_zero(key, sizeof(key));
            return NULL;
        }
        const uint8_t *aad_data;
        size_t aad_len;
        uint8_t *aad_alloc = combine_aad(ct_data, CIPHER_PW_HEADER_SIZE, aad, &aad_data, &aad_len);
        if (aad_len == SIZE_MAX) {
            cipher_secure_zero(key, sizeof(key));
            cipher_secure_zero(plain_data, plain_len > 0 ? (size_t)plain_len : 1u);
            free(plain_data);
            return NULL;
        }

        long decrypt_result =
            approved_payload
                ? rt_aes256_gcm_decrypt(
                      key, nonce, aad_data, aad_len, encrypted, (size_t)encrypted_len, plain_data)
                : rt_chacha20_poly1305_decrypt(
                      key, nonce, aad_data, aad_len, encrypted, (size_t)encrypted_len, plain_data);
        if (aad_alloc)
            free(aad_alloc);
        cipher_secure_zero(key, sizeof(key));
        if (decrypt_result < 0 || decrypt_result != plain_len) {
            if (plain_len > 0)
                cipher_secure_zero(plain_data, (size_t)plain_len);
            free(plain_data);
            // Current-format authentication failed. In compatibility mode this
            // may be a legacy frame whose random salt collided with the magic,
            // so retry the (AEAD-authenticated) legacy interpretation.
            if (allow_legacy_fallback)
                goto try_legacy;
            return NULL;
        }
        void *result = rt_bytes_from_raw(plain_data, (size_t)plain_len);
        if (plain_len > 0)
            cipher_secure_zero(plain_data, (size_t)plain_len);
        free(plain_data);
        return result;
    }

try_legacy:
    if (rt_crypto_module_is_approved_mode()) {
        rt_trap("Cipher.Decrypt: legacy cipher format is disabled in approved mode");
        return NULL;
    }

    if (aad) {
        int64_t aad_len = bytes_len(aad);
        if (aad_len < 0) {
            rt_trap("Cipher.Decrypt: invalid AAD length");
            return NULL;
        }
        if (aad_len > 0)
            return NULL;
    }

    int64_t min_len = CIPHER_SALT_SIZE + CIPHER_NONCE_SIZE + CIPHER_TAG_SIZE;
    if (ct_len < min_len) {
        rt_trap("Cipher.Decrypt: ciphertext too short");
        return NULL;
    }

    const uint8_t *salt = ct_data;
    const uint8_t *nonce = ct_data + CIPHER_SALT_SIZE;
    const uint8_t *encrypted = ct_data + CIPHER_SALT_SIZE + CIPHER_NONCE_SIZE;
    int64_t encrypted_len = ct_len - CIPHER_SALT_SIZE - CIPHER_NONCE_SIZE;
    int64_t plain_len = encrypted_len - CIPHER_TAG_SIZE;
    uint8_t *plain_data = cipher_plain_temp_alloc(plain_len, "Cipher.Decrypt: allocation failed");
    if (!plain_data)
        return NULL;

    uint8_t key[CIPHER_KEY_SIZE];
    derive_key_pbkdf2(pwd, pwd_len, salt, CIPHER_SALT_SIZE, key);
    long decrypt_result = rt_chacha20_poly1305_decrypt(
        key, nonce, NULL, 0, encrypted, (size_t)encrypted_len, plain_data);

    if (decrypt_result < 0 || decrypt_result != plain_len) {
        if (plain_len > 0)
            cipher_secure_zero(plain_data, (size_t)plain_len);
        derive_key_legacy(pwd, pwd_len, salt, CIPHER_SALT_SIZE, key);
        decrypt_result = rt_chacha20_poly1305_decrypt(
            key, nonce, NULL, 0, encrypted, (size_t)encrypted_len, plain_data);
        if (decrypt_result < 0 || decrypt_result != plain_len) {
            cipher_secure_zero(key, sizeof(key));
            if (plain_len > 0)
                cipher_secure_zero(plain_data, (size_t)plain_len);
            free(plain_data);
            return NULL;
        }
    }
    cipher_secure_zero(key, sizeof(key));
    void *result = rt_bytes_from_raw(plain_data, (size_t)plain_len);
    if (plain_len > 0)
        cipher_secure_zero(plain_data, (size_t)plain_len);
    free(plain_data);
    return result;
}

/// @brief Decrypt password-encrypted data with AAD and return a Result.
/// @details Converts NULL authentication failure into an explicit Err and
///          captures traps from the underlying decryptor as Err strings.
/// @param ciphertext Framed ciphertext produced by rt_cipher_encrypt_aad.
/// @param password Password string used for key derivation.
/// @param aad Additional authenticated data; may be NULL.
/// @return Opaque Zanna.Result containing plaintext bytes or a diagnostic string.
void *rt_cipher_decrypt_aad_result(void *ciphertext, rt_string password, void *aad) {
    return cipher_password_aad_result(rt_cipher_decrypt_aad,
                                      ciphertext,
                                      password,
                                      aad,
                                      "Cipher.DecryptAAD: authentication failed",
                                      "Cipher.DecryptAAD failed");
}

/// @brief Attempt password-based AAD decryption and return an Option.
/// @details Converts authentication failure and decryptor traps into `None`.
/// @param ciphertext Framed ciphertext produced by rt_cipher_encrypt_aad.
/// @param password Password string used for key derivation.
/// @param aad Additional authenticated data; may be NULL.
/// @return Opaque Zanna.Option containing plaintext bytes, or None.
void *rt_cipher_try_decrypt_aad(void *ciphertext, rt_string password, void *aad) {
    return cipher_password_aad_option(rt_cipher_decrypt_aad, ciphertext, password, aad);
}

//=============================================================================
// Key-Based Encryption
//=============================================================================

/// @brief Encrypt data using a raw 256-bit key with the mode-selected AEAD.
/// @details Like rt_cipher_encrypt but skips key derivation — the caller
///          provides a pre-derived 32-byte key (e.g., from rt_cipher_generate_key
///          or rt_cipher_derive_key). A full 96-bit CSPRNG nonce is generated
///          per call (VDOC-172).
///
///          Wire format: [magic(4) | nonce(12) | ciphertext | tag(16)].
///
///          Note: no salt is stored because no password derivation occurs.
/// @param plaintext  Bytes object containing data to encrypt (traps if NULL).
/// @param key_bytes  Bytes object containing exactly 32 bytes (traps if wrong size).
/// @return Bytes object containing the encrypted payload.
void *rt_cipher_encrypt_with_key(void *plaintext, void *key_bytes) {
    return rt_cipher_encrypt_with_key_aad(plaintext, key_bytes, NULL);
}

/// @brief Raw-key authenticated encryption with caller-supplied AAD.
/// @details Implements `Zanna.Crypto.Cipher.EncryptWithKeyAAD(plaintext, key, aad)`.
///          Like rt_cipher_encrypt_aad but uses a 32-byte raw key directly
///          (no PBKDF2 derivation, no salt). Output uses CIPHER_KEY_MAGIC
///          so the format-detection guard distinguishes it from password-
///          derived ciphertexts. Wire layout:
///          [CIPHER_KEY_MAGIC | nonce(12) | ciphertext | tag(16)]. The active
///          AEAD tag authenticates [magic||nonce||@p aad].
/// @param plaintext  Bytes to encrypt. Required.
/// @param key_bytes  Exactly 32 bytes. Other lengths trap.
/// @param aad        Optional bytes object with application AAD; may be NULL.
/// @return New bytes object owning the framed ciphertext.
void *rt_cipher_encrypt_with_key_aad(void *plaintext, void *key_bytes, void *aad) {
    if (!plaintext) {
        rt_trap("Cipher.EncryptWithKey: plaintext is null");
        return NULL;
    }
    if (!rt_bytes_is_bytes(plaintext)) {
        rt_trap("Cipher.EncryptWithKey: plaintext must be a Bytes object");
        return NULL;
    }

    if (!key_bytes) {
        rt_trap("Cipher.EncryptWithKey: key must be exactly 32 bytes");
        return NULL;
    }
    if (!rt_bytes_is_bytes(key_bytes)) {
        rt_trap("Cipher.EncryptWithKey: key must be a Bytes object");
        return NULL;
    }
    if (bytes_len(key_bytes) != CIPHER_KEY_SIZE) {
        rt_trap("Cipher.EncryptWithKey: key must be exactly 32 bytes");
        return NULL;
    }

    uint8_t *plain_data = bytes_data(plaintext);
    int64_t plain_len = bytes_len(plaintext);
    if (plain_len < 0 || (plain_len > 0 && !plain_data))
        return NULL;
    const uint8_t *key = bytes_data(key_bytes);
    if (!key)
        return NULL;
    int approved = rt_crypto_module_is_approved_mode();
    int64_t out_len =
        cipher_checked_output_len(plain_len,
                                  CIPHER_KEY_HEADER_SIZE + CIPHER_TAG_SIZE,
                                  approved ? CIPHER_AES_GCM_MAX_BYTES : CIPHER_CHACHA20_MAX_BYTES,
                                  "Cipher.EncryptWithKey: plaintext length is invalid");
    if (out_len == 0)
        return NULL;

    // Generate a full 96-bit CSPRNG nonce.
    uint8_t nonce[CIPHER_NONCE_SIZE];
    cipher_random_nonce(nonce);

    void *result = cipher_bytes_new_or_trap(out_len, "Cipher.EncryptWithKey: allocation failed");
    if (!result)
        return NULL;
    uint8_t *out_data = bytes_data(result);
    if (!out_data)
        return NULL;

    memcpy(out_data,
           approved ? CIPHER_KEY_APPROVED_MAGIC : CIPHER_KEY_MAGIC,
           sizeof(CIPHER_KEY_MAGIC));
    memcpy(out_data + 4, nonce, CIPHER_NONCE_SIZE);

    const uint8_t *aad_data;
    size_t aad_len;
    uint8_t *aad_alloc = combine_aad(out_data, CIPHER_KEY_HEADER_SIZE, aad, &aad_data, &aad_len);
    if (aad_len == SIZE_MAX) {
        if (result && rt_obj_release_check0(result))
            rt_obj_free(result);
        return NULL;
    }

    size_t encrypted_len = approved
                               ? rt_aes256_gcm_encrypt(key,
                                                       nonce,
                                                       aad_data,
                                                       aad_len,
                                                       plain_data,
                                                       (size_t)plain_len,
                                                       out_data + CIPHER_KEY_HEADER_SIZE)
                               : rt_chacha20_poly1305_encrypt(key,
                                                              nonce,
                                                              aad_data,
                                                              aad_len,
                                                              plain_data,
                                                              (size_t)plain_len,
                                                              out_data + CIPHER_KEY_HEADER_SIZE);
    if (aad_alloc)
        free(aad_alloc);
    if (encrypted_len == 0 && plain_len != 0) {
        if (result && rt_obj_release_check0(result))
            rt_obj_free(result);
        rt_trap("Cipher.EncryptWithKey: encryption failed");
        return NULL;
    }

    return result;
}

/// @brief Decrypt data that was encrypted with rt_cipher_encrypt_with_key.
/// @details Extracts the 12-byte nonce from the ciphertext header, then
///          decrypts and verifies the active AEAD tag using the provided
///          32-byte key. Returns NULL if authentication fails.
///
///          Current wire format: [magic(4) | nonce(12) | ciphertext | tag(16)].
/// @param ciphertext Bytes object containing the encrypted payload.
/// @param key_bytes  Bytes object containing exactly 32 bytes.
/// @return Bytes object containing the decrypted plaintext.
void *rt_cipher_decrypt_with_key(void *ciphertext, void *key_bytes) {
    return rt_cipher_decrypt_with_key_aad(ciphertext, key_bytes, NULL);
}

/// @brief Decrypt raw-key encrypted data and return a Result.
/// @details Converts NULL authentication failure into an explicit Err and
///          captures traps such as invalid key length or malformed ciphertext.
/// @param ciphertext Bytes object containing encrypted data.
/// @param key_bytes Bytes object containing exactly 32 bytes.
/// @return Opaque Zanna.Result containing plaintext bytes or a diagnostic string.
void *rt_cipher_decrypt_with_key_result(void *ciphertext, void *key_bytes) {
    return cipher_key_result(rt_cipher_decrypt_with_key,
                             ciphertext,
                             key_bytes,
                             "Cipher.DecryptWithKey: authentication failed",
                             "Cipher.DecryptWithKey failed");
}

/// @brief Attempt raw-key decryption and return an Option.
/// @details Converts authentication failure, invalid key length, malformed
///          ciphertext, and other decryptor traps into `None`.
/// @param ciphertext Bytes object containing encrypted data.
/// @param key_bytes Bytes object containing exactly 32 bytes.
/// @return Opaque Zanna.Option containing plaintext bytes, or None.
void *rt_cipher_try_decrypt_with_key(void *ciphertext, void *key_bytes) {
    return cipher_key_option(rt_cipher_decrypt_with_key, ciphertext, key_bytes);
}

/// @brief Raw-key authenticated decryption with AAD verification.
/// @details Inverse of rt_cipher_encrypt_with_key_aad. Validates the leading
///          current key-format magic and runs the selected AEAD verify-and-decrypt with
///          [magic||nonce||@p aad] as the expected AAD. Any mismatch
///          returns NULL — caller must treat NULL as authentication failure,
///          not as plaintext.
/// @param ciphertext Framed ciphertext from rt_cipher_encrypt_with_key_aad. Required.
/// @param key_bytes  Same 32-byte key used at encrypt time.
/// @param aad        Same AAD used at encrypt time; may be NULL.
/// @return New bytes object with the decrypted plaintext, or NULL on auth failure.
void *rt_cipher_decrypt_with_key_aad(void *ciphertext, void *key_bytes, void *aad) {
    if (!ciphertext) {
        rt_trap("Cipher.DecryptWithKey: ciphertext is null");
        return NULL;
    }
    if (!rt_bytes_is_bytes(ciphertext)) {
        rt_trap("Cipher.DecryptWithKey: ciphertext must be a Bytes object");
        return NULL;
    }

    if (!key_bytes) {
        rt_trap("Cipher.DecryptWithKey: key must be exactly 32 bytes");
        return NULL;
    }
    if (!rt_bytes_is_bytes(key_bytes)) {
        rt_trap("Cipher.DecryptWithKey: key must be a Bytes object");
        return NULL;
    }
    if (bytes_len(key_bytes) != CIPHER_KEY_SIZE) {
        rt_trap("Cipher.DecryptWithKey: key must be exactly 32 bytes");
        return NULL;
    }

    uint8_t *ct_data = bytes_data(ciphertext);
    int64_t ct_len = bytes_len(ciphertext);
    const uint8_t *key = bytes_data(key_bytes);

    int approved_payload = has_magic(ct_data, ct_len, CIPHER_KEY_APPROVED_MAGIC);
    if (rt_crypto_module_is_approved_mode() && !approved_payload) {
        rt_trap("Cipher.DecryptWithKey: non-approved cipher format is disabled in approved mode");
        return NULL;
    }

    // A legacy raw-key frame has no magic (`[nonce(12)][ct][tag]`), so its
    // random first four nonce bytes can coincidentally equal CIPHER_KEY_MAGIC
    // (~1 in 2^31). When that happens, `versioned` parsing shifts the nonce and
    // ciphertext boundary and the AEAD tag fails to verify. In compatibility
    // mode we then retry the unversioned interpretation (VDOC-173): that retry
    // is AEAD-authenticated, so it only ever yields plaintext for a genuinely
    // valid legacy frame, and it never runs in approved mode.
    int magic_versioned = has_magic(ct_data, ct_len, CIPHER_KEY_MAGIC) || approved_payload;
    const int allow_legacy_fallback =
        !approved_payload && !rt_crypto_module_is_approved_mode();

    for (int versioned = magic_versioned;; versioned = 0) {
        int64_t header_len = versioned ? CIPHER_KEY_HEADER_SIZE : CIPHER_NONCE_SIZE;
        int64_t min_len = header_len + CIPHER_TAG_SIZE;
        if (ct_len < min_len) {
            if (versioned && allow_legacy_fallback)
                continue; // retry as legacy
            rt_trap("Cipher.DecryptWithKey: ciphertext too short");
            return NULL;
        }
        if (!versioned && aad) {
            int64_t aad_len_in = bytes_len(aad);
            if (aad_len_in < 0) {
                rt_trap("Cipher.DecryptWithKey: invalid AAD length");
                return NULL;
            }
            if (aad_len_in > 0)
                return NULL;
        }

        const uint8_t *nonce = versioned ? ct_data + 4 : ct_data;
        const uint8_t *encrypted = ct_data + header_len;
        int64_t encrypted_len = ct_len - header_len;

        int64_t plain_len = encrypted_len - CIPHER_TAG_SIZE;
        uint8_t *plain_data =
            cipher_plain_temp_alloc(plain_len, "Cipher.DecryptWithKey: allocation failed");
        if (!plain_data)
            return NULL;

        const uint8_t *aad_data = NULL;
        size_t aad_len = 0;
        uint8_t *aad_alloc = NULL;
        if (versioned)
            aad_alloc = combine_aad(ct_data, CIPHER_KEY_HEADER_SIZE, aad, &aad_data, &aad_len);
        if (aad_len == SIZE_MAX) {
            cipher_secure_zero(plain_data, plain_len > 0 ? (size_t)plain_len : 1u);
            free(plain_data);
            return NULL;
        }

        long decrypt_result =
            approved_payload
                ? rt_aes256_gcm_decrypt(
                      key, nonce, aad_data, aad_len, encrypted, (size_t)encrypted_len, plain_data)
                : rt_chacha20_poly1305_decrypt(
                      key, nonce, aad_data, aad_len, encrypted, (size_t)encrypted_len, plain_data);
        if (aad_alloc)
            free(aad_alloc);

        if (decrypt_result < 0 || decrypt_result != plain_len) {
            if (plain_len > 0)
                cipher_secure_zero(plain_data, (size_t)plain_len);
            free(plain_data);
            if (versioned && allow_legacy_fallback)
                continue; // magic collision: retry as legacy
            return NULL;
        }

        void *result = rt_bytes_from_raw(plain_data, (size_t)plain_len);
        if (plain_len > 0)
            cipher_secure_zero(plain_data, (size_t)plain_len);
        free(plain_data);
        return result;
    }
}

/// @brief Decrypt raw-key encrypted data with AAD and return a Result.
/// @details Converts NULL authentication failure into an explicit Err and
///          captures traps such as invalid key length or malformed ciphertext.
/// @param ciphertext Framed ciphertext from rt_cipher_encrypt_with_key_aad.
/// @param key_bytes Bytes object containing exactly 32 bytes.
/// @param aad Additional authenticated data; may be NULL.
/// @return Opaque Zanna.Result containing plaintext bytes or a diagnostic string.
void *rt_cipher_decrypt_with_key_aad_result(void *ciphertext, void *key_bytes, void *aad) {
    return cipher_key_aad_result(rt_cipher_decrypt_with_key_aad,
                                 ciphertext,
                                 key_bytes,
                                 aad,
                                 "Cipher.DecryptWithKeyAAD: authentication failed",
                                 "Cipher.DecryptWithKeyAAD failed");
}

/// @brief Attempt raw-key AAD decryption and return an Option.
/// @details Converts authentication failure, invalid key length, malformed
///          ciphertext, and other decryptor traps into `None`.
/// @param ciphertext Framed ciphertext from rt_cipher_encrypt_with_key_aad.
/// @param key_bytes Bytes object containing exactly 32 bytes.
/// @param aad Additional authenticated data; may be NULL.
/// @return Opaque Zanna.Option containing plaintext bytes, or None.
void *rt_cipher_try_decrypt_with_key_aad(void *ciphertext, void *key_bytes, void *aad) {
    return cipher_key_aad_option(rt_cipher_decrypt_with_key_aad, ciphertext, key_bytes, aad);
}

//=============================================================================
// Key Generation
//=============================================================================

/// @brief Generate a random 256-bit (32-byte) encryption key from the OS CSPRNG.
/// @details The key is suitable for use with rt_cipher_encrypt_with_key and
///          rt_cipher_decrypt_with_key. Uses the same CSPRNG as rt_crypto_rand_bytes
///          (arc4random on macOS, BCryptGenRandom on Windows, /dev/urandom on Linux).
/// @return Bytes object containing 32 cryptographically random bytes.
void *rt_cipher_generate_key(void) {
    void *key = cipher_bytes_new_or_trap(CIPHER_KEY_SIZE, "Cipher.GenerateKey: allocation failed");
    if (!key)
        return NULL;
    uint8_t *key_data = bytes_data(key);
    if (!key_data)
        return NULL;
    rt_crypto_random_bytes(key_data, CIPHER_KEY_SIZE);
    return key;
}

/// @brief Derive a deterministic 256-bit key from a password and salt.
/// @details Uses PBKDF2-HMAC-SHA256 with `CIPHER_PBKDF2_ITERATIONS`
///          iterations (currently 300,000). The same password and salt always
///          produce the same key, which is the point — this enables key
///          agreement between parties who share a password. The salt should be
///          at least 16 bytes of random data to prevent rainbow-table attacks.
///          For one-time encryption, prefer rt_cipher_encrypt (which generates
///          salt automatically).
/// @param password   Password string (traps if empty).
/// @param salt_bytes Bytes object containing the salt (traps if NULL or empty).
/// @return Bytes object containing the 32-byte derived key.
void *rt_cipher_derive_key(rt_string password, void *salt_bytes) {
    if (!salt_bytes) {
        rt_trap("Cipher.DeriveKey: salt is null");
        return NULL;
    }
    if (!rt_bytes_is_bytes(salt_bytes)) {
        rt_trap("Cipher.DeriveKey: salt must be a Bytes object");
        return NULL;
    }

    size_t pwd_len;
    int pwd_ok;
    const char *pwd =
        cipher_password_bytes(password, &pwd_len, "Cipher.DeriveKey: password is null", &pwd_ok);
    if (!pwd_ok)
        return NULL;
    if (pwd_len == 0) {
        rt_trap("Cipher.DeriveKey: password is empty");
        return NULL;
    }

    const uint8_t *salt = bytes_data(salt_bytes);
    int64_t salt_len = bytes_len(salt_bytes);
    if (salt_len <= 0) {
        rt_trap("Cipher.DeriveKey: salt must not be empty");
        return NULL;
    }

    void *key = cipher_bytes_new_or_trap(CIPHER_KEY_SIZE, "Cipher.DeriveKey: allocation failed");
    if (!key)
        return NULL;
    uint8_t *key_data = bytes_data(key);
    if (!key_data)
        return NULL;
    derive_key_pbkdf2(pwd, pwd_len, salt, (size_t)salt_len, key_data);

    return key;
}
