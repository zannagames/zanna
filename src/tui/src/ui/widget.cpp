//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// tui/src/ui/widget.cpp
//
// This translation unit provides the default behaviour for the base widget
// interface used by the terminal UI toolkit.  The definitions here establish
// a minimal contract for layout, painting, event handling, and focus so that
// derived widgets can override only the pieces they need.  No global state is
// touched, keeping the primitives straightforward to embed inside higher-level
// containers and applications.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements the default layout, rendering, input, and focus behavior
///        of the TUI widget base class.
/// @details These definitions provide intentionally minimal hooks so derived
///          widgets can override only the lifecycle operations they need.

#include "tui/ui/widget.hpp"

namespace zanna::tui::ui {
/// @brief Record the layout rectangle supplied by a parent container.
///
/// @details The default implementation simply stores the rectangle and does
///          not perform any additional work.  Containers call this when they
///          assign screen real estate to the widget, allowing derived classes
///          to reference @ref rect_ when painting or dispatching events.  The
///          base implementation keeps the behaviour predictable for widgets
///          that do not need custom layout logic.
/// @param r Rectangle assigned by the parent layout.
void Widget::layout(const Rect &r) {
    rect_ = r;
}

/// @brief Render the widget's contents into the provided screen buffer.
///
/// @details The base implementation intentionally does nothing, acting as a
///          sentinel for widgets that only manage layout or input.  Derived
///          classes override this to draw characters and styles into the
///          @ref render::ScreenBuffer passed from the compositor.  Keeping the
///          default empty avoids accidental double-painting when intermediate
///          classes forget to call into their base.
/// @param sb Screen buffer supplied by the compositor; unused by the base
///        implementation.
void Widget::paint(render::ScreenBuffer &) {}

/// @brief Handle an input event directed at this widget.
///
/// @details The default handler consumes nothing, returning @c false to signal
///          that the event should continue bubbling to other widgets.  Widgets
///          that respond to keyboard or mouse input override this method to
///          implement the required behaviour while using the return value to
///          communicate whether the event stream should stop propagating.
/// @param ev Routed event; ignored by the base implementation.
/// @return Always @c false so the event may continue propagating.
bool Widget::onEvent(const Event &) {
    return false;
}

/// @brief Indicate whether the widget is interested in receiving focus.
///
/// @details Returning @c false opt-outs the widget from focus traversal.
///          Focus-enabled widgets override this to return @c true so that the
///          focus manager can route keyboard events to them.  The separation
///          keeps passive display elements lightweight while letting interactive
///          widgets opt in explicitly.
/// @return Always @c false in the base implementation.
bool Widget::wantsFocus() const {
    return false;
}

/// @brief React to transitions into or out of the focused state.
///
/// @details The boolean indicates whether focus was gained (@c true) or lost
///          (@c false).  The base implementation is a no-op so passive widgets
///          do not incur overhead, but interactive widgets can override this to
///          update internal state or trigger repaints when focus changes.
/// @param focused Whether the widget gained focus; ignored by the base
///        implementation.
void Widget::onFocusChanged(bool) {}

/// @brief Retrieve the rectangle assigned during the last layout pass.
///
/// @details Containers call @ref layout to provide geometry information.  The
///          stored rectangle is returned verbatim so that derived classes and
///          callers can reason about the widget's position when painting or
///          handling input.
/// @return Rectangle stored by the most recent @ref layout call.
Rect Widget::rect() const {
    return rect_;
}

} // namespace zanna::tui::ui
