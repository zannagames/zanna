//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Declares ANSI Control Sequence Introducer input decoding.
/// @details CsiParser interprets complete `ESC [` sequences as special keys,
///          modifiers, SGR mouse reports, and bracketed-paste transitions,
///          appending events to caller-owned output buffers.
//
// The parser handles:
//   - Cursor movement keys (arrows, Home, End, Page Up/Down, Insert, Delete)
//   - Function keys (F1-F12) with their various terminal encodings
//   - Modifier keys (Shift, Alt, Ctrl) encoded in CSI parameters
//   - SGR mouse reporting (button presses, releases, movement, scroll)
//   - Bracketed paste mode begin/end markers
//
// Key invariants:
//   - handle() produces events by appending to the referenced vectors.
//   - The parser does not own the event output buffers; they are borrowed.
//   - CsiResult.start_paste indicates entry into bracketed paste mode.
//
// Ownership: CsiParser borrows references to the KeyEvent, MouseEvent,
// and paste buffer vectors owned by the InputDecoder.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "tui/term/key_event.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace zanna::tui::term {
/// @brief Outcome of processing a CSI escape sequence.
/// @details Indicates whether the sequence was successfully mapped to an input
///          event and whether it triggered bracketed paste mode entry.
struct CsiResult {
    bool start_paste{false}; ///< True when bracketed paste mode begins.
    bool handled{false};     ///< True if the sequence mapped to a known action.
};

/// @brief Parser for ANSI CSI escape sequences, producing keyboard and mouse events.
/// @details Interprets CSI sequences (ESC [ ... final_byte) from the terminal and
///          converts them into structured KeyEvent and MouseEvent objects. Handles
///          special keys, function keys, modifier encoding, SGR mouse reporting,
///          and bracketed paste mode markers.
class CsiParser {
  public:
    /// @brief Construct a parser bound to output event buffers.
    /// @param keys Borrowed destination for decoded key events.
    /// @param mouse Borrowed destination for decoded mouse events.
    /// @param paste_buffer Borrowed accumulation buffer for bracketed paste text.
    CsiParser(std::vector<KeyEvent> &keys,
              std::vector<MouseEvent> &mouse,
              std::string &paste_buffer);

    /// @brief Process a complete CSI sequence and produce input events.
    /// @details Maps the final byte and parameter payload to keyboard or mouse events,
    ///          appending results to the referenced output vectors. Recognizes arrows,
    ///          function keys, modifiers, SGR mouse, and bracketed paste markers.
    /// @param final The final byte of the CSI sequence (determines the key or action).
    /// @param params The parameter bytes preceding the final byte (semicolon-separated numbers).
    /// @return Result indicating whether the sequence was handled and paste mode state.
    [[nodiscard]] CsiResult handle(char final, std::string_view params);

    /// @brief Parse semicolon-separated numeric parameters from a CSI sequence.
    /// @details Splits the parameter string on semicolons and converts each segment
    ///          to an integer. Missing parameters default to 0.
    /// @param params The raw parameter string from the CSI sequence.
    /// @return Vector of parsed integer values.
    [[nodiscard]] std::vector<int> parse_params(std::string_view params) const;

    /// @brief Decode VT-style modifier bits into the TUI modifier flag format.
    /// @details CSI sequences encode modifiers as (modifier_value + 1), where bits
    ///          represent Shift (1), Alt (2), and Ctrl (4). This function subtracts
    ///          the offset and maps to the KeyEvent::Mods flags.
    /// @param value The raw modifier value from the CSI parameter.
    /// @return Bitwise combination of KeyEvent::Mods flags.
    [[nodiscard]] unsigned decode_mod(int value) const;

  private:
    /// @brief Decode and append one SGR mouse report.
    /// @param final CSI final byte distinguishing press/motion from release.
    /// @param params Semicolon-separated button and coordinate parameters.
    void handle_sgr_mouse(char final, std::string_view params);

    std::vector<KeyEvent> &key_events_;     ///< Borrowed key-event destination.
    std::vector<MouseEvent> &mouse_events_; ///< Borrowed mouse-event destination.
    std::string &paste_buffer_;             ///< Borrowed bracketed-paste buffer.
};

} // namespace zanna::tui::term
