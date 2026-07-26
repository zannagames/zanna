//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Declares incremental terminal-byte decoding into structured input events.
/// @details InputDecoder coordinates UTF-8, CSI, SS3, and bracketed-paste state
///          machines and owns chronological key, mouse, and paste output queues.
//
// The decoder maintains an internal state machine that handles:
//   - UTF-8 multi-byte character sequences (via Utf8Decoder)
//   - ANSI CSI escape sequences (via CsiParser) for special keys and mouse
//   - SS3 escape sequences for legacy arrow/function key encodings
//   - Bracketed paste mode (capturing pasted text between markers)
//
// Raw bytes are fed incrementally via feed(), and decoded events are
// retrieved via drain(), drain_mouse(), and drain_paste(). The decoder
// correctly handles partial sequences across feed() calls, making it
// suitable for non-blocking or chunked input reading.
//
// Key invariants:
//   - Partial UTF-8 or escape sequences are buffered until complete.
//   - drain() clears the internal event queue and returns all events.
//   - Events are produced in the order they were decoded from input.
//
// Ownership: InputDecoder owns all internal buffers and event queues.
// It borrows no external state.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "tui/term/CsiParser.hpp"
#include "tui/term/Utf8Decoder.hpp"
#include "tui/term/key_event.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace zanna::tui::term {
/// @brief Stateful terminal input decoder that converts raw byte streams into
///        structured keyboard, mouse, and paste events.
/// @details Implements a state machine handling UTF-8 character decoding, ANSI
///          CSI escape sequences (for special keys, mouse, and mode changes),
///          SS3 legacy sequences, and bracketed paste mode. Bytes are fed
///          incrementally and decoded events accumulate in internal queues.
class InputDecoder {
  public:
    /// @brief Create a decoder with empty output queues.
    InputDecoder();

    /// @brief Feed raw bytes from the terminal into the decoder.
    /// @details Processes each byte through the state machine, potentially producing
    ///          key events, mouse events, or paste events. Partial sequences are
    ///          buffered internally and completed on subsequent feed() calls.
    /// @param bytes UTF-8 encoded terminal input data.
    void feed(std::string_view bytes);

    /// @brief Retrieve and clear all decoded key events.
    /// @details Returns the accumulated key events and empties the internal queue.
    ///          Call this after feed() to collect decoded input.
    /// @return Vector of decoded key events in chronological order.
    [[nodiscard]] std::vector<KeyEvent> drain();
    /// @brief Retrieve and clear all decoded mouse events.
    /// @return Vector of decoded mouse events in chronological order.
    [[nodiscard]] std::vector<MouseEvent> drain_mouse();
    /// @brief Retrieve and clear all decoded paste events.
    /// @return Vector of decoded paste events in chronological order.
    [[nodiscard]] std::vector<PasteEvent> drain_paste();

  private:
    /// @brief Active terminal-input grammar state.
    enum class State { Utf8, Esc, CSI, SS3, Paste, PasteEsc, PasteCSI };

    /// @brief Emit one decoded Unicode code point as a key event.
    /// @param cp Unicode scalar value produced by the UTF-8 decoder.
    void emit(uint32_t cp);

    /// @brief Decode a completed CSI sequence and select the next state.
    /// @param final CSI final byte.
    /// @param params Parameter bytes preceding @p final.
    /// @return State to enter after handling the sequence.
    State handle_csi(char final, std::string_view params);

    /// @brief Decode a completed SS3 special-key sequence.
    /// @param final SS3 final byte identifying the key.
    /// @param params Optional parameter bytes preceding @p final.
    void handle_ss3(char final, std::string_view params);

    State state_{State::Utf8};              ///< Current input grammar state.
    std::string seq_{};                     ///< Incomplete escape-sequence bytes.
    Utf8Decoder utf8_decoder_{};            ///< Incremental Unicode decoder.
    std::vector<KeyEvent> key_events_{};    ///< Pending key events.
    std::vector<MouseEvent> mouse_events_{}; ///< Pending mouse events.
    std::vector<PasteEvent> paste_events_{}; ///< Pending completed paste events.
    std::string paste_buf_{};               ///< Text accumulated during bracketed paste.
    CsiParser csi_parser_;                  ///< CSI decoder bound to owned queues.
};

} // namespace zanna::tui::term
