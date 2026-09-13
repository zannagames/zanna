//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/services/rt_services_dynlib_posix.c
// Purpose: macOS and Linux adapter for explicit-path provider library loading.
// Key invariants:
//   - dlopen always receives the caller's exact path with RTLD_NOW|RTLD_LOCAL,
//     so missing symbols fail at load time and provider exports never leak into
//     the global symbol namespace.
//   - Handles are never passed to dlclose (see rt_services_dynlib.h).
// Ownership/Lifetime:
//   - The dynamic loader owns library handles for the process lifetime.
// Links: src/runtime/services/rt_services_dynlib.h
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_services_dynlib_posix.c
 * @brief Implements provider library loading with dlopen and dlsym.
 * @details Error text comes from dlerror, which already names the path and the
 *          loader's reason (missing dependency, wrong architecture, invalid
 *          code signature, and so on).
 */

#include "rt_services_dynlib.h"

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/// @brief Report whether a path names an existing file system entry.
/// @param utf8_path NUL-terminated path; NULL or empty reports 0.
/// @return 1 when access(F_OK) succeeds, otherwise 0.
int8_t rt_services_dynlib_file_exists(const char *utf8_path) {
    if (!utf8_path || !*utf8_path)
        return 0;
    return access(utf8_path, F_OK) == 0 ? 1 : 0;
}

/// @brief Open a shared library by exact path with dlopen.
/// @param utf8_path NUL-terminated library path.
/// @param error Buffer receiving dlerror text on failure; may be NULL.
/// @param error_capacity Size of @p error in bytes.
/// @return dlopen handle, or NULL on failure.
void *rt_services_dynlib_open(const char *utf8_path, char *error, size_t error_capacity) {
    if (error && error_capacity > 0)
        error[0] = '\0';
    if (!utf8_path || !*utf8_path) {
        if (error && error_capacity > 0)
            snprintf(error, error_capacity, "empty library path");
        return NULL;
    }
    void *library = dlopen(utf8_path, RTLD_NOW | RTLD_LOCAL);
    if (!library && error && error_capacity > 0) {
        const char *reason = dlerror();
        snprintf(error, error_capacity, "%s", reason && *reason ? reason : "dlopen failed");
    }
    return library;
}

/// @brief Resolve an exported symbol with dlsym.
/// @param library Handle returned by @ref rt_services_dynlib_open.
/// @param name Exported symbol name.
/// @return Symbol address, or NULL when absent.
void *rt_services_dynlib_symbol(void *library, const char *name) {
    if (!library || !name || !*name)
        return NULL;
    return dlsym(library, name);
}
