//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tui/src/version.cpp
// Purpose: Provide the single entry point that reports the Zanna TUI library
//          version string to embedders.
// Key invariants: Returned pointer references an immutable, null-terminated
//                 string literal that outlives the process.
// Ownership/Lifetime: String literal resides in read-only program storage and
//                     requires no caller-managed lifetime.
// Links: docs/internals/architecture.md#zannatui-architecture
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements the public Zanna TUI version query.
/// @details The implementation exposes the build-provided version literal
///          without allocation, copying, or runtime initialization.

#include "tui/version.hpp"
#include "zanna/version.hpp"

namespace zanna::tui {
/// @brief Expose the semantic version string for the Zanna TUI component.
/// @details The version is embedded at build time and returned as a pointer to
///          a string literal with static storage duration.  Consumers may cache
///          the pointer or copy the characters as needed; the literal remains
///          valid for the lifetime of the program.  The function performs no
///          I/O and is safe to call from static constructors.
/// @return Null-terminated UTF-8 string literal describing the library version.
const char *zanna_tui_version() noexcept {
    return ZANNA_VERSION_STR;
}
} // namespace zanna::tui
