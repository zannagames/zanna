//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE in the project root for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/text/rt_aes.c
// Purpose: Implements the Zanna.Crypto.Aes runtime — AES-128 and AES-256 block
//          cipher in CBC mode with PKCS7 padding (FIPS-197), plus AES-GCM
//          authenticated encryption (Aes.EncryptAuth / DecryptAuth) wrapping
//          the GCM ciphertext in a 16-byte magic-plus-nonce header so plain-CBC and
//          authenticated-GCM payloads cannot be confused at the API level.
//          Pure C implementation with no external dependencies.
//
// Key invariants:
//   - Key sizes: 16 bytes (AES-128) or 32 bytes (AES-256); others trap.
//   - CBC mode: IV is 16 bytes (one AES block); callers must supply a unique
//     random IV. PKCS7 padding is applied during encryption and stripped on
//     decryption. Ciphertext length is always a multiple of 16 bytes.
//   - GCM mode: prepended with a 4-byte `VAK1` magic and 12-byte random nonce
//     (16 bytes total) so an unmodified decrypt path
//     refuses ciphertexts that lack the header. The header is folded into
//     the GCM AAD, so altering it invalidates the tag.
//   - aes_combine_aad composes the application-provided AAD with the magic
//     header so the GCM tag authenticates both.
//   - AES S-box substitutions use a constant-access scan helper instead of
//     indexing lookup tables by secret data.
//
// Ownership/Lifetime:
//   - Returned ciphertext and plaintext rt_bytes are fresh allocations owned
//     by the caller.
//   - Input key, IV, AAD, and data buffers are borrowed for the duration of
//     the call.
//
// Links: src/runtime/text/rt_aes.h (public API),
//        src/runtime/text/rt_cipher.h (high-level mode-selecting AEAD facade),
//        src/runtime/text/rt_rand.h (IV / nonce generation),
//        src/runtime/text/rt_keyderive_internal.h (HMAC helpers used by GCM)
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_aes.c
 * @brief Implements AES-CBC and authenticated AES-GCM runtime services.
 * @details The in-tree implementation supports AES-128 and AES-256 key
 *          expansion and blocks, PKCS7-padded CBC compatibility operations,
 *          framed GCM encryption with authenticated headers and optional AAD,
 *          and password-derived authenticated String helpers.
 */

#include "rt_aes.h"

#include "rt_bytes.h"
#include "rt_crypto.h"
#include "rt_crypto_module.h"
#include "rt_internal.h"
#include "rt_keyderive_internal.h"
#include "rt_object.h"
#include "rt_option.h"
#include "rt_result.h"
#include "rt_string.h"

#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Forward declarations
#include "rt_trap.h"
static void generate_random_bytes(uint8_t *buf, size_t len);

#define AES_STR_MAGIC0 'V'
#define AES_STR_MAGIC1 'A'
#define AES_STR_MAGIC2 'G'
#define AES_STR_MAGIC3 '1'
#define AES_STR_HEADER_LEN 36
#define AES_STR_PBKDF2_ITERATIONS 300000U
#define AES_AUTH_MAGIC0 'V'
#define AES_AUTH_MAGIC1 'A'
#define AES_AUTH_MAGIC2 'K'
#define AES_AUTH_MAGIC3 '1'
#define AES_AUTH_HEADER_LEN 16

/// @brief Decrypt an AES Bytes payload through a wrapper-selected primitive.
/// @param data Borrowed encrypted Bytes object.
/// @param key Borrowed key or password-derived key input.
/// @param context Optional borrowed authentication context.
/// @return New plaintext Bytes object, or NULL on authentication or decoding failure.
typedef void *(*aes_bytes_decrypt_fn)(void *data, void *key, void *context);
/// @brief Decrypt an AES payload into a runtime string using a password.
/// @param data Borrowed encrypted Bytes object.
/// @param password Borrowed password string.
/// @return New plaintext runtime string, or NULL on authentication or decoding failure.
typedef rt_string (*aes_string_decrypt_fn)(void *data, rt_string password);

/// @brief Release a temporary runtime object created by an AES decryptor.
/// @details `Result.Ok` and `Option.Some` retain object payloads. Wrappers call
///          this helper after storing a freshly allocated plaintext Bytes
///          object so only the container owns the retained payload.
/// @param obj Temporary runtime object reference; NULL is ignored.
static void aes_release_temp_object(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief Release a temporary runtime string created by an AES decryptor.
/// @details `Result.OkStr` and `Option.SomeStr` retain string payloads, so the
///          wrapper can drop the decryptor's original reference afterwards.
/// @param value Temporary runtime string reference; NULL is ignored.
static void aes_release_temp_string(rt_string value) {
    if (value)
        rt_string_unref(value);
}

/// @brief Return a runtime string for the active trap message or fallback.
/// @param fallback Message used when no nonempty trap diagnostic is active.
/// @return Borrowed constant runtime string containing the selected diagnostic.
static rt_string aes_current_error_message(const char *fallback) {
    const char *err = rt_trap_get_error();
    if (!err || !err[0])
        err = fallback && fallback[0] ? fallback : "AES decrypt failed";
    return rt_const_cstr(err);
}

/// @brief Wrap a freshly allocated Bytes object as `Result.Ok`.
/// @details The Result retains @p plaintext and this helper releases the
///          decryptor's temporary reference. NULL becomes Result.Err.
/// @param plaintext Newly allocated Bytes object, or NULL.
/// @param null_message Diagnostic text used for a NULL plaintext.
/// @return Caller-owned Result containing Bytes or an error string.
static void *aes_plaintext_result(void *plaintext, const char *null_message) {
    if (!plaintext)
        return rt_result_err_str(rt_const_cstr(null_message));
    void *result = rt_result_ok(plaintext);
    aes_release_temp_object(plaintext);
    return result;
}

/// @brief Wrap a freshly allocated Bytes object as `Option.Some`.
/// @details The Option retains @p plaintext before the temporary decryptor
///          reference is released. NULL becomes Option.None.
/// @param plaintext Newly allocated Bytes object, or NULL.
/// @return Caller-owned Option containing Bytes or None.
static void *aes_plaintext_option(void *plaintext) {
    if (!plaintext)
        return rt_option_none();
    void *option = rt_option_some(plaintext);
    aes_release_temp_object(plaintext);
    return option;
}

/// @brief Wrap a plaintext string as `Result.OkStr`.
/// @details The Result retains @p plaintext before its temporary decryptor
///          reference is released. NULL becomes Result.Err.
/// @param plaintext Newly allocated plaintext string, or NULL.
/// @param null_message Diagnostic text used for a NULL plaintext.
/// @return Caller-owned Result containing a string or error string.
static void *aes_string_result(rt_string plaintext, const char *null_message) {
    if (!plaintext)
        return rt_result_err_str(rt_const_cstr(null_message));
    void *result = rt_result_ok_str(plaintext);
    aes_release_temp_string(plaintext);
    return result;
}

/// @brief Wrap a plaintext string as `Option.SomeStr`.
/// @param plaintext Newly allocated plaintext string, or NULL.
/// @return Caller-owned Option containing the string or None; successful
///         wrapping releases the decryptor's temporary string reference.
static void *aes_string_option(rt_string plaintext) {
    if (!plaintext)
        return rt_option_none();
    void *option = rt_option_some_str(plaintext);
    aes_release_temp_string(plaintext);
    return option;
}

/// @brief Run a bytes decryptor and convert traps/NULL into `Result`.
/// @param fn Decryptor to invoke inside the temporary trap recovery scope.
/// @param data Opaque ciphertext argument forwarded to @p fn.
/// @param key Opaque key argument forwarded to @p fn.
/// @param context Opaque IV or AAD argument forwarded to @p fn.
/// @param null_message Error used when the decryptor returns NULL normally.
/// @param trap_fallback Error used when a trap has no diagnostic text.
/// @return Caller-owned Result containing plaintext Bytes or an error string.
static void *aes_bytes_result(aes_bytes_decrypt_fn fn,
                              void *data,
                              void *key,
                              void *context,
                              const char *null_message,
                              const char *trap_fallback) {
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        rt_string message = aes_current_error_message(trap_fallback);
        rt_trap_clear_recovery();
        return rt_result_err_str(message);
    }
    void *plaintext = fn(data, key, context);
    rt_trap_clear_recovery();
    return aes_plaintext_result(plaintext, null_message);
}

/// @brief Run a bytes decryptor and convert traps/NULL into `Option`.
/// @param fn Decryptor to invoke inside the temporary trap recovery scope.
/// @param data Opaque ciphertext argument forwarded to @p fn.
/// @param key Opaque key argument forwarded to @p fn.
/// @param context Opaque IV or AAD argument forwarded to @p fn.
/// @return Caller-owned Option containing plaintext Bytes, or None after a
///         trap or NULL decryptor result.
static void *aes_bytes_option(aes_bytes_decrypt_fn fn, void *data, void *key, void *context) {
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        rt_trap_clear_recovery();
        return rt_option_none();
    }
    void *plaintext = fn(data, key, context);
    rt_trap_clear_recovery();
    return aes_plaintext_option(plaintext);
}

/// @brief Run a string decryptor and convert traps into `Result`.
/// @param fn String decryptor to invoke inside the trap recovery scope.
/// @param data Opaque ciphertext Bytes object forwarded to @p fn.
/// @param password Runtime password forwarded to @p fn.
/// @param trap_fallback Diagnostic used for a trap without text or a NULL result.
/// @return Caller-owned Result containing plaintext string or diagnostic string.
static void *aes_string_decrypt_result(aes_string_decrypt_fn fn,
                                       void *data,
                                       rt_string password,
                                       const char *trap_fallback) {
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        rt_string message = aes_current_error_message(trap_fallback);
        rt_trap_clear_recovery();
        return rt_result_err_str(message);
    }
    rt_string plaintext = fn(data, password);
    rt_trap_clear_recovery();
    return aes_string_result(plaintext, trap_fallback);
}

