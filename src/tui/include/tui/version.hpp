//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Declares the process-lifetime Zanna TUI version query.
/// @details The semantic version string is embedded at build time and exposed
///          through a stable, allocation-free public API.
//
// Key invariants:
//   - The returned string has static storage duration and is always non-null.
//   - The string is null-terminated and valid for the lifetime of the process.
//
// Ownership: No dynamic resources; the version string lives in the
// read-only data segment.
//
//===----------------------------------------------------------------------===//

#pragma once

namespace zanna::tui {

/// @brief Return the compile-time Zanna TUI version string.
/// @return Non-null pointer to null-terminated static storage.
/// @invariant The returned pointer remains valid for the lifetime of the process.
/// @ownership The caller must not modify or free the returned string.
const char *zanna_tui_version() noexcept;
} // namespace zanna::tui
