//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Declares the primary editable text view for Zanna TUI.
/// @details TextView renders a borrowed TextBuffer with cursor navigation,
///          selection, scrolling, optional line numbers, syntax spans, and
///          arbitrary highlighted byte ranges.
//
// TextView is a Widget subclass that binds to a TextBuffer (non-owning
// reference) and a Theme. It handles keyboard events for cursor movement
// (arrows, Home, End, Page Up/Down) and selection (Shift+arrow). The view
// maintains a viewport (top_row_) that scrolls to keep the cursor visible.
//
// Optional features:
//   - Line numbers: rendered in a left gutter when showLineNumbers is true.
//   - Syntax highlighting: applied via an attached SyntaxRuleSet pointer.
//   - Match highlighting: byte ranges set via setHighlights() are rendered
//     with the selection style.
//
// Key invariants:
//   - The cursor position (cursor_row_, cursor_col_) is always within valid
//     buffer bounds.
//   - The viewport scrolls automatically to keep the cursor visible.
//   - Selection is tracked as a byte range (sel_start_, sel_end_).
//
// Ownership: TextView borrows the TextBuffer, Theme, and optional
// SyntaxRuleSet by reference/pointer. All must outlive the TextView.
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstddef>
#include <string_view>
#include <utility>
#include <vector>

#include "tui/render/screen.hpp"
#include "tui/style/theme.hpp"
#include "tui/text/text_buffer.hpp"
#include "tui/ui/widget.hpp"
#include "tui/util/unicode.hpp"

namespace zanna::tui::syntax {
class SyntaxRuleSet;
}

namespace zanna::tui::views {

/// @brief Interactive text editing view bound to a TextBuffer.
/// @details Renders buffer content with optional line numbers, syntax
///          highlighting, and match highlighting. Handles keyboard navigation,
///          selection, and scrolling. Designed to be embedded in containers
///          and managed by the App's focus system.
class TextView : public ui::Widget {
  public:
    /// @brief Construct a TextView bound to a text buffer and theme.
    /// @param buf Text buffer to display and edit. Must outlive the view.
    /// @param theme Theme providing color styles. Must outlive the view.
    /// @param showLineNumbers When true, render line numbers in a left gutter.
    TextView(text::TextBuffer &buf, const style::Theme &theme, bool showLineNumbers = false);

    /// @brief Paint visible lines into the screen buffer.
    /// @details Renders the viewport region of the text buffer, applying
    ///          syntax highlighting, selection highlighting, and optional
    ///          line numbers. Scrolls the viewport to keep the cursor visible.
    /// @param sb Screen buffer to paint into.
    void paint(render::ScreenBuffer &sb) override;

    /// @brief Handle navigation and editing key events.
    /// @details Processes arrow keys, Home/End, Page Up/Down, and their
    ///          Shift variants for selection. Also handles character insertion
    ///          and deletion keys.
    /// @param ev Input event to handle.
    /// @return True if the event was consumed.
    bool onEvent(const ui::Event &ev) override;

    /// @brief TextView always wants focus for text editing.
    /// @return Always true.
    [[nodiscard]] bool wantsFocus() const override;

    /// @brief Get the current cursor row (0-based line number).
    /// @return Cursor row index.
    [[nodiscard]] std::size_t cursorRow() const;

    /// @brief Get the current cursor column in display cells (0-based).
    /// @return Cursor column index.
    [[nodiscard]] std::size_t cursorCol() const;

    /// @brief Set byte ranges to highlight (e.g., search matches).
    /// @details Each range is a (start, length) pair. Ranges are rendered
    ///          using the selection style from the theme.
    /// @param ranges Vector of (offset, length) pairs to highlight.
    void setHighlights(std::vector<std::pair<std::size_t, std::size_t>> ranges);

    /// @brief Move the cursor to a specific byte offset in the buffer.
    /// @details Updates cursor_row_ and cursor_col_ to match the given offset
    ///          and scrolls the viewport if necessary.
    /// @param off Byte offset to move the cursor to.
    void moveCursorToOffset(std::size_t off);

    /// @brief Attach a syntax rule set for source code highlighting.
    /// @details When set, syntax spans are computed per visible line during
    ///          paint(). Pass nullptr to disable syntax highlighting.
    /// @param syntax Pointer to a SyntaxRuleSet. Borrowed; must outlive the view.
    void setSyntax(syntax::SyntaxRuleSet *syntax);

  private:
    text::TextBuffer &buf_;             ///< Borrowed editable document.
    const style::Theme &theme_;         ///< Borrowed render palette.
    bool show_line_numbers_{};          ///< Whether to reserve and paint a line gutter.
    syntax::SyntaxRuleSet *syntax_{nullptr}; ///< Optional borrowed syntax rules.

    std::size_t cursor_row_{0}; ///< Zero-based logical cursor line.
    std::size_t cursor_col_{0}; ///< Zero-based display-cell cursor column.
    std::size_t target_col_{0}; ///< Preferred column preserved across vertical movement.
    std::size_t top_row_{0};    ///< First logical line visible in the viewport.

    std::size_t sel_start_{0};    ///< First selection byte boundary.
    std::size_t sel_end_{0};      ///< Second selection byte boundary.
    std::size_t cursor_offset_{0}; ///< Logical cursor byte offset.

    std::vector<std::pair<std::size_t, std::size_t>> highlights_{}; ///< Extra byte ranges.

    // helpers
    /// @brief Decode one UTF-8 scalar beginning at a byte offset.
    /// @param s UTF-8 text containing the character.
    /// @param off Starting byte offset.
    /// @return Code point and number of consumed bytes.
    static std::pair<char32_t, std::size_t> decodeChar(std::string_view s, std::size_t off);

    /// @brief Measure a UTF-8 line in terminal display cells.
    /// @param line Line bytes excluding a newline.
    /// @return Sum of decoded Unicode display widths.
    static std::size_t lineWidth(std::string_view line);

    /// @brief Convert a display-cell column into a UTF-8 byte offset.
    /// @param line Line bytes excluding a newline.
    /// @param col Target display column.
    /// @return Nearest byte boundary at or before/after the requested column.
    static std::size_t columnToOffset(std::string_view line, std::size_t col);

    /// @brief Convert a logical row and display column to a buffer byte offset.
    /// @param row Zero-based logical line.
    /// @param col Zero-based display column.
    /// @return Clamped logical byte offset.
    std::size_t offsetFromRowCol(std::size_t row, std::size_t col) const;

    /// @brief Return the number of logical lines in the bound buffer.
    /// @return At least one.
    std::size_t totalLines() const;

    /// @brief Update the cursor, selection, preferred column, and viewport.
    /// @param row Requested logical row.
    /// @param col Requested display column.
    /// @param shift Whether to extend the active selection.
    /// @param updateTarget Whether to replace the preferred vertical-movement column.
    void setCursor(std::size_t row, std::size_t col, bool shift, bool updateTarget);
};

} // namespace zanna::tui::views
