//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tui/src/views/text_view_cursor.cpp
// Purpose: Manage cursor state, selections, and geometry helpers for the
//          terminal TextView widget.
// Key invariants: Cursor offsets remain within buffer bounds, and UTF-8
//                 decoding mirrors the rendering/input paths so navigation is
//                 consistent across the widget.
// Ownership/Lifetime: TextView borrows TextBuffer and Theme instances owned by
//                     the application; this file only mutates TextView's own
//                     bookkeeping fields.
// Links: docs/internals/architecture.md#zannatui-architecture
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements cursor-centric helpers for the TextView widget.
/// @details The routines here translate between byte offsets and display
///          columns, clamp scroll positions, and keep selection anchors in sync
///          with cursor motions. They are used by both rendering and input
///          handlers to provide a consistent editing experience.

#include "tui/views/text_view.hpp"

#include "tui/util/numeric.hpp"

#include <algorithm>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

using zanna::tui::util::char_width;
using zanna::tui::util::clampAdd;

namespace zanna::tui::views {

/// @brief Construct a TextView bound to a text buffer and theme.
/// @details Stores references to the backing @ref text::TextBuffer and
///          @ref style::Theme plus configuration flags such as line-number
///          visibility. Cursor state starts at the beginning of the buffer.
/// @param buf Text buffer rendered by the view.
/// @param theme Colour/style palette used when painting cells.
/// @param showLineNumbers Whether the gutter with line numbers is enabled.
TextView::TextView(text::TextBuffer &buf, const style::Theme &theme, bool showLineNumbers)
    : buf_(buf), theme_(theme), show_line_numbers_(showLineNumbers) {}

/// @brief Report whether the view wants keyboard focus.
/// @return Always true; TextView handles keyboard navigation and editing.
bool TextView::wantsFocus() const {
    return true;
}

/// @brief Retrieve the zero-based row containing the cursor.
/// @return Cursor row in visual coordinates.
std::size_t TextView::cursorRow() const {
    return cursor_row_;
}

/// @brief Retrieve the zero-based column containing the cursor.
/// @return Cursor column measured in terminal cell units.
std::size_t TextView::cursorCol() const {
    return cursor_col_;
}

/// @brief Decode a UTF-8 sequence at @p off and return its scalar value.
/// @details Mirrors the decoding logic used by the terminal input pipeline so
///          cursor navigation treats multibyte sequences consistently. Invalid
///          sequences return U+FFFD and advance by one byte, maintaining forward
///          progress.
/// @param s UTF-8 string view containing the encoded text.
/// @param off Starting byte offset within @p s.
/// @return Pair of decoded code point and number of bytes consumed.
std::pair<char32_t, std::size_t> TextView::decodeChar(std::string_view s, std::size_t off) {
    // UTF-8 decoding mirrors the terminal input pipeline: invalid sequences are
    // replaced with U+FFFD and treated as having a single cell of display width.
    if (off >= s.size())
        return {U'\uFFFD', 1};
    unsigned char c = static_cast<unsigned char>(s[off]);
    if (c < 0x80)
        return {c, 1};
    if ((c >> 5) == 0x6 && off + 1 < s.size())
        return {static_cast<char32_t>(((c & 0x1F) << 6) |
                                      (static_cast<unsigned char>(s[off + 1]) & 0x3F)),
                2};
    if ((c >> 4) == 0xE && off + 2 < s.size())
        return {static_cast<char32_t>(((c & 0x0F) << 12) |
                                      ((static_cast<unsigned char>(s[off + 1]) & 0x3F) << 6) |
                                      (static_cast<unsigned char>(s[off + 2]) & 0x3F)),
                3};
    if ((c >> 3) == 0x1E && off + 3 < s.size())
        return {static_cast<char32_t>(((c & 0x07) << 18) |
                                      ((static_cast<unsigned char>(s[off + 1]) & 0x3F) << 12) |
                                      ((static_cast<unsigned char>(s[off + 2]) & 0x3F) << 6) |
                                      (static_cast<unsigned char>(s[off + 3]) & 0x3F)),
                4};
    return {U'\uFFFD', 1};
}

/// @brief Compute the display width of a UTF-8 encoded line.
/// @details Iterates through the line decoding each code point and summing the
///          terminal column widths returned by @ref char_width. This allows the
///          view to clamp cursor columns and scroll offsets accurately.
/// @param line UTF-8 line slice sourced from the text buffer.
/// @return Total number of terminal columns required to display @p line.
std::size_t TextView::lineWidth(std::string_view line) {
    std::size_t i = 0;
    std::size_t c = 0;
    while (i < line.size()) {
        auto [cp, len] = decodeChar(line, i);
        c += char_width(cp);
        i += len;
    }
    return c;
}

/// @brief Translate a display column into a byte offset within a line.
/// @details Walks decoded code points until the accumulated display width would
///          exceed @p col, returning the byte index preceding that position.
///          This is used when the cursor moves horizontally to find the
///          corresponding byte offset inside the buffer.
/// @param line UTF-8 encoded line segment.
/// @param col Target visual column.
/// @return Byte offset within @p line nearest to @p col.
std::size_t TextView::columnToOffset(std::string_view line, std::size_t col) {
    std::size_t i = 0;
    std::size_t c = 0;
    while (i < line.size()) {
        auto [cp, len] = decodeChar(line, i);
        std::size_t w = static_cast<std::size_t>(char_width(cp));
        if (c + w > col)
            break;
        i += len;
        c += w;
    }
    return i;
}

/// @brief Convert a row/column coordinate into a buffer byte offset.
/// @details Uses the text buffer's segmented storage to locate the requested
///          line efficiently and then iterates through its UTF-8 data until the
///          desired visual column is reached. The routine clamps inputs to the
///          buffer size so callers can pass out-of-range coordinates safely.
/// @param row Zero-based row.
/// @param col Zero-based column in terminal cells.
/// @return Byte offset within the buffer representing @p row/@p col.
std::size_t TextView::offsetFromRowCol(std::size_t row, std::size_t col) const {
    const std::size_t total = buf_.lineCount();
    if (total == 0)
        return 0;
    if (row >= total)
        return buf_.size();

    auto line = buf_.lineView(row);
    std::size_t byteOffset = 0;
    std::size_t currentCol = 0;
    /// @brief Accumulate bytes until the requested visual column is reached.
    /// @param segment Contiguous UTF-8 segment of the selected line.
    /// @return `true` while subsequent segments may still contribute.
    line.forEachSegment([&](std::string_view segment) -> bool {
        std::size_t idx = 0;
        while (idx < segment.size()) {
            auto [cp, len] = decodeChar(segment, idx);
            if (len == 0)
                return false;
            std::size_t width = static_cast<std::size_t>(char_width(cp));
            if (currentCol + width > col)
                return false;
            idx += len;
            byteOffset += len;
            currentCol += width;
        }
        return true;
    });
    return line.offset() + byteOffset;
}

/// @brief Report the number of lines currently in the buffer.
/// @return Line count as reported by the backing text buffer.
std::size_t TextView::totalLines() const {
    return buf_.lineCount();
}

/// @brief Update the cursor position and manage selection anchors.
/// @details Writes the supplied row/column into TextView state, updates the
///          cached byte offset, and either extends the selection (when @p shift
///          is true) or collapses it to the new caret. When @p updateTarget is
///          true the "sticky" target column used for vertical navigation is
///          refreshed as well.
/// @param row New cursor row.
/// @param col New cursor column.
/// @param shift Whether the selection anchor should be preserved.
/// @param updateTarget Whether to update the sticky target column.
void TextView::setCursor(std::size_t row, std::size_t col, bool shift, bool updateTarget) {
    cursor_row_ = row;
    cursor_col_ = col;
    if (updateTarget)
        target_col_ = col;
    cursor_offset_ = offsetFromRowCol(row, col);
    if (shift)
        sel_end_ = cursor_offset_;
    else
        sel_start_ = sel_end_ = cursor_offset_;
}

/// @brief Replace the active highlight ranges.
/// @details Highlights are stored as (offset,length) pairs in absolute buffer
///          coordinates. The renderer consults this vector to accent spans such
///          as search results.
/// @param ranges New highlight ranges to adopt.
void TextView::setHighlights(std::vector<std::pair<std::size_t, std::size_t>> ranges) {
    highlights_ = std::move(ranges);
}

/// @brief Move the cursor to an absolute byte offset within the buffer.
/// @details Performs a binary search over line offsets to find the owning line
///          before translating the intra-line byte distance into a visual
///          column. Scrolling is adjusted to keep the caret inside the viewport
///          when possible.
/// @param off Absolute byte offset requested by the caller.
void TextView::moveCursorToOffset(std::size_t off) {
    const std::size_t size = buf_.size();
    const std::size_t clamped = std::min(off, size);
    const std::size_t total = buf_.lineCount();

    std::size_t row = 0;
    std::size_t rowStart = 0;

    if (total > 0) {
        std::size_t low = 0;
        std::size_t high = total;
        while (low < high) {
            std::size_t mid = (low + high) / 2;
            std::size_t start = buf_.lineOffset(mid);
            if (start <= clamped) {
                row = mid;
                rowStart = start;
                low = mid + 1;
            } else {
                high = mid;
            }
        }
        rowStart = buf_.lineOffset(row);

        std::size_t length = buf_.lineLength(row);
        if (row + 1 < total && clamped == clampAdd(rowStart, length)) {
            ++row;
            if (row >= total)
                row = total - 1;
            rowStart = buf_.lineOffset(row);
            length = buf_.lineLength(row);
        }

        std::size_t inLineOffset = clamped > rowStart ? clamped - rowStart : 0;
        if (inLineOffset > length)
            inLineOffset = length;

        std::size_t col = 0;
        std::size_t consumed = 0;
        auto line = buf_.lineView(row);
        /// @brief Accumulate display width up to the requested byte offset.
        /// @param segment Contiguous UTF-8 segment of the selected line.
        /// @return `true` while more bytes are needed from later segments.
        line.forEachSegment([&](std::string_view segment) -> bool {
            std::size_t idx = 0;
            while (idx < segment.size() && consumed < inLineOffset) {
                auto [cp, len] = decodeChar(segment, idx);
                if (len == 0)
                    return false;
                std::size_t advance = std::min(len, inLineOffset - consumed);
                consumed += advance;
                col += static_cast<std::size_t>(char_width(cp));
                idx += len;
                if (consumed >= inLineOffset)
                    break;
            }
            return consumed < inLineOffset;
        });

        setCursor(row, col, false, true);
    } else {
        setCursor(0, 0, false, true);
    }

    if (cursor_row_ < top_row_)
        top_row_ = cursor_row_;
    if (cursor_row_ >= top_row_ + static_cast<std::size_t>(rect_.h))
        top_row_ = cursor_row_ - static_cast<std::size_t>(rect_.h) + 1;
}

/// @brief Attach syntax highlighting metadata to the view.
/// @details The renderer consults the rule set to colourize buffer contents.
///          Passing nullptr disables syntax highlighting.
/// @param syntax Rule set owned by the caller; may be nullptr.
void TextView::setSyntax(syntax::SyntaxRuleSet *syntax) {
    syntax_ = syntax;
}

} // namespace zanna::tui::views
