//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/text/rt_hash.h
// Purpose: C ABI for MD5, SHA-1, SHA-256, matching HMAC constructions,
//          CRC32, fixed-time equality, and per-process-keyed SipHash-2-4.
//
// Key invariants:
//   - MD5 and SHA-1 are cryptographically broken; use only for legacy
//     interoperability. CRC32 is non-cryptographic.
//   - Digest and MAC strings use lowercase hexadecimal.
//   - rt_hash_crc32 returns the CRC32 as an integer, not a string.
//   - MD5, SHA-1, CRC32, and SipHash entry points are disabled by approved-mode
//     crypto policy.
//   - String inputs must be valid non-null runtime strings; null Bytes
//     references are consistently treated as empty buffers.
//
// Ownership/Lifetime:
//   - Returned strings are newly allocated; caller must release.
//   - Input strings and Bytes objects are borrowed; callers retain ownership.
//
// Links: src/runtime/text/rt_hash.c (implementation),
//        src/runtime/core/rt_string.h (runtime strings),
//        src/runtime/collections/rt_bytes.h (binary inputs),
//        src/runtime/core/rt_crc32.h (CRC32 primitive),
//        src/runtime/text/rt_hash_util.h (SipHash primitive)
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_hash.h
 * @brief Declares runtime digest, MAC, checksum, keyed-hash, and equality services.
 * @details String and Bytes overloads produce caller-owned lowercase digest or
 *          HMAC text, CRC32 returns an integer checksum, SipHash provides
 *          process-keyed noncryptographic hashing, and fixed-time comparison
 *          helpers avoid revealing the first mismatching byte.
 */

#pragma once

#include "rt_string.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Compute MD5 hash of a string.
/// @details MD5 is cryptographically broken and is disabled in approved mode.
///          Null or invalid strings trap.
/// @param str Borrowed non-null input string.
/// @return Newly allocated 32-character lowercase hexadecimal digest, or an
///         empty fallback string after a trapped policy/input error.
rt_string rt_hash_md5(rt_string str);

/// @brief Compute MD5 hash of a Bytes object.
/// @details MD5 is cryptographically broken and is disabled in approved mode.
/// @param bytes Borrowed input Bytes object, or null for empty input.
/// @return Newly allocated 32-character lowercase hexadecimal digest, or an
///         empty fallback string after a trapped policy/type error.
rt_string rt_hash_md5_bytes(void *bytes);

/// @brief Compute SHA1 hash of a string.
/// @details SHA-1 is collision-broken and is disabled in approved mode. Null
///          or invalid strings trap.
/// @param str Borrowed non-null input string.
/// @return Newly allocated 40-character lowercase hexadecimal digest, or an
///         empty fallback string after a trapped policy/input error.
rt_string rt_hash_sha1(rt_string str);

/// @brief Compute SHA1 hash of a Bytes object.
/// @details SHA-1 is collision-broken and is disabled in approved mode.
/// @param bytes Borrowed input Bytes object, or null for empty input.
/// @return Newly allocated 40-character lowercase hexadecimal digest, or an
///         empty fallback string after a trapped policy/type error.
rt_string rt_hash_sha1_bytes(void *bytes);

/// @brief Compute SHA256 hash of a string.
/// @param str Borrowed non-null input string; invalid input traps.
/// @return Newly allocated 64-character lowercase hexadecimal digest, or an
///         empty fallback string after a trapped input error.
rt_string rt_hash_sha256(rt_string str);

/// @brief Compute SHA256 hash of a Bytes object.
/// @param bytes Borrowed input Bytes object, or null for empty input.
/// @return Newly allocated 64-character lowercase hexadecimal digest, or an
///         empty fallback string after a trapped type error.
rt_string rt_hash_sha256_bytes(void *bytes);

/// @brief Compute CRC32 checksum of a string.
/// @details Uses the IEEE reflected polynomial. CRC32 detects accidental
///          corruption but provides no cryptographic integrity and is disabled
///          in approved mode.
/// @param str Borrowed non-null input string; invalid input traps.
/// @return Unsigned 32-bit checksum represented in `int64_t`, or zero after a
///         trapped policy/input error.
int64_t rt_hash_crc32(rt_string str);

/// @brief Compute CRC32 checksum of a Bytes object.
/// @details Uses the IEEE reflected polynomial and is disabled in approved
///          mode.
/// @param bytes Borrowed input Bytes object, or null for empty input.
/// @return Unsigned 32-bit checksum represented in `int64_t`, or zero after a
///         trapped policy/type error.
int64_t rt_hash_crc32_bytes(void *bytes);

//=========================================================================
// HMAC Functions
//=========================================================================

/// @brief Compute HMAC-MD5 of string data with string key.
/// @details HMAC-MD5 is a legacy construction and is disabled in approved
///          mode. Null or invalid strings trap.
/// @param key Borrowed non-null secret key string.
/// @param data Borrowed non-null message string.
/// @return Newly allocated 32-character lowercase hexadecimal MAC, or an empty
///         fallback string after a trapped error.
rt_string rt_hash_hmac_md5(rt_string key, rt_string data);

