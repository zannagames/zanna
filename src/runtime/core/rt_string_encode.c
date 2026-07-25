//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/core/rt_string_encode.c
// Purpose: Implements the boundary between length-counted runtime byte strings,
//          individual byte values, and conventional NUL-terminated C strings.
//
// Key invariants:
//   - CHR$ accepts byte codes in [0, 255]; values outside this range trap with
//     a diagnostic containing the offending decimal value.
//   - CHR$(0) is a one-byte runtime string containing an embedded NUL, not the
//     zero-length empty singleton.
//   - ASC returns 0 for empty strings (matching legacy BASIC semantics) and
//     traps on null or invalid handles.
//   - Borrowed C string views (rt_string_cstr) must not be mutated or freed by
//     callers and expose only the prefix before an embedded NUL to C consumers.
//   - rt_const_cstr copies only through the first C terminator and deliberately
//     returns NULL for a null source without trapping.
//
// Ownership/Lifetime:
//   - String-returning functions transfer one owned reference, which may be
//     newly allocated or the shared immortal empty singleton.
//   - rt_string_cstr returns a pointer into the string's internal buffer;
//     the pointer is valid only while the rt_string is alive.
//   - No heap allocation is performed by read-only accessors (ASC, cstr).
//
// Links: src/runtime/core/rt_string.h (rt_string type and allocation API),
//        src/runtime/core/rt_string_ops.c (higher-level string operations),
//        src/runtime/core/rt_int_format.c (integer formatting for diagnostics)
//
//===----------------------------------------------------------------------===//
/// @file
/// @brief Runtime byte-value and NUL-terminated C-string conversion helpers.

#include "rt_int_format.h"
#include "rt_internal.h"
#include "rt_string.h"
#include "rt_string_internal.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/// @brief Construct a runtime string containing a single byte value.
/// @details Validates that @p code falls within the 0–255 range, formats an
///          informative trap message for out-of-range values, and copies the
///          low byte into length-counted string storage. Code zero therefore
///          produces a length-one string containing an embedded NUL. If an
///          out-of-range trap hook returns, the empty singleton is used.
/// @param code Integer byte value in the inclusive range 0 through 255.
/// @return Owned one-byte runtime string, empty fallback after a returning
///         range trap, or `NULL` after allocation/singleton failure.
rt_string rt_str_chr(int64_t code) {
    if (code < 0 || code > 255) {
        char buf[64];
        char numbuf[32];
        rt_i64_to_cstr(code, numbuf, sizeof(numbuf));
        snprintf(buf, sizeof(buf), "CHR$: code must be 0-255 (got %s)", numbuf);
        rt_trap(buf);
        return rt_str_empty();
    }
    char ch = (char)(unsigned char)code;
    return rt_string_from_bytes(&ch, 1);
}

/// @brief Extract the first byte of a runtime string as an integer.
/// @details Validates the live handle through the global string registry before
///          reading storage. The byte is promoted as unsigned, so the result is
///          always 0–255. Empty strings return zero, as does a returning trap
///          for null or invalid input.
/// @param s Borrowed runtime string handle; must be live and non-null.
/// @return Unsigned value of the first stored byte, or zero for empty/fallback.
int64_t rt_str_asc(rt_string s) {
    if (!s) {
        rt_trap("rt_str_asc: null");
        return 0;
    }
    if (!rt_string_is_handle((void *)s)) {
        rt_trap("rt_str_asc: invalid string handle");
        return 0;
    }
    size_t len = (size_t)rt_str_len(s);
    if (len == 0 || !s->data)
        return 0;
    return (int64_t)(unsigned char)s->data[0];
}

/// @brief Borrow a @c const char* view of a runtime-managed string.
/// @details Validates the live handle and backing pointer, then exposes its
///          existing trailing-NUL-terminated buffer without allocation.
///          Embedded NUL bytes remain in storage and make ordinary C APIs see
///          only a prefix. The view becomes invalid when the handle is released
///          or its storage otherwise ceases to be live.
/// @param s Borrowed runtime string handle; must be live and non-null.
/// @return Borrowed internal byte buffer, or the static empty C string after a
///         returning validation trap.
const char *rt_string_cstr(rt_string s) {
    if (!s) {
        rt_trap("rt_string_cstr: null string");
        return "";
    }
    if (!rt_string_is_handle((void *)s)) {
        rt_trap("rt_string_cstr: invalid string handle");
        return "";
    }
    if (!s->data) {
        rt_trap("rt_string_cstr: null data");
        return "";
    }
    return s->data;
}

/// @brief Copy a conventional C string into a runtime string.
/// @details Measures through the first NUL with `strlen`, so bytes after an
///          embedded terminator cannot be represented through this entry
///          point. Empty input uses the shared empty singleton. The source is
///          borrowed only for the duration of the copy.
/// @param c Borrowed NUL-terminated source; may be NULL.
/// @return Owned copied string or empty singleton, `NULL` for a null source,
///         or `NULL` on allocation/singleton failure.
rt_string rt_const_cstr(const char *c) {
    if (!c)
        return NULL;
    size_t len = strlen(c);
    if (len == 0)
        return rt_str_empty();
    return rt_string_from_bytes(c, len);
}
