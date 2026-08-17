//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/io/rt_path.h
// Purpose: Cross-platform file path manipulation utilities for Zanna.IO.Path, providing join,
// dirname, basename, extension, normalization, and existence queries.
//
// Key invariants:
//   - Returned strings never borrow input storage; nonempty results are copied and empty results
//     may use the shared canonical empty string.
//   - Join and normalization emit platform-native separators; component extraction otherwise
//     preserves path bytes selected from the input.
//   - rt_path_join handles absolute and relative path combination correctly.
//   - rt_path_normalize resolves . and .. components and removes redundant separators.
//   - Link inspection never follows the final path component and never traps.
//
// Ownership/Lifetime:
//   - Every returned runtime string transfers a managed reference to the caller.
//   - rt_path_exe_dir_cstr is the sole exception: it returns malloc-owned C storage that the
//     caller must free.
//   - Input strings are borrowed; they are not retained.
//
// Links: src/runtime/io/rt_path.c (implementation), src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//
/**
 * @file
 * @brief Declares cross-platform lexical and filesystem path utilities.
 * @details Exposes joining, component extraction, extension replacement,
 * lexical normalization, absolute-path conversion, final-component link
 * inspection, native separator lookup, and executable-directory discovery
 * with explicit managed versus malloc-owned return contracts.
 */
#pragma once

#include "rt_string.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Join two path components with the platform separator.
/// @param a First path component.
/// @param b Second path component.
/// @return Runtime-managed joined path that does not borrow either input.
/// @details If b is absolute, returns b. Otherwise joins with platform separator.
rt_string rt_path_join(rt_string a, rt_string b);

/// @brief Get the directory portion of a path.
/// @param path Path to extract directory from.
/// @return Runtime-managed directory path, or "." if no directory component.
/// @details For "/foo/bar/baz.txt" returns "/foo/bar".
///          For "baz.txt" returns ".".
rt_string rt_path_dir(rt_string path);

/// @brief Get the filename portion of a path.
/// @param path Path to extract filename from.
/// @return Runtime-managed filename string.
/// @details For "/foo/bar/baz.txt" returns "baz.txt".
rt_string rt_path_name(rt_string path);

/// @brief Get the filename without extension.
/// @param path Path to extract stem from.
/// @return Runtime-managed stem string.
/// @details For "/foo/bar/baz.txt" returns "baz".
///          For ".hidden" returns ".hidden" (no extension).
rt_string rt_path_stem(rt_string path);

/// @brief Get the file extension including the dot.
/// @param path Path to extract extension from.
/// @return Runtime-managed extension string (e.g., ".txt"), or empty if none.
/// @details For "/foo/bar/baz.txt" returns ".txt".
///          For ".hidden" returns "" (leading dot is not an extension).
rt_string rt_path_ext(rt_string path);

/// @brief Replace the extension of a path.
/// @param path Original path.
/// @param new_ext New extension (with or without leading dot).
/// @return Runtime-managed path with the replacement extension.
/// @details For "/foo/bar.txt" with ".md" returns "/foo/bar.md".
rt_string rt_path_with_ext(rt_string path, rt_string new_ext);

/// @brief Check if a path is absolute.
/// @param path Path to check.
/// @return 1 if absolute, 0 if relative.
/// @details On Unix: starts with "/".
///          On Windows: starts with drive letter (C:\) or UNC path (\\server).
int64_t rt_path_is_abs(rt_string path);

/// @brief Check whether the final path component is a symbolic link or reparse point.
/// @param path Existing or missing filesystem path to inspect.
/// @return 1 for a POSIX symbolic link or Windows reparse point; otherwise 0.
/// @details Does not follow the final component and returns 0 for invalid,
///          missing, inaccessible, or ordinary paths without trapping.
int64_t rt_path_is_link(rt_string path);

/// @brief Compare two existing filesystem entries by stable identity.
/// @param left First file, directory, or other stat-able entry path.
/// @param right Second entry path.
/// @return 1 when both paths resolve to the same entry; otherwise 0.
/// @details Follows symbolic links/reparse points. Invalid, missing, and inaccessible operands
///          compare unequal without trapping.
int64_t rt_path_same_entry(rt_string left, rt_string right);

/// @brief Convert a relative path to absolute.
/// @param path Path to convert.
/// @return Runtime-managed absolute normalized path.
/// @details Prepends current working directory and normalizes.
rt_string rt_path_abs(rt_string path);

/// @brief Normalize a path by removing redundant separators and resolving . and ..
/// @param path Path to normalize.
/// @return Runtime-managed normalized path.
/// @details Removes "." components, resolves ".." where possible,
///          collapses multiple separators, returns "." for empty result.
rt_string rt_path_norm(rt_string path);

/// @brief Get the platform-specific path separator.
/// @return Runtime-managed string containing "/" (Unix) or "\" (Windows).
rt_string rt_path_sep(void);

/// @brief Get the directory containing the running executable.
/// @return Runtime-managed executable-directory string, or "." if detection fails.
rt_string rt_path_exe_dir_str(void);

/// @brief Get the directory containing the running executable (C string).
/// @return Malloc-owned C string for the caller to free, or NULL on failure.
char *rt_path_exe_dir_cstr(void);

// Upward marker search (ADR 0253).
/// @brief Walk up from @p start_dir looking for a relative marker path.
/// @details Returns the directory *containing* the marker. The marker is probed
///          as both a file and a directory. The level bound prevents an
///          unbounded climb when the marker is absent.
/// @param start_dir Start directory; empty or NULL means the working directory.
/// @param relative_marker Path fragment probed at each level.
/// @param max_levels Parent levels above the start; clamped to 0..64.
/// @return Owned directory path, or the empty string when nothing matched.
rt_string rt_path_find_upward(rt_string start_dir, rt_string relative_marker, int64_t max_levels);
/// @brief Resolve a data root from an environment override, then a marker walk.
/// @param env_var Environment variable consulted first; may be NULL.
/// @param relative_marker Path fragment identifying the root.
/// @return Owned directory path, or the empty string when unresolved.
rt_string rt_path_resolve_data_root(rt_string env_var, rt_string relative_marker);

#ifdef __cplusplus
}
#endif
