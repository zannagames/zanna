//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Declares stacked modal routing and centered popup widgets.
/// @details ModalHost owns a root and LIFO overlay stack, paints overlays above
///          the root, and routes input exclusively to the topmost modal; Popup
///          provides a bordered dismissible overlay.
//
// ModalHost is a special widget that wraps a root widget and manages a stack
// of modal widgets rendered on top. When modals are present, they receive
// all input events, preventing interaction with the underlying root widget.
// Modals are pushed and popped in LIFO order.
//
// Popup is a simple modal widget that draws a bordered rectangle centered
// within its parent's area. It supports an onClose callback invoked when the
// popup is dismissed (typically via the Escape key).
//
// Key invariants:
//   - The topmost modal always receives input events exclusively.
//   - When no modals are active, events pass through to the root widget.
//   - ModalHost is always focusable (wantsFocus returns true) so it can
//     intercept events for modal routing.
//
// Ownership: ModalHost owns the root widget and the modal stack (all via
// unique_ptr). Popup owns its onClose callback.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "tui/ui/widget.hpp"

#include <functional>
#include <memory>
#include <vector>

namespace zanna::tui::ui {
class Popup;

/// @brief Widget host managing a root widget and a stack of modal overlays.
/// @details Renders the root widget first, then any active modals on top.
///          Input events are routed to the topmost modal when present,
///          preventing interaction with the root widget underneath.
class ModalHost : public Widget {
  public:
    /// @brief Construct a modal host wrapping a root widget.
    /// @param root The primary widget displayed beneath any modals. Must not be null.
    explicit ModalHost(std::unique_ptr<Widget> root);

    /// @brief Push a modal widget on top of the modal stack.
    /// @details The modal will receive all input events until it is popped.
    /// @param modal Widget to display as a modal overlay.
    void pushModal(std::unique_ptr<Widget> modal);

    /// @brief Pop the topmost modal from the stack.
    /// @details If the stack is empty, this is a no-op.
    void popModal();

    /// @brief Layout the root widget and all active modals.
    /// @param r Rectangle for the entire host area.
    void layout(const Rect &r) override;

    /// @brief Paint the root widget followed by all active modals.
    /// @param sb Screen buffer to paint into.
    void paint(render::ScreenBuffer &sb) override;

    /// @brief Route input to the topmost modal or the root widget.
    /// @param ev Input event to dispatch.
    /// @return True if the event was consumed.
    bool onEvent(const Event &ev) override;

    /// @brief ModalHost is always focusable to intercept events for routing.
    /// @return Always true.
    [[nodiscard]] bool wantsFocus() const override;

    /// @brief Access the underlying root widget.
    /// @return Raw pointer to the root widget.
    [[nodiscard]] Widget *root();

  private:
    std::unique_ptr<Widget> root_{}; ///< Owned underlying application widget.
    std::vector<std::unique_ptr<Widget>> modals_{}; ///< Owned overlays in bottom-to-top order.
};

/// @brief Simple popup widget with a bordered rectangle and dismiss callback.
/// @details Draws a centered bordered box within the parent's area. Handles
///          the Escape key to invoke the dismiss callback.
class Popup : public Widget {
  public:
    /// @brief Construct a popup with specified dimensions.
    /// @param w Width of the popup box in columns.
    /// @param h Height of the popup box in rows.
    Popup(int w, int h);

    /// @brief Set the callback invoked when the popup is dismissed.
    /// @param cb Callback to invoke on dismiss (e.g., from Escape key).
    void setOnClose(std::function<void()> cb);

    /// @brief Center the popup box within the given layout rectangle.
    /// @param r Parent rectangle within which the popup is centered.
    void layout(const Rect &r) override;

    /// @brief Paint the popup's bordered rectangle.
    /// @param sb Screen buffer to paint into.
    void paint(render::ScreenBuffer &sb) override;

    /// @brief Handle input events, dismissing the popup on Escape.
    /// @param ev Input event to handle.
    /// @return True if the event was consumed.
    bool onEvent(const Event &ev) override;

    /// @brief Popups are always focusable.
    /// @return Always true.
    [[nodiscard]] bool wantsFocus() const override;

  private:
    int width_{0};                   ///< Requested popup width in columns.
    int height_{0};                  ///< Requested popup height in rows.
    Rect box_{};                     ///< Centered rectangle computed during layout.
    std::function<void()> onClose_{}; ///< Optional dismissal callback.
};

} // namespace zanna::tui::ui