/// @brief Run a string decryptor and convert traps into `Option`.
/// @param fn String decryptor to invoke inside the trap recovery scope.
/// @param data Opaque ciphertext Bytes object forwarded to @p fn.
/// @param password Runtime password forwarded to @p fn.
/// @return Caller-owned Option containing plaintext string, or None after a
///         trap or NULL result.
static void *aes_string_decrypt_option(aes_string_decrypt_fn fn,
                                       void *data,
                                       rt_string password) {
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        rt_trap_clear_recovery();
        return rt_option_none();
    }
    rt_string plaintext = fn(data, password);
    rt_trap_clear_recovery();
    return aes_string_option(plaintext);
}

/// @brief Zero out `len` bytes at `ptr` in a way that the optimizer can't elide.
///
/// `volatile uint8_t*` write defeats dead-store elimination so
/// transient key material in stack buffers really does get cleared.
/// @param ptr Start of the writable sensitive-memory span.
/// @param len Number of bytes to overwrite.
static void aes_secure_zero(void *ptr, size_t len) {
    volatile uint8_t *p = (volatile uint8_t *)ptr;
    while (len-- > 0)
        *p++ = 0;
}

/// @brief Extract a raw byte pointer and byte count from a required rt_string.
///        Returns an empty C string and sets *len = 0 for real zero-length input.
/// @param str Runtime string to validate and inspect.
/// @param len Receives the byte length; must be writable.
/// @param null_message Trap diagnostic for a NULL string or missing byte data.
/// @param ok Optional success flag cleared initially and set after a valid view.
/// @return Borrowed string bytes, or a pointer to an empty static byte string
///         after validation failure or for a valid empty string.
static const uint8_t *aes_string_bytes(rt_string str, size_t *len, const char *null_message, int *ok) {
    if (ok)
        *ok = 0;
    if (!str) {
        rt_trap(null_message);
        *len = 0;
        return (const uint8_t *)"";
    }
    if (!rt_string_is_handle((const void *)str)) {
        rt_trap("AES: invalid string handle");
        *len = 0;
        return (const uint8_t *)"";
    }
    int64_t len64 = rt_str_len(str);
    if (len64 < 0) {
        rt_trap("AES: invalid string length");
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
        rt_trap(null_message);
        *len = 0;
        return (const uint8_t *)"";
    }

    *len = (size_t)len64;
    if (ok)
        *ok = 1;
    return (const uint8_t *)cstr;
}

/// @brief Build the authenticated-data span for AES-GCM framed payloads.
/// @details The GCM tag authenticates both the Zanna magic header and the
///          caller-provided AAD. When no user AAD is present, @p aad_out points
///          directly at @p header and @p alloc_out is NULL. When user AAD is
///          present, this helper allocates `[header || user_aad]`, stores it in
///          @p alloc_out, and points @p aad_out at that allocation. Returning a
///          status keeps allocation failure distinct from the normal
///          no-allocation path.
/// @param header      Magic header bytes to authenticate.
/// @param header_len  Number of bytes in @p header.
/// @param aad_obj     Optional rt_bytes object containing user AAD.
/// @param aad_out     Out: authenticated-data pointer for the AES-GCM call.
/// @param aad_len_out Out: byte length of @p aad_out.
/// @param alloc_out   Out: heap allocation to free after use, or NULL.
/// @return Non-zero on success; zero after reporting an invalid input or
///         allocation failure.
static int aes_combine_aad(const uint8_t *header,
                           size_t header_len,
                           void *aad_obj,
                           const uint8_t **aad_out,
                           size_t *aad_len_out,
                           uint8_t **alloc_out) {
    if (aad_out)
        *aad_out = NULL;
    if (aad_len_out)
        *aad_len_out = 0;
    if (alloc_out)
        *alloc_out = NULL;
    if (!aad_out || !aad_len_out || !alloc_out) {
        rt_trap("AES: invalid AAD output pointer");
        return 0;
    }
    if (aad_obj && !rt_bytes_is_bytes(aad_obj)) {
        rt_trap("AES: AAD must be a Bytes object");
        return 0;
    }
    int64_t user_len64 = aad_obj ? rt_bytes_len(aad_obj) : 0;
    if (user_len64 < 0) {
        rt_trap("AES: invalid AAD length");
        return 0;
    }
    size_t user_len = (size_t)user_len64;
    const uint8_t *user = user_len > 0 ? rt_bytes_data_const(aad_obj) : NULL;
    if (user_len > 0 && !user) {
        rt_trap("AES: invalid AAD data");
        return 0;
    }
    if (user_len == 0) {
        *aad_out = header_len > 0 ? header : NULL;
        *aad_len_out = header_len;
        return 1;
    }
    if (header_len > SIZE_MAX - user_len) {
        rt_trap("AES: AAD too large");
        return 0;
    }
    uint8_t *combined = (uint8_t *)malloc(header_len + user_len);
    if (!combined) {
        rt_trap("AES: memory allocation failed");
        return 0;
    }
    if (header_len > 0)
        memcpy(combined, header, header_len);
    memcpy(combined + header_len, user, user_len);
    *aad_out = combined;
    *aad_len_out = header_len + user_len;
    *alloc_out = combined;
    return 1;
}

//=============================================================================
// AES Constants (FIPS-197)
//=============================================================================

/// AES block size in bytes (always 16 for AES)
#ifndef AES_BLOCK_SIZE
#define AES_BLOCK_SIZE 16
#endif

