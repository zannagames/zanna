//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Declares circular keyboard-focus management for TUI widgets.
/// @details FocusManager stores non-owning pointers to eligible widgets,
///          advances or reverses focus for Tab navigation, and adjusts its
///          active index as widgets unregister.
//
// Widgets register with the focus manager via registerWidget(). Only widgets
// that return true from wantsFocus() are added to the ring. The focus
// manager supports Tab-style navigation via next() and prev(), which cycle
// through the ring and return the newly focused widget.
//
// The App calls next()/prev() in response to Tab/Shift+Tab key events and
// notifies widgets of focus changes via Widget::onFocusChanged().
//
// Key invariants:
//   - The ring contains only widgets with wantsFocus() == true.
//   - current() returns nullptr when the ring is empty.
//   - Unregistering the currently focused widget adjusts the index safely.
//
// Ownership: FocusManager stores non-owning raw pointers to widgets. The
// widgets must outlive their registration in the manager.
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstddef>
#include <vector>

namespace zanna::tui::ui {
class Widget;

/// @brief Manages a circular ring of focusable widgets for keyboard navigation.
/// @details Maintains a list of registered focusable widgets and tracks the
///          current focus position. Supports forward and backward traversal
///          for Tab/Shift+Tab focus cycling.
class FocusManager {
  public:
    /// @brief Register a widget if it wants focus.
    /// @details Only adds the widget to the ring if widget->wantsFocus()
    ///          returns true. Duplicate registrations are not checked.
    /// @param w Widget to register. Must remain valid while registered.
    void registerWidget(Widget *w);

    /// @brief Remove a widget from the focus ring.
    /// @details Safe to call during widget destruction. Adjusts the focus
    ///          index if the removed widget was at or before the current position.
    /// @param w Widget to unregister.
    void unregisterWidget(Widget *w);

    /// @brief Advance focus to the next widget in the ring.
    /// @return Pointer to the newly focused widget, or nullptr if ring is empty.
    [[nodiscard]] Widget *next();

    /// @brief Move focus to the previous widget in the ring.
    /// @return Pointer to the newly focused widget, or nullptr if ring is empty.
    [[nodiscard]] Widget *prev();

    /// @brief Get the currently focused widget.
    /// @return Pointer to the focused widget, or nullptr if no widget has focus.
    [[nodiscard]] Widget *current() const;

  private:
    std::vector<Widget *> ring_{}; ///< Non-owning focusable widgets in traversal order.
    std::size_t index_{0};         ///< Current entry within @c ring_ when nonempty.
};

} // namespace zanna::tui::ui
