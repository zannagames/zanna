//===----------------------------------------------------------------------===//
/// @file
/// @brief Declares stateless structured persistence for REPL command history.
/// @details The codec preserves arbitrary entry bytes with length-prefixed
///          records, accepts legacy line-delimited files, and returns value-owned
///          decoded strings without retaining file or session state.
///
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/repl/ReplHistoryCodec.hpp
// Purpose: Structured persistence for REPL command history.
// Key invariants:
//   - Multi-line entries round-trip without being split.
//   - Legacy newline-delimited history files remain readable.
// Ownership/Lifetime:
//   - Functions return owned std::string entries.
//   - No persistent state is kept between calls.
// Links: src/repl/ReplLineEditor.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace zanna::repl {

/// @brief Result of loading a REPL history file.
/// @details @c entries contains the decoded history entries, oldest first.
///          @c decodedEntryCount is the number of entries found before max-size
///          trimming, which lets callers report how many entries were present on
///          disk even when they retain only the most recent subset.
struct ReplHistoryLoadResult {
    std::vector<std::string> entries; ///< Decoded history entries, oldest first.
    size_t decodedEntryCount{0};      ///< Number of entries decoded before trimming.
};

/// @brief Encode and decode REPL history files.
/// @details The current format starts with a magic header followed by
///          length-prefixed entries. Length-prefixing keeps embedded newlines,
///          tabs, spaces, and NUL bytes unambiguous. The loader also accepts the
///          previous plain-text format, treating each non-empty line as one
///          history entry.
class ReplHistoryCodec {
  public:
    /// @brief Load a history file from disk.
    /// @details If the file begins with the codec magic header, entries are
    ///          decoded from length-prefixed records. Otherwise the file is read
    ///          as legacy newline-delimited history. In both cases empty entries
    ///          are ignored and the result is trimmed to @p maxEntries by keeping
    ///          the most recent commands.
    /// @param path History file path.
    /// @param maxEntries Maximum number of entries to retain in the result.
    /// @return Decoded entries plus the pre-trim entry count.
    static ReplHistoryLoadResult load(const std::filesystem::path &path, size_t maxEntries);

    /// @brief Save history entries to disk using the structured format.
    /// @details Parent directories are created when needed. Entries are written
    ///          with byte lengths, so multi-line commands round-trip exactly.
    /// @param path History file path.
    /// @param entries Entries to persist, oldest first.
    /// @return True when the file was written successfully.
    static bool save(const std::filesystem::path &path, const std::vector<std::string> &entries);
};

} // namespace zanna::repl
