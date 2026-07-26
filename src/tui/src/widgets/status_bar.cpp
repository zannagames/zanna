//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Implements the status bar widget responsible for rendering left- and
// right-aligned messages on the bottom line of the terminal UI.  The widget is
// intentionally lightweight so it can be reused by different application modes
// without incurring extra allocations on each frame.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements the status bar widget used in the Zanna TUI.
/// @details The widget paints a horizontal strip using the active theme and
///          displays independent left and right strings.  It clears the row
///          before drawing text so previous frames do not leak visual artefacts.

#include "tui/widgets/status_bar.hpp"
#include "tui/render/text.hpp"

#include <algorithm>

namespace zanna::tui::widgets {
/// @brief Construct a status bar with initial left and right text segments.
/// @details Stores references to the immutable theme while taking ownership of
///          the text strings.  The constructor does not perform any rendering;
///          paint operations occur later via @ref paint.
/// @param left Initial left-aligned message, moved into the widget.
/// @param right Initial right-aligned message, moved into the widget.
/// @param theme Borrowed theme that must outlive the status bar.
StatusBar::StatusBar(std::string left, std::string right, const style::Theme &theme)
    : left_(std::move(left)), right_(std::move(right)), theme_(theme) {}

/// @brief Replace the left-hand message displayed by the status bar.
/// @details The by-value argument is moved into widget storage.
/// @param left New left-aligned message.
void StatusBar::setLeft(std::string left) {
    left_ = std::move(left);
}

/// @brief Replace the right-hand message displayed by the status bar.
/// @details Mirrors @ref setLeft while targeting the right-aligned segment.
/// @param right New right-aligned message.
void StatusBar::setRight(std::string right) {
    right_ = std::move(right);
}

/// @brief Paint the status bar into the supplied screen buffer.
/// @details Clears the target row, writes the left string starting at the
///          widget's origin, and paints the right string aligned to the widget's
///          far edge.  Strings longer than the available space are clipped to
///          avoid wrapping artefacts.
/// @param sb Screen buffer that receives the cleared row and both messages.
void StatusBar::paint(render::ScreenBuffer &sb) {
    const auto &st = theme_.style(style::Role::Normal);
    int y = rect_.y + rect_.h - 1;
    // Clear the row with spaces
    sb.fillRect(rect_.x, y, rect_.w, 1, U' ', &st);
    // Render left-aligned text
    render::renderText(sb, y, rect_.x, rect_.w, left_, st);
    // Render right-aligned text
    render::renderTextRight(sb, y, rect_.x, rect_.w, right_, st);
}

} // namespace zanna::tui::widgets
