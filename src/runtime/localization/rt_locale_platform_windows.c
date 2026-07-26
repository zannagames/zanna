//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/localization/rt_locale_platform_windows.c
// Purpose: Windows implementation of rt_locale_platform_detect_system. Uses
//          GetUserDefaultLocaleName (Vista+) which already returns a BCP-47
//          tag ("en-US", "zh-Hans-CN") as UTF-16; we just transcode to UTF-8
//          and pass through. Falls back to the LC_ALL, LC_MESSAGES, and LANG
//          environment variables (for MinGW/MSYS POSIX-flavored environments)
//          when the Win32 call returns an empty result.
//
// Key invariants:
//   - Only compiled when RT_PLATFORM_WINDOWS is set.
//   - GetUserDefaultLocaleName output is already BCP-47 per the Windows API
//     contract (LOCALE_NAME_MAX_LENGTH = 85); no underscore-to-dash conversion
//     is needed. Casing is already the canonical form.
//   - UTF-16 -> UTF-8 transcoding is restricted to BCP-47 content which is
//     ASCII by construction; the transcoder is a simple truncation.
//
// Ownership/Lifetime:
//   - Output buffer is caller-owned; adapter writes at most cap-1 bytes.
//
// Links: src/runtime/localization/rt_locale_platform.h (interface).
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Implements Windows user-locale detection with environment fallback.
 * @details Reads the Win32 BCP-47 locale name first, validates its bounded
 * ASCII representation, then snapshots and cleans LC_ALL, LC_MESSAGES, or
 * LANG as strict UTF-8 for MSYS- and MinGW-style environments.
 */

#include "rt_locale_platform.h"
#include "rt_locale_posix_tag.h"
#include "rt_platform.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if RT_PLATFORM_WINDOWS

/** Restrict the Windows SDK surface to the lean core declarations. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/// @brief Snapshot a Windows environment variable as strict UTF-8.
/// @details `getenv()` exposes storage invalidated by concurrent environment
///          mutation. The Win32 copy API gives this adapter an owned snapshot
///          and retries bounded growth races.
/// @param name NUL-terminated UTF-16 environment-variable name. NULL and empty
///             names are rejected.
/// @return Heap-allocated NUL-terminated UTF-8 value, or NULL if the variable
///         is absent, empty, invalid, changes repeatedly, or cannot be copied.
///         The caller owns the returned allocation and must free it.
static char *rt_locale_environment_utf8(const wchar_t *name) {
    DWORD capacity;

    if (!name || !*name)
        return NULL;
    capacity = GetEnvironmentVariableW(name, NULL, 0);
    if (capacity == 0)
        return NULL;
    for (int attempt = 0; attempt < 8; ++attempt) {
        wchar_t *wide;
        DWORD length;
        int bytes;
        char *utf8;

        if ((size_t)capacity > SIZE_MAX / sizeof(wchar_t))
            return NULL;
        wide = (wchar_t *)malloc((size_t)capacity * sizeof(wchar_t));
        if (!wide)
            return NULL;
        length = GetEnvironmentVariableW(name, wide, capacity);
        if (length == 0 || length >= capacity) {
            free(wide);
            if (length == 0 || length == MAXDWORD)
                return NULL;
            capacity = length + 1u;
            continue;
        }
        if (length > INT_MAX) {
            free(wide);
            return NULL;
        }
        bytes = WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, wide, (int)length, NULL, 0, NULL, NULL);
        if (bytes <= 0) {
            free(wide);
            return NULL;
        }
        utf8 = (char *)malloc((size_t)bytes + 1u);
        if (!utf8) {
            free(wide);
            return NULL;
        }
        if (WideCharToMultiByte(
                CP_UTF8, WC_ERR_INVALID_CHARS, wide, (int)length, utf8, bytes, NULL, NULL) !=
            bytes) {
            free(utf8);
            free(wide);
            return NULL;
        }
        free(wide);
        utf8[bytes] = '\0';
        return utf8;
    }
    return NULL;
}

/// @brief Detect the Windows system locale and write a BCP-47 tag to @p out.
/// @details Primary path: calls `GetUserDefaultLocaleName` (Vista+) which returns a
///          BCP-47 tag as UTF-16. Since BCP-47 is ASCII by construction, the
///          UTF-16 → UTF-8 transcoding is a safe byte truncation. Rejects any
///          non-ASCII wide character as a corrupt locale indicator.
///          Fallback path: when the Win32 call returns empty (MSYS/MinGW environments),
///          polls `LC_ALL`, `LC_MESSAGES`, and `LANG` env vars and applies the
///          POSIX cleanup path.
/// @param out  Caller-provided buffer to receive the BCP-47 tag.
/// @param cap  Capacity of @p out in bytes (must be ≥ 2).
/// @return 0 if a usable tag was written to @p out; -1 if detection failed.
int rt_locale_platform_detect_system(char *out, size_t cap) {
    if (out && cap > 0)
        out[0] = '\0';
    if (!out || cap < 2)
        return -1;

    // Try Win32 first. LOCALE_NAME_MAX_LENGTH is 85 (wide chars) which is far
    // larger than any real BCP-47 tag, so we size the local buffer to match.
    wchar_t wbuf[LOCALE_NAME_MAX_LENGTH];
    int n = GetUserDefaultLocaleName(wbuf, LOCALE_NAME_MAX_LENGTH);
    if (n > 1) {
        // n includes the terminator. BCP-47 tags are ASCII so the UTF-16 ->
        // UTF-8 transcoding is a simple byte truncation for anything valid.
        // Any non-ASCII byte here would indicate a corrupt locale; reject to
        // keep downstream parsing simple.
        size_t need = (size_t)(n - 1);
        if (need + 1 > cap)
            return -1;
        for (int i = 0; i < n - 1; ++i) {
            wchar_t wc = wbuf[i];
            if (wc > 0x7F)
                return -1;
        }
        for (int i = 0; i < n - 1; ++i)
            out[i] = (char)wbuf[i];
        out[need] = '\0';
        return 0;
    }

    // Fallback: MSYS/MinGW environments commonly set LANG. Reuse the POSIX
    // cleanup path for that case.
    static const char *const kVars[] = {"LC_ALL", "LC_MESSAGES", "LANG", NULL};
    static const wchar_t *const kWideVars[] = {L"LC_ALL", L"LC_MESSAGES", L"LANG", NULL};
    for (size_t i = 0; kVars[i]; ++i) {
        char *val = rt_locale_environment_utf8(kWideVars[i]);
        if (rt_locale_posix_value_is_invariant(val)) {
            free(val);
            continue;
        }
        if (rt_locale_clean_posix_tag(val, out, cap) == 0) {
            free(val);
            return 0;
        }
        free(val);
    }
    return -1;
}

#endif // RT_PLATFORM_WINDOWS
