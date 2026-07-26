//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/localization/rt_locale_platform_macos.c
// Purpose: macOS implementation of rt_locale_platform_detect_system. Uses the
//          same POSIX env var cascade as Linux (not CoreFoundation) so the
//          localization module stays independent of Foundation.framework. v0.2.5
//          removed Security.framework for the same reason; this preserves the
//          zero-framework story for the locale path as well.
//
// Key invariants:
//   - Only compiled when RT_PLATFORM_MACOS is set.
//   - No CoreFoundation / Foundation dependency. Users who need the
//     preferredLanguages array can set $LANG in their launch environment.
//
// Ownership/Lifetime:
//   - Output buffer is caller-owned; adapter writes at most cap-1 bytes.
//
// Links: src/runtime/localization/rt_locale_platform.h (interface).
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Implements framework-free macOS locale detection from POSIX settings.
 * @details Applies environment precedence, rejects invariant sentinels, and
 * normalizes the first usable locale value into the caller's bounded buffer.
 */

#include "rt_locale_platform.h"
#include "rt_locale_posix_tag.h"
#include "rt_platform.h"

#include <stddef.h>
#include <stdlib.h>

#if RT_PLATFORM_MACOS

/// @brief Detect the macOS system locale via the POSIX environment variable cascade.
/// @details Polls `LC_ALL`, then `LC_MESSAGES`, then `LANG` in that order of
///          precedence. Skips values that are C/POSIX sentinels. Cleans each
///          candidate with `rt_locale_clean_posix_tag` to produce a BCP-47-compatible tag.
///          Uses env vars rather than CoreFoundation to avoid a framework dependency.
/// @param out  Caller-provided buffer to receive the BCP-47 tag.
/// @param cap  Capacity of @p out in bytes (must be ≥ 2).
/// @return 0 if a usable tag was written to @p out; -1 if detection failed.
int rt_locale_platform_detect_system(char *out, size_t cap) {
    if (out && cap > 0)
        out[0] = '\0';
    if (!out || cap < 2)
        return -1;

    // macOS does honor POSIX env vars when launched from a terminal or shell,
    // and most developers set them. GUI-launched apps get "C" unless the user
    // explicitly sets them; in that case we fall back to the invariant locale,
    // which is honest: we don't have Foundation available to ask for
    // preferredLanguages.
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

#endif // RT_PLATFORM_MACOS
