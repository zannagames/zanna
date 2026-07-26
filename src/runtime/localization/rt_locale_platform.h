//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/localization/rt_locale_platform.h
// Purpose: Platform adapter interface exposing a single detection function that
//          returns the host system's user-preferred locale in a near-BCP-47
//          shape (underscores normalized to dashes, encoding/modifier suffixes
//          stripped). Final canonicalization (case folding, script/region
//          casing) is handled by Locale.Parse, not the adapter.
//
// Key invariants:
//   - Exactly one platform adapter translation unit is compiled per build
//     (selected by the CMake conditional on WIN32/APPLE/UNIX).
//   - The adapter never traps; detection failure is reported via return
//     value so LocaleManager can fall back to the baked invariant locale.
//   - Output buffer is NUL-terminated on success and cleared on failure when
//     the caller supplies at least one byte of writable storage.
//
// Ownership/Lifetime:
//   - Caller owns the output buffer; the adapter writes at most `cap - 1`
//     bytes of tag content plus a terminator.
//
// Links: src/runtime/localization/rt_locale_platform_posix.c,
//        src/runtime/localization/rt_locale_platform_windows.c,
//        src/runtime/localization/rt_locale_platform_macos.c,
//        src/runtime/localization/rt_locale_manager.c (sole consumer).
//
//===----------------------------------------------------------------------===//
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Detect the host system's preferred locale.
/// @details Writes a best-effort BCP-47 tag into @p out. Encoding suffixes
///          (`.UTF-8`) and POSIX modifiers (`@latin`) are stripped; underscores
///          are normalized to dashes. The returned string is not guaranteed to
///          be fully canonical — language case and region case may still need
///          normalization by the caller. The platform-specific fallback chain
///          is: Windows uses GetUserDefaultLocaleName; macOS and POSIX walk
///          `$LC_ALL` -> `$LC_MESSAGES` -> `$LANG`.
/// @param out Destination buffer. NUL-terminated on success.
/// @param cap Total capacity of @p out in bytes, including space for the
///            terminator. Values below 2 are rejected; the complete detected
///            tag and terminator must fit.
/// @return 0 on success; -1 if no system locale could be determined or @p out
///         is NULL / @p cap is too small.
int rt_locale_platform_detect_system(char *out, size_t cap);

#ifdef __cplusplus
}
#endif
