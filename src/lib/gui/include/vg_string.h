//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: lib/gui/include/vg_string.h
// Purpose: Small portable string helpers shared by the GUI implementation.
// Key invariants:
//   - Helpers use only the C standard library so GUI sources do not rely on
//     POSIX-only allocation routines such as strdup().
//   - Returned strings are heap-owned by the caller and must be released with
//     free().
// Ownership/Lifetime:
//   - Every successful vg_strdup() result is a new allocation owned by the caller.
// Links: lib/gui/include/vg_widget.h,
//        lib/gui/include/vg_theme.h
//
//===----------------------------------------------------------------------===//
#pragma once

#include <stdlib.h>
#include <string.h>

/// @file
/// @brief Declares small standard-C string helpers used by GUI implementation files.
/// @details The header avoids platform-specific allocation routines while keeping ownership
/// explicit: every successful duplicate is an independent heap allocation returned to the caller.

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Duplicate a NUL-terminated string using standard C allocation.
/// @details This is the GUI-local replacement for POSIX strdup(). Passing NULL
///          returns NULL. On success the returned buffer contains exactly
///          strlen(text) bytes plus the trailing NUL and must be freed with
///          free().
/// @param text Source string to duplicate; may be NULL.
/// @return Newly allocated copy of @p text, or NULL on allocation failure/NULL input.
static inline char *vg_strdup(const char *text) {
    if (!text)
        return NULL;
    size_t len = strlen(text);
    char *copy = (char *)malloc(len + 1u);
    if (!copy)
        return NULL;
    memcpy(copy, text, len + 1u);
    return copy;
}

#ifdef __cplusplus
}
#endif
