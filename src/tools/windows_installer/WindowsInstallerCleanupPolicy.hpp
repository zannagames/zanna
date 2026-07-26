//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Declares the pure exact-path validation policy used by the detached
///        Windows installer cleanup helper.
///
/// Only fully qualified drive or UNC paths below a root are accepted. Device
/// names, alternate streams, traversal, control characters, and Win32
/// normalization ambiguities are rejected. Inputs are borrowed only for each call.
///
/// @see WindowsInstallerCleanup.cpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include <string_view>

namespace zanna::installer::cleanup {

/// @brief Return whether a path is safe for an exact, non-recursive cleanup operation.
/// @param path Candidate normal or extended-length drive/UNC path.
/// @return @c true only for a bounded absolute path with safe child components.
bool isSafeAbsolutePath(std::wstring_view path) noexcept;

/// @brief Compare two validated Windows path spellings case-insensitively.
/// @param left First path.
/// @param right Second path.
/// @return @c true after ASCII case folding and slash normalization.
bool pathsEqual(std::wstring_view left, std::wstring_view right) noexcept;

} // namespace zanna::installer::cleanup
