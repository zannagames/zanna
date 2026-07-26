//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Declares a themed, keyboard-activated button widget.
/// @details Button paints a centered label inside a border and invokes an
///          optional owned callback when Enter is pressed while focused.
//
// The button renders its label text centered within a bordered rectangle
// using theme-appropriate styles. When the user presses Enter while the
// button has focus, the registered onClick callback is invoked.
//
// Key invariants:
//   - The button is always focusable (wantsFocus returns true).
//   - The onClick callback may be empty (activation is a no-op).
//   - The border consumes 1 cell on each side of the button area.
//
// Ownership: Button owns its label string and callback by value.
// The Theme reference is borrowed (must outlive the widget).
//
//===----------------------------------------------------------------------===//

#pragma once

#include <functional>
#include <string>

#include "tui/style/theme.hpp"
#include "tui/ui/widget.hpp"

namespace zanna::tui::widgets {

/// @brief Interactive button widget with border, label, and activation callback.
/// @details Renders a bordered rectangle with centered label text. When focused,
///          responds to Enter/Return key presses by invoking the onClick callback.
///          Styled using the theme's normal and accent roles for unfocused and
///          focused states respectively.
class Button : public ui::Widget {
  public:
    /// @brief Callback invoked when the button is activated.
    /// @details Takes no arguments and returns no value.
    using OnClick = std::function<void()>;

    /// @brief Construct button.
    /// @param text Button label text.
    /// @param onClick Callback invoked on activation.
    /// @param theme Theme providing styles.
    Button(std::string text, OnClick onClick, const style::Theme &theme);

    /// @brief Paint button with border and label.
    /// @param sb Screen buffer receiving border, fill, and centered text.
    void paint(render::ScreenBuffer &sb) override;

    /// @brief Handle key events for activation.
    /// @param ev Input event to inspect for Enter.
    /// @return True if event consumed.
    bool onEvent(const ui::Event &ev) override;

    /// @brief Buttons want focus to receive input.
    /// @return Always true.
    [[nodiscard]] bool wantsFocus() const override;

  private:
    std::string text_{};          ///< Owned label text.
    OnClick onClick_{};           ///< Optional owned activation callback.
    const style::Theme &theme_;   ///< Borrowed render palette.
};

} // namespace zanna::tui::widgets
