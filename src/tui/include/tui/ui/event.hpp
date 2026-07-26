//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Declares the generic event envelope routed through TUI widgets.
/// @details The current value type carries keyboard input and leaves room for
///          future mouse, paste, or application-specific payloads.
//
// The Event struct currently wraps a single KeyEvent, but is designed as
// an extensible envelope that may later include mouse events, paste events,
// or custom application events.
//
// Key invariants:
//   - Events are value types and are cheap to copy.
//   - The key field is always valid (default-constructed if no key data).
//
// Ownership: Event owns its KeyEvent by value; no heap allocation.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "tui/term/key_event.hpp"

namespace zanna::tui::ui {
/// @brief Generic input event wrapper for the TUI widget system.
/// @details Encapsulates terminal input data routed through the widget tree.
///          Currently wraps a KeyEvent; designed for future extension to include
///          mouse, paste, and custom events.
struct Event {
    term::KeyEvent key{}; ///< Decoded keyboard input carried by this event.
};
} // namespace zanna::tui::ui
