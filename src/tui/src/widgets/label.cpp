//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Implements the simplest text widget used by the terminal UI: a static label
// that renders a string using the active theme.  The code lives here instead of
// inline in the header so future enhancements (alignment, wrapping, ellipsis)
// can be added without expanding the header surface.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Plain-text label widget implementation.
/// @details Labels render their contents directly into a @ref ScreenBuffer using
///          the role provided by the bound theme.  The implementation is small
///          but documented thoroughly to serve as an example for other widgets.

#include "tui/widgets/label.hpp"
#include "tui/render/text.hpp"

namespace zanna::tui::widgets {

/// @brief Construct a label with static text and a borrowed theme.
/// @details Stores the provided string by value and keeps a reference to the
///          theme so later paint calls can query style roles.  Labels remain
///          lightweight so they can be sprinkled liberally throughout layouts.
/// @param text Text displayed by the label.
/// @param theme Borrowed theme that must outlive the label.
Label::Label(std::string text, const style::Theme &theme) : text_(std::move(text)), theme_(theme) {}

/// @brief Render the label contents into the supplied screen buffer.
/// @details Uses the renderText utility to paint the text, clipping to the
///          widget rectangle.  Characters beyond the available width are truncated
///          so the widget never wraps implicitly.
/// @param sb Screen buffer that receives the clipped label text.
void Label::paint(render::ScreenBuffer &sb) {
    const auto &st = theme_.style(style::Role::Normal);
    render::renderText(sb, rect_.y, rect_.x, rect_.w, text_, st);
}

} // namespace zanna::tui::widgets
