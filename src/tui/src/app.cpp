//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Implements the interactive and headless Zanna TUI application driver.
/// @details Drains input exactly once per tick, applies focus/keymap routing,
///          lays out and paints the owned widget tree, and emits a differential
///          frame through a borrowed terminal backend.
// Key invariants: Each tick drains queued input events exactly once, applies
//                 focus changes prior to widget dispatch, and renders a fresh
//                 frame into the backing screen buffer.
// Ownership/Lifetime: App owns the widget tree, focus manager, and screen
//                     buffer; it borrows the terminal I/O backend supplied by
//                     the embedder.
// Links: docs/internals/architecture.md#zannatui-architecture
//
//===----------------------------------------------------------------------===//

#include "tui/app.hpp"

namespace zanna::tui {
/// @brief Construct an application driver with the provided root widget and terminal.
/// @details Transfers ownership of the root widget into the application, wires
///          up the ANSI renderer to the supplied terminal I/O implementation,
///          and sizes the backing screen buffer to match the requested
///          dimensions.  The constructor performs no rendering; callers must
///          invoke @ref tick() to process events and draw frames.
/// @param root Widget tree that represents the UI hierarchy.
/// @param tio Terminal I/O bridge used for emitting escape sequences.
/// @param rows Initial terminal row count.
/// @param cols Initial terminal column count.
/// @param truecolor Whether the renderer should emit 24-bit colour sequences.
App::App(std::unique_ptr<ui::Widget> root,
         ::zanna::tui::term::TermIO &tio,
         int rows,
         int cols,
         bool truecolor)
    : root_(std::move(root)), renderer_(tio, truecolor), rows_(rows), cols_(cols) {
    screen_.resize(rows, cols);
}

/// @brief Queue an input event to be processed during the next tick.
/// @details Events are appended to an internal FIFO vector.  The subsequent
///          call to @ref tick() consumes the queue in order, dispatching focus
///          changes for Tab traversal before forwarding the remaining events to
///          either the keymap or the focused widget.
/// @param ev Input event captured by the embedder.
void App::pushEvent(const ui::Event &ev) {
    events_.push_back(ev);
}

/// @brief Access the focus manager associated with the application.
/// @details Exposing the focus manager allows embedders and widgets to register
///          focusable components or to adjust focus policy directly.  The
///          manager remains owned by the application, so the returned reference
///          is always valid for the lifetime of the @ref App instance.
/// @return Reference to the focus manager governing keyboard routing.
ui::FocusManager &App::focus() {
    return focus_;
}

/// @brief Install a keymap used to translate keystrokes into commands.
/// @details The application stores a borrowed pointer to the provided keymap.
///          When present, the keymap has first opportunity to consume key
///          events during @ref tick(); unhandled events continue on to the
///          focused widget.  Passing @c nullptr disables keymap dispatch.
/// @param km Keymap instance whose lifetime must exceed the application.
void App::setKeymap(input::Keymap *km) {
    keymap_ = km;
}

/// @brief Advance the application by one frame.
/// @details The tick routine performs the entire event → render pipeline:
///          1. Drain the queued events, handling Tab traversal locally to keep
///             focus cycling consistent.
///          2. Offer remaining key events to the active keymap when present.
///          3. Forward unhandled events to the currently focused widget.
///          4. Layout the root widget to the current terminal dimensions.
///          5. Clear and repaint the screen buffer via the widget tree.
///          6. Emit the diff through the renderer and snapshot the frame for
///             the next iteration.
///          The routine is idempotent with respect to an empty event queue and
///          tolerates a null root widget (no rendering performed).
void App::tick() {
    for (const auto &ev : events_) {
        if (ev.key.code == term::KeyEvent::Code::Tab) {
            if (ev.key.mods & term::KeyEvent::Shift) {
                (void)focus_.prev();
            } else {
                (void)focus_.next();
            }
            continue;
        }
        bool handled = false;
        if (keymap_) {
            handled = keymap_->handle(focus_.current(), ev.key);
        }
        if (!handled) {
            if (auto *w = focus_.current()) {
                w->onEvent(ev);
            }
        }
    }
    events_.clear();
    if (root_) {
        ui::Rect full{0, 0, cols_, rows_};
        root_->layout(full);
        screen_.clear(render::Style{});
        root_->paint(screen_);
    }
    renderer_.draw(screen_);
    screen_.snapshotPrev();
}

/// @brief Update the terminal dimensions tracked by the application.
/// @details Stores the new geometry and resizes the backing screen buffer so
///          that subsequent @ref tick() calls perform layout against the latest
///          size.  Existing widget instances are not destroyed; they will be
///          relaid out on the next frame.
/// @param rows Updated terminal row count.
/// @param cols Updated terminal column count.
void App::resize(int rows, int cols) {
    rows_ = rows;
    cols_ = cols;
    screen_.resize(rows_, cols_);
}

} // namespace zanna::tui
