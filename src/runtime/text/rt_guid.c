//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE in the project root for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/text/rt_guid.c
// Purpose: Implements UUID version 4 (random) generation per RFC 9562 (which
//          obsoletes RFC 4122) for the Zanna.Text.Guid class. Generates 128-bit
//          random identifiers formatted as xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx
//          (lowercase hex with dashes).
//
// Key invariants:
//   - Version bits (4) are always set at position 6 of byte 6.
//   - Variant bits (10xx) are always set in byte 8 per RFC 9562.
//   - Random bytes come from the shared crypto RNG (rt_crypto_random_bytes:
//     approved-mode DRBG, BCryptGenRandom on Windows, getrandom() with a
//     fallback on Linux, arc4random_buf() on macOS).
//   - Output format is always lowercase 36-character string with 4 dashes.
//   - Parse accepts both uppercase and lowercase hex; validates format strictly.
//   - All functions are thread-safe.
//
// Ownership/Lifetime:
//   - Generated UUID strings are fresh rt_string allocations owned by the caller.
//
// Links: src/runtime/text/rt_guid.h (public API),
//        src/runtime/network/rt_crypto.h (shared crypto RNG)
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_guid.c
 * @brief Implements RFC 9562 UUID version-four generation and conversion.
 * @details The module draws 128 cryptographically random bits, sets canonical
 *          version and variant fields, formats lowercase hyphenated text,
 *          validates UUID syntax, and converts between standard textual form
 *          and exact 16-byte managed representations.
 */

#include "rt_guid.h"

#include "rt_bytes.h"
#include "rt_crypto.h"
#include "rt_internal.h"
#include "rt_string.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/// @brief Fill buffer with cryptographically random bytes.
/// @details Delegates directly to the shared runtime crypto source. Entropy
///          failures trap rather than substituting predictable bytes.
/// @param buf Writable destination buffer.
/// @param len Number of bytes to fill.
static void get_random_bytes(uint8_t *buf, size_t len) {
    rt_crypto_random_bytes(buf, len);
}

//=============================================================================
// Public API
//=============================================================================

/// @brief Generates a new random UUID version 4.
///
/// Creates a new UUID using cryptographically secure random numbers. The
/// generated UUID conforms to RFC 9562 (formerly RFC 4122) with version 4
/// (random) and the standard variant bits.
///
/// **UUID Properties:**
/// - 122 bits of randomness (6 bits used for version and variant)
/// - Probability of collision: ~1 in 5.3×10^36 for each pair (uniqueness is
///   probabilistic, not guaranteed — astronomically unlikely to collide)
/// - Output is always lowercase with dashes
///
/// **Example output:**
/// ```
/// a1b2c3d4-e5f6-4789-abcd-ef0123456789
///              ^    ^
///              4    a/b/8/9 (version and variant)
/// ```
///
/// **Usage example:**
/// ```
/// ' Generate unique IDs for database records
/// Dim id = Guid.New()
/// Print "New record ID: " & id
///
/// ' Multiple unique IDs
/// Dim id1 = Guid.New()
/// Dim id2 = Guid.New()
/// ' id1 and id2 differ except with astronomically small probability
/// ```
///
/// @return A 36-character string in the format
///         `xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx`.
///
/// @note O(1) time complexity.
/// @note Randomness comes from the shared crypto RNG (rt_crypto_random_bytes):
///       approved-mode DRBG, BCryptGenRandom on Windows, getrandom() with a
///       fallback on Linux, arc4random_buf() on macOS.
///
/// @see rt_guid_empty For the nil UUID constant
/// @see rt_guid_is_valid For validating UUID strings
rt_string rt_guid_new(void) {
    uint8_t bytes[16];
    get_random_bytes(bytes, 16);

    // Set version 4 (random UUID) in byte 6: high nibble = 0100
    bytes[6] = (bytes[6] & 0x0F) | 0x40;

    // Set variant (RFC 9562) in byte 8: high bits = 10
    bytes[8] = (bytes[8] & 0x3F) | 0x80;

    // Format as lowercase hex with dashes (36 chars + null)
    char buf[37];
    snprintf(buf,
             sizeof(buf),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             bytes[0],
             bytes[1],
             bytes[2],
             bytes[3],
             bytes[4],
             bytes[5],
             bytes[6],
             bytes[7],
             bytes[8],
             bytes[9],
             bytes[10],
             bytes[11],
             bytes[12],
             bytes[13],
             bytes[14],
             bytes[15]);

    return rt_string_from_bytes(buf, 36);
}

