//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Declares literal and regular-expression search over TUI text buffers.
/// @details Search results use byte offsets, remain non-overlapping and ordered,
///          and treat invalid ECMAScript regular expressions as no matches.
//
// The findAll() function returns all non-overlapping matches of a query
// within the buffer. The findNext() function finds the first match at or
// after a given byte offset, enabling incremental forward search.
//
// When useRegex is true, the query string is interpreted as an ECMAScript
// regular expression. When false, it is treated as a literal substring.
//
// Key invariants:
//   - Match offsets are byte positions within the buffer's content.
//   - Matches returned by findAll() are non-overlapping and in order.
//   - Invalid regex patterns result in an empty result set.
//
// Ownership: Match structs are plain value types with no heap allocation.
// Functions operate on const TextBuffer references without mutation.
//
//===----------------------------------------------------------------------===//

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "tui/text/text_buffer.hpp"

namespace zanna::tui::text {
/// @brief Byte-range result of a text search operation.
/// @details Represents a contiguous match within the text buffer, stored as
///          a starting byte offset and a length in bytes.
struct Match {
    size_t start{0};  ///< Starting byte offset in the logical buffer.
    size_t length{0}; ///< Matched byte length.
};

/// @brief Find all matches of query in buffer.
/// @param buf TextBuffer to search.
/// @param query Literal text or regex pattern.
/// @param useRegex Interpret query as regex when true.
/// @return Ordered, non-overlapping byte ranges; empty for no match or invalid regex.
[[nodiscard]] std::vector<Match> findAll(const TextBuffer &buf,
                                         std::string_view query,
                                         bool useRegex);

/// @brief Find next match starting at or after offset.
/// @param buf TextBuffer to search.
/// @param query Literal text or regex pattern.
/// @param from Starting byte offset within buffer.
/// @param useRegex Interpret query as regex when true.
/// @return First match at or after @p from, or nullopt.
[[nodiscard]] std::optional<Match> findNext(const TextBuffer &buf,
                                            std::string_view query,
                                            size_t from,
                                            bool useRegex);
} // namespace zanna::tui::text
