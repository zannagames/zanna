//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Declares regex-driven syntax highlighting with per-line caching.
/// @details SyntaxRuleSet loads pattern/style pairs, computes highlighted byte
///          spans in registration order, and reuses results while a line's
///          content remains unchanged.
//
// Each SyntaxRule pairs a regular expression pattern with a render Style.
// When a line is queried, all rules are applied to produce a list of
// Span records describing styled ranges. Results are cached per line
// and invalidated when the line content changes.
//
// Key invariants:
//   - Rules are applied in registration order; later rules can override
//     earlier ones for overlapping regions.
//   - Cached spans are indexed by line number; cache entries are invalidated
//     when the content changes (detected via string comparison).
//   - Invalid regex patterns in a rules file are silently skipped.
//
// Ownership: SyntaxRuleSet owns its rules and cache. Span vectors in the
// cache are owned by the cache entries.
//
//===----------------------------------------------------------------------===//

#pragma once

#include <regex>
#include <string>
#include <unordered_map>
#include <vector>

#include "tui/render/screen.hpp"

namespace zanna::tui::syntax {

/// @brief Associates a regular expression pattern with a visual style for highlighting.
/// @details When the pattern matches a region of text, the corresponding style is
///          applied to that region during rendering. Rules are evaluated in order
///          against each line of text.
struct SyntaxRule {
    std::regex pattern;  ///< Regular expression to match.
    render::Style style; ///< Style applied to matches.
};

/// @brief Highlighted text span within a single line.
/// @details Records the byte offset, length, and visual style for a syntax-highlighted
///          region. Multiple spans can cover a single line; the renderer applies them
///          in order during painting.
struct Span {
    std::size_t start{0};  ///< Byte offset within line.
    std::size_t length{0}; ///< Length in bytes.
    render::Style style{}; ///< Style for the span.
};

/// @brief Manages syntax highlighting rules with per-line caching.
/// @details Loads highlighting rules from configuration files, applies regex patterns
///          to text lines, and caches the resulting styled spans for efficient
///          re-rendering. Cache entries are automatically invalidated when line
///          content changes.
class SyntaxRuleSet {
  public:
    /// @brief Load syntax highlighting rules from a JSON configuration file.
    /// @details Parses the file for pattern/style pairs and compiles the regex
    ///          patterns. Invalid patterns are silently skipped.
    /// @param path Filesystem path to the JSON rules file.
    /// @return True if the file was successfully loaded and at least one rule was parsed.
    bool loadFromFile(const std::string &path);

    /// @brief Compute or retrieve cached syntax spans for a line.
    /// @details Returns the cached spans if the line content has not changed since
    ///          the last computation. Otherwise, recomputes spans by applying all
    ///          rules to the line text.
    /// @param lineNo Zero-based line number (used as cache key).
    /// @param line The current text content of the line.
    /// @return Reference to the vector of styled spans for this line.
    const std::vector<Span> &spans(std::size_t lineNo, const std::string &line);

    /// @brief Invalidate the cached spans for a specific line.
    /// @details Forces recomputation of syntax highlighting on the next spans() call
    ///          for the given line. Call this when line content is modified.
    /// @param lineNo Zero-based line number to invalidate.
    void invalidate(std::size_t lineNo);

  private:
    std::vector<SyntaxRule> rules_{}; ///< Highlighting rules in application order.
    /// @brief Cached source text and spans keyed by zero-based line number.
    std::unordered_map<std::size_t, std::pair<std::string, std::vector<Span>>> cache_{};
};

} // namespace zanna::tui::syntax
