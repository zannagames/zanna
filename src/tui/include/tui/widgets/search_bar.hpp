//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Declares an incremental literal/regex search bar for a TUI text view.
/// @details SearchBar owns query and match state while borrowing a text buffer,
///          view, and theme; every edit recomputes highlights and Enter advances
///          through matches.
//
// As the user types, matches are incrementally computed via the text
// search utilities (findAll). The matched ranges are highlighted in the
// associated TextView. Pressing Enter advances to the next match.
//
// Key invariants:
//   - The search bar borrows TextBuffer, TextView, and Theme references.
//   - Match highlighting is updated on every keystroke via setHighlights().
//   - Empty queries clear all highlights.
//   - Regex mode can be toggled via setRegex().
//
// Ownership: SearchBar borrows all external references (TextBuffer,
// TextView, Theme). It owns the query string and match results.
//
//===----------------------------------------------------------------------===//

#pragma once

#include <string>
#include <vector>

#include "tui/style/theme.hpp"
#include "tui/text/search.hpp"
#include "tui/text/text_buffer.hpp"
#include "tui/ui/widget.hpp"
#include "tui/views/text_view.hpp"

namespace zanna::tui::widgets {
/// @brief Interactive text search widget with incremental match highlighting.
/// @details Provides a '/' prefixed query input that searches through a TextBuffer
///          and highlights matches in the associated TextView. Supports both literal
///          substring and regex search modes. Pressing Enter navigates to the next match.
class SearchBar : public ui::Widget {
  public:
    /// @brief Construct search bar bound to buffer and view.
    /// @param buf Borrowed text buffer to search.
    /// @param view Borrowed text view receiving highlights and cursor movement.
    /// @param theme Borrowed render palette.
    SearchBar(text::TextBuffer &buf, views::TextView &view, const style::Theme &theme);

    /// @brief Paint search text prefixed by '/'.
    /// @param sb Screen buffer receiving the query line.
    void paint(render::ScreenBuffer &sb) override;

    /// @brief Handle typing, backspace, and Enter for next match.
    /// @param ev Input event to interpret.
    /// @return true when the search bar consumed the event.
    bool onEvent(const ui::Event &ev) override;

    /// @brief Enable regex search mode.
    /// @param regex true to treat the query as ECMAScript regex; false for literal text.
    void setRegex(bool regex);

    /// @brief Number of current matches.
    /// @return Size of the latest match vector.
    [[nodiscard]] std::size_t matchCount() const;

  private:
    text::TextBuffer &buf_;         ///< Borrowed searchable document.
    views::TextView &view_;         ///< Borrowed view updated with match highlights.
    const style::Theme &theme_;     ///< Borrowed render palette.
    std::string query_{};           ///< Current query bytes.
    bool regex_{};                  ///< Whether @c query_ is interpreted as regex.
    std::vector<text::Match> matches_{}; ///< Current ordered search results.
    std::size_t current_{0};        ///< Index of the active match.

    /// @brief Recompute matches and replace the text view's highlighted ranges.
    void updateMatches();

    /// @brief Activate a match and move the text-view cursor to it.
    /// @param idx Match index, interpreted within the current result set.
    void gotoMatch(std::size_t idx);
};
} // namespace zanna::tui::widgets
