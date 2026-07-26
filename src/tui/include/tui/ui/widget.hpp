//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Declares the base TUI widget contract and rectangle layout primitive.
/// @details Widget defines layout, paint, input, and focus hooks for visual tree
///          nodes, following a parent-driven layout-before-paint lifecycle.
//
// The widget lifecycle follows a layout-then-paint pattern: the parent calls
// layout() to assign the widget's rectangle, then paint() to render content
// into a screen buffer. Input events arrive via onEvent() when the widget
// has focus, and wantsFocus() controls whether the widget participates in
// the focus ring.
//
// Key invariants:
//   - layout() must be called before paint() for correct rendering.
//   - The rect_ member reflects the last layout() call.
//   - Subclasses should call Widget::layout() to store the rectangle.
//   - wantsFocus() defaults to false; focusable widgets must override.
//
// Ownership: Widget is a polymorphic base class. Containers own their
// children via unique_ptr. Widget itself holds no heap resources.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "tui/ui/event.hpp"

namespace zanna::tui::render {
class ScreenBuffer;
}

namespace zanna::tui::ui {
/// @brief Axis-aligned rectangle used for widget layout and hit testing.
/// @details Stores position (x, y) and dimensions (w, h) in terminal cell
///          coordinates. Used by the layout system to assign screen regions
///          to widgets.
struct Rect {
    int x{0}; ///< Zero-based left column.
    int y{0}; ///< Zero-based top row.
    int w{0}; ///< Width in terminal columns.
    int h{0}; ///< Height in terminal rows.
};

/// @brief Abstract base class for all visual elements in the TUI widget tree.
/// @details Defines the core interface for layout computation, screen painting,
///          input event handling, and focus management. Subclasses implement
///          specific visual behaviors (buttons, text views, containers, etc.).
///          Widgets are organized in a tree hierarchy and rendered in depth-first
///          order by the App's render loop.
class Widget {
  public:
    /// @brief Destroy a widget polymorphically.
    virtual ~Widget() = default;

    /// @brief Set widget bounds and layout children.
    /// @param r Rectangle assigned by the parent layout.
    virtual void layout(const Rect &r);

    /// @brief Paint widget contents into a screen buffer.
    /// @param sb Mutable render target covering the application screen.
    virtual void paint(render::ScreenBuffer &sb);

    /// @brief Handle an input event.
    /// @param ev Routed event to inspect.
    /// @return True if the event was consumed.
    virtual bool onEvent(const Event &ev);

    /// @brief Whether this widget can receive focus.
    /// @return true for focus-ring participation; false by default.
    [[nodiscard]] virtual bool wantsFocus() const;

    /// @brief Notifies widget when it gains or loses input focus.
    /// Default implementation is a no-op.
    /// @param focused true after gaining focus; false after losing it.
    virtual void onFocusChanged(bool focused);

    /// @brief Retrieve widget rectangle.
    /// @return Rectangle stored by the most recent layout call.
    [[nodiscard]] Rect rect() const;

  protected:
    Rect rect_{}; ///< Bounds assigned by the most recent layout pass.
};

} // namespace zanna::tui::ui
