//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/io/rt_tempfile.h
// Purpose: Temporary path, file, and directory utilities using the system
//          temporary directory and collision-resistant names.
//
// Key invariants:
//   - Candidate names contain a 128-bit platform-entropy identifier, except
//     the POSIX mkstemp fast path, which supplies its own unpredictable suffix.
//   - Create operations reserve names atomically and never replace an existing
//     file or directory; path-only operations do not reserve their result.
//   - Prefixes and extensions are filename fragments, not paths, and reject
//     separators, colon, embedded NUL, and invalid runtime strings.
//   - Created files and directories are never removed automatically.
//
// Ownership/Lifetime:
//   - Returned runtime string references must be released by the caller.
//   - Create operations close their native file descriptor/handle before
//     returning; callers own removal of every created filesystem object.
//
// Links: src/runtime/io/rt_tempfile.c (implementation), src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//
/**
 * @file
 * @brief Declares secure temporary path, file, and directory utilities.
 * @details Provides normalized temporary-directory discovery, unreserved
 * random candidate paths, and atomic exclusive creation of persistent empty
 * files or directories using validated portable prefix and extension
 * fragments.
 */
#pragma once

#include "rt_string.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Get the system temporary directory path.
/// @details Windows validates GetTempPath2W/GetTempPathW results and falls back
///          to the current directory. POSIX accepts a safe, usable absolute
///          TMPDIR and otherwise returns `/tmp`.
/// @return Runtime string reference containing the normalized directory path.
rt_string rt_tempfile_dir(void);

/// @brief Create a unique temporary file path without creating the file.
/// @details Generates a random filename in the temp directory. This helper is
///          path-only and is not suitable for security-sensitive creation unless
///          the caller subsequently opens the path atomically with exclusive
///          create semantics.
/// @return Fresh runtime string containing a candidate path with prefix
///         `zanna_` and extension `.tmp`, or an empty string after failure.
rt_string rt_tempfile_path(void);

/// @brief Create a unique temporary file path with prefix without creating the file.
/// @details Generates a unique filename with given prefix. The returned path is
///          path-only; prefer @ref rt_tempfile_create_with_prefix when the file
///          should be created safely.
/// @param prefix Portable filename fragment placed before the random identifier.
/// @return Fresh runtime string containing a candidate `.tmp` path, or an
///         empty string after failure.
rt_string rt_tempfile_path_with_prefix(rt_string prefix);

/// @brief Create a unique temporary file path with prefix and extension.
/// @details Generates a unique filename with given prefix and extension without
///          creating it. Use only when the caller will perform an atomic
///          exclusive create operation.
/// @param prefix Portable filename fragment placed before the random identifier.
/// @param extension Portable trailing fragment, commonly including a leading period.
/// @return Fresh runtime string containing the candidate path, or an empty
///         string after failure.
rt_string rt_tempfile_path_with_ext(rt_string prefix, rt_string extension);

/// @brief Create an empty temporary file and return its path.
/// @details Creation is exclusive, the native handle is closed before return,
///          and the file persists until explicitly removed.
/// @return Runtime string reference naming the created file, or an empty
///         string after failure.
rt_string rt_tempfile_create(void);

/// @brief Create an empty temporary file with prefix.
/// @details POSIX prefers `mkstemp`; other attempts use independently random
///          candidate paths with exclusive-create semantics.
/// @param prefix Portable filename fragment placed before the generated suffix.
/// @return Runtime string reference naming the closed, empty file, or an empty
///         string after failure.
rt_string rt_tempfile_create_with_prefix(rt_string prefix);

/// @brief Create a unique temporary directory.
/// @details The directory is created atomically and persists until explicitly removed.
/// @return Runtime string reference naming the created directory, or an empty
///         string after failure.
rt_string rt_tempdir_create(void);

/// @brief Create a unique temporary directory with prefix.
/// @details POSIX requests owner-only mode 0700, subject to the process umask.
/// @param prefix Portable filename fragment placed before the random identifier.
/// @return Runtime string reference naming the created directory, or an empty
///         string after failure.
rt_string rt_tempdir_create_with_prefix(rt_string prefix);

#ifdef __cplusplus
}
#endif
