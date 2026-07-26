//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// tui/src/ui/modal.cpp
//
// Provides modal presentation primitives for the terminal UI toolkit.  The
// ModalHost coordinates a primary content widget along with a stack of modal
// overlays, handling focus redirection, backdrop rendering, and dismissal
// plumbing.  The Popup helper implements a simple centred dialog complete with
// border drawing and keyboard-driven dismissal.  Together they form the basis
// for blocking interactions such as command palettes or confirmation prompts.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements modal overlay hosting and centred popup presentation.
/// @details A modal host owns its root widget and an ordered overlay stack,
///          routes input to the topmost active layer, and paints overlays after
///          the root.  Popups provide a focusable, dismissible boxed overlay.

#include "tui/ui/modal.hpp"

#include "tui/render/box.hpp"
#include "tui/render/screen.hpp"

#include <algorithm>

namespace zanna::tui::ui {
/// @brief Construct a modal host that wraps the root content widget.
///
/// @details Ownership of the root widget transfers to the host so it can manage
///          layout and painting alongside modal overlays.  The constructor keeps
///          the stack of modals empty; they are pushed as needed via
///          @ref pushModal.
/// @param root Root content widget whose ownership transfers to the host.
ModalHost::ModalHost(std::unique_ptr<Widget> root) : root_(std::move(root)) {}

/// @brief Access the non-modal root widget managed by the host.
///
/// @details Exposing the raw pointer lets callers register the root with focus
///          managers or perform additional configuration without taking
///          ownership away from the host.  The pointer remains valid for the
///          host's lifetime.
/// @return Non-owning pointer to the root widget, or @c nullptr when the host
///         was constructed without one.
Widget *ModalHost::root() {
    return root_.get();
}

/// @brief Declare that the modal host should receive keyboard focus.
///
/// @details Returning @c true allows the host to intercept events before they
///          reach underlying widgets, ensuring that modal overlays behave as a
///          focus trap until dismissed.
/// @return Always @c true.
bool ModalHost::wantsFocus() const {
    return true;
}

/// @brief Push a new modal widget onto the stack and wire up dismissal.
///
/// @details If the incoming widget derives from @ref Popup its close callback is
///          automatically set to pop the modal host entry, allowing the popup to
///          dismiss itself.  Regardless of type, the modal receives ownership via
///          @c unique_ptr and is appended to the stack, making it the active
///          overlay during painting and event handling.
/// @param modal Overlay widget whose ownership transfers to the host.
void ModalHost::pushModal(std::unique_ptr<Widget> modal) {
    if (auto *p = dynamic_cast<Popup *>(modal.get())) {
        /// @brief Remove the active modal when the popup requests closure.
        p->setOnClose([this] { popModal(); });
    }
    modals_.push_back(std::move(modal));
}

/// @brief Remove the top-most modal from the stack if one exists.
///
/// @details When the stack is non-empty the host simply discards the last entry,
///          transferring focus back to the next modal or the root widget.  Empty
///          stacks are ignored so callers can safely invoke this even when no
///          modal is present.
void ModalHost::popModal() {
    if (!modals_.empty()) {
        modals_.pop_back();
    }
}

/// @brief Layout the root content and all active modals within the host bounds.
///
/// @details The host first records its own rectangle via @ref Widget::layout,
///          then propagates the exact same rectangle to the root widget and each
///          modal overlay.  This keeps overlays full-screen while allowing them
///          to centre or otherwise position their internal content relative to
///          the available space.
/// @param r Rectangle assigned to the host, root, and each modal layer.
void ModalHost::layout(const Rect &r) {
    Widget::layout(r);
    if (root_) {
        root_->layout(r);
    }
    for (auto &m : modals_) {
        m->layout(r);
    }
}

/// @brief Paint the root widget and any active modal overlays with backdrop.
///
/// @details The root content is painted first, after which a translucent-style
///          backdrop is emulated by filling the host rectangle with spaces when
///          at least one modal exists.  Finally, each modal paints itself in
///          stack order so that later entries appear on top.
/// @param sb Screen buffer that receives the root and overlay rendering.
void ModalHost::paint(render::ScreenBuffer &sb) {
    if (root_) {
        root_->paint(sb);
    }
    if (!modals_.empty()) {
        sb.fillRect(rect_.x, rect_.y, rect_.w, rect_.h);
        for (auto &m : modals_) {
            m->paint(sb);
        }
    }
}

/// @brief Route input events to the active modal or the root widget.
///
/// @details When modals are present the newest modal receives the event and the
///          function reports it as handled.  Without modals the root widget is
///          given a chance to handle the event; failing that, the call returns
///          @c false to indicate the event was not consumed.
/// @param ev Input event to dispatch.
/// @return @c true when a modal is active or the root consumes @p ev; otherwise
///         @c false.
bool ModalHost::onEvent(const Event &ev) {
    if (!modals_.empty()) {
        (void)modals_.back()->onEvent(ev);
        return true;
    }
    if (root_) {
        return root_->onEvent(ev);
    }
    return false;
}

/// @brief Create a popup with a preferred width and height.
///
/// @details The popup stores the requested size so that during layout it can
///          centre a bounding box that fits inside the host rectangle.  Actual
///          dimensions are clamped to the available space to avoid painting
///          outside the terminal surface.
/// @param w Preferred popup width in terminal cells.
/// @param h Preferred popup height in terminal cells.
Popup::Popup(int w, int h) : width_(w), height_(h) {}

/// @brief Indicate that popups require keyboard focus for dismissal keys.
///
/// @details Returning @c true guarantees the popup receives key events, allowing
///          it to handle escape or enter presses for dismissal without relying
///          on bubbling through other widgets.
/// @return Always @c true.
bool Popup::wantsFocus() const {
    return true;
}

/// @brief Register a callback to be invoked when the popup closes itself.
///
/// @details The callback is stored by value to allow inline lambdas capturing
///          the modal host.  It is invoked by @ref onEvent when the user presses
///          a dismissal key, allowing the host to remove the popup from its
///          stack.
/// @param cb Callback to store; an empty function disables close notification.
void Popup::setOnClose(std::function<void()> cb) {
    onClose_ = std::move(cb);
}

/// @brief Compute a centred bounding box for the popup within the host rect.
///
/// @details The box dimensions are clamped so they never exceed the available
///          space.  The resulting rectangle is cached in @ref box_ for later use
///          when painting borders and background.
/// @param r Available host rectangle within which the popup is centred.
void Popup::layout(const Rect &r) {
    Widget::layout(r);
    int w = std::min(width_, r.w);
    int h = std::min(height_, r.h);
    int x = r.x + (r.w - w) / 2;
    int y = r.y + (r.h - h) / 2;
    box_ = {x, y, w, h};
}

/// @brief Draw the popup border and clear its interior region.
///
/// @details The border is rendered using ASCII box-drawing characters while the
///          interior is filled with spaces.  Characters are written directly into
///          the supplied screen buffer and rely on the precomputed @ref box_.
///          The routine assumes the box fits entirely within the buffer; the
///          layout stage enforces this by clamping dimensions.
/// @param sb Screen buffer that receives the popup border and interior.
void Popup::paint(render::ScreenBuffer &sb) {
    render::drawBox(sb, box_.x, box_.y, box_.w, box_.h);
}

/// @brief Handle key events that should dismiss the popup.
///
/// @details Pressing escape or enter triggers the registered on-close callback
///          when present, signalling the modal host to remove the popup.  Other
///          keys are ignored and bubble up by returning @c false, allowing the
///          application to decide how to handle them.
/// @param ev Input event to inspect.
/// @return @c true for an escape or enter key event; otherwise @c false.
bool Popup::onEvent(const Event &ev) {
    const auto &k = ev.key;
    if (k.code == term::KeyEvent::Code::Esc || k.code == term::KeyEvent::Code::Enter) {
        if (onClose_) {
            onClose_();
        }
        return true;
    }
    return false;
}

} // namespace zanna::tui::ui
