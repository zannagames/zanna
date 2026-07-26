//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// tui/src/ui/focus.cpp
//
// Implements the focus management subsystem for the terminal UI toolkit.  The
// focus manager owns the traversal ring that keeps track of focusable widgets,
// providing registration, removal, and navigation helpers.  It does not own the
// widgets themselves, allowing layout containers to maintain lifetime control
// while this module orchestrates focus changes and the associated callbacks.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements non-owning keyboard-focus registration and traversal.
/// @details The focus manager maintains an ordered ring of widget pointers,
///          sends transition notifications when the current entry changes, and
///          leaves widget lifetime management to the surrounding UI hierarchy.

#include "tui/ui/focus.hpp"
#include "tui/ui/widget.hpp"

#include <algorithm>

namespace zanna::tui::ui {
/// @brief Register a widget as focusable if it opts in via wantsFocus().
///
/// @details The manager ignores @c nullptr values as well as widgets that
///          decline focus.  When the ring transitions from empty to non-empty
///          the index is reset to the first entry so that subsequent navigation
///          calls have a well-defined starting point.
/// @param w Non-owning pointer to the widget to register; may be @c nullptr.
void FocusManager::registerWidget(Widget *w) {
    if (!w || !w->wantsFocus()) {
        return;
    }
    ring_.push_back(w);
    if (ring_.size() == 1) {
        index_ = 0;
    }
}

/// @brief Remove a widget from the focus ring and update state accordingly.
///
/// @details If the widget is not present the call is ignored.  When the removed
///          widget owned focus the method transfers it to the next candidate and
///          dispatches @ref Widget::onFocusChanged notifications to both the old
///          and new widgets.  Emptying the ring clears the index so future
///          registrations start from the beginning.
/// @param w Non-owning pointer to the widget to remove; may be @c nullptr.
void FocusManager::unregisterWidget(Widget *w) {
    if (!w)
        return;
    auto it = std::find(ring_.begin(), ring_.end(), w);
    if (it == ring_.end())
        return;

    std::size_t pos = static_cast<std::size_t>(it - ring_.begin());
    bool wasCurrent = (!ring_.empty() && pos == index_);
    ring_.erase(it);

    if (ring_.empty()) {
        index_ = 0;
        if (wasCurrent) {
            w->onFocusChanged(false);
        }
        return;
    }

    if (pos < index_ || index_ >= ring_.size())
        index_ = index_ ? (index_ - 1) : 0;

    if (wasCurrent) {
        w->onFocusChanged(false);
        if (Widget *now = ring_[index_]) {
            now->onFocusChanged(true);
        }
    }
}

/// @brief Advance focus to the next widget in the ring.
///
/// @details When focusable widgets exist, the manager increments the index with
///          wrap-around semantics, notifies the previously focused widget that it
///          lost focus, and informs the new widget that it gained focus.  The
///          newly focused widget pointer is returned to facilitate additional
///          caller logic such as repaint requests.
/// @return Non-owning pointer to the newly focused widget, or @c nullptr when
///         the focus ring is empty.
Widget *FocusManager::next() {
    if (ring_.empty())
        return nullptr;
    Widget *old = ring_[index_];
    index_ = (index_ + 1) % ring_.size();
    Widget *now = ring_[index_];
    if (now != old) {
        if (old)
            old->onFocusChanged(false);
        if (now)
            now->onFocusChanged(true);
    }
    return now;
}

/// @brief Move focus to the previous widget in the ring.
///
/// @details The implementation mirrors @ref next() but decrements the index and
///          wraps when reaching the beginning.  Notifications are issued to both
///          the old and new widgets whenever the focus target changes.  The
///          pointer to the widget now holding focus is returned for callers that
///          need to react.
/// @return Non-owning pointer to the newly focused widget, or @c nullptr when
///         the focus ring is empty.
Widget *FocusManager::prev() {
    if (ring_.empty())
        return nullptr;
    Widget *old = ring_[index_];
    index_ = (index_ + ring_.size() - 1) % ring_.size();
    Widget *now = ring_[index_];
    if (now != old) {
        if (old)
            old->onFocusChanged(false);
        if (now)
            now->onFocusChanged(true);
    }
    return now;
}

/// @brief Retrieve the widget currently designated as focused.
///
/// @details Returns @c nullptr when no focusable widgets are registered.  This
///          accessor allows higher-level systems to query focus state without
///          mutating it, for example when deciding which widget should receive
///          an incoming keyboard event.
/// @return Non-owning pointer to the focused widget, or @c nullptr when the
///         focus ring is empty.
Widget *FocusManager::current() const {
    if (ring_.empty()) {
        return nullptr;
    }
    return ring_[index_];
}

} // namespace zanna::tui::ui
