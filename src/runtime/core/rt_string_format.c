//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/core/rt_string_format.c
// Purpose: Implements strict INPUT-style numeric parsing, permissive BASIC VAL
//          prefix parsing, and allocation of runtime strings for integer and
//          floating-point values.
//
// Key invariants:
//   - Strict integer/double entry points reject embedded NUL and require the
//     complete ASCII-trimmed input to satisfy their numeric grammar.
//   - Strict floating parsing and all formatting are C-locale stable; VAL calls
//     process-locale strtod directly and intentionally accepts a prefix.
//   - Integer ERANGE and strict-double overflow trap. VAL traps only when
//     ERANGE produces a non-finite value.
//   - Formatting preserves the declared source precision: Float32 conversion
//     narrows before formatting, while Float64 uses round-trip text.
//
// Ownership/Lifetime:
//   - String-returning functions transfer one owned reference, potentially the
//     shared empty singleton on a returning error path.
//   - Intermediate scratch buffers are stack-allocated or freed before return.
//   - Parser inputs are borrowed; temporary integer parsing storage is obtained
//     through rt_alloc and released through rt_free.
//
// Links: src/runtime/core/rt_string.h (rt_string type),
//        src/runtime/core/rt_format.c (double formatting helpers),
//        src/runtime/core/rt_string_builder.c (growable buffer),
//        docs/runtime/strings.md#numeric-formatting
//
//===----------------------------------------------------------------------===//
/// @file
/// @brief Runtime numeric parsing and numeric-to-string formatting.

#include "rt_format.h"
#include "rt_int_format.h"
#include "rt_internal.h"
#include "rt_numeric.h"
#include "rt_string.h"
#include "rt_string_builder.h"
#include "rt_string_internal.h"

#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/// @brief Validate @p s and borrow its length-counted byte storage.
/// @details Initializes an optional output length to zero, then verifies the
///          non-null handle through the live string registry. Null input traps
///          with @p fn_name; invalid handles and data use INPUT diagnostics. A
///          returning trap yields the static empty C string and zero length.
/// @param s Borrowed runtime string expected to be live and non-null.
/// @param len Optional output receiving the stored byte length on success.
/// @param fn_name Borrowed diagnostic used for a null-input trap.
/// @return Borrowed internal byte buffer, or the static empty C string after a
///         returning validation trap.
static const char *rt_string_format_bytes(rt_string s, size_t *len, const char *fn_name) {
    if (len)
        *len = 0;
    if (!s) {
        rt_trap(fn_name);
        return "";
    }
    if (!rt_string_is_handle(s)) {
        rt_trap("INPUT: invalid string handle");
        return "";
    }
    size_t n = rt_string_len_bytes(s);
    const char *data = rt_string_cstr(s);
    if (!data) {
        rt_trap("INPUT: invalid string data");
        return "";
    }
    if (len)
        *len = n;
    return data;
}

/// @brief Test whether @p s contains NUL within its stored byte length.
/// @details Validates non-null inputs through
///          @ref rt_string_format_bytes. NULL itself returns false without
///          trapping so the strict caller can issue its own null diagnostic.
/// @param s Borrowed runtime string; may be NULL.
/// @return True when a stored payload byte is NUL; otherwise false.
static bool rt_string_contains_embedded_nul(rt_string s) {
    if (!s)
        return false;
    size_t len = 0;
    const char *data = rt_string_format_bytes(s, &len, "rt_to_double: null");
    return data && memchr(data, '\0', len) != NULL;
}

/// @brief Locale-independent test for ASCII whitespace characters.
/// @details Avoids host `LC_CTYPE` state and recognizes exactly space, tab,
///          newline, carriage return, form feed, and vertical tab.
/// @param ch Unsigned byte to classify.
/// @return True only for one of the six accepted ASCII whitespace bytes.
static bool rt_string_format_is_ascii_space(unsigned char ch) {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v';
}

