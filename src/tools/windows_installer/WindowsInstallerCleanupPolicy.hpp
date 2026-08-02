//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tools/windows_installer/WindowsInstallerCleanupPolicy.hpp
// Purpose: Declare fail-closed command-line and path validation for the detached
//          Windows installer cleanup helper.
// Key invariants:
//   - Only unambiguous absolute child paths and nonzero decimal process IDs pass.
//   - Comparison recognizes equivalent Win32 namespace and Unicode-case spellings.
// Ownership/Lifetime:
//   - Inputs are borrowed for the duration of each call; no references are retained.
//   - Validation and comparison functions do not allocate or mutate filesystem state.
// Links: src/tools/windows_installer/WindowsInstallerCleanup.cpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>
#include <string_view>

namespace zanna::installer::cleanup {

/// @brief Return whether a path is safe for an exact, non-recursive cleanup operation.
/// @param path Candidate normal or extended-length drive/UNC path.
/// @return @c true only for a bounded absolute path with safe child components.
bool isSafeAbsolutePath(std::wstring_view path) noexcept;

/// @brief Compare two validated Windows path spellings using Windows ordinal rules.
/// @param left First path.
/// @param right Second path.
/// @return @c true after namespace, separator, and Unicode case normalization.
bool pathsEqual(std::wstring_view left, std::wstring_view right) noexcept;

/// @brief Parse one nonzero decimal Windows process identifier.
/// @param text Digits-only process identifier text.
/// @param processId Receives the parsed value on success and zero on failure.
/// @return @c true when @p text is canonical and fits in 32 bits.
bool parseProcessId(std::wstring_view text, std::uint32_t &processId) noexcept;

} // namespace zanna::installer::cleanup
