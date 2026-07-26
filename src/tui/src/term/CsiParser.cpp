//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tui/src/term/CsiParser.cpp
// Purpose: Decode Control Sequence Introducer (CSI) escape sequences emitted by
//          terminals into structured key, mouse, and paste events.
// Key invariants: The parser never allocates, writes decoded events into the
//                 caller-owned buffers supplied at construction, and respects
//                 VT-style modifier encodings for arrow/function keys and SGR
//                 mouse events.
// Ownership/Lifetime: CsiParser borrows event buffers and a paste accumulator
//                     from the embedder; it stores references without assuming
//                     ownership.
// Links: docs/internals/architecture.md#zannatui-architecture, ECMA-48 §5.4
//
//===----------------------------------------------------------------------===//

/**
 * @file CsiParser.cpp
 * @brief Implements terminal CSI decoding for structured input events.
 * @details The parser interprets VT parameter lists, modifier encodings,
 *          cursor and function keys, SGR mouse reports, and bracketed-paste
 *          boundaries, appending decoded events or paste bytes into
 *          caller-owned buffers retained by reference.
 */

#include "tui/term/CsiParser.hpp"

namespace zanna::tui::term {

/// @brief Bind the parser to the event buffers that receive decoded sequences.
/// @details The constructor stores references to the caller-provided key, mouse,
///          and paste buffers.  Parsed CSI sequences append into those
///          containers, allowing the embedder to decide on storage strategy and
///          lifetime.  No work is performed until @ref handle() is invoked.
/// @param keys Buffer that receives decoded key events.
/// @param mouse Buffer that receives decoded mouse events.
/// @param paste_buffer String accumulator that stores bracketed paste payloads.
CsiParser::CsiParser(std::vector<KeyEvent> &keys,
                     std::vector<MouseEvent> &mouse,
                     std::string &paste_buffer)
    : key_events_(keys), mouse_events_(mouse), paste_buffer_(paste_buffer) {}

/// @brief Tokenise the semicolon-separated integer parameters of a CSI sequence.
/// @details Walks the parameter substring one byte at a time, collecting base-10
///          integers and treating consecutive semicolons as zero-valued
///          parameters (per VT conventions).  Non-digit characters other than
///          semicolons terminate the current number.  The helper performs no
///          allocations beyond the returned vector.
/// @param params Slice of the CSI payload between the introducer and final byte.
/// @return Vector of parsed integer parameters; empty when no digits present.
std::vector<int> CsiParser::parse_params(std::string_view params) const {
    std::vector<int> out;
    int val = 0;
    bool have = false;
    for (char c : params) {
        if (c >= '0' && c <= '9') {
            val = val * 10 + (c - '0');
            have = true;
        } else if (c == ';') {
            out.push_back(have ? val : 0);
            val = 0;
            have = false;
        }
    }
    if (have) {
        out.push_back(val);
    }
    return out;
}

/// @brief Decode modifier bits encoded using the VT "modifiers plus one" scheme.
/// @details The VT protocol encodes Shift/Ctrl/Alt modifiers as parameter values
///          offset by one.  Values below @c 2 indicate no modifiers.  The helper
///          subtracts one when the parameter is present and returns the raw bit
///          mask understood by @ref KeyEvent.
/// @param value Parameter extracted from the CSI sequence.
/// @return Bitmask describing active modifiers (Shift=1, Alt=2, Ctrl=4).
unsigned CsiParser::decode_mod(int value) const {
    if (value < 2) {
        return 0;
    }
    return static_cast<unsigned>(value - 1);
}

/// @brief Interpret SGR mouse reporting sequences into structured events.
/// @details Supports the "<b;x;y" parameter format used by modern terminals to
///          describe button presses, releases, motion, and wheel events.  The
///          routine extracts modifier bits, normalises coordinates to be zero
///          based, distinguishes press/release/move/wheel transitions, and
///          appends a populated @ref MouseEvent to the shared buffer.  Malformed
///          sequences (insufficient parameters) are ignored without side effects.
/// @param final Final byte of the CSI sequence ('M' for press/move, 'm' for release).
/// @param params Parameter substring (without the leading '<').
void CsiParser::handle_sgr_mouse(char final, std::string_view params) {
    auto nums = parse_params(params);
    if (nums.size() < 3) {
        return;
    }
    int b = nums[0];
    MouseEvent ev{};
    ev.x = nums[1] - 1;
    ev.y = nums[2] - 1;
    if (b & 4) {
        ev.mods |= KeyEvent::Shift;
    }
    if (b & 8) {
        ev.mods |= KeyEvent::Alt;
    }
    if (b & 16) {
        ev.mods |= KeyEvent::Ctrl;
    }

    if ((b >= 64 && b <= 65) || (b >= 96 && b <= 97)) {
        ev.type = MouseEvent::Type::Wheel;
        ev.buttons = (b & 1) ? 2u : 1u;
    } else if (final == 'm') {
        ev.type = MouseEvent::Type::Up;
        ev.buttons = 1u << (b & 3);
    } else if (b & 32) {
        ev.type = MouseEvent::Type::Move;
        ev.buttons = 1u << (b & 3);
    } else {
        ev.type = MouseEvent::Type::Down;
        ev.buttons = 1u << (b & 3);
    }
    mouse_events_.push_back(ev);
}

/// @brief Decode a CSI control sequence into key or mouse events.
/// @details Examines the final byte to select between SGR mouse reporting,
///          tilde-terminated function-key sequences, and cursor movement
///          sequences.  The helper handles bracketed paste start notifications
///          (@c CSI 200~) by clearing the paste buffer and setting the
///          @ref CsiResult::start_paste flag.  Recognised events are appended to
///          the bound buffers, and @ref CsiResult::handled is set to true.  The
///          function leaves the buffers untouched for unsupported sequences so
///          callers can fall back to alternative decoders.
/// @param final Final byte that terminates the CSI sequence.
/// @param params Substring between the introducer and @p final.
/// @return Result structure describing whether the sequence was consumed and if
///          a bracketed paste began.
CsiResult CsiParser::handle(char final, std::string_view params) {
    CsiResult result{};
    if ((final == 'M' || final == 'm') && !params.empty() && params.front() == '<') {
        handle_sgr_mouse(final, params.substr(1));
        result.handled = true;
        return result;
    }

    auto nums = parse_params(params);
    unsigned mods = 0;
    if (final == '~') {
        if (nums.size() >= 2) {
            mods = decode_mod(nums[1]);
        }
        if (!nums.empty()) {
            if (nums[0] == 200) {
                paste_buffer_.clear();
                result.start_paste = true;
                result.handled = true;
                return result;
            }
        }
        if (nums.empty()) {
            return result;
        }
        KeyEvent ev{};
        ev.mods = mods;
        switch (nums[0]) {
            case 1:
                ev.code = KeyEvent::Code::Home;
                break;
            case 2:
                ev.code = KeyEvent::Code::Insert;
                break;
            case 3:
                ev.code = KeyEvent::Code::Delete;
                break;
            case 4:
                ev.code = KeyEvent::Code::End;
                break;
            case 5:
                ev.code = KeyEvent::Code::PageUp;
                break;
            case 6:
                ev.code = KeyEvent::Code::PageDown;
                break;
            case 11:
                ev.code = KeyEvent::Code::F1;
                break;
            case 12:
                ev.code = KeyEvent::Code::F2;
                break;
            case 13:
                ev.code = KeyEvent::Code::F3;
                break;
            case 14:
                ev.code = KeyEvent::Code::F4;
                break;
            case 15:
                ev.code = KeyEvent::Code::F5;
                break;
            case 17:
                ev.code = KeyEvent::Code::F6;
                break;
            case 18:
                ev.code = KeyEvent::Code::F7;
                break;
            case 19:
                ev.code = KeyEvent::Code::F8;
                break;
            case 20:
                ev.code = KeyEvent::Code::F9;
                break;
            case 21:
                ev.code = KeyEvent::Code::F10;
                break;
            case 23:
                ev.code = KeyEvent::Code::F11;
                break;
            case 24:
                ev.code = KeyEvent::Code::F12;
                break;
            default:
                return result;
        }
        key_events_.push_back(ev);
        result.handled = true;
        return result;
    }

    if (nums.size() >= 2) {
        mods = decode_mod(nums[1]);
    }

    KeyEvent ev{};
    ev.mods = mods;
    switch (final) {
        case 'A':
            ev.code = KeyEvent::Code::Up;
            break;
        case 'B':
            ev.code = KeyEvent::Code::Down;
            break;
        case 'C':
            ev.code = KeyEvent::Code::Right;
            break;
        case 'D':
            ev.code = KeyEvent::Code::Left;
            break;
        case 'H':
            ev.code = KeyEvent::Code::Home;
            break;
        case 'F':
            ev.code = KeyEvent::Code::End;
            break;
        default:
            return result;
    }
    key_events_.push_back(ev);
    result.handled = true;
    return result;
}

} // namespace zanna::tui::term
