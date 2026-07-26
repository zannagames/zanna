//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tools/basic/common.hpp
// Purpose: Shared utilities for BASIC command-line tools.
// Key invariants: Helpers must preserve existing CLI diagnostics.
// Ownership/Lifetime: Callers retain ownership of buffers and SourceManager instances.
// Links: docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares shared source-loading support for BASIC developer tools.
/// @details The helper centralizes usage reporting, bounded file loading,
///          diagnostic formatting, and SourceManager registration so small
///          BASIC command-line utilities expose identical failure behavior.

#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace il::support {
class SourceManager;
}

namespace il::tools::basic {

/// @brief Load BASIC source into a buffer and register it with the SourceManager.
///
/// @param path Path to the BASIC source file. If null, a usage message is printed and
/// the load fails.
/// @param buffer Destination for the loaded source contents. On success it is populated
/// with the file contents; on failure it is left unchanged.
/// @param sm Source manager responsible for tracking file identifiers.
///
/// @return The assigned file identifier on success; std::nullopt if the usage check or
/// file loading fails.
/// @details Emits the configured BASIC-tool usage text when @p path is null.
///          Other loading and registration failures are printed as canonical
///          diagnostics. The destination buffer changes only on success.
std::optional<std::uint32_t> loadBasicSource(const char *path,
                                             std::string &buffer,
                                             il::support::SourceManager &sm);

} // namespace il::tools::basic
