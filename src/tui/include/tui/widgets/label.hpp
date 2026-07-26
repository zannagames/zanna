//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Declares a single-line, read-only TUI label widget.
/// @details Label paints owned text at the top-left of its bounds with a
///          borrowed theme's normal style and does not participate in focus.
//
// Labels are non-focusable and do not handle input events. They are
// commonly used for static text, headings, or descriptive captions
// within container layouts.
//
// Key invariants:
//   - Labels do not accept focus (wantsFocus returns false).
//   - Text is rendered at the top-left of the layout rectangle.
//   - Text exceeding the widget width is truncated (no wrapping).
//
// Ownership: Label owns its text string by value and borrows the
// Theme reference (must outlive the widget).
//
//===----------------------------------------------------------------------===//

#pragma once

#include <string>

#include "tui/style/theme.hpp"
#include "tui/ui/widget.hpp"

namespace zanna::tui::widgets {

/// @brief Simple read-only text display widget.
/// @details Renders a single line of text at the top-left of its layout rectangle
///          using the theme's normal style. Non-focusable and non-interactive.
///          Used for static labels, headings, and descriptive text in the UI.
class Label : public ui::Widget {
  public:
    /// @brief Construct label with text and theme.
    /// @param text Text to display.
    /// @param theme Theme providing colors.
    explicit Label(std::string text, const style::Theme &theme);

    /// @brief Paint text into the screen buffer.
    /// @param sb Screen buffer receiving clipped label text.
    void paint(render::ScreenBuffer &sb) override;

  private:
    std::string text_{};        ///< Owned label text.
    const style::Theme &theme_; ///< Borrowed render palette.
};

} // namespace zanna::tui::widgets
