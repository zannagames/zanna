//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/io/rt_glob.h
// Purpose: Byte-oriented glob matching with *, **, ?, and bracket classes, plus non-recursive and
// recursive filesystem enumeration that returns joined matching paths as a Seq.
//
// Key invariants:
//   - Ordinary *, ?, and bracket classes do not cross separators; ** consumes across components.
//   - Matching is byte-oriented and case-sensitive on POSIX. Windows folds bytes with `tolower`
//     and treats '/' and '\' as equivalent separators.
//   - Bracket classes support literals, escaped bytes, normalized ranges, and leading !/^
//     negation; malformed opening brackets are treated literally.
//   - Recursive traversal does not follow symbolic links or Windows reparse-point directories.
//   - Enumeration returns an empty owning Seq, not NULL, for invalid operands or no matches.
//
// Ownership/Lifetime:
//   - Returned Seq objects are fresh runtime-managed containers that own their path elements.
//   - Input strings are borrowed and are not retained after the call.
//
// Links: src/runtime/io/rt_glob.c (implementation),
//        src/runtime/io/rt_dir.h (directory enumeration),
//        src/runtime/io/rt_path.h (path joining),
//        src/runtime/core/rt_string.h (runtime strings)
//
//===----------------------------------------------------------------------===//
/**
 * @file
 * @brief Declares byte-oriented glob matching and filesystem enumeration.
 * @details Supports component-local wildcards, separator-crossing double
 * stars, single-byte wildcards, bracket classes, direct entry/file searches,
 * and symlink-safe bounded recursive file traversal with owning Seq results.
 */
#pragma once

#include "rt_string.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Check if a path matches a glob pattern.
/// @details `*` matches zero or more non-separator bytes, `**` may cross separators, `?` matches
///          one non-separator byte, and bracket classes match one selected byte. This is a pure
///          string operation and does not access the filesystem.
/// @param path Borrowed runtime string containing the complete path text to test.
/// @param pattern Borrowed runtime glob pattern such as `*.txt` or `src/**/[a-z]*.c`.
/// @return 1 when the complete path matches; otherwise 0, including invalid or embedded-NUL
///         operands.
int8_t rt_glob_match(rt_string path, rt_string pattern);

/// @brief Find all files matching a glob pattern in a directory.
/// @details Enumerates only direct regular-file children and matches @p pattern against each
///          filename. Missing/invalid inputs produce an empty result; allocation and append
///          failures trap after partial-result cleanup.
/// @param dir Borrowed runtime path of the directory to search.
/// @param pattern Borrowed pattern such as `*.txt`, matched against names only.
/// @return Fresh owning Seq of matching paths joined to @p dir.
void *rt_glob_files(rt_string dir, rt_string pattern);

/// @brief Find all files matching a glob pattern recursively.
/// @details Performs depth-first traversal without following symlink/reparse-point directories.
///          The pattern is matched against slash-separated paths relative to @p base, and
///          traversal depth is capped at 4096.
/// @param base Borrowed runtime path of the traversal root.
/// @param pattern Borrowed relative-path pattern such as `**/*.txt` or `src/**/*.c`.
/// @return Fresh owning Seq of full matching file paths.
void *rt_glob_files_recursive(rt_string base, rt_string pattern);

/// @brief Find direct file and directory entries matching a glob pattern.
/// @details Matches @p pattern against each direct entry name and includes both files and
///          directories. Missing/invalid inputs produce an empty owning Seq.
/// @param dir Borrowed runtime path of the directory to search.
/// @param pattern Borrowed glob pattern matched against entry names.
/// @return Fresh owning Seq of matching paths joined to @p dir.
void *rt_glob_entries(rt_string dir, rt_string pattern);

#ifdef __cplusplus
}
#endif
