//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/text/rt_guid.h
// Purpose: UUID version 4 generation, validation, and binary conversion per
//          RFC 9562, using the standard 8-4-4-4-12 textual representation.
//
// Key invariants:
//   - Generates RFC 9562 version 4 UUIDs with proper version and variant bits.
//   - Output is always lowercase hex with hyphens: xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx.
//   - Uses a cryptographically secure random source where available.
//   - rt_guid_is_valid validates the format but not the version/variant bits.
//
// Ownership/Lifetime:
//   - Returned strings are newly allocated; caller must release.
//   - Returned Bytes objects are newly allocated; caller must release.
//   - String and Bytes inputs are borrowed for the duration of each call.
//   - No persistent state; each call generates an independent GUID.
//
// Links: src/runtime/text/rt_guid.c (implementation),
//        src/runtime/core/rt_string.h (runtime strings),
//        src/runtime/collections/rt_bytes.h (binary UUID storage),
//        src/runtime/network/rt_crypto.h (secure random source)
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_guid.h
 * @brief Declares UUID generation, syntax validation, and binary conversion.
 * @details The API creates cryptographically random RFC 9562 version-four
 *          identifiers, returns the nil UUID, validates standard hyphenated
 *          hexadecimal syntax, and converts caller-borrowed text or Bytes into
 *          newly allocated managed counterparts.
 */

#pragma once

#include <stdint.h>

#include "rt_string.h"

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Generate a new random UUID v4.
/// @details Draws 128 bits from the shared cryptographic random source, then
///          sets the RFC 9562 version-four and variant bits. The textual
///          result uses lowercase hexadecimal and four hyphens.
/// @return Newly allocated string in the form
///         `xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx`, owned by the caller.
rt_string rt_guid_new(void);

/// @brief Return the nil UUID (all zeros).
/// @return Newly allocated string
///         `00000000-0000-0000-0000-000000000000`, owned by the caller.
rt_string rt_guid_empty(void);

/// @brief Check whether a string has the standard hyphenated UUID syntax.
/// @details Requires exactly 36 bytes, hexadecimal digits in either case, and
///          hyphens at offsets 8, 13, 18, and 23. Version and variant bits are
///          intentionally not validated, so the nil UUID is accepted.
/// @param str Borrowed string to validate; may be null.
/// @return `1` for valid syntax, or `0` for null or malformed input.
int8_t rt_guid_is_valid(rt_string str);

/// @brief Convert GUID string to 16-byte array.
/// @details Parses hexadecimal pairs in RFC network byte order, skipping the
///          four hyphens. Invalid syntax raises a runtime trap.
/// @param str Borrowed standard-format UUID string to convert.
/// @return Newly allocated Bytes object containing exactly 16 bytes, owned by
///         the caller, or null if an invalid-input trap hook returns.
void *rt_guid_to_bytes(rt_string str);

/// @brief Convert 16-byte array to GUID string.
/// @details Formats the bytes in stored order using lowercase hexadecimal and
///          standard hyphen positions. The function does not impose UUID
///          version or variant bits. Any length other than 16 raises a trap.
/// @param bytes Borrowed Bytes object containing exactly 16 bytes.
/// @return Newly allocated 36-byte UUID string owned by the caller, or null if
///         a wrong-length trap hook returns.
rt_string rt_guid_from_bytes(void *bytes);

#ifdef __cplusplus
}
#endif
