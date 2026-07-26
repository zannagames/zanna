//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Declares a themed single-row status bar widget.
/// @details StatusBar owns independently updated left- and right-aligned text,
///          clips both to its width, and paints the full row using a borrowed
///          theme's accent style.
//
// Common uses include showing the current file name on the left and
// cursor position or mode indicator on the right. The bar fills its
// entire allocated width with the theme's accent style.
//
// Key invariants:
//   - The bar occupies exactly one row of its layout rectangle.
//   - Left and right text are truncated if they exceed available width.
//   - The bar does not accept focus (wantsFocus returns false by default).
//
// Ownership: StatusBar owns its text strings by value and borrows
// the Theme reference (must outlive the widget).
//
//===----------------------------------------------------------------------===//

#pragma once

#include <string>

#include "tui/style/theme.hpp"
#include "tui/ui/widget.hpp"

namespace zanna::tui::widgets {
/// @brief Single-line status display widget with left and right text segments.
/// @details Renders a horizontal bar spanning the full width of its layout rectangle,
///          displaying left-aligned text on the left and right-aligned text on the right.
///          Styled using the theme's accent role. Typically placed at the bottom of the
///          screen to show file information, cursor position, or mode indicators.
class StatusBar : public ui::Widget {
  public:
    /// @brief Construct status bar with initial texts.
    /// @param left Text shown on the left side.
    /// @param right Text shown on the right side.
    /// @param theme Theme providing colors.
    StatusBar(std::string left, std::string right, const style::Theme &theme);

    /// @brief Set text on the left segment.
    /// @param left Replacement text, moved into owned storage.
    void setLeft(std::string left);

    /// @brief Set text on the right segment.
    /// @param right Replacement text, moved into owned storage.
    void setRight(std::string right);

    /// @brief Paint status bar into screen buffer.
    /// @param sb Screen buffer receiving background fill and aligned text.
    void paint(render::ScreenBuffer &sb) override;

  private:
    std::string left_{};          ///< Owned left-aligned status text.
    std::string right_{};         ///< Owned right-aligned status text.
    const style::Theme &theme_;   ///< Borrowed render palette.
};

} // namespace zanna::tui::widgets
