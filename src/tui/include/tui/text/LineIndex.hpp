//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Declares an incrementally maintained text line-start index.
/// @details LineIndex stores sorted byte offsets for newline-delimited lines,
///          supports constant-time start lookup, and adjusts only affected
///          entries after insertions and erasures.
//
// Lines are defined by newline characters ('\n'). The index always contains
// at least one entry (offset 0) representing the first line, even when the
// document is empty.
//
// Key invariants:
//   - line_starts_ is sorted in ascending order.
//   - line_starts_[0] is always 0.
//   - count() returns at least 1 for any state.
//   - start(line) returns the byte offset where line begins.
//
// Ownership: LineIndex owns the line_starts_ vector by value. No external
// resources or heap indirection beyond the vector.
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstddef>
#include <string_view>
#include <vector>

namespace zanna::tui::text {
/// @brief Maintains sorted line-start byte offsets for fast line-number access.
/// @details Tracks newline character positions to enable O(1) lookup of line start
///          offsets and O(n) incremental updates on insert/erase where n is the
///          number of newlines affected. Used by TextBuffer to implement
///          line-based operations without scanning the entire document.
class LineIndex {
  public:
    /// @brief Rebuild the line index from scratch for the given text.
    /// @details Scans the text for newline characters and populates the line_starts_
    ///          vector. Always starts with offset 0.
    /// @param text The full text content to index.
    void reset(std::string_view text);

    /// @brief Update the line index after text was inserted at the given position.
    /// @details Scans the inserted text for newlines and inserts corresponding
    ///          offsets into the line_starts_ vector. Adjusts all subsequent line
    ///          offsets by the length of the inserted text.
    /// @param pos Byte offset where the insertion occurred.
    /// @param text The inserted text (scanned for newlines).
    void onInsert(std::size_t pos, std::string_view text);

    /// @brief Update the line index after text was erased at the given position.
    /// @details Removes line-start entries that fall within the erased range and
    ///          adjusts all subsequent offsets by the negative length of the erasure.
    /// @param pos Byte offset where the erasure started.
    /// @param text The erased text (scanned for newlines to determine count).
    void onErase(std::size_t pos, std::string_view text);

    /// @brief Return the number of indexed lines.
    /// @details Always returns at least 1, even for an empty document.
    /// @return Number of lines in the index.
    [[nodiscard]] std::size_t count() const;

    /// @brief Return the starting byte offset of the given line.
    /// @param line Zero-based line number.
    /// @return Byte offset where the line begins; undefined if line >= count().
    [[nodiscard]] std::size_t start(std::size_t line) const;

  private:
    std::vector<std::size_t> line_starts_{0}; ///< Sorted offsets, always beginning with zero.
};
} // namespace zanna::tui::text
