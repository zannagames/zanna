//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Implements the incremental search bar widget for the Zanna TUI text view.  It
// captures keystrokes, maintains the active query, and highlights matches within
// an attached text buffer while synchronising the cursor position in the bound
// text view.  Behaviour mimics modal editors: typing extends the query, pressing
// Enter or F3 advances to the next match, and backspace shrinks the query.
//
// Invariants:
//   * The `matches_` vector is rebuilt on every query change so highlights never
//     refer to stale buffer positions.
//   * The `current_` index is clamped to the match count and resets to zero when
//     a fresh query is entered.
// Ownership:
//   * SearchBar borrows the text buffer, view, and theme; it does not manage
//     their lifetimes, allowing integration into larger editor components.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements the interactive search bar for text views.
/// @details The widget reacts to key events, translates them into query updates,
///          invokes buffer search helpers, and renders the current query and
///          match summary.  Highlight ranges are synchronised with the attached
///          view to keep visual feedback in lockstep with navigation.

#include "tui/widgets/search_bar.hpp"
#include "tui/render/screen.hpp"

namespace zanna::tui::widgets {
using zanna::tui::term::KeyEvent;

/// @brief Construct a search bar bound to a text buffer and view.
/// @details Stores references to the supplied buffer, view, and theme.  No work
///          is performed until the first key event arrives, keeping construction
///          cheap so the widget can be instantiated eagerly.
/// @param buf Borrowed text buffer searched by the widget.
/// @param view Borrowed text view that receives highlights and cursor updates.
/// @param theme Borrowed theme used for painting; all three collaborators must
///        outlive the search bar.
SearchBar::SearchBar(text::TextBuffer &buf, views::TextView &view, const style::Theme &theme)
    : buf_(buf), view_(view), theme_(theme) {}

/// @brief Return the number of matches currently highlighted within the buffer.
/// @details Useful for status indicators showing how many results were located.
/// @return Count of active match ranges.
std::size_t SearchBar::matchCount() const {
    return matches_.size();
}

/// @brief Toggle regular-expression matching and refresh results.
/// @details Switching modes triggers a full rescan of the buffer so the match
///          list reflects the new semantics immediately.
/// @param regex True to interpret the query as a regular expression.
void SearchBar::setRegex(bool regex) {
    regex_ = regex;
    updateMatches();
}

/// @brief Recompute match ranges and update view highlights.
/// @details Delegates to `text::findAll` to locate matches, maps them into a
///          simple pair list consumed by the text view, and resets the active
///          match index.  When matches exist the cursor is moved to the first
///          occurrence so the user immediately sees the result.
void SearchBar::updateMatches() {
    matches_ = text::findAll(buf_, query_, regex_);
    std::vector<std::pair<std::size_t, std::size_t>> ranges;
    ranges.reserve(matches_.size());
    for (const auto &m : matches_) {
        ranges.emplace_back(m.start, m.length);
    }
    view_.setHighlights(std::move(ranges));
    current_ = 0;
    if (!matches_.empty()) {
        gotoMatch(0);
    }
}

/// @brief Focus the view on a specific match index.
/// @details Bounds-checks @p idx and, when valid, moves the view cursor to the
///          start of the requested match.  Out-of-range requests are ignored so
///          callers can pass unchecked values when cycling through matches.
/// @param idx Match to focus within the cached results.
void SearchBar::gotoMatch(std::size_t idx) {
    if (idx >= matches_.size()) {
        return;
    }
    view_.moveCursorToOffset(matches_[idx].start);
}

/// @brief Handle keyboard events that modify the query or navigate matches.
/// @details Recognises printable ASCII characters, backspace, Enter, and F3.
///          Printable characters append to the query; backspace removes the last
///          character when present; Enter and F3 advance to the next match
///          wrapping around the result set.  Every change triggers @ref
///          updateMatches() so the highlights stay current.
/// @param ev UI event containing the key input.
/// @return True when the event was consumed by the search bar.
bool SearchBar::onEvent(const ui::Event &ev) {
    using Code = KeyEvent::Code;
    if (ev.key.code == Code::Backspace) {
        if (!query_.empty()) {
            query_.pop_back();
            updateMatches();
        }
        return true;
    }
    if (ev.key.code == Code::Enter || ev.key.code == Code::F3) {
        if (!matches_.empty()) {
            current_ = (current_ + 1) % matches_.size();
            gotoMatch(current_);
        }
        return true;
    }
    if (ev.key.code == Code::Unknown && ev.key.codepoint >= 32 && ev.key.codepoint <= 126) {
        query_.push_back(static_cast<char>(ev.key.codepoint));
        updateMatches();
        return true;
    }
    return false;
}

/// @brief Paint the current query string into the screen buffer.
/// @details Prefixes the query with `/` (mirroring editor conventions) and fills
///          the remainder of the widget width with spaces while applying the
///          normal theme role.  Painting only affects a single row aligned with
///          the widget rectangle.
/// @param sb Screen buffer receiving the rendered characters.
void SearchBar::paint(render::ScreenBuffer &sb) {
    const auto &st = theme_.style(style::Role::Normal);
    std::string text = "/" + query_;
    for (int i = 0; i < rect_.w; ++i) {
        auto &cell = sb.at(rect_.y, rect_.x + i);
        if (i < static_cast<int>(text.size())) {
            cell.ch = static_cast<char32_t>(text[i]);
        } else {
            cell.ch = U' ';
        }
        cell.style = st;
    }
}

} // namespace zanna::tui::widgets
