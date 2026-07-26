//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/localization/rt_locale_posix_tag.h
// Purpose: Internal validation and normalization helpers for POSIX-style
//          locale environment values used by all locale platform adapters.
//
// Key invariants:
//   - C/POSIX sentinels are recognized case-insensitively before suffixes.
//   - Normalized tags contain only non-empty 1-8 byte ASCII alphanumeric
//     subtags separated by single dashes; the first subtag is alphabetic.
//   - Output is empty on every failure.
//
// Ownership/Lifetime:
//   - Helpers borrow input and write only to caller-owned storage.
//
// Links: src/runtime/localization/rt_locale_platform_posix.c,
//        src/runtime/localization/rt_locale_platform_macos.c,
//        src/runtime/localization/rt_locale_platform_windows.c
//
//===----------------------------------------------------------------------===//
#pragma once

#include <stddef.h>

/// @brief Convert one ASCII uppercase letter to lowercase.
/// @details Values outside `A` through `Z` are returned unchanged. This helper
///          is locale-independent and does not interpret multibyte text.
/// @param ch Byte value to inspect, represented as an integer.
/// @return The corresponding lowercase ASCII value, or @p ch unchanged.
static inline int rt_locale_ascii_lower(int ch) {
    return ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch;
}

/// @brief Test whether a value represents an ASCII alphabetic character.
/// @param ch Byte value to test, represented as an integer.
/// @return Non-zero for `A` through `Z` or `a` through `z`; otherwise zero.
static inline int rt_locale_ascii_is_alpha(int ch) {
    ch = rt_locale_ascii_lower(ch);
    return ch >= 'a' && ch <= 'z';
}

/// @brief Test whether a value represents an ASCII letter or decimal digit.
/// @param ch Byte value to test, represented as an integer.
/// @return Non-zero for an ASCII alphabetic character or `0` through `9`;
///         otherwise zero.
static inline int rt_locale_ascii_is_alnum(int ch) {
    return rt_locale_ascii_is_alpha(ch) || (ch >= '0' && ch <= '9');
}

/// @brief Recognize C and POSIX invariant-locale values before .encoding/@modifier suffixes.
/// @details Treats NULL and empty values as invariant so platform adapters skip
///          unset environment variables. The `C` and `POSIX` spellings are
///          matched case-insensitively up to the first `.` or `@`.
/// @param value NUL-terminated POSIX locale value, or NULL.
/// @return Non-zero if @p value is absent, empty, `C`, or `POSIX` after
///         ignoring an encoding or modifier suffix; otherwise zero.
static inline int rt_locale_posix_value_is_invariant(const char *value) {
    static const char posix[] = "posix";
    size_t len = 0;
    if (!value || !*value)
        return 1;
    while (value[len] && value[len] != '.' && value[len] != '@')
        len++;
    if (len == 1 && rt_locale_ascii_lower((unsigned char)value[0]) == 'c')
        return 1;
    if (len != sizeof(posix) - 1u)
        return 0;
    for (size_t i = 0; i < len; i++) {
        if (rt_locale_ascii_lower((unsigned char)value[i]) != posix[i])
            return 0;
    }
    return 1;
}

/// @brief Normalize one POSIX locale value to a validated near-BCP-47 tag.
/// @details Strips `.encoding` and `@modifier` suffixes, maps underscores to
///          dashes, rejects empty or over-eight-byte subtags, and requires an
///          alphabetic first subtag. Only ASCII alphanumeric subtag bytes are
///          accepted. @p out is cleared before validation and after any error.
/// @param src NUL-terminated POSIX locale value to normalize.
/// @param out Destination buffer for the normalized tag.
/// @param cap Capacity of @p out in bytes, including its terminator.
/// @return 0 on success, -1 on invalid input, malformed subtags, or overflow.
static inline int rt_locale_clean_posix_tag(const char *src, char *out, size_t cap) {
    size_t written = 0;
    size_t subtag_len = 0;
    size_t subtag_index = 0;
    if (out && cap > 0)
        out[0] = '\0';
    if (!src || !out || cap < 2)
        return -1;

    for (const char *p = src; *p && *p != '.' && *p != '@'; ++p) {
        unsigned char byte = (unsigned char)*p;
        char c = byte == '_' ? '-' : (char)byte;
        if (c == '-') {
            if (subtag_len == 0)
                goto fail;
            subtag_index++;
            subtag_len = 0;
        } else {
            if (!rt_locale_ascii_is_alnum(byte) ||
                (subtag_index == 0 && !rt_locale_ascii_is_alpha(byte)) || subtag_len >= 8)
                goto fail;
            subtag_len++;
        }
        if (written + 1 >= cap)
            goto fail;
        out[written++] = c;
    }
    if (written == 0 || subtag_len == 0)
        goto fail;
    out[written] = '\0';
    return 0;

fail:
    out[0] = '\0';
    return -1;
}