/// @brief Parse a runtime string as a signed 64-bit integer.
/// @details Trims the six ASCII whitespace bytes, rejects empty input and
///          embedded NUL, copies the remaining span to terminated scratch
///          storage, and invokes base-10 `strtoll`. The conversion must consume
///          the complete trimmed span. Size, allocation, syntax, and ERANGE
///          failures trap with INPUT diagnostics.
/// @param s Borrowed live runtime string containing signed decimal text.
/// @return Parsed value, or zero after a returning validation/parse trap.
int64_t rt_to_int(rt_string s) {
    size_t len = 0;
    const char *p = rt_string_format_bytes(s, &len, "rt_to_int: null");
    size_t i = 0;
    while (i < len && rt_string_format_is_ascii_space((unsigned char)p[i]))
        ++i;
    size_t j = len;
    while (j > i && rt_string_format_is_ascii_space((unsigned char)p[j - 1]))
        --j;
    if (i == j) {
        rt_trap("INPUT: expected numeric value");
        return 0;
    }
    size_t sz = j - i;
    if (memchr(p + i, '\0', sz)) {
        rt_trap("INPUT: expected numeric value");
        return 0;
    }
    if (sz == SIZE_MAX || sz > (size_t)INT64_MAX - 1) {
        rt_trap("INPUT: numeric value too large");
        return 0;
    }
    char *buf = (char *)rt_alloc((int64_t)(sz + 1));
    if (!buf) {
        rt_trap("INPUT: allocation failed");
        return 0;
    }
    memcpy(buf, p + i, sz);
    buf[sz] = '\0';
    errno = 0;
    char *endp = NULL;
    long long v = strtoll(buf, &endp, 10);
    if (errno == ERANGE) {
        rt_free(buf);
        rt_trap("INPUT: numeric overflow");
        return 0;
    }
    if (!endp || *endp != '\0') {
        rt_free(buf);
        rt_trap("INPUT: expected numeric value");
        return 0;
    }
    rt_free(buf);
    return (int64_t)v;
}

/// @brief Parse a runtime string into a double.
/// @details Rejects embedded NUL before delegating to the C-locale strict
///          parser, which trims accepted ASCII whitespace and requires complete
///          consumption of a decimal or supported signed non-finite token.
///          Overflow receives a dedicated INPUT trap; every other parse error
///          uses the expected-numeric diagnostic.
/// @param s Borrowed live runtime string containing floating-point text.
/// @return Parsed value, or `0.0` after a returning validation/parse trap.
double rt_to_double(rt_string s) {
    if (rt_string_contains_embedded_nul(s)) {
        rt_trap("INPUT: expected numeric value");
        return 0.0;
    }
    double value = 0.0;
    size_t len = 0;
    const char *data = rt_string_format_bytes(s, &len, "rt_to_double: null");
    (void)len;
    int32_t err = rt_parse_double(data, &value);
    if (err == (int32_t)Err_Overflow) {
        rt_trap("INPUT: numeric overflow");
        return 0.0;
    }
    if (err != (int32_t)Err_None) {
        rt_trap("INPUT: expected numeric value");
        return 0.0;
    }
    return value;
}

/// @brief Format a signed 64-bit integer into a newly allocated runtime string.
/// @details Appends locale-independent base-10 text to an inline-first
///          @ref rt_string_builder, copies its bytes into immutable string
///          storage, and frees builder storage. Builder failure maps to a
///          context-specific trap and uses an owned empty-string fallback when
///          the hook returns.
/// @param v Integer value to format.
/// @return Owned decimal string, empty fallback, or `NULL` if final string
///         allocation fails.
rt_string rt_int_to_str(int64_t v) {
    rt_string_builder sb;
    rt_sb_init(&sb);
    rt_sb_status_t status = rt_sb_append_int(&sb, v);
    if (status != RT_SB_OK) {
        const char *msg = "rt_int_to_str: format";
        if (status == RT_SB_ERROR_ALLOC)
            msg = "rt_int_to_str: alloc";
        else if (status == RT_SB_ERROR_OVERFLOW)
            msg = "rt_int_to_str: overflow";
        else if (status == RT_SB_ERROR_INVALID)
            msg = "rt_int_to_str: invalid";
        rt_sb_free(&sb);
        rt_trap(msg);
        return rt_string_from_bytes("", 0);
    }

    rt_string s = rt_string_from_bytes(sb.data, sb.len);
    rt_sb_free(&sb);
    return s;
}

