//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Declares horizontal and vertical two-pane splitter widgets.
/// @details Splitters own two children, clamp proportional allocation to
///          [0.05, 0.95], and support keyboard adjustment through shared CRTP
///          paint and event forwarding.
//
// HSplitter splits horizontally (left | right) and VSplitter splits
// vertically (top | bottom). Both use a ratio parameter (0.0 to 1.0) to
// control the proportional division of space, clamped to [0.05, 0.95] to
// prevent either child from being completely hidden.
//
// Splitter widgets support keyboard-driven ratio adjustment: pressing
// the appropriate arrow keys shifts the split position. The SplitterBase
// CRTP template provides shared painting and event dispatch logic.
//
// Key invariants:
//   - The ratio is clamped to [0.05, 0.95] on every adjustment.
//   - Both children receive layout() calls during the parent's layout.
//   - Paint order is first child then second child (no z-ordering).
//
// Ownership: Each splitter owns its two child widgets via unique_ptr.
//
//===----------------------------------------------------------------------===//

#pragma once

#include <memory>

#include "tui/ui/widget.hpp"

namespace zanna::tui::widgets {

namespace detail {
/// @brief Clamp splitter ratios to a practical interactive range.
/// @details Prevents callers from collapsing a child entirely (less than 5%) or
///          hiding the opposite child (greater than 95%).
/// @param r Requested ratio from user input.
/// @return Ratio snapped into the inclusive [0.05, 0.95] interval.
inline float clampRatio(float r) {
    if (r < 0.05F)
        return 0.05F;
    if (r > 0.95F)
        return 0.95F;
    return r;
}
} // namespace detail

/// @brief CRTP base class providing shared logic for horizontal and vertical splitters.
/// @details Implements common paint (delegates to both children) and event dispatch
///          (forwards to the derived class's onKeyEvent handler). The ratio_ member
///          controls proportional space allocation between the two children.
/// @tparam Derived The concrete splitter type (HSplitter or VSplitter) for static dispatch.
template <typename Derived> class SplitterBase : public ui::Widget {
  public:
    /// @brief Paint both child widgets into the provided screen buffer.
    /// @param sb Screen buffer passed to each existing child in order.
    void paint(render::ScreenBuffer &sb) override {
        auto &self = static_cast<Derived &>(*this);
        if (self.first_)
            self.first_->paint(sb);
        if (self.second_)
            self.second_->paint(sb);
    }

    /// @brief Bridge generic UI events to the derived class key handler.
    /// @param ev Routed UI event.
    /// @return Derived key-handler consumption result.
    bool onEvent(const ui::Event &ev) override {
        return static_cast<Derived &>(*this).onKeyEvent(ev.key);
    }

  protected:
    float ratio_{0.5F}; ///< Fraction assigned to the first child.
};

/// @brief Horizontal splitter dividing its area into left and right child regions.
/// @details The ratio parameter controls what fraction of the total width is allocated
///          to the left child. The right child receives the remainder. Keyboard input
///          (Left/Right arrow keys) adjusts the split ratio interactively.
class HSplitter : public SplitterBase<HSplitter> {
  public:
    /// @brief Construct horizontal splitter.
    /// @param left Widget placed on the left side.
    /// @param right Widget placed on the right side.
    /// @param ratio Fraction [0,1] of width for the left widget.
    HSplitter(std::unique_ptr<ui::Widget> left, std::unique_ptr<ui::Widget> right, float ratio);

    /// @brief Layout children within given rectangle using ratio.
    /// @param r Full splitter bounds.
    void layout(const ui::Rect &r) override;

    /// @brief Handle keyboard events for adjusting split ratio.
    /// @param ev Key event to inspect for Left/Right adjustment.
    /// @return true when an adjustment key was consumed.
    bool onKeyEvent(const zanna::tui::term::KeyEvent &ev);

  private:
    friend class SplitterBase<HSplitter>;
    std::unique_ptr<ui::Widget> first_{};  ///< Owned left child.
    std::unique_ptr<ui::Widget> second_{}; ///< Owned right child.
};

/// @brief Vertical splitter dividing its area into top and bottom child regions.
/// @details The ratio parameter controls what fraction of the total height is allocated
///          to the top child. The bottom child receives the remainder. Keyboard input
///          (Up/Down arrow keys) adjusts the split ratio interactively.
class VSplitter : public SplitterBase<VSplitter> {
  public:
    /// @brief Construct vertical splitter.
    /// @param top Widget placed at the top.
    /// @param bottom Widget placed at the bottom.
    /// @param ratio Fraction [0,1] of height for the top widget.
    VSplitter(std::unique_ptr<ui::Widget> top, std::unique_ptr<ui::Widget> bottom, float ratio);

    /// @brief Layout children within given rectangle using ratio.
    /// @param r Full splitter bounds.
    void layout(const ui::Rect &r) override;

    /// @brief Handle keyboard events for adjusting split ratio.
    /// @param ev Key event to inspect for Up/Down adjustment.
    /// @return true when an adjustment key was consumed.
    bool onKeyEvent(const zanna::tui::term::KeyEvent &ev);

  private:
    friend class SplitterBase<VSplitter>;
    std::unique_ptr<ui::Widget> first_{};  ///< Owned top child.
    std::unique_ptr<ui::Widget> second_{}; ///< Owned bottom child.
};

} // namespace zanna::tui::widgets
