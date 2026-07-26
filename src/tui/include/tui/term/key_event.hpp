//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Declares value types for decoded terminal key, mouse, and paste input.
/// @details These transport-independent events preserve special-key identity,
///          Unicode code points, modifier state, mouse coordinates/buttons,
///          and owned bracketed-paste text.
//
// KeyEvent represents a single keypress with optional modifier flags
// (Shift, Alt, Ctrl). It can represent either a special key (arrows,
// function keys, etc.) via the Code enum, or a Unicode character via
// the codepoint field.
//
// MouseEvent represents mouse button presses, releases, movement, and
// scroll wheel events with screen coordinates and modifier state.
//
// PasteEvent carries bracketed paste text from the terminal.
//
// Key invariants:
//   - For character input, code is Code::Unknown and codepoint is non-zero.
//   - For special keys, code identifies the key and codepoint may be zero.
//   - Modifier flags can be combined with bitwise OR.
//   - MouseEvent coordinates are 0-based terminal cell positions.
//
// Ownership: All event structs are value types with no heap allocation
// (except PasteEvent which owns its text string).
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>
#include <string>

namespace zanna::tui::term {
/// @brief Represents a single keyboard input event from the terminal.
/// @details Captures either a Unicode character (via codepoint) or a special
///          key (via Code enum), along with modifier flags (Shift, Alt, Ctrl).
///          Produced by the InputDecoder from raw terminal escape sequences.
struct KeyEvent {
    /// @brief Identifies special (non-character) keys on the keyboard.
    enum class Code {
        Enter,
        Esc,
        Tab,
        Backspace,
        Up,
        Down,
        Left,
        Right,
        Home,
        End,
        PageUp,
        PageDown,
        Insert,
        Delete,
        F1,
        F2,
        F3,
        F4,
        F5,
        F6,
        F7,
        F8,
        F9,
        F10,
        F11,
        F12,
        Unknown
    };

    /// @brief Modifier key flags that can be combined with bitwise OR.
    enum Mods : unsigned { Shift = 1, Alt = 2, Ctrl = 4 };

    /// @brief Unicode scalar value for character input; 0 for special keys.
    uint32_t codepoint{0};
    /// @brief Special key identifier; Code::Unknown for character input.
    Code code{Code::Unknown};
    /// @brief Bitwise combination of Shift, Alt, and Ctrl modifier flags.
    unsigned mods{0};
};

/// @brief Represents a mouse input event with screen coordinates and button state.
/// @details Captures button presses, releases, movement, and scroll wheel actions.
///          Coordinates are 0-based terminal cell positions.
struct MouseEvent {
    /// @brief Mouse action represented by an event.
    enum class Type { Down, Up, Move, Wheel };

    Type type{Type::Move}; ///< Button, motion, or wheel action.
    int x{0};              ///< Zero-based terminal column.
    int y{0};              ///< Zero-based terminal row.
    unsigned buttons{0};   ///< Terminal-decoder button-state bitset.
    unsigned mods{0};      ///< Bitwise combination of KeyEvent modifier flags.
};

/// @brief Carries text from a bracketed paste operation.
/// @details The terminal sends bracketed paste sequences when the user pastes
///          text from the clipboard while bracketed paste mode is enabled.
///          The text field contains the raw pasted content.
struct PasteEvent {
    std::string text{};
};

} // namespace zanna::tui::term