/// @brief Convert a double to a runtime string using exact round-trip formatting.
/// @details Relies on @ref rt_format_f64_roundtrip to produce locale-stable
///          text for finite values and the runtime's canonical spellings for
///          non-finite values, then copies through the generated terminator.
/// @param v Floating-point value to format.
/// @return Owned formatted runtime string, or `NULL` on allocation failure.
rt_string rt_f64_to_str(double v) {
    char buf[64];
    rt_format_f64_roundtrip(v, buf, sizeof(buf));
    return rt_string_from_bytes(buf, strlen(buf));
}

/// @brief Legacy Float64 allocation entry point.
/// @details Retained for ABI compatibility and independently applies the same
///          round-trip formatter and copy sequence as @ref rt_f64_to_str.
/// @param v Floating-point value to format.
/// @return Owned formatted runtime string, or `NULL` on allocation failure.
rt_string rt_str_d_alloc(double v) {
    char buf[64];
    rt_format_f64_roundtrip(v, buf, sizeof(buf));
    return rt_string_from_bytes(buf, strlen(buf));
}

/// @brief Format a value with single-precision (Float32) semantics.
/// @details Accepts a C `double` so the exported ABI matches the registered
///          `str(f64)` dispatch signature. It narrows to `float`, promotes that
///          rounded value back to double, and applies ordinary runtime float
///          formatting rather than Float64 round-trip formatting.
/// @param v Value to format (narrowed to single precision).
/// @return Owned formatted runtime string, or `NULL` on allocation failure.
rt_string rt_str_f_alloc(double v) {
    char buf[64];
    rt_format_f64((double)(float)v, buf, sizeof(buf));
    return rt_string_from_bytes(buf, strlen(buf));
}

/// @brief Format a 32-bit integer into a runtime string.
/// @details Writes minimal signed base-10 text into a fixed stack buffer with
///          @ref rt_str_from_i32, then copies through its terminating NUL.
/// @param v Integer value to format.
/// @return Owned decimal runtime string, or `NULL` on allocation failure.
rt_string rt_str_i32_alloc(int32_t v) {
    char buf[32];
    rt_str_from_i32(v, buf, sizeof(buf), NULL);
    return rt_string_from_bytes(buf, strlen(buf));
}

/// @brief Format a 16-bit integer into a runtime string.
/// @details Writes minimal signed base-10 text into a fixed stack buffer with
///          @ref rt_str_from_i16, then copies through its terminating NUL.
/// @param v Integer value to format.
/// @return Owned decimal runtime string, or `NULL` on allocation failure.
rt_string rt_str_i16_alloc(int16_t v) {
    char buf[16];
    rt_str_from_i16(v, buf, sizeof(buf), NULL);
    return rt_string_from_bytes(buf, strlen(buf));
}

/// @brief Parse a runtime string using BASIC's `VAL` semantics.
/// @details Skips the six ASCII whitespace bytes, then calls process-locale
///          `strtod` and accepts its longest numeric prefix. Trailing bytes are
///          ignored, and an embedded NUL terminates the visible input. Null
///          handles trap; empty/no-prefix input returns zero. ERANGE traps only
///          when the parsed result is non-finite, while underflow/subnormal
///          results are returned.
/// @param s Borrowed runtime string; must be non-null and valid.
/// @return Parsed prefix, or `0.0` when no numeric prefix exists.
double rt_val(rt_string s) {
    if (!s) {
        rt_trap("rt_val: null");
        return 0.0;
    }
    const char *data = s->data;
    if (!data)
        return 0.0;
    // BASIC VAL semantics: skip leading ASCII whitespace, parse the longest
    // numeric prefix via process-locale strtod, and ignore trailing garbage.
    while (*data == ' ' || *data == '\t' || *data == '\n' || *data == '\r' || *data == '\v' ||
           *data == '\f')
        ++data;
    if (!*data)
        return 0.0;
    errno = 0;
    char *end = NULL;
    double value = strtod(data, &end);
    if (end == data)
        return 0.0; // no numeric prefix at all
    if (errno == ERANGE && !isfinite(value))
        rt_trap("rt_val: overflow");
    return value;
}

/// @brief Convenience wrapper mirroring the historic `STR$` intrinsic.
/// @details Forwards to @ref rt_f64_to_str so the intrinsic reuses the same
///          Float64 round-trip, locale, and non-finite formatting behavior.
/// @param v Floating-point value to format.
/// @return Owned formatted runtime string, or `NULL` on allocation failure.
rt_string rt_str(double v) {
    return rt_f64_to_str(v);
}
