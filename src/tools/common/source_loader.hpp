//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tools/common/source_loader.hpp
// Purpose: Shared helpers for loading source files used by language frontend CLI tools.
// Key invariants: LoadedSource accurately captures file contents and SourceManager registration.
// Ownership/Lifetime: The caller owns the returned LoadedSource and may use it after the call.
// Links: docs/internals/architecture.md
//
//===----------------------------------------------------------------------===//

#pragma once

/// @file
/// @brief Declares bounded source-file loading helpers for frontend tools.

#include "support/diag_expected.hpp"
#include "support/source_manager.hpp"

#include <cstdint>
#include <string>

namespace il::tools::common {

/// @brief Result of loading a source file into memory.
/// @details Contains the file contents as a string and the identifier assigned
///          by the SourceManager. The fileId can be used for diagnostic reporting.
struct LoadedSource {
    std::string buffer; ///< Full contents of the source file.
    uint32_t fileId{0}; ///< Identifier assigned by SourceManager (0 indicates failure).
};

/// @brief Load a source file into memory and register it with the source manager.
///
/// Opens @p path, reads the entire file into a string buffer, and registers the
/// file with @p sm so diagnostics can resolve the location later. Errors are
/// propagated as diagnostics inside an Expected value.
///
/// @param path Filesystem path to the source file.
/// @param sm Source manager tracking file identifiers for diagnostics.
/// @return Loaded source buffer on success; otherwise a diagnostic describing
///         the I/O failure or SourceManager overflow.
il::support::Expected<LoadedSource> loadSourceBuffer(const std::string &path,
                                                     il::support::SourceManager &sm);

/// @brief Load a source file into memory without SourceManager registration.
///
/// Opens @p path and reads the entire file into a string buffer. This variant
/// is useful when SourceManager registration is handled separately or not needed.
///
/// @param path Filesystem path to the source file.
/// @return File contents on success; otherwise a diagnostic describing the I/O failure.
il::support::Expected<std::string> loadSourceFile(const std::string &path);

} // namespace il::tools::common
