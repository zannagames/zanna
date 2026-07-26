//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// tui/src/widgets/button.cpp
//
// Implements a basic push button widget for the terminal UI toolkit.  The
// widget renders a bordered rectangle, centres its label text, and invokes a
// caller-supplied callback when activated via keyboard.  It relies on the
// global theme palette to colour the border and text, keeping visuals consistent
// across the application without embedding styling decisions here.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements a themed, keyboard-activatable push button widget.
/// @details Buttons own their label and activation callback, borrow a theme,
///          paint a bordered control, and participate in keyboard focus
///          traversal.

#include "tui/widgets/button.hpp"

#include "tui/render/box.hpp"
#include "tui/render/text.hpp"

#include <algorithm>

namespace zanna::tui::widgets {
/// @brief Construct a button with label text, callback, and theme reference.
///
/// @details The label is stored by value while the click handler and theme are
///          retained as an owned callable and a borrowed reference,
///          respectively.  Callbacks can be empty, in which case activation
///          simply performs no action.
/// @param text Label displayed inside the button border.
/// @param onClick Callback invoked for Enter or Space activation.
/// @param theme Borrowed theme that must outlive the button.
Button::Button(std::string text, OnClick onClick, const style::Theme &theme)
    : text_(std::move(text)), onClick_(std::move(onClick)), theme_(theme) {}

/// @brief Paint the button's border, fill, and label text into the screen buffer.
///
/// @details The routine first queries the theme for accent and normal styles,
///          then draws a rectangular border using ASCII characters.  Interior
///          cells are cleared to spaces with the normal style applied.  When the
///          height allows, the label text is centred vertically and truncated to
///          fit horizontally.  All drawing respects the widget's layout
///          rectangle, ensuring compatibility with container-managed geometry.
/// @param sb Screen buffer that receives the button border, fill, and label.
void Button::paint(render::ScreenBuffer &sb) {
    const auto &border = theme_.style(style::Role::Accent);
    const auto &txt = theme_.style(style::Role::Normal);

    int x0 = rect_.x;
    int y0 = rect_.y;
    int w = rect_.w;
    int h = rect_.h;

    // Draw bordered box with styled fill
    render::drawBox(sb, x0, y0, w, h, &border, &txt, true);

    // Minimum height of 3 required to render text inside the border.
    if (h >= 3) {
        // Text centered vertically while staying inside the border.
        int row = std::clamp(y0 + h / 2, y0 + 1, y0 + h - 2);
        render::renderText(sb, row, x0 + 1, w - 2, text_, txt);
    }
}

/// @brief Handle key events that should trigger the button's onClick callback.
///
/// @details The widget reacts to Enter and Space activations.  When a callback
///          is registered it is invoked immediately, and the event is reported as
///          handled.  Other keys fall through so the event system can continue
///          propagation to other widgets if needed.
/// @param ev Input event to inspect for an activation key.
/// @return @c true for Enter or Space, regardless of whether a callback is
///         registered; otherwise @c false.
bool Button::onEvent(const ui::Event &ev) {
    const auto &k = ev.key;
    if (k.code == term::KeyEvent::Code::Enter || k.codepoint == U' ') {
        if (onClick_) {
            onClick_();
        }
        return true;
    }
    return false;
}

/// @brief Request focus participation so activation keys reach the widget.
///
/// @details Buttons need focus to receive keyboard events, so the method
///          returns @c true.  Containers consult this when building traversal
///          order, ensuring that interactive controls behave as expected.
/// @return Always @c true.
bool Button::wantsFocus() const {
    return true;
}

} // namespace zanna::tui::widgets