/// @brief Compute HMAC-MD5 of Bytes data with Bytes key.
/// @details HMAC-MD5 is disabled in approved mode.
/// @param key Borrowed Bytes key, or null for an empty key.
/// @param data Borrowed Bytes message, or null for empty input.
/// @return Newly allocated 32-character lowercase hexadecimal MAC, or an empty
///         fallback string after a trapped error.
rt_string rt_hash_hmac_md5_bytes(void *key, void *data);

/// @brief Compute HMAC-SHA1 of string data with string key.
/// @details HMAC-SHA-1 is disabled in approved mode. Null or invalid strings
///          trap.
/// @param key Borrowed non-null secret key string.
/// @param data Borrowed non-null message string.
/// @return Newly allocated 40-character lowercase hexadecimal MAC, or an empty
///         fallback string after a trapped error.
rt_string rt_hash_hmac_sha1(rt_string key, rt_string data);

/// @brief Compute HMAC-SHA1 of Bytes data with Bytes key.
/// @details HMAC-SHA-1 is disabled in approved mode.
/// @param key Borrowed Bytes key, or null for an empty key.
/// @param data Borrowed Bytes message, or null for empty input.
/// @return Newly allocated 40-character lowercase hexadecimal MAC, or an empty
///         fallback string after a trapped error.
rt_string rt_hash_hmac_sha1_bytes(void *key, void *data);

/// @brief Compute HMAC-SHA256 of string data with string key.
/// @param key Borrowed non-null secret key string.
/// @param data Borrowed non-null message string.
/// @return Newly allocated 64-character lowercase hexadecimal MAC, or an empty
///         fallback string after a trapped input error.
rt_string rt_hash_hmac_sha256(rt_string key, rt_string data);

/// @brief Compute HMAC-SHA256 of Bytes data with Bytes key.
/// @param key Borrowed Bytes key, or null for an empty key.
/// @param data Borrowed Bytes message, or null for empty input.
/// @return Newly allocated 64-character lowercase hexadecimal MAC, or an empty
///         fallback string after a trapped type error.
rt_string rt_hash_hmac_sha256_bytes(void *key, void *data);

/// @brief Fixed-length equality for string digests/MACs.
/// @details Length mismatch returns immediately. Equal-length inputs examine
///          every byte without data-dependent early exit. Null or invalid
///          strings trap and compare unequal if trap recovery returns.
/// @param a Borrowed first non-null string.
/// @param b Borrowed second non-null string.
/// @return 1 when byte lengths and contents match, 0 otherwise.
int8_t rt_hash_constant_time_equals(rt_string a, rt_string b);

/// @brief Fixed-length equality for Bytes digests/MACs.
/// @details Length mismatch returns immediately. Equal-length inputs examine
///          every byte without data-dependent early exit.
/// @param a Borrowed first Bytes object, or null for an empty buffer.
/// @param b Borrowed second Bytes object, or null for an empty buffer.
/// @return 1 when byte lengths and contents match, 0 otherwise.
int8_t rt_hash_constant_time_equals_bytes(void *a, void *b);

//=========================================================================
// Fast Per-Process-Keyed Hash (SipHash-2-4)
//=========================================================================

/// @brief Compute per-process-keyed SipHash-2-4 over a string's bytes.
/// @details Intended for fast keyed hashing and HashDoS resistance, not as a
///          persistent digest or authentication tag. Disabled in approved mode.
/// @param str Borrowed non-null input string; invalid input traps.
/// @return SipHash bit pattern represented as signed `int64_t`, or zero after a
///         trapped policy/input error; stable only within one process run.
int64_t rt_hash_fast(rt_string str);

/// @brief Compute per-process-keyed SipHash-2-4 over a Bytes payload.
/// @param bytes Borrowed Bytes object, or null for an empty buffer.
/// @return SipHash bit pattern represented as signed `int64_t`, or zero after a
///         trapped policy/type error; stable only within one process run.
int64_t rt_hash_fast_bytes(void *bytes);

/// @brief Compute keyed SipHash-2-4 over an integer's little-endian bytes.
/// @param value Signed integer whose two's-complement bit pattern is hashed.
/// @return SipHash bit pattern represented as signed `int64_t`, or zero after a
///         trapped policy error; stable only within one process run.
int64_t rt_hash_fast_int(int64_t value);

//=========================================================================
// Internal HMAC functions (for PBKDF2)
//=========================================================================

/// @brief Compute raw HMAC-SHA256 (returns binary, not hex).
/// @details This low-level helper performs no approved-mode policy check. Null
///          key or data pointers are permitted only with a corresponding zero
///          length. Intermediate key material is erased before return.
/// @param key Borrowed key bytes.
/// @param key_len Number of key bytes.
/// @param data Borrowed message bytes.
/// @param data_len Number of message bytes.
/// @param out Writable output buffer of at least 32 bytes.
void rt_hash_hmac_sha256_raw(
    const uint8_t *key, size_t key_len, const uint8_t *data, size_t data_len, uint8_t out[32]);

#ifdef __cplusplus
}
#endif
