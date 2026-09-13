//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/services/rt_services_dynlib_win.c
// Purpose: Windows adapter for explicit-path provider library loading.
// Key invariants:
//   - UTF-8 paths are converted to UTF-16 and passed to LoadLibraryW
//     unchanged, so a full path never triggers the DLL search order.
//   - Module handles are never passed to FreeLibrary (see rt_services_dynlib.h).
// Ownership/Lifetime:
//   - The Windows loader owns module handles for the process lifetime.
//   - Temporary UTF-16 conversions are heap-allocated and freed before return.
// Links: src/runtime/services/rt_services_dynlib.h
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_services_dynlib_win.c
 * @brief Implements provider library loading with LoadLibraryW and GetProcAddress.
 * @details Loader failures report the Win32 error code, which distinguishes a
 *          missing dependency (126), a bad image or architecture (193), and
 *          similar conditions without requiring message-table lookups.
 */

#include "rt_services_dynlib.h"

#include "rt_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

/// @brief Convert a UTF-8 path to a heap-allocated UTF-16 string with backslash separators.
/// @details LoadLibraryW only treats a name as a path reliably when it uses
///          backslashes, so forward slashes (common in CMake and
///          cross-platform configuration) are rewritten.
/// @param utf8_path NUL-terminated UTF-8 input.
/// @return Owned wide string the caller frees, or NULL on invalid input or allocation failure.
static wchar_t *dynlib_utf8_to_wide(const char *utf8_path) {
    int wide_len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8_path, -1, NULL, 0);
    if (wide_len <= 0)
        return NULL;
    wchar_t *wide = (wchar_t *)malloc((size_t)wide_len * sizeof(wchar_t));
    if (!wide)
        return NULL;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8_path, -1, wide, wide_len) !=
        wide_len) {
        free(wide);
        return NULL;
    }
    for (int i = 0; i < wide_len; ++i) {
        if (wide[i] == L'/')
            wide[i] = L'\\';
    }
    return wide;
}

/// @brief Report whether a path names an existing file system entry.
/// @param utf8_path NUL-terminated UTF-8 path; NULL or empty reports 0.
/// @return 1 when GetFileAttributesW finds the entry, otherwise 0.
int8_t rt_services_dynlib_file_exists(const char *utf8_path) {
    if (!utf8_path || !*utf8_path)
        return 0;
    wchar_t *wide = dynlib_utf8_to_wide(utf8_path);
    if (!wide)
        return 0;
    DWORD attributes = GetFileAttributesW(wide);
    free(wide);
    return attributes != INVALID_FILE_ATTRIBUTES ? 1 : 0;
}

/// @brief Open a DLL by exact path with LoadLibraryW.
/// @param utf8_path NUL-terminated UTF-8 DLL path.
/// @param error Buffer receiving the failure reason; may be NULL.
/// @param error_capacity Size of @p error in bytes.
/// @return HMODULE as an opaque pointer, or NULL on failure.
void *rt_services_dynlib_open(const char *utf8_path, char *error, size_t error_capacity) {
    if (error && error_capacity > 0)
        error[0] = '\0';
    if (!utf8_path || !*utf8_path) {
        if (error && error_capacity > 0)
            snprintf(error, error_capacity, "empty library path");
        return NULL;
    }
    wchar_t *wide = dynlib_utf8_to_wide(utf8_path);
    if (!wide) {
        if (error && error_capacity > 0)
            snprintf(error, error_capacity, "path is not valid UTF-8");
        return NULL;
    }
    HMODULE module = LoadLibraryW(wide);
    DWORD last_error = module ? 0 : GetLastError();
    free(wide);
    if (!module && error && error_capacity > 0)
        snprintf(error,
                 error_capacity,
                 "LoadLibraryW failed with Win32 error %lu",
                 (unsigned long)last_error);
    return (void *)module;
}

/// @brief Resolve an exported symbol with GetProcAddress.
/// @param library HMODULE returned by @ref rt_services_dynlib_open.
/// @param name Exported symbol name.
/// @return Symbol address, or NULL when absent.
void *rt_services_dynlib_symbol(void *library, const char *name) {
    if (!library || !name || !*name)
        return NULL;
    FARPROC proc = GetProcAddress((HMODULE)library, name);
    return proc ? RT_FN_PTR_CAST((void *)proc) : NULL;
}
