//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Declares the top-level Zanna TUI application driver.
/// @details App orchestrates a widget tree, focus management, FIFO input-event
///          processing, layout, and differential terminal rendering in an
///          interactive or headless-capable loop.
//
// The tick() method implements a simple game-loop style update: pending
// events are dispatched to the focused widget (or the global keymap),
// the widget tree is laid out, and a single frame is rendered to the
// terminal via the Renderer.
//
// Key invariants:
//   - The root widget must not be null after construction.
//   - Events are processed in FIFO order from the pushEvent() queue.
//   - The keymap pointer may be null; when set, global bindings take
//     priority over widget-specific event handling.
//
// Ownership: App owns the root widget via unique_ptr and holds
// non-owning references to the TermIO and optional Keymap.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "tui/input/keymap.hpp"
#include "tui/render/renderer.hpp"
#include "tui/ui/container.hpp"
#include "tui/ui/focus.hpp"
#include "tui/ui/widget.hpp"

#include <memory>
#include <vector>

namespace zanna::tui {
/// @brief Top-level TUI application that drives the widget tree, focus ring, and rendering loop.
/// @details Manages the complete lifecycle of a terminal UI session: event
///          queueing, focus-aware dispatch, layout computation, and differential
///          screen rendering. Can operate headlessly for testing.
class App {
  public:
    /// @brief Construct app with root widget and terminal I/O.
    /// @param root   The root widget tree. Ownership is transferred to App. Must not be null.
    /// @param tio    Terminal I/O backend used for reading input and writing ANSI output.
    /// @param rows   Initial terminal height in rows.
    /// @param cols   Initial terminal width in columns.
    /// @param truecolor Enable 24-bit true-color rendering (default: false for 256-color).
    App(std::unique_ptr<ui::Widget> root,
        ::zanna::tui::term::TermIO &tio,
        int rows,
        int cols,
        bool truecolor = false);

    /// @brief Enqueue an input event for processing during the next tick().
    /// @details Events are appended to an internal FIFO queue and dispatched in
    ///          order when tick() runs. Multiple events can be queued between ticks.
    /// @param ev The event to enqueue (typically a key press or mouse action).
    void pushEvent(const ui::Event &ev);

    /// @brief Process all pending events and render one frame to the terminal.
    /// @details Dispatches queued events to the focused widget (falling back to the
    ///          global keymap if set), performs layout on the widget tree, paints into
    ///          the screen buffer, computes differential updates, and writes ANSI
    ///          escape sequences to the terminal output.
    void tick();

    /// @brief Access the focus manager for widget focus ring operations.
    /// @return Mutable reference to the focus manager used for Tab/Shift+Tab navigation.
    [[nodiscard]] ui::FocusManager &focus();

    /// @brief Install a global keymap for command dispatch.
    /// @details When a keymap is set, key events are first checked against global
    ///          bindings before being passed to the focused widget. Pass nullptr
    ///          to disable global key handling.
    /// @param km Pointer to the keymap to use, or nullptr to clear.
    void setKeymap(input::Keymap *km);

    /// @brief Resize the app's screen and request re-layout on next tick.
    /// @details Updates the internal screen dimensions. The next call to tick()
    ///          will re-layout the widget tree and repaint with the new size.
    /// @param rows New number of rows (height).
    /// @param cols New number of columns (width).
    void resize(int rows, int cols);

  private:
    std::unique_ptr<ui::Widget> root_{}; ///< Owned root of the widget hierarchy.
    render::ScreenBuffer screen_{};     ///< Mutable cell buffer for the current frame.
    render::Renderer renderer_;         ///< Differential renderer bound to terminal I/O.
    std::vector<ui::Event> events_{};   ///< FIFO events awaiting the next tick.
    int rows_{0};                       ///< Current terminal height in rows.
    int cols_{0};                       ///< Current terminal width in columns.
    ui::FocusManager focus_{};          ///< Focus ring and active-widget manager.
    input::Keymap *keymap_{nullptr};     ///< Optional non-owning global keymap.
};

} // namespace zanna::tui
