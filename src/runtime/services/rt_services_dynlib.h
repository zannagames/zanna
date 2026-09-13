//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/services/rt_services_dynlib.h
// Purpose: Internal platform adapter that loads provider redistributable
//          libraries by explicit path and resolves their exported symbols.
// Key invariants:
//   - Libraries are opened only by the exact path supplied; the adapter never
//     asks the platform loader to search default library paths.
//   - Opened libraries are never closed. Platform redistributables may keep
//     threads or registered destructors alive after their own shutdown call,
//     so unmapping them is unsafe; reopening the same path returns the same
//     platform handle.
//   - Paths are UTF-8 on every platform. The Windows adapter converts forward
//     slashes to backslashes, which LoadLibraryW requires for a path.
// Ownership/Lifetime:
//   - Library handles are process-lifetime platform handles owned by the
//     platform loader.
//   - Error text is written into caller-provided buffers.
// Links: src/runtime/services/rt_services_dynlib_posix.c,
//        src/runtime/services/rt_services_dynlib_win.c
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_services_dynlib.h
 * @brief Declares explicit-path shared-library loading for services providers.
 * @details POSIX builds use dlopen/dlsym; Windows builds use LoadLibraryW and
 *          GetProcAddress. Symbol addresses are returned as data pointers so
 *          providers cast them once with RT_FN_PTR_CAST to the documented
 *          C function-pointer types.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Report whether a regular file or symlink target exists at a path.
/// @param utf8_path NUL-terminated UTF-8 path; NULL or empty reports 0.
/// @return 1 when the path names an existing file system entry, otherwise 0.
int8_t rt_services_dynlib_file_exists(const char *utf8_path);

/// @brief Open a shared library by exact path.
/// @details The library is loaded with immediate symbol binding and local
///          symbol scope where the platform supports it. On failure a
///          one-line loader reason is written to @p error.
/// @param utf8_path NUL-terminated UTF-8 path to the library file.
/// @param error Buffer receiving the loader's failure reason; may be NULL.
/// @param error_capacity Size of @p error in bytes.
/// @return Opaque process-lifetime library handle, or NULL on failure.
void *rt_services_dynlib_open(const char *utf8_path, char *error, size_t error_capacity);

/// @brief Resolve an exported symbol from an opened library.
/// @param library Handle returned by @ref rt_services_dynlib_open.
/// @param name NUL-terminated exported symbol name.
/// @return Symbol address, or NULL when the library does not export @p name.
void *rt_services_dynlib_symbol(void *library, const char *name);

#ifdef __cplusplus
}
#endif
