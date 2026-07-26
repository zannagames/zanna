//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Implements horizontal and vertical splitter widgets for the Zanna TUI. The
// widgets divide their assigned rectangle between two child widgets based on a
// persisted ratio that can be tweaked interactively with Ctrl+arrow key input.
// Each splitter honours the `ui::Widget` lifecycle by recomputing child layouts
// whenever the parent rectangle changes and by delegating painting and event
// dispatch to its children.  This translation unit focuses purely on runtime
// behaviour; view-specific styling is delegated to the nested widgets.
//
// Invariants:
//   * Keyboard adjustments clamp the stored ratio between 5% and 95%; an
//     arbitrary constructor ratio is tolerated because layout clamps the
//     resulting child extent to the available rectangle.
//   * Layout propagation maintains the splitter's rectangle so repainting uses
//     the same geometry observed during event handling.
// Ownership:
//   * Splitter instances take ownership of their child widgets via
//     `std::unique_ptr` and destroy them alongside the splitter itself.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Defines horizontal and vertical splitter widget implementations.
/// @details The helpers here compute child rectangles, forward paint requests,
///          and adjust ratios when keyboard events are received.  Keeping the
///          implementation in a single unit avoids duplication between the two
///          orientations while retaining straightforward control flow.

#include "tui/widgets/splitter.hpp"
#include "tui/render/screen.hpp"

#include <algorithm>

namespace zanna::tui::widgets {

//=============================================================================
// HSplitter Implementation
//=============================================================================

/// @brief Create a horizontal splitter with the given children and initial ratio.
/// @details Transfers ownership of @p left and @p right into the splitter and
///          stores the requested division ratio without clamping.  The first
///          call to @ref layout() computes the actual child rectangles based on
///          the available width.
/// @param left First child, placed on the left; ownership transfers to the
///        splitter.
/// @param right Second child, placed on the right; ownership transfers to the
///        splitter.
/// @param ratio Initial fraction of the width requested for @p left.
HSplitter::HSplitter(std::unique_ptr<ui::Widget> left,
                     std::unique_ptr<ui::Widget> right,
                     float ratio)
    : first_(std::move(left)), second_(std::move(right)) {
    ratio_ = ratio;
}

/// @brief Distribute the assigned rectangle between the splitter children.
/// @details The helper multiplies the total width by the stored ratio, clamps
///          the result within the parent's bounds, and recomputes child
///          rectangles before forwarding the layout call to each present child.
///          Using the parent's rectangle ensures resize events keep the layout
///          consistent with the geometry used for painting.
/// @param r Rectangle to divide horizontally between the two children.
void HSplitter::layout(const ui::Rect &r) {
    Widget::layout(r);
    int leftW = static_cast<int>(static_cast<float>(r.w) * ratio_);
    leftW = std::clamp(leftW, 0, r.w);
    int rightW = r.w - leftW;
    ui::Rect lr{r.x, r.y, leftW, r.h};
    ui::Rect rr{r.x + leftW, r.y, rightW, r.h};
    if (first_)
        first_->layout(lr);
    if (second_)
        second_->layout(rr);
}

/// @brief Handle keyboard input for resizing the horizontal splitter.
/// @details Reacts to Ctrl+Left and Ctrl+Right by nudging the ratio in 5%
///          increments, clamps the stored value, and triggers a relayout of the
///          cached rectangle.  Other events are ignored so focus traversal and
///          child handling continue to work as normal.
/// @param ev Key event describing the user input.
/// @return True when the ratio changed and a relayout occurred.
bool HSplitter::onKeyEvent(const zanna::tui::term::KeyEvent &ev) {
    using zanna::tui::term::KeyEvent;
    if ((ev.mods & KeyEvent::Ctrl) == 0)
        return false;

    bool changed = false;
    if (ev.code == KeyEvent::Code::Left) {
        ratio_ = detail::clampRatio(ratio_ - 0.05F);
        changed = true;
    }
    if (ev.code == KeyEvent::Code::Right) {
        ratio_ = detail::clampRatio(ratio_ + 0.05F);
        changed = true;
    }
    if (!changed)
        return false;

    layout(rect_);
    return true;
}

//=============================================================================
// VSplitter Implementation
//=============================================================================

/// @brief Construct a vertical splitter with the supplied child widgets.
/// @details Ownership is transferred to the splitter and the starting ratio is
///          stored verbatim.  Layout clamps the resulting top-child height to
///          the available rectangle, while keyboard adjustments clamp future
///          ratio values to the interactive range.
/// @param top First child, placed above the divider; ownership transfers to the
///        splitter.
/// @param bottom Second child, placed below the divider; ownership transfers to
///        the splitter.
/// @param ratio Initial fraction of the height requested for @p top.
VSplitter::VSplitter(std::unique_ptr<ui::Widget> top,
                     std::unique_ptr<ui::Widget> bottom,
                     float ratio)
    : first_(std::move(top)), second_(std::move(bottom)) {
    ratio_ = ratio;
}

/// @brief Split the rectangle into stacked child regions and propagate layout.
/// @details Calculates the terminal-row height for the top child from the ratio,
///          clamps it within bounds, and assigns the remainder to the bottom
///          child before invoking their respective @ref layout() methods.
/// @param r Rectangle to divide vertically between the two children.
void VSplitter::layout(const ui::Rect &r) {
    Widget::layout(r);
    int topH = static_cast<int>(static_cast<float>(r.h) * ratio_);
    topH = std::clamp(topH, 0, r.h);
    int bottomH = r.h - topH;
    ui::Rect tr{r.x, r.y, r.w, topH};
    ui::Rect br{r.x, r.y + topH, r.w, bottomH};
    if (first_)
        first_->layout(tr);
    if (second_)
        second_->layout(br);
}

/// @brief Handle keyboard input for resizing the vertical splitter.
/// @details Mirrors the horizontal behaviour but maps Ctrl+Up and Ctrl+Down to
///          ratio adjustments.  Successful updates recompute child layouts so
///          painting reflects the new heights immediately.
/// @param ev Key event describing the user input.
/// @return True when the ratio changed and children were relaid out.
bool VSplitter::onKeyEvent(const zanna::tui::term::KeyEvent &ev) {
    using zanna::tui::term::KeyEvent;
    if ((ev.mods & KeyEvent::Ctrl) == 0)
        return false;

    bool changed = false;
    if (ev.code == KeyEvent::Code::Up) {
        ratio_ = detail::clampRatio(ratio_ - 0.05F);
        changed = true;
    }
    if (ev.code == KeyEvent::Code::Down) {
        ratio_ = detail::clampRatio(ratio_ + 0.05F);
        changed = true;
    }
    if (!changed)
        return false;

    layout(rect_);
    return true;
}

} // namespace zanna::tui::widgets
