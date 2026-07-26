//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/localization/rt_locale_platform_posix.c
// Purpose: POSIX (Linux, BSD) implementation of rt_locale_platform_detect_system.
//          Walks the standard POSIX env var cascade ($LC_ALL -> $LC_MESSAGES ->
//          $LANG) and cleans the result into a near-BCP-47 shape by
//          stripping encoding suffixes (.UTF-8) and modifier suffixes
//          (@latin), then normalizing underscores to dashes.
//
// Key invariants:
//   - Only compiled when the build target is not Windows and not APPLE
//     (Linux, FreeBSD, OpenBSD, NetBSD, etc.).
//   - "C" and "POSIX" env values are treated as detection failure so the
//     caller falls back to the invariant locale rather than trying to
//     format numbers with C-locale separators.
//   - No allocation; works entirely with the caller's output buffer and the
//     process environment block.
//
// Ownership/Lifetime:
//   - Output buffer is caller-owned; adapter writes at most cap-1 bytes.
//
// Links: src/runtime/localization/rt_locale_platform.h (interface).
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Implements POSIX environment-based host-locale detection.
 * @details Walks LC_ALL, LC_MESSAGES, and LANG in precedence order, ignores
 * C/POSIX invariant values, cleans encoding and modifier suffixes, and emits
 * an inert alternate symbol when this adapter is not selected.
 */

#include "rt_locale_platform.h"
#include "rt_locale_posix_tag.h"
#include "rt_platform.h"

#include <stddef.h>
#include <stdlib.h>

#if !RT_PLATFORM_WINDOWS && !RT_PLATFORM_MACOS

/// @brief Detect the system locale on POSIX platforms via the standard env var cascade.
/// @details Polls `LC_ALL`, then `LC_MESSAGES`, then `LANG` per IEEE Std 1003.1.
///          LC_ALL and LC_MESSAGES are preferred over LC_NUMERIC/LC_COLLATE because
///          the message/display locale is what users expect as "the system locale".
///          Skips C/POSIX sentinels and cleans each candidate with
///          `rt_locale_clean_posix_tag`.
/// @param out  Caller-provided buffer to receive the BCP-47 tag.
/// @param cap  Capacity of @p out in bytes (must be ≥ 2).
/// @return 0 if a usable tag was written to @p out; -1 if detection failed.
int rt_locale_platform_detect_system(char *out, size_t cap) {
    if (out && cap > 0)
        out[0] = '\0';
    if (!out || cap < 2)
        return -1;

    // POSIX precedence per IEEE Std 1003.1: LC_ALL overrides category vars,
    // which override LANG. We poll LC_ALL and LC_MESSAGES specifically because
    // message / display locale is what users generally mean by "the system
    // locale"; LC_NUMERIC or LC_COLLATE could legitimately differ.
    static const char *const kVars[] = {"LC_ALL", "LC_MESSAGES", "LANG", NULL};
    for (size_t i = 0; kVars[i]; ++i) {
        const char *val = getenv(kVars[i]);
        if (rt_locale_posix_value_is_invariant(val))
            continue;
        if (rt_locale_clean_posix_tag(val, out, cap) == 0)
            return 0;
    }
    return -1;
}

#else

/// @brief Provide an inert symbol when the POSIX adapter is built for another platform.
/// @details Avoids defining the public adapter symbol alongside the selected
///          Windows or macOS implementation. This function is not part of the
///          localization interface and always reports failure.
/// @param out Unused prospective destination buffer.
/// @param cap Unused prospective buffer capacity.
/// @return Always -1.
int rt_locale_platform_detect_system_posix_unused_(char *out, size_t cap) {
    (void)out;
    (void)cap;
    return -1;
}

#endif // !RT_PLATFORM_WINDOWS && !RT_PLATFORM_MACOS