/// @brief Returns the nil UUID (all zeros).
///
/// Returns the special "nil" UUID that consists of all zeros. The nil UUID
/// is defined in RFC 9562 and is useful as a placeholder or default value.
///
/// **Nil UUID:** `00000000-0000-0000-0000-000000000000`
///
/// **Usage example:**
/// ```
/// Dim userId = Guid.Empty()
///
/// If userId = Guid.Empty() Then
///     Print "User not assigned"
/// End If
/// ```
///
/// @return The nil UUID string "00000000-0000-0000-0000-000000000000".
///
/// @note O(1) time complexity.
/// @note Returns a fresh runtime string that the caller owns.
/// @note The nil UUID is valid according to rt_guid_is_valid.
///
/// @see rt_guid_new For generating unique UUIDs
/// @see rt_guid_is_valid For validation
rt_string rt_guid_empty(void) {
    return rt_const_cstr("00000000-0000-0000-0000-000000000000");
}

/// @brief Validates whether a string is a properly formatted UUID.
///
/// Checks if the input string matches the standard UUID format:
/// - Exactly 36 characters
/// - Contains 32 hexadecimal digits (0-9, a-f, A-F)
/// - Has dashes at positions 8, 13, 18, and 23
///
/// **Valid format:** `xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx`
///
/// This function only validates the format, not whether the UUID has valid
/// version and variant bits.
///
/// **Usage example:**
/// ```
/// If Guid.IsValid(userInput) Then
///     ProcessUserId(userInput)
/// Else
///     Print "Invalid UUID format"
/// End If
/// ```
///
/// **Examples:**
/// ```
/// Guid.IsValid("a1b2c3d4-e5f6-4789-abcd-ef0123456789")  ' True
/// Guid.IsValid("00000000-0000-0000-0000-000000000000")  ' True (nil UUID)
/// Guid.IsValid("A1B2C3D4-E5F6-4789-ABCD-EF0123456789")  ' True (uppercase OK)
/// Guid.IsValid("a1b2c3d4e5f64789abcdef0123456789")      ' False (no dashes)
/// Guid.IsValid("not-a-valid-uuid")                       ' False
/// Guid.IsValid("")                                       ' False
/// ```
///
/// @param str The string to validate.
///
/// @return 1 (true) if valid UUID format, 0 (false) otherwise.
///
/// @note O(n) time complexity where n = 36 (constant).
/// @note Accepts both uppercase and lowercase hex digits.
/// @note NULL input returns false.
///
/// @see rt_guid_new For generating valid UUIDs
/// @see rt_guid_to_bytes For converting valid UUIDs to bytes
int8_t rt_guid_is_valid(rt_string str) {
    if (!str) {
        return 0;
    }

    const char *s = rt_string_cstr(str);
    if (!s || rt_str_len(str) != 36) {
        return 0;
    }

    // Expected format: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
    // Dash positions: 8, 13, 18, 23
    for (int i = 0; i < 36; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            // Dash positions
            if (s[i] != '-') {
                return 0;
            }
        } else {
            // Hex digit positions
            if (!isxdigit((unsigned char)s[i])) {
                return 0;
            }
        }
    }

    return 1;
}