/// S-box substitution table
static const uint8_t sbox[256] = {
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

/// Inverse S-box substitution table
static const uint8_t inv_sbox[256] = {
    0x52, 0x09, 0x6a, 0xd5, 0x30, 0x36, 0xa5, 0x38, 0xbf, 0x40, 0xa3, 0x9e, 0x81, 0xf3, 0xd7, 0xfb,
    0x7c, 0xe3, 0x39, 0x82, 0x9b, 0x2f, 0xff, 0x87, 0x34, 0x8e, 0x43, 0x44, 0xc4, 0xde, 0xe9, 0xcb,
    0x54, 0x7b, 0x94, 0x32, 0xa6, 0xc2, 0x23, 0x3d, 0xee, 0x4c, 0x95, 0x0b, 0x42, 0xfa, 0xc3, 0x4e,
    0x08, 0x2e, 0xa1, 0x66, 0x28, 0xd9, 0x24, 0xb2, 0x76, 0x5b, 0xa2, 0x49, 0x6d, 0x8b, 0xd1, 0x25,
    0x72, 0xf8, 0xf6, 0x64, 0x86, 0x68, 0x98, 0x16, 0xd4, 0xa4, 0x5c, 0xcc, 0x5d, 0x65, 0xb6, 0x92,
    0x6c, 0x70, 0x48, 0x50, 0xfd, 0xed, 0xb9, 0xda, 0x5e, 0x15, 0x46, 0x57, 0xa7, 0x8d, 0x9d, 0x84,
    0x90, 0xd8, 0xab, 0x00, 0x8c, 0xbc, 0xd3, 0x0a, 0xf7, 0xe4, 0x58, 0x05, 0xb8, 0xb3, 0x45, 0x06,
    0xd0, 0x2c, 0x1e, 0x8f, 0xca, 0x3f, 0x0f, 0x02, 0xc1, 0xaf, 0xbd, 0x03, 0x01, 0x13, 0x8a, 0x6b,
    0x3a, 0x91, 0x11, 0x41, 0x4f, 0x67, 0xdc, 0xea, 0x97, 0xf2, 0xcf, 0xce, 0xf0, 0xb4, 0xe6, 0x73,
    0x96, 0xac, 0x74, 0x22, 0xe7, 0xad, 0x35, 0x85, 0xe2, 0xf9, 0x37, 0xe8, 0x1c, 0x75, 0xdf, 0x6e,
    0x47, 0xf1, 0x1a, 0x71, 0x1d, 0x29, 0xc5, 0x89, 0x6f, 0xb7, 0x62, 0x0e, 0xaa, 0x18, 0xbe, 0x1b,
    0xfc, 0x56, 0x3e, 0x4b, 0xc6, 0xd2, 0x79, 0x20, 0x9a, 0xdb, 0xc0, 0xfe, 0x78, 0xcd, 0x5a, 0xf4,
    0x1f, 0xdd, 0xa8, 0x33, 0x88, 0x07, 0xc7, 0x31, 0xb1, 0x12, 0x10, 0x59, 0x27, 0x80, 0xec, 0x5f,
    0x60, 0x51, 0x7f, 0xa9, 0x19, 0xb5, 0x4a, 0x0d, 0x2d, 0xe5, 0x7a, 0x9f, 0x93, 0xc9, 0x9c, 0xef,
    0xa0, 0xe0, 0x3b, 0x4d, 0xae, 0x2a, 0xf5, 0xb0, 0xc8, 0xeb, 0xbb, 0x3c, 0x83, 0x53, 0x99, 0x61,
    0x17, 0x2b, 0x04, 0x7e, 0xba, 0x77, 0xd6, 0x26, 0xe1, 0x69, 0x14, 0x63, 0x55, 0x21, 0x0c, 0x7d};

/// Round constants for key expansion
static const uint8_t rcon[11] = {0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36};

//=============================================================================
// AES Helper Functions
//=============================================================================

/// @brief Return 0xFF when @p a equals @p b, otherwise 0x00, without branching.
/// @details Used by constant-access S-box lookup so secret state/key bytes do
///          not select a cache line directly.
/// @param a First byte.
/// @param b Second byte.
/// @return 0xFF for equality, otherwise 0x00.
static uint8_t aes_ct_mask_eq_u8(uint8_t a, uint8_t b) {
    uint8_t x = (uint8_t)(a ^ b);
    x |= (uint8_t)(x >> 4);
    x |= (uint8_t)(x >> 2);
    x |= (uint8_t)(x >> 1);
    return (uint8_t)(0u - (uint8_t)((x ^ 1u) & 1u));
}

/// @brief Constant-access lookup into a 256-byte AES substitution table.
/// @details Scans every table entry and masks in only the requested byte. This
///          avoids a data-dependent table index for key expansion, SubBytes, and
///          inverse SubBytes at the cost of extra work per substituted byte.
/// @param table Complete 256-byte substitution table.
/// @param index Byte value to select.
/// @return Table value corresponding to @p index.
static uint8_t aes_ct_table_lookup(const uint8_t table[256], uint8_t index) {
    const volatile uint8_t *vtable = (const volatile uint8_t *)table;
    uint8_t out = 0;
    for (uint16_t i = 0; i < 256; i++)
        out |= (uint8_t)(vtable[i] & aes_ct_mask_eq_u8((uint8_t)i, index));
    return out;
}

/// @brief Multiply by 2 in GF(2⁸) using AES's irreducible polynomial `x⁸ + x⁴ + x³ + x + 1`.
/// @details Left-shift by 1, then conditionally XOR with `0x1b` (the
///          low byte of the polynomial) when the high bit was set —
///          that's the modular reduction. Used by `mix_columns` to
///          implement the per-column matrix multiply without
///          materializing a full lookup table for the rare case of
///          ×2 / ×3 multiplications.
/// @param x Field element to double.
/// @return @p x multiplied by two in the AES finite field.
static inline uint8_t xtime(uint8_t x) {
    return (uint8_t)((x << 1) ^ (((x >> 7) & 1) * 0x1b));
}

/// @brief Multiply two bytes in GF(2⁸) — full peasant-multiplication algorithm.
/// @details Walks `b`'s bits low-to-high. For each set bit, accumulates
///          the running power-of-2 multiple of `a` into the result;
///          after each step, double `a` via `xtime` (with the modular
///          reduction baked in). Used by `inv_mix_columns` where the
///          inverse matrix multiplies are arbitrary GF(2⁸) constants
///          (0x09, 0x0b, 0x0d, 0x0e), too varied to hard-code as
///          xtime chains.
/// @param a First field element.
/// @param b Second field element.
/// @return Product of @p a and @p b in GF(2^8).
static inline uint8_t gf_mul(uint8_t a, uint8_t b) {
    uint8_t result = 0;
    uint8_t hi_bit;
    for (int i = 0; i < 8; i++) {
        if (b & 1)
            result ^= a;
        hi_bit = a & 0x80;
        a <<= 1;
        if (hi_bit)
            a ^= 0x1b; // Reduction polynomial
        b >>= 1;
    }
    return result;
}

//=============================================================================
// SHA-256 Implementation (for key derivation)
//=============================================================================

/// SHA-256 round constants
static const uint32_t sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

/// SHA-256 initial hash values
static const uint32_t sha256_h0[8] = {
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

#define SHA256_ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define SHA256_CH(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define SHA256_MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define SHA256_EP0(x) (SHA256_ROTR(x, 2) ^ SHA256_ROTR(x, 13) ^ SHA256_ROTR(x, 22))
#define SHA256_EP1(x) (SHA256_ROTR(x, 6) ^ SHA256_ROTR(x, 11) ^ SHA256_ROTR(x, 25))
#define SHA256_SIG0(x) (SHA256_ROTR(x, 7) ^ SHA256_ROTR(x, 18) ^ ((x) >> 3))
#define SHA256_SIG1(x) (SHA256_ROTR(x, 17) ^ SHA256_ROTR(x, 19) ^ ((x) >> 10))

/// @brief Compute SHA-256 hash of data.
/// @param data Input data
/// @param len Length of input data
/// @param hash Output hash (32 bytes)
/// @return 0 after writing the digest, or -1 when input sizing overflows or the
///         padded-message allocation fails.
static int local_sha256(const uint8_t *data, size_t len, uint8_t hash[32]) {
    uint32_t h[8];
    for (int i = 0; i < 8; i++)
        h[i] = sha256_h0[i];

    // Pre-processing: adding padding bits.
    // Guard against integer overflow: len must be small enough that len+8 doesn't wrap.
    if (len > UINT64_MAX / 8 || len > SIZE_MAX - 72)
        return -1; // input too large to hash
    size_t padded_len = ((len + 8) / 64 + 1) * 64;
    uint8_t *padded = (uint8_t *)calloc(padded_len, 1);
    if (!padded)
        return -1;

    memcpy(padded, data, len);
    padded[len] = 0x80;

    // Append length in bits as big-endian
    uint64_t bit_len = len * 8;
    padded[padded_len - 8] = (uint8_t)(bit_len >> 56);
    padded[padded_len - 7] = (uint8_t)(bit_len >> 48);
    padded[padded_len - 6] = (uint8_t)(bit_len >> 40);
    padded[padded_len - 5] = (uint8_t)(bit_len >> 32);
    padded[padded_len - 4] = (uint8_t)(bit_len >> 24);
    padded[padded_len - 3] = (uint8_t)(bit_len >> 16);
    padded[padded_len - 2] = (uint8_t)(bit_len >> 8);
    padded[padded_len - 1] = (uint8_t)(bit_len);

    // Process each 64-byte chunk
    for (size_t chunk = 0; chunk < padded_len; chunk += 64) {
        uint32_t w[64];

        // Break chunk into sixteen 32-bit big-endian words
        for (int i = 0; i < 16; i++) {
            w[i] = ((uint32_t)padded[chunk + i * 4 + 0] << 24) |
                   ((uint32_t)padded[chunk + i * 4 + 1] << 16) |
                   ((uint32_t)padded[chunk + i * 4 + 2] << 8) |
                   ((uint32_t)padded[chunk + i * 4 + 3]);
        }

        // Extend the sixteen 32-bit words into sixty-four 32-bit words
        for (int i = 16; i < 64; i++)
            w[i] = SHA256_SIG1(w[i - 2]) + w[i - 7] + SHA256_SIG0(w[i - 15]) + w[i - 16];

        // Initialize working variables
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];

        // Main loop
        for (int i = 0; i < 64; i++) {
            uint32_t t1 = hh + SHA256_EP1(e) + SHA256_CH(e, f, g) + sha256_k[i] + w[i];
            uint32_t t2 = SHA256_EP0(a) + SHA256_MAJ(a, b, c);
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }

        // Add compressed chunk to current hash value
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += hh;
    }

    free(padded);

    // Produce final hash value (big-endian)
    for (int i = 0; i < 8; i++) {
        hash[i * 4 + 0] = (uint8_t)(h[i] >> 24);
        hash[i * 4 + 1] = (uint8_t)(h[i] >> 16);
        hash[i * 4 + 2] = (uint8_t)(h[i] >> 8);
        hash[i * 4 + 3] = (uint8_t)(h[i]);
    }
    return 0;
}

//=============================================================================
// AES Key Expansion
//=============================================================================

/// @brief Expand the cipher key into the key schedule.
/// @param key Original key (16 or 32 bytes)
/// @param w Expanded key schedule (176 or 240 bytes)
/// @param nk Number of 32-bit words in key (4 for AES-128, 8 for AES-256)
/// @param nr Number of rounds (10 for AES-128, 14 for AES-256)
static void aes_key_expansion(const uint8_t *key, uint8_t *w, int nk, int nr) {
    int nb = 4; // Number of columns (always 4 for AES)
    int i = 0;

    // First nk words are the original key
    while (i < nk) {
        w[4 * i + 0] = key[4 * i + 0];
        w[4 * i + 1] = key[4 * i + 1];
        w[4 * i + 2] = key[4 * i + 2];
        w[4 * i + 3] = key[4 * i + 3];
        i++;
    }

    // Generate remaining words
    uint8_t temp[4];
    i = nk;
    while (i < nb * (nr + 1)) {
        temp[0] = w[4 * (i - 1) + 0];
        temp[1] = w[4 * (i - 1) + 1];
        temp[2] = w[4 * (i - 1) + 2];
        temp[3] = w[4 * (i - 1) + 3];

        if (i % nk == 0) {
            // RotWord + SubWord + Rcon
            uint8_t t = temp[0];
            temp[0] = aes_ct_table_lookup(sbox, temp[1]) ^ rcon[i / nk];
            temp[1] = aes_ct_table_lookup(sbox, temp[2]);
            temp[2] = aes_ct_table_lookup(sbox, temp[3]);
            temp[3] = aes_ct_table_lookup(sbox, t);
        } else if (nk > 6 && i % nk == 4) {
            // Extra SubWord for AES-256
            temp[0] = aes_ct_table_lookup(sbox, temp[0]);
            temp[1] = aes_ct_table_lookup(sbox, temp[1]);
            temp[2] = aes_ct_table_lookup(sbox, temp[2]);
            temp[3] = aes_ct_table_lookup(sbox, temp[3]);
        }

        w[4 * i + 0] = w[4 * (i - nk) + 0] ^ temp[0];
        w[4 * i + 1] = w[4 * (i - nk) + 1] ^ temp[1];
        w[4 * i + 2] = w[4 * (i - nk) + 2] ^ temp[2];
        w[4 * i + 3] = w[4 * (i - nk) + 3] ^ temp[3];
        i++;
    }
}

//=============================================================================
// AES Cipher Transformations
//=============================================================================

/// @brief AES `SubBytes` step — replace every byte in state via the S-box lookup.
/// @details Provides the cipher's non-linearity: the S-box is built
///          from `x ↦ x⁻¹` in GF(2⁸) followed by an affine transform,
///          chosen so it has no fixed points and resists linear /
///          differential cryptanalysis.
/// @param state Mutable 16-byte AES state in column-major order.
static void sub_bytes(uint8_t *state) {
    for (int i = 0; i < 16; i++)
        state[i] = aes_ct_table_lookup(sbox, state[i]);
}

/// @brief Inverse of `SubBytes` — replace every byte via the inverse S-box.
/// @details The inverse table is precomputed (not derived) so decryption
///          is the same number of operations as encryption.
/// @param state Mutable 16-byte AES state in column-major order.
static void inv_sub_bytes(uint8_t *state) {
    for (int i = 0; i < 16; i++)
        state[i] = aes_ct_table_lookup(inv_sbox, state[i]);
}

/// @brief AES `ShiftRows` step — cyclically rotate each row of the 4×4 state.
/// @details Row 0 stays put; rows 1, 2, 3 rotate left by 1, 2, 3 positions
///          respectively. Combined with `MixColumns`, this provides the
///          *diffusion* that AES needs — without it, each byte of
///          ciphertext would depend on only one byte of plaintext.
///          State layout is column-major: `state[row + 4*col]`, so
///          row 1 spans indices 1, 5, 9, 13 etc.
/// @param state Mutable 16-byte AES state in column-major order.
static void shift_rows(uint8_t *state) {
    uint8_t temp;

    // Row 1: shift left by 1
    temp = state[1];
    state[1] = state[5];
    state[5] = state[9];
    state[9] = state[13];
    state[13] = temp;

    // Row 2: shift left by 2
    temp = state[2];
    state[2] = state[10];
    state[10] = temp;
    temp = state[6];
    state[6] = state[14];
    state[14] = temp;

    // Row 3: shift left by 3 (= shift right by 1)
    temp = state[15];
    state[15] = state[11];
    state[11] = state[7];
    state[7] = state[3];
    state[3] = temp;
}

/// @brief Inverse of `ShiftRows` — cyclically rotate each row to the right.
/// @details Mirror of `shift_rows`: row 1 right-rotates by 1, row 2 by
///          2 (same as left-rotate by 2 since the row is 4 wide),
///          row 3 right-rotates by 3 (= left-rotate by 1).
/// @param state Mutable 16-byte AES state in column-major order.
static void inv_shift_rows(uint8_t *state) {
    uint8_t temp;

    // Row 1: shift right by 1
    temp = state[13];
    state[13] = state[9];
    state[9] = state[5];
    state[5] = state[1];
    state[1] = temp;

    // Row 2: shift right by 2
    temp = state[2];
    state[2] = state[10];
    state[10] = temp;
    temp = state[6];
    state[6] = state[14];
    state[14] = temp;

    // Row 3: shift right by 3 (= shift left by 1)
    temp = state[3];
    state[3] = state[7];
    state[7] = state[11];
    state[11] = state[15];
    state[15] = temp;
}

/// @brief AES `MixColumns` step — multiply each column by the fixed MDS matrix in GF(2⁸).
/// @details Each output column is the matrix product
///          ```
///          [02 03 01 01]   [a0]
///          [01 02 03 01] × [a1]
///          [01 01 02 03]   [a2]
///          [03 01 01 02]   [a3]
///          ```
///          where multiplication is in GF(2⁸). Implemented inline as
///          `xtime` (which is ×2 in the field) plus XORs because the
///          matrix entries are all ∈ {01, 02, 03} — no general
///          `gf_mul` needed. Provides the second half of AES's
///          diffusion: now each output byte depends on all four
///          input bytes of the same column.
/// @param state Mutable 16-byte AES state in column-major order.
static void mix_columns(uint8_t *state) {
    for (int c = 0; c < 4; c++) {
        int i = c * 4;
        uint8_t a0 = state[i + 0];
        uint8_t a1 = state[i + 1];
        uint8_t a2 = state[i + 2];
        uint8_t a3 = state[i + 3];

        state[i + 0] = xtime(a0) ^ xtime(a1) ^ a1 ^ a2 ^ a3;
        state[i + 1] = a0 ^ xtime(a1) ^ xtime(a2) ^ a2 ^ a3;
        state[i + 2] = a0 ^ a1 ^ xtime(a2) ^ xtime(a3) ^ a3;
        state[i + 3] = xtime(a0) ^ a0 ^ a1 ^ a2 ^ xtime(a3);
    }
}

/// @brief Inverse of `MixColumns` — multiply each column by the inverse MDS matrix.
/// @details Inverse matrix entries are `{0e, 0b, 0d, 09}` — too varied
///          to express via xtime chains, so falls back to general
///          `gf_mul`. This is why decryption is slower than encryption
///          on architectures without an AES instruction.
/// @param state Mutable 16-byte AES state in column-major order.
static void inv_mix_columns(uint8_t *state) {
    for (int c = 0; c < 4; c++) {
        int i = c * 4;
        uint8_t a0 = state[i + 0];
        uint8_t a1 = state[i + 1];
        uint8_t a2 = state[i + 2];
        uint8_t a3 = state[i + 3];

        state[i + 0] = gf_mul(a0, 0x0e) ^ gf_mul(a1, 0x0b) ^ gf_mul(a2, 0x0d) ^ gf_mul(a3, 0x09);
        state[i + 1] = gf_mul(a0, 0x09) ^ gf_mul(a1, 0x0e) ^ gf_mul(a2, 0x0b) ^ gf_mul(a3, 0x0d);
        state[i + 2] = gf_mul(a0, 0x0d) ^ gf_mul(a1, 0x09) ^ gf_mul(a2, 0x0e) ^ gf_mul(a3, 0x0b);
        state[i + 3] = gf_mul(a0, 0x0b) ^ gf_mul(a1, 0x0d) ^ gf_mul(a2, 0x09) ^ gf_mul(a3, 0x0e);
    }
}

/// @brief AES `AddRoundKey` step — XOR the round key into the state, byte-by-byte.
/// @details The only step that actually mixes the secret key into the
///          state. Repeated `nr+1` times per block (once before the
///          first round, once after every round including the last).
///          XOR is its own inverse, which is why decryption uses the
///          same operation in reverse round order.
/// @param state Mutable 16-byte AES state.
/// @param round_key Borrowed 16-byte slice of the expanded key schedule.
static void add_round_key(uint8_t *state, const uint8_t *round_key) {
    for (int i = 0; i < 16; i++)
        state[i] ^= round_key[i];
}

//=============================================================================
// AES Block Cipher
//=============================================================================

/// @brief Encrypt a single 16-byte block
/// @param input 16-byte plaintext block
/// @param output 16-byte ciphertext block
/// @param w Expanded key schedule
/// @param nr Number of rounds (10 or 14)
static void aes_encrypt_block(const uint8_t *input, uint8_t *output, const uint8_t *w, int nr) {
    uint8_t state[16];
    memcpy(state, input, 16);

    // Initial round key addition
    add_round_key(state, w);

    // Main rounds
    for (int round = 1; round < nr; round++) {
        sub_bytes(state);
        shift_rows(state);
        mix_columns(state);
        add_round_key(state, w + round * 16);
    }

    // Final round (no MixColumns)
    sub_bytes(state);
    shift_rows(state);
    add_round_key(state, w + nr * 16);

    memcpy(output, state, 16);
}

/// @brief Decrypt a single 16-byte block
/// @param input 16-byte ciphertext block
/// @param output 16-byte plaintext block
/// @param w Expanded key schedule
/// @param nr Number of rounds (10 or 14)
static void aes_decrypt_block(const uint8_t *input, uint8_t *output, const uint8_t *w, int nr) {
    uint8_t state[16];
    memcpy(state, input, 16);

    // Initial round key addition
    add_round_key(state, w + nr * 16);

    // Main rounds (in reverse)
    for (int round = nr - 1; round > 0; round--) {
        inv_shift_rows(state);
        inv_sub_bytes(state);
        add_round_key(state, w + round * 16);
        inv_mix_columns(state);
    }

    // Final round (no InvMixColumns)
    inv_shift_rows(state);
    inv_sub_bytes(state);
    add_round_key(state, w);

    memcpy(output, state, 16);
}

//=============================================================================
// CBC Mode and PKCS7 Padding
//=============================================================================

/// @brief Apply PKCS7 padding to data
/// @param data Input data
/// @param len Length of input data
/// @param out_len Output: length of padded data (always multiple of 16)
/// @return Newly allocated padded data
static uint8_t *pkcs7_pad(const uint8_t *data, size_t len, size_t *out_len) {
    if (!data && len > 0) {
        rt_trap("AES: input buffer is null");
        return NULL;
    }
    size_t pad_len = AES_BLOCK_SIZE - (len % AES_BLOCK_SIZE);
    if (len > SIZE_MAX - pad_len) {
        rt_trap("AES: input too large");
        return NULL;
    }
    *out_len = len + pad_len;

    uint8_t *padded = (uint8_t *)malloc(*out_len);
    if (!padded) {
        rt_trap("AES: memory allocation failed");
        *out_len = 0;
        return NULL;
    }

    if (len > 0)
        memcpy(padded, data, len);
    memset(padded + len, (uint8_t)pad_len, pad_len);

    return padded;
}

/// @brief Remove PKCS7 padding from data (S-05: constant-time implementation)
/// @param data Padded data
/// @param len Length of padded data
/// @param out_len Output: length of unpadded data
/// @return 0 on success, -1 on invalid padding
static int pkcs7_unpad(const uint8_t *data, size_t len, size_t *out_len) {
    if (len == 0 || len % AES_BLOCK_SIZE != 0)
        return -1;

    uint8_t pad_byte = data[len - 1];
    if (pad_byte == 0 || pad_byte > AES_BLOCK_SIZE)
        return -1;

    /* S-05: Constant-time padding check — accumulate mismatch bits without
     * branching on individual byte values to prevent timing side-channels. */
    uint8_t mismatch = 0;
    for (size_t i = 0; i < (size_t)AES_BLOCK_SIZE; i++) {
        /* Only check bytes that fall within the padding region */
        uint8_t in_range = (uint8_t)(i < (size_t)pad_byte ? 0xFF : 0x00);
        mismatch |= in_range & (data[len - 1 - i] ^ pad_byte);
    }

    if (mismatch != 0)
        return -1;

    *out_len = len - pad_byte;
    return 0;
}

/// @brief Encrypt data using AES-CBC
/// @param plaintext Input data
/// @param len Length of input data
/// @param key Encryption key
/// @param iv Initialization vector (16 bytes)
/// @param nk Key words (4 for AES-128, 8 for AES-256)
/// @param nr Number of rounds (10 for AES-128, 14 for AES-256)
/// @param out_len Output: length of ciphertext
/// @return Newly allocated ciphertext
static uint8_t *aes_cbc_encrypt(const uint8_t *plaintext,
                                size_t len,
                                const uint8_t *key,
                                const uint8_t *iv,
                                int nk,
                                int nr,
                                size_t *out_len) {
    // Expand key
    size_t w_size = (size_t)(16 * (nr + 1));
    uint8_t *w = (uint8_t *)malloc(w_size);
    if (!w) {
        rt_trap("AES: memory allocation failed");
        return NULL;
    }
    aes_key_expansion(key, w, nk, nr);

    // Pad plaintext
    size_t padded_len;
    uint8_t *padded = pkcs7_pad(plaintext, len, &padded_len);
    if (!padded) {
        aes_secure_zero(w, w_size);
        free(w);
        return NULL;
    }

    // Allocate ciphertext
    uint8_t *ciphertext = (uint8_t *)malloc(padded_len);
    if (!ciphertext) {
        aes_secure_zero(w, w_size);
        aes_secure_zero(padded, padded_len);
        free(w);
        free(padded);
        rt_trap("AES: memory allocation failed");
        return NULL;
    }

    // CBC encryption
    uint8_t prev_block[AES_BLOCK_SIZE];
    memcpy(prev_block, iv, AES_BLOCK_SIZE);

    for (size_t i = 0; i < padded_len; i += AES_BLOCK_SIZE) {
        uint8_t block[AES_BLOCK_SIZE];

        // XOR with previous ciphertext (or IV for first block)
        for (int j = 0; j < AES_BLOCK_SIZE; j++)
            block[j] = padded[i + j] ^ prev_block[j];

        // Encrypt block
        aes_encrypt_block(block, ciphertext + i, w, nr);

        // Save for next iteration
        memcpy(prev_block, ciphertext + i, AES_BLOCK_SIZE);
    }

    aes_secure_zero(w, w_size);
    aes_secure_zero(padded, padded_len);
    free(w);
    free(padded);
    *out_len = padded_len;
    return ciphertext;
}

/// @brief Decrypt data using AES-CBC
/// @param ciphertext Input data
/// @param len Length of input data (must be multiple of 16)
/// @param key Encryption key
/// @param iv Initialization vector (16 bytes)
/// @param nk Key words (4 for AES-128, 8 for AES-256)
/// @param nr Number of rounds (10 for AES-128, 14 for AES-256)
/// @param out_len Output: length of plaintext
/// @return Newly allocated plaintext, or NULL on error
static uint8_t *aes_cbc_decrypt(const uint8_t *ciphertext,
                                size_t len,
                                const uint8_t *key,
                                const uint8_t *iv,
                                int nk,
                                int nr,
                                size_t *out_len) {
    if (len == 0 || len % AES_BLOCK_SIZE != 0)
        return NULL;

    // Expand key
    size_t w_size = (size_t)(16 * (nr + 1));
    uint8_t *w = (uint8_t *)malloc(w_size);
    if (!w) {
        rt_trap("AES: memory allocation failed");
        return NULL;
    }
    aes_key_expansion(key, w, nk, nr);

    // Allocate plaintext buffer
    uint8_t *plaintext = (uint8_t *)malloc(len);
    if (!plaintext) {
        aes_secure_zero(w, w_size);
        free(w);
        rt_trap("AES: memory allocation failed");
        return NULL;
    }

    // CBC decryption
    uint8_t prev_block[AES_BLOCK_SIZE];
    memcpy(prev_block, iv, AES_BLOCK_SIZE);

    for (size_t i = 0; i < len; i += AES_BLOCK_SIZE) {
        uint8_t decrypted[AES_BLOCK_SIZE];

        // Decrypt block
        aes_decrypt_block(ciphertext + i, decrypted, w, nr);

        // XOR with previous ciphertext (or IV for first block)
        for (int j = 0; j < AES_BLOCK_SIZE; j++)
            plaintext[i + j] = decrypted[j] ^ prev_block[j];

        // Save current ciphertext for next iteration
        memcpy(prev_block, ciphertext + i, AES_BLOCK_SIZE);
    }

    aes_secure_zero(w, w_size);
    free(w);

    // Remove PKCS7 padding
    size_t unpadded_len;
    if (pkcs7_unpad(plaintext, len, &unpadded_len) != 0) {
        aes_secure_zero(plaintext, len);
        free(plaintext);
        return NULL; // Invalid padding
    }

    *out_len = unpadded_len;
    return plaintext;
}

//=============================================================================
// Public API
//=============================================================================

/// @brief Encrypt data using AES-CBC.
///
/// Encrypts binary data using AES in CBC mode with PKCS7 padding.
/// Key length determines AES variant: 16 bytes = AES-128, 32 bytes = AES-256.
///
/// @param data Bytes object containing plaintext
/// @param key Bytes object containing key (16 or 32 bytes)
/// @param iv Bytes object containing initialization vector (must be 16 bytes)
/// @return Bytes object containing ciphertext
void *rt_aes_encrypt(void *data, void *key, void *iv) {
    if (rt_crypto_module_is_approved_mode()) {
        rt_trap("AES.CBC is not an approved-mode Zanna service; use AES-GCM");
        return NULL;
    }
    if (!data) {
        rt_trap("AES: plaintext is null");
        return NULL;
    }
    if (!key) {
        rt_trap("AES: key is null");
        return NULL;
    }
    if (!iv) {
        rt_trap("AES: IV is null");
        return NULL;
    }
    if (!rt_bytes_is_bytes(data)) {
        rt_trap("AES: plaintext must be a Bytes object");
        return NULL;
    }
    if (!rt_bytes_is_bytes(key)) {
        rt_trap("AES: key must be a Bytes object");
        return NULL;
    }
    if (!rt_bytes_is_bytes(iv)) {
        rt_trap("AES: IV must be a Bytes object");
        return NULL;
    }
    size_t data_len, key_len, iv_len;
    uint8_t *data_raw = rt_bytes_extract_raw(data, &data_len);
    uint8_t *key_raw = rt_bytes_extract_raw(key, &key_len);
    uint8_t *iv_raw = rt_bytes_extract_raw(iv, &iv_len);

    // Validate key length
    int nk, nr;
    if (key_len == 16) {
        if (!key_raw) {
            if (data_raw)
                free(data_raw);
            if (iv_raw)
                free(iv_raw);
            return NULL;
        }
        nk = 4;
        nr = 10;
    } else if (key_len == 32) {
        if (!key_raw) {
            if (data_raw)
                free(data_raw);
            if (iv_raw)
                free(iv_raw);
            return NULL;
        }
        nk = 8;
        nr = 14;
    } else {
        if (data_raw)
            free(data_raw);
        if (key_raw) {
            aes_secure_zero(key_raw, key_len);
            free(key_raw);
        }
        if (iv_raw)
            free(iv_raw);
        rt_trap("AES: key must be 16 bytes (AES-128) or 32 bytes (AES-256)");
        return NULL;
    }

    // Validate IV length
    if (iv_len != AES_BLOCK_SIZE) {
        if (data_raw)
            free(data_raw);
        if (key_raw) {
            aes_secure_zero(key_raw, key_len);
            free(key_raw);
        }
        if (iv_raw)
            free(iv_raw);
        rt_trap("AES: IV must be exactly 16 bytes");
        return NULL;
    }
    if (!iv_raw) {
        if (data_raw)
            free(data_raw);
        if (key_raw) {
            aes_secure_zero(key_raw, key_len);
            free(key_raw);
        }
        return NULL;
    }
    if (data_len > 0 && !data_raw) {
        aes_secure_zero(key_raw, key_len);
        free(key_raw);
        free(iv_raw);
        return NULL;
    }

    // Encrypt
    size_t cipher_len;
    uint8_t *cipher = aes_cbc_encrypt(data_raw, data_len, key_raw, iv_raw, nk, nr, &cipher_len);
    if (!cipher) {
        free(data_raw);
        if (key_raw)
            aes_secure_zero(key_raw, key_len);
        free(key_raw);
        free(iv_raw);
        return NULL;
    }

    // Create result
    void *result = rt_bytes_from_raw(cipher, cipher_len);

    free(data_raw);
    if (key_raw)
        aes_secure_zero(key_raw, key_len);
    free(key_raw);
    free(iv_raw);
    free(cipher);

    return result;
}

/// @brief Decrypt data using AES-CBC.
///
/// Decrypts binary data using AES in CBC mode with PKCS7 padding removal.
/// Key length determines AES variant: 16 bytes = AES-128, 32 bytes = AES-256.
///
/// @param data Bytes object containing ciphertext
/// @param key Bytes object containing key (16 or 32 bytes)
/// @param iv Bytes object containing initialization vector (must be 16 bytes)
/// @return Bytes object containing plaintext, or NULL on decryption error
void *rt_aes_decrypt(void *data, void *key, void *iv) {
    if (rt_crypto_module_is_approved_mode()) {
        rt_trap("AES.CBC is not an approved-mode Zanna service; use AES-GCM");
        return NULL;
    }
    if (!data) {
        rt_trap("AES: ciphertext is null");
        return NULL;
    }
    if (!key) {
        rt_trap("AES: key is null");
        return NULL;
    }
    if (!iv) {
        rt_trap("AES: IV is null");
        return NULL;
    }
    if (!rt_bytes_is_bytes(data)) {
        rt_trap("AES: ciphertext must be a Bytes object");
        return NULL;
    }
    if (!rt_bytes_is_bytes(key)) {
        rt_trap("AES: key must be a Bytes object");
        return NULL;
    }
    if (!rt_bytes_is_bytes(iv)) {
        rt_trap("AES: IV must be a Bytes object");
        return NULL;
    }
    size_t data_len, key_len, iv_len;
    uint8_t *data_raw = rt_bytes_extract_raw(data, &data_len);
    uint8_t *key_raw = rt_bytes_extract_raw(key, &key_len);
    uint8_t *iv_raw = rt_bytes_extract_raw(iv, &iv_len);

    // Validate key length
    int nk, nr;
    if (key_len == 16) {
        if (!key_raw) {
            if (data_raw)
                free(data_raw);
            if (iv_raw)
                free(iv_raw);
            return NULL;
        }
        nk = 4;
        nr = 10;
    } else if (key_len == 32) {
        if (!key_raw) {
            if (data_raw)
                free(data_raw);
            if (iv_raw)
                free(iv_raw);
            return NULL;
        }
        nk = 8;
        nr = 14;
    } else {
        if (data_raw)
            free(data_raw);
        if (key_raw) {
            aes_secure_zero(key_raw, key_len);
            free(key_raw);
        }
        if (iv_raw)
            free(iv_raw);
        rt_trap("AES: key must be 16 bytes (AES-128) or 32 bytes (AES-256)");
        return NULL;
    }

    // Validate IV length
    if (iv_len != AES_BLOCK_SIZE) {
        if (data_raw)
            free(data_raw);
        if (key_raw) {
            aes_secure_zero(key_raw, key_len);
            free(key_raw);
        }
        if (iv_raw)
            free(iv_raw);
        rt_trap("AES: IV must be exactly 16 bytes");
        return NULL;
    }
    if (!iv_raw || (data_len > 0 && !data_raw)) {
        if (data_raw)
            free(data_raw);
        if (key_raw) {
            aes_secure_zero(key_raw, key_len);
            free(key_raw);
        }
        if (iv_raw)
            free(iv_raw);
        return NULL;
    }

    // Decrypt
    size_t plain_len;
    uint8_t *plain = aes_cbc_decrypt(data_raw, data_len, key_raw, iv_raw, nk, nr, &plain_len);

    if (data_raw)
        free(data_raw);
    if (key_raw)
        aes_secure_zero(key_raw, key_len);
    free(key_raw);
    free(iv_raw);

    if (!plain) {
        return NULL;
    }

    // Create result
    void *result = rt_bytes_from_raw(plain, plain_len);
    aes_secure_zero(plain, data_len);
    free(plain);

    return result;
}

/// @brief Decrypt AES-CBC data and return a Result.
/// @details Converts invalid padding, approved-mode rejection, invalid key/IV
///          sizes, malformed inputs, and traps into `Err(str)` while preserving
///          successful plaintext as `Ok(Bytes)`.
/// @param data Bytes object containing ciphertext.
/// @param key Bytes object containing a 16-byte or 32-byte AES key.
/// @param iv Bytes object containing the 16-byte initialization vector.
/// @return Opaque Zanna.Result containing plaintext bytes or a diagnostic string.
void *rt_aes_decrypt_result(void *data, void *key, void *iv) {
    return aes_bytes_result(rt_aes_decrypt,
                            data,
                            key,
                            iv,
                            "AES.Decrypt: invalid padding or ciphertext",
                            "AES.Decrypt failed");
}

/// @brief Attempt AES-CBC decryption and return an Option.
/// @details Converts invalid padding, approved-mode rejection, malformed
///          inputs, invalid key/IV sizes, and traps into `None`.
/// @param data Bytes object containing ciphertext.
/// @param key Bytes object containing a 16-byte or 32-byte AES key.
/// @param iv Bytes object containing the 16-byte initialization vector.
/// @return Opaque Zanna.Option containing plaintext bytes, or None.
void *rt_aes_try_decrypt(void *data, void *key, void *iv) {
    return aes_bytes_option(rt_aes_decrypt, data, key, iv);
}

/// @brief AES-GCM authenticated encryption with magic-header framing.
/// @details Implements `Zanna.Crypto.Aes.EncryptAuth(data, key, aad)`. Generates
///          a fresh 12-byte nonce, prepends the AES_AUTH_HEADER and the nonce
///          to the output, runs AES-128-GCM or AES-256-GCM over the plaintext with
///          [magic_header || user_aad] as the AEAD AAD, and appends the
///          16-byte GCM tag. The returned bytes object layout is:
///          [magic(4)|nonce(12)|ciphertext|tag(16)].
///          Decryption via rt_aes_decrypt_auth refuses to proceed if the
///          header doesn't match, so plain-CBC ciphertexts can't be passed
///          through here by accident.
/// @param data Plaintext bytes. Required.
/// @param key  16-byte key (AES-128) or 32-byte key (AES-256). Other lengths trap.
/// @param aad  Optional additional authenticated data; may be NULL.
/// @return New bytes object owning the framed ciphertext, or NULL on bad key.
void *rt_aes_encrypt_auth(void *data, void *key, void *aad) {
    if (!data) {
        rt_trap("AES.Auth: plaintext is null");
        return NULL;
    }
    if (!key) {
        rt_trap("AES.Auth: key is null");
        return NULL;
    }
    if (!rt_bytes_is_bytes(data)) {
        rt_trap("AES.Auth: plaintext must be a Bytes object");
        return NULL;
    }
    if (!rt_bytes_is_bytes(key)) {
        rt_trap("AES.Auth: key must be a Bytes object");
        return NULL;
    }
    size_t data_len, key_len;
    uint8_t *data_raw = rt_bytes_extract_raw(data, &data_len);
    uint8_t *key_raw = rt_bytes_extract_raw(key, &key_len);
    if (key_len != 16 && key_len != 32) {
        free(data_raw);
        if (key_raw) {
            aes_secure_zero(key_raw, key_len);
            free(key_raw);
        }
        rt_trap("AES.Auth: key must be exactly 16 or 32 bytes");
        return NULL;
    }
    if (!key_raw || (data_len > 0 && !data_raw)) {
        free(data_raw);
        if (key_raw) {
            aes_secure_zero(key_raw, key_len);
            free(key_raw);
        }
        return NULL;
    }
    if (data_len > SIZE_MAX - AES_AUTH_HEADER_LEN - 16) {
        free(data_raw);
        aes_secure_zero(key_raw, key_len);
        free(key_raw);
        rt_trap("AES.Auth: plaintext too large");
        return NULL;
    }

    size_t total_len = AES_AUTH_HEADER_LEN + data_len + 16;
    uint8_t *out = (uint8_t *)malloc(total_len);
    if (!out) {
        free(data_raw);
        aes_secure_zero(key_raw, key_len);
        free(key_raw);
        rt_trap("AES.Auth: memory allocation failed");
        return NULL;
    }

    out[0] = AES_AUTH_MAGIC0;
    out[1] = AES_AUTH_MAGIC1;
    out[2] = AES_AUTH_MAGIC2;
    out[3] = AES_AUTH_MAGIC3;
    generate_random_bytes(out + 4, 12);

    const uint8_t *aad_data;
    size_t aad_len;
    uint8_t *aad_alloc;
    if (!aes_combine_aad(out, AES_AUTH_HEADER_LEN, aad, &aad_data, &aad_len, &aad_alloc)) {
        free(data_raw);
        aes_secure_zero(key_raw, key_len);
        free(key_raw);
        aes_secure_zero(out, total_len);
        free(out);
        return NULL;
    }
    size_t encrypted_len = key_len == 16
                               ? rt_aes128_gcm_encrypt(key_raw,
                                                       out + 4,
                                                       aad_data,
                                                       aad_len,
                                                       data_raw ? data_raw : (const uint8_t *)"",
                                                       data_len,
                                                       out + AES_AUTH_HEADER_LEN)
                               : rt_aes256_gcm_encrypt(key_raw,
                                                       out + 4,
                                                       aad_data,
                                                       aad_len,
                                                       data_raw ? data_raw : (const uint8_t *)"",
                                                       data_len,
                                                       out + AES_AUTH_HEADER_LEN);
    if (aad_alloc)
        free(aad_alloc);
    if (encrypted_len != data_len + 16) {
        free(data_raw);
        aes_secure_zero(key_raw, key_len);
        free(key_raw);
        aes_secure_zero(out, total_len);
        free(out);
        rt_trap("AES.Auth: encryption failed");
        return NULL;
    }

    void *result = rt_bytes_from_raw(out, total_len);
    free(data_raw);
    aes_secure_zero(key_raw, key_len);
    free(key_raw);
    aes_secure_zero(out, total_len);
    free(out);
    return result;
}

/// @brief AES-GCM authenticated decryption with magic-header verification.
/// @details Inverse of rt_aes_encrypt_auth. Validates the leading
///          AES_AUTH_HEADER, extracts the 12-byte nonce, runs AES-GCM
///          decryption with [magic_header || user_aad] as the expected
///          AEAD AAD, and verifies the trailing 16-byte tag. Any mismatch
///          (wrong header, wrong key, modified ciphertext, modified AAD)
///          returns NULL — the caller must treat NULL as authentication
///          failure, not as plaintext.
/// @param data Framed ciphertext from rt_aes_encrypt_auth. Required.
/// @param key  16-byte key (AES-128) or 32-byte key (AES-256). Other lengths trap.
/// @param aad  Same AAD that was used at encryption time; may be NULL.
/// @return New bytes object with the decrypted plaintext, or NULL on any
///         authentication failure.
void *rt_aes_decrypt_auth(void *data, void *key, void *aad) {
    if (!data) {
        rt_trap("AES.Auth: ciphertext is null");
        return NULL;
    }
    if (!key) {
        rt_trap("AES.Auth: key is null");
        return NULL;
    }
    if (!rt_bytes_is_bytes(data)) {
        rt_trap("AES.Auth: ciphertext must be a Bytes object");
        return NULL;
    }
    if (!rt_bytes_is_bytes(key)) {
        rt_trap("AES.Auth: key must be a Bytes object");
        return NULL;
    }
    size_t data_len, key_len;
    uint8_t *encoded = rt_bytes_extract_raw(data, &data_len);
    uint8_t *key_raw = rt_bytes_extract_raw(key, &key_len);
    if (key_len != 16 && key_len != 32) {
        free(encoded);
        if (key_raw) {
            aes_secure_zero(key_raw, key_len);
            free(key_raw);
        }
        rt_trap("AES.Auth: key must be exactly 16 or 32 bytes");
        return NULL;
    }
    if (!key_raw || (data_len > 0 && !encoded)) {
        free(encoded);
        if (key_raw) {
            aes_secure_zero(key_raw, key_len);
            free(key_raw);
        }
        return NULL;
    }
    if (!encoded || data_len < AES_AUTH_HEADER_LEN + 16 || encoded[0] != AES_AUTH_MAGIC0 ||
        encoded[1] != AES_AUTH_MAGIC1 || encoded[2] != AES_AUTH_MAGIC2 ||
        encoded[3] != AES_AUTH_MAGIC3) {
        free(encoded);
        aes_secure_zero(key_raw, key_len);
        free(key_raw);
        return NULL;
    }

    size_t cipher_len = data_len - AES_AUTH_HEADER_LEN;
    uint8_t *plain = (uint8_t *)malloc(cipher_len - 16 + 1);
    if (!plain) {
        free(encoded);
        aes_secure_zero(key_raw, key_len);
        free(key_raw);
        rt_trap("AES.Auth: memory allocation failed");
        return NULL;
    }

    const uint8_t *aad_data;
    size_t aad_len;
    uint8_t *aad_alloc;
    if (!aes_combine_aad(encoded, AES_AUTH_HEADER_LEN, aad, &aad_data, &aad_len, &aad_alloc)) {
        free(plain);
        free(encoded);
        aes_secure_zero(key_raw, key_len);
        free(key_raw);
        return NULL;
    }
    long plain_len = key_len == 16 ? rt_aes128_gcm_decrypt(key_raw,
                                                           encoded + 4,
                                                           aad_data,
                                                           aad_len,
                                                           encoded + AES_AUTH_HEADER_LEN,
                                                           cipher_len,
                                                           plain)
                                   : rt_aes256_gcm_decrypt(key_raw,
                                                           encoded + 4,
                                                           aad_data,
                                                           aad_len,
                                                           encoded + AES_AUTH_HEADER_LEN,
                                                           cipher_len,
                                                           plain);
    if (aad_alloc)
        free(aad_alloc);
    aes_secure_zero(key_raw, key_len);
    free(key_raw);
    aes_secure_zero(encoded, data_len);
    free(encoded);
    if (plain_len < 0) {
        aes_secure_zero(plain, cipher_len - 16);
        free(plain);
        return NULL;
    }

    void *result = rt_bytes_from_raw(plain, (size_t)plain_len);
    aes_secure_zero(plain, (size_t)plain_len);
    free(plain);
    return result;
}

/// @brief Decrypt AES-GCM authenticated data and return a Result.
/// @details Converts tag mismatch, malformed input, invalid key size, and traps
///          into `Err(str)` while preserving successful plaintext as `Ok(Bytes)`.
/// @param data Framed ciphertext produced by rt_aes_encrypt_auth.
/// @param key Bytes object containing a 16-byte or 32-byte AES key.
/// @param aad Additional authenticated data; may be NULL.
/// @return Opaque Zanna.Result containing plaintext bytes or a diagnostic string.
void *rt_aes_decrypt_auth_result(void *data, void *key, void *aad) {
    return aes_bytes_result(rt_aes_decrypt_auth,
                            data,
                            key,
                            aad,
                            "AES.DecryptAuth: authentication failed",
                            "AES.DecryptAuth failed");
}

/// @brief Attempt AES-GCM authenticated decryption and return an Option.
/// @details Converts authentication failure, malformed input, invalid key size,
///          and traps into `None`.
/// @param data Framed ciphertext produced by rt_aes_encrypt_auth.
/// @param key Bytes object containing a 16-byte or 32-byte AES key.
/// @param aad Additional authenticated data; may be NULL.
/// @return Opaque Zanna.Option containing plaintext bytes, or None.
void *rt_aes_try_decrypt_auth(void *data, void *key, void *aad) {
    return aes_bytes_option(rt_aes_decrypt_auth, data, key, aad);
}

/// @brief Derive a 32-byte key from password using iterated SHA-256 (S-06).
///
/// Uses 10 000 rounds of SHA-256 with a fixed application salt and a length
/// prefix for domain separation. This is not PBKDF2 (no per-call salt), but
/// significantly harder to brute-force than a single-pass SHA-256.
/// For production-grade security, use PBKDF2-HMAC-SHA256 with a random salt.
#define DERIVE_KEY_ROUNDS 10000

/// @brief Legacy v0/v1 key derivation — fixed-domain iterated SHA-256.
///
/// Kept for backward-compatibility decryption only; new encryptions
/// must use `derive_key_pbkdf2`. It hashes a fixed domain separator, a
/// one-byte length prefix, and at most the first 256 password bytes, then
/// performs 10,000 SHA-256 rounds. It has no per-payload salt and is retained
/// only to read existing ciphertexts created with the legacy format.
///
/// WEAKNESS (VDOC-178): the length prefix is a single byte, so passwords of
/// exactly 256 bytes or longer store a zero length prefix, and only the first
/// 256 password bytes contribute to the key. Long passwords sharing their
/// first 256 bytes therefore derive the same legacy key. This math CANNOT be
/// changed without breaking decryption of existing legacy ciphertexts; on a
/// successful legacy decrypt, immediately re-encrypt with the current
/// authenticated `VAG1`/`Zanna.Crypto.Cipher` format.
/// @param password Borrowed password bytes.
/// @param pass_len Password byte count; only the first 256 bytes are used.
/// @param key Receives the 32-byte legacy AES key and must later be zeroized.
static void derive_key_legacy(const uint8_t *password, size_t pass_len, uint8_t key[32]) {
    /* Fixed application-level domain separator (S-06) */
    static const uint8_t kSalt[16] = {0x56,
                                      0x49,
                                      0x50,
                                      0x45,
                                      0x52,
                                      0x5f,
                                      0x41,
                                      0x45,
                                      0x53,
                                      0x5f,
                                      0x4b,
                                      0x44,
                                      0x46,
                                      0x5f,
                                      0x76,
                                      0x31};

    /* Build initial block: salt || length_byte || password */
    uint8_t block[16 + 1 + 256];
    size_t capped = pass_len < 256 ? pass_len : 256;
    memcpy(block, kSalt, 16);
    block[16] = (uint8_t)capped;
    if (capped > 0)
        memcpy(block + 17, password, capped);

    if (local_sha256(block, 17 + capped, key) != 0) {
        aes_secure_zero(block, sizeof(block));
        rt_trap("AES: legacy key derivation failed");
    }

    /* Iterate to slow down brute-force attacks */
    for (int r = 1; r < DERIVE_KEY_ROUNDS; r++) {
        if (local_sha256(key, 32, key) != 0) {
            aes_secure_zero(block, sizeof(block));
            aes_secure_zero(key, 32);
            rt_trap("AES: legacy key derivation failed");
        }
    }
    aes_secure_zero(block, sizeof(block));
}

#undef DERIVE_KEY_ROUNDS

/// @brief Current PBKDF2-HMAC-SHA256 key derivation for authenticated string encryption.
///
/// `AES_STR_PBKDF2_ITERATIONS` iterations of HMAC-SHA256
/// over `(password, salt)` produce the 16-byte AES-128 key. The high
/// iteration count makes brute-force attacks cost-prohibitive.
/// @param password Borrowed password bytes; NULL is treated as an empty span.
/// @param password_len Number of password bytes.
/// @param salt Borrowed salt bytes.
/// @param salt_len Number of salt bytes.
/// @param iterations PBKDF2 iteration count.
/// @param key Receives the derived 16-byte AES-128 key.
static void derive_key_pbkdf2(const uint8_t *password,
                              size_t password_len,
                              const uint8_t *salt,
                              size_t salt_len,
                              uint32_t iterations,
                              uint8_t key[16]) {
    rt_keyderive_pbkdf2_sha256_raw(password ? password : (const uint8_t *)"",
                                   password_len,
                                   salt,
                                   salt_len,
                                   iterations,
                                   key,
                                   16);
}

/// @brief Fill `buf` with cryptographically secure bytes from the active module RNG.
/// @param buf Writable destination.
/// @param len Number of random bytes required.
static void generate_random_bytes(uint8_t *buf, size_t len) {
    rt_crypto_random_bytes(buf, len);
}

/// @brief Detect whether a raw byte stream has the current GCM-string-format prefix.
///
/// Checks the 4-byte magic prefix that distinguishes the current
/// PBKDF2+GCM format from the legacy CBC format, so the decryptor
/// can dispatch to the intended routine. A legacy random IV that happens to
/// equal the magic is therefore ambiguous and is classified as current
/// (VDOC-173). Unlike the authenticated Cipher formats, legacy AES-CBC is
/// unauthenticated, so there is NO safe automatic fallback: retrying CBC after
/// a GCM authentication failure would return unauthenticated garbage for
/// tampered current-format frames (a downgrade). Such a ~2^-32 colliding legacy
/// frame is therefore intentionally left undecryptable — re-encrypt legacy
/// AES-CBC string data with the current authenticated format.
/// @param data Borrowed encrypted payload bytes.
/// @param len Number of available bytes.
/// @return 1 when the payload is long enough and begins with the VAG1 magic,
///         otherwise 0.
static int aes_is_gcm_string_payload(const uint8_t *data, size_t len) {
    return len >= AES_STR_HEADER_LEN && data[0] == AES_STR_MAGIC0 && data[1] == AES_STR_MAGIC1 &&
           data[2] == AES_STR_MAGIC2 && data[3] == AES_STR_MAGIC3;
}

/// @brief Encrypt a string using authenticated AES-128-GCM with PBKDF2-derived keys.
///
/// Output format:
///   [magic "VAG1"(4)][PBKDF2 iterations BE32(4)][salt(16)][nonce(12)][ciphertext][tag(16)]
///
/// Decrypt remains backward-compatible with the legacy
/// [iv(16)][aes-256-cbc-ciphertext] format.
/// @param data Runtime plaintext string; must not be NULL.
/// @param password Nonempty runtime password string.
/// @return Newly allocated Bytes object containing the authenticated frame, or
///         NULL after validation, size, allocation, derivation, RNG, or
///         encryption failure.
void *rt_aes_encrypt_str(rt_string data, rt_string password) {
    size_t plain_len;
    size_t pass_len;
    int data_ok;
    int pass_ok;
    const uint8_t *data_bytes =
        aes_string_bytes(data, &plain_len, "AES: plaintext is null", &data_ok);
    const uint8_t *pass_bytes =
        aes_string_bytes(password, &pass_len, "AES: password is null", &pass_ok);
    uint8_t salt[16];
    uint8_t nonce[12];
    uint8_t key[16];
    size_t cipher_len;
    size_t total_len;
    uint8_t *out;
    void *result;

    if (!data_ok || !pass_ok)
        return NULL;
    if (pass_len == 0) {
        rt_trap("AES: password is empty");
        return NULL;
    }

    generate_random_bytes(salt, sizeof(salt));
    generate_random_bytes(nonce, sizeof(nonce));
    derive_key_pbkdf2(pass_bytes, pass_len, salt, sizeof(salt), AES_STR_PBKDF2_ITERATIONS, key);

    if (plain_len > SIZE_MAX - 16) {
        aes_secure_zero(key, sizeof(key));
        aes_secure_zero(salt, sizeof(salt));
        aes_secure_zero(nonce, sizeof(nonce));
        rt_trap("AES: plaintext too large");
        return NULL;
    }
    cipher_len = plain_len + 16;
    if (cipher_len > SIZE_MAX - AES_STR_HEADER_LEN) {
        aes_secure_zero(key, sizeof(key));
        aes_secure_zero(salt, sizeof(salt));
        aes_secure_zero(nonce, sizeof(nonce));
        rt_trap("AES: plaintext too large");
        return NULL;
    }

    total_len = AES_STR_HEADER_LEN + cipher_len;
    out = (uint8_t *)malloc(total_len);
    if (!out) {
        aes_secure_zero(key, sizeof(key));
        aes_secure_zero(salt, sizeof(salt));
        aes_secure_zero(nonce, sizeof(nonce));
        rt_trap("AES: memory allocation failed");
        return NULL;
    }

    out[0] = AES_STR_MAGIC0;
    out[1] = AES_STR_MAGIC1;
    out[2] = AES_STR_MAGIC2;
    out[3] = AES_STR_MAGIC3;
    out[4] = (uint8_t)((AES_STR_PBKDF2_ITERATIONS >> 24) & 0xFF);
    out[5] = (uint8_t)((AES_STR_PBKDF2_ITERATIONS >> 16) & 0xFF);
    out[6] = (uint8_t)((AES_STR_PBKDF2_ITERATIONS >> 8) & 0xFF);
    out[7] = (uint8_t)(AES_STR_PBKDF2_ITERATIONS & 0xFF);
    memcpy(out + 8, salt, sizeof(salt));
    memcpy(out + 24, nonce, sizeof(nonce));

    cipher_len = rt_aes128_gcm_encrypt(
        key, nonce, out, AES_STR_HEADER_LEN, data_bytes, plain_len, out + AES_STR_HEADER_LEN);
    if (cipher_len == 0 && plain_len != 0) {
        aes_secure_zero(key, sizeof(key));
        aes_secure_zero(salt, sizeof(salt));
        aes_secure_zero(nonce, sizeof(nonce));
        aes_secure_zero(out, total_len);
        free(out);
        rt_trap("AES: authenticated encryption failed");
        return NULL;
    }

    result = rt_bytes_from_raw(out, total_len);
    aes_secure_zero(key, sizeof(key));
    aes_secure_zero(salt, sizeof(salt));
    aes_secure_zero(nonce, sizeof(nonce));
    aes_secure_zero(out, total_len);
    free(out);
    return result;
}

/// @brief Decrypt a string encrypted by rt_aes_encrypt_str.
///
/// Accepts both the current authenticated VAG1 format and the legacy
/// AES-256-CBC string format for backward compatibility.
/// @param data Bytes object containing a VAG1 frame or legacy IV-prefixed CBC
///        payload.
/// @param password Nonempty runtime password string.
/// @return Newly allocated plaintext runtime string on successful authenticated
///         or legacy decryption; failures trap and yield an empty fallback if
///         trap handling returns.
rt_string rt_aes_decrypt_str(void *data, rt_string password) {
    size_t pass_len;
    if (!data) {
        rt_trap("AES: encrypted data is null");
        return rt_const_cstr("");
    }
    int pass_ok;
    const uint8_t *pass_bytes =
        aes_string_bytes(password, &pass_len, "AES: password is null", &pass_ok);
    if (!pass_ok)
        return rt_const_cstr("");
    if (pass_len == 0) {
        rt_trap("AES: password is empty");
        return rt_const_cstr("");
    }
    if (!rt_bytes_is_bytes(data)) {
        rt_trap("AES: encrypted data must be a Bytes object");
        return rt_const_cstr("");
    }

    int64_t total_len = rt_bytes_len(data);
    if (total_len <= 0) {
        rt_trap("AES: encrypted data is empty");
        return rt_const_cstr("");
    }

    uint8_t *encoded = (uint8_t *)malloc((size_t)total_len);
    if (!encoded) {
        rt_trap("AES: memory allocation failed");
        return rt_const_cstr("");
    }
    for (int64_t i = 0; i < total_len; i++)
        encoded[i] = (uint8_t)rt_bytes_get(data, i);

    if (aes_is_gcm_string_payload(encoded, (size_t)total_len)) {
        uint32_t iterations = ((uint32_t)encoded[4] << 24) | ((uint32_t)encoded[5] << 16) |
                              ((uint32_t)encoded[6] << 8) | (uint32_t)encoded[7];
        const uint8_t *salt = encoded + 8;
        const uint8_t *nonce = encoded + 24;
        const uint8_t *cipher = encoded + AES_STR_HEADER_LEN;
        size_t cipher_len = (size_t)total_len - AES_STR_HEADER_LEN;
        uint8_t key[16];
        uint8_t *plain;
        long plain_len;
        rt_string result;

        if (iterations < RT_PBKDF2_MIN_ITERATIONS || iterations > RT_PBKDF2_MAX_ITERATIONS) {
            aes_secure_zero(encoded, (size_t)total_len);
            free(encoded);
            rt_trap("AES: encrypted data uses an unsupported PBKDF2 iteration count");
            return rt_const_cstr("");
        }
        if (cipher_len < 16) {
            aes_secure_zero(encoded, (size_t)total_len);
            free(encoded);
            rt_trap("AES: encrypted data too short");
            return rt_const_cstr("");
        }

        derive_key_pbkdf2(pass_bytes, pass_len, salt, 16, iterations, key);
        plain = (uint8_t *)malloc(cipher_len - 16 + 1);
        if (!plain) {
            aes_secure_zero(key, sizeof(key));
            aes_secure_zero(encoded, (size_t)total_len);
            free(encoded);
            rt_trap("AES: memory allocation failed");
            return rt_const_cstr("");
        }

        plain_len = rt_aes128_gcm_decrypt(
            key, nonce, encoded, AES_STR_HEADER_LEN, cipher, cipher_len, plain);
        aes_secure_zero(key, sizeof(key));
        aes_secure_zero(encoded, (size_t)total_len);
        free(encoded);
        if (plain_len < 0) {
            aes_secure_zero(plain, cipher_len - 16);
            free(plain);
            rt_trap("AES: decryption failed (wrong password or corrupted data)");
            return rt_const_cstr("");
        }

        result = rt_string_from_bytes((const char *)plain, (size_t)plain_len);
        aes_secure_zero(plain, (size_t)plain_len);
        free(plain);
        return result;
    }

    if (total_len < 16) {
        aes_secure_zero(encoded, (size_t)total_len);
        free(encoded);
        rt_trap("AES: encrypted data too short (missing IV)");
        return rt_const_cstr("");
    }

    if (total_len == 16) {
        aes_secure_zero(encoded, (size_t)total_len);
        free(encoded);
        rt_trap("AES: encrypted data too short (missing ciphertext)");
        return rt_const_cstr("");
    }

    uint8_t iv[16];
    uint8_t key[32];
    uint8_t *cipher = (uint8_t *)malloc((size_t)(total_len - 16));
    size_t plain_len;
    uint8_t *plain;
    rt_string result;

    if (!cipher) {
        aes_secure_zero(encoded, (size_t)total_len);
        free(encoded);
        rt_trap("AES: memory allocation failed");
        return rt_const_cstr("");
    }

    memcpy(iv, encoded, sizeof(iv));
    memcpy(cipher, encoded + 16, (size_t)(total_len - 16));
    aes_secure_zero(encoded, (size_t)total_len);
    free(encoded);

    derive_key_legacy(pass_bytes, pass_len, key);
    plain = aes_cbc_decrypt(cipher, (size_t)(total_len - 16), key, iv, 8, 14, &plain_len);
    aes_secure_zero(key, sizeof(key));
    aes_secure_zero(cipher, (size_t)(total_len - 16));
    free(cipher);

    if (!plain) {
        rt_trap("AES: decryption failed (wrong password or corrupted data)");
        return rt_const_cstr("");
    }

    result = rt_string_from_bytes((const char *)plain, plain_len);
    aes_secure_zero(plain, (size_t)(total_len - 16));
    free(plain);
    return result;
}

/// @brief Decrypt an AES encrypted string and return a Result.
/// @details Captures traps from @ref rt_aes_decrypt_str as `Err(str)` and
///          returns all successful plaintext strings, including empty strings,
///          as `Ok(str)`.
/// @param data Bytes object containing encrypted string payload.
/// @param password Password string used for key derivation.
/// @return Opaque Zanna.Result containing plaintext string or a diagnostic string.
void *rt_aes_decrypt_str_result(void *data, rt_string password) {
    return aes_string_decrypt_result(rt_aes_decrypt_str, data, password, "AES.DecryptStr failed");
}

/// @brief Attempt AES encrypted string decryption and return an Option.
/// @details Captures traps from @ref rt_aes_decrypt_str as `None` and returns
///          all successful plaintext strings, including empty strings, as
///          `Some(str)`.
/// @param data Bytes object containing encrypted string payload.
/// @param password Password string used for key derivation.
/// @return Opaque Zanna.Option containing plaintext string, or None.
void *rt_aes_try_decrypt_str(void *data, rt_string password) {
    return aes_string_decrypt_option(rt_aes_decrypt_str, data, password);
}
