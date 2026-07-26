//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Provides literal and regular-expression search utilities for the terminal UI
// text buffer.  The implementation keeps runtime safeguards in place (size caps,
// exception handling) so interactive searches cannot stall the UI or surface
// uncaught regex errors to users.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Search helpers for @ref zanna::tui::text::TextBuffer.
/// @details The functions offer simple literal scans and regular-expression
///          matches while constraining the scanned text to a configurable limit
///          to keep latency predictable.  Results are returned by value so the
///          caller can store or display them freely.

#include "tui/text/search.hpp"

#include <regex>

namespace zanna::tui::text {
namespace {
/// @brief Maximum number of characters a search considers before truncating.
/// @details Keeps regex operations bounded so pathological patterns cannot freeze
///          the UI.  One megabyte is large enough for typical buffers yet small
///          enough for interactive latency.
constexpr size_t kMaxSearchSize = 1 << 20; // 1MB cap
} // namespace

/// @brief Locate every match for @p query within @p buf.
/// @details The helper implements two strategies:
///          1. Literal scans use @c std::string::find in a loop, advancing by the
///             match length so successive results do not overlap.
///          2. Regex searches compile @p query into @c std::regex and iterate the
///             match results.  Compilation happens inside a @c try/@c catch block
///             so malformed expressions simply yield no matches.
///          In both cases the buffer is truncated to @ref kMaxSearchSize bytes to
///          prevent unbounded work when the user searches enormous files.
/// @param buf Text buffer whose contents are searched.
/// @param query Literal text or regular-expression pattern to match.
/// @param useRegex When true, interpret @p query as a regular expression;
///        otherwise compare it literally.
/// @return Matches in ascending byte-offset order, or an empty vector when the
///         query is empty, invalid, or absent from the scanned prefix.
std::vector<Match> findAll(const TextBuffer &buf, std::string_view query, bool useRegex) {
    std::vector<Match> hits;
    if (query.empty()) {
        return hits;
    }
    std::string hay = buf.str();
    if (hay.size() > kMaxSearchSize) {
        hay.resize(kMaxSearchSize);
    }
    if (!useRegex) {
        size_t pos = 0;
        while ((pos = hay.find(query, pos)) != std::string::npos) {
            hits.push_back(Match{pos, query.size()});
            pos += query.size() > 0 ? query.size() : 1;
        }
        return hits;
    }
    try {
        std::regex re{std::string(query)};
        auto begin = std::sregex_iterator(hay.begin(), hay.end(), re);
        auto end = std::sregex_iterator();
        for (auto it = begin; it != end; ++it) {
            hits.push_back(
                Match{static_cast<size_t>(it->position()), static_cast<size_t>(it->length())});
        }
    } catch (const std::regex_error &) {
        return {};
    }
    return hits;
}

/// @brief Find the next match starting at @p from.
/// @details Reuses the same literal-vs-regex split as @ref findAll while
///          stopping after the first hit on or after the requested offset.  Regex
///          searches adjust the returned position by the starting offset so the
///          caller receives coordinates relative to the full buffer.  Failures to
///          compile or execute the regular expression result in @c std::nullopt
///          rather than throwing, allowing the UI to keep running.
/// @param buf Text buffer whose contents are searched.
/// @param query Literal text or regular-expression pattern to match.
/// @param from Byte offset at which the search begins; offsets beyond the
///        scanned text behave as an end-of-buffer search.
/// @param useRegex When true, interpret @p query as a regular expression;
///        otherwise compare it literally.
/// @return The first match at or after @p from, or @c std::nullopt when no match
///         exists or the regular expression is invalid.
std::optional<Match> findNext(const TextBuffer &buf,
                              std::string_view query,
                              size_t from,
                              bool useRegex) {
    if (query.empty()) {
        return std::nullopt;
    }
    std::string hay = buf.str();
    if (hay.size() > kMaxSearchSize) {
        hay.resize(kMaxSearchSize);
    }
    if (!useRegex) {
        size_t pos = hay.find(query, from);
        if (pos != std::string::npos) {
            return Match{pos, query.size()};
        }
        return std::nullopt;
    }
    try {
        std::regex re{std::string(query)};
        std::cmatch m;
        const char *start = hay.c_str() + std::min(from, hay.size());
        if (std::regex_search(start, hay.c_str() + hay.size(), m, re)) {
            return Match{static_cast<size_t>(m.position()) + std::min(from, hay.size()),
                         static_cast<size_t>(m.length())};
        }
    } catch (const std::regex_error &) {
        return std::nullopt;
    }
    return std::nullopt;
}

} // namespace zanna::tui::text