/// @brief Converts a UUID string to its 16-byte binary representation.
///
/// Parses a UUID string and returns its raw 16-byte representation as a Bytes
/// object. This is useful for:
/// - Storing UUIDs compactly in binary files
/// - Network protocols that require binary UUIDs
/// - Database columns that store UUIDs as BLOB
///
/// **Byte ordering:**
/// The bytes are returned in network byte order (big-endian), which is the
/// standard order defined by RFC 9562.
///
/// **Usage example:**
/// ```
/// Dim id = Guid.New()
/// Dim bytes = Guid.ToBytes(id)
/// Print "UUID is " & bytes.Len() & " bytes"  ' Always 16
///
/// ' Store binary UUID
/// file.Write(bytes, 0, 16)
///
/// ' Convert back to string
/// Dim restored = Guid.FromBytes(bytes)
/// ```
///
/// @param str A valid UUID string (36 characters with dashes).
///
/// @return A Bytes object containing exactly 16 bytes. Traps if the input
///         string is not a valid UUID format.
///
/// @note O(1) time complexity (fixed 36-character input).
/// @note The input must pass rt_guid_is_valid or the function will trap.
///
/// @see rt_guid_from_bytes For the inverse operation
/// @see rt_guid_is_valid For validating before conversion
void *rt_guid_to_bytes(rt_string str) {
    if (!rt_guid_is_valid(str)) {
        rt_trap("Guid.ToBytes: invalid GUID format");
        return NULL;
    }

    void *bytes = rt_bytes_new(16);
    const char *s = rt_string_cstr(str);
    int str_pos = 0;
    int byte_idx = 0;

    while (s[str_pos] && byte_idx < 16) {
        // Skip dashes
        if (s[str_pos] == '-') {
            str_pos++;
            continue;
        }

        // Parse two hex digits using shared utility
        int hi = rt_hex_digit_value(s[str_pos]);
        int lo = rt_hex_digit_value(s[str_pos + 1]);
        rt_bytes_set(bytes, byte_idx, (hi << 4) | lo);

        byte_idx++;
        str_pos += 2;
    }

    return bytes;
}

/// @brief Converts a 16-byte binary representation to a UUID string.
///
/// Creates a UUID string from raw binary data. This is the inverse of
/// rt_guid_to_bytes and is useful for:
/// - Reading UUIDs from binary files
/// - Deserializing UUIDs from network protocols
/// - Displaying UUIDs stored as BLOB in databases
///
/// **Output format:**
/// The output is always lowercase with dashes in the standard positions:
/// `xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx`
///
/// **Usage example:**
/// ```
/// ' Read binary UUID from file
/// Dim bytes = Bytes.New(16)
/// file.Read(bytes, 0, 16)
///
/// Dim id = Guid.FromBytes(bytes)
/// Print "Loaded UUID: " & id
/// ```
///
/// **Creating specific UUIDs:**
/// ```
/// ' Create the nil UUID manually
/// Dim zeros = Bytes.New(16)  ' All zeros
/// Dim nilUuid = Guid.FromBytes(zeros)
/// ' nilUuid = "00000000-0000-0000-0000-000000000000"
/// ```
///
/// @param bytes A Bytes object containing exactly 16 bytes.
///
/// @return A 36-character UUID string. Traps if bytes does not contain
///         exactly 16 bytes.
///
/// @note O(1) time complexity (fixed 16-byte input).
/// @note Does not validate version or variant bits - creates the string as-is.
///
/// @see rt_guid_to_bytes For the inverse operation
/// @see rt_guid_new For generating new UUIDs
rt_string rt_guid_from_bytes(void *bytes) {
    if (rt_bytes_len(bytes) != 16) {
        rt_trap("Guid.FromBytes: requires exactly 16 bytes");
        return NULL;
    }

    // Extract byte values
    uint8_t data[16];
    for (int i = 0; i < 16; i++) {
        data[i] = (uint8_t)rt_bytes_get(bytes, i);
    }

    // Format as GUID string
    char buf[37];
    snprintf(buf,
             sizeof(buf),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             data[0],
             data[1],
             data[2],
             data[3],
             data[4],
             data[5],
             data[6],
             data[7],
             data[8],
             data[9],
             data[10],
             data[11],
             data[12],
             data[13],
             data[14],
             data[15]);

    return rt_string_from_bytes(buf, 36);
}
