//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tui/src/term/clipboard.cpp
// Purpose: Implement OSC 52 clipboard integration and testing utilities for the
//          Zanna terminal UI toolkit.
// Key invariants: The real clipboard honours the ZANNATUI_DISABLE_OSC52
//                 safeguard, never stores global state, and always flushes the
//                 terminal after emitting the escape sequence.  The mock variant
//                 mirrors the encoded form so tests can inspect exact payloads.
// Ownership/Lifetime: Osc52Clipboard borrows the TermIO interface supplied by
//                     the embedder; MockClipboard owns its captured escape
//                     sequence without touching the terminal.
// Links: docs/internals/architecture.md#zannatui-architecture, ECMA-48 §8.3.93
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements OSC 52 clipboard helpers for both production and testing.
/// @details The helpers translate UTF-8 text into the base64-encoded sequences
///          understood by terminals that support OSC 52 clipboard transfers.
///          They centralise environment guarding, escape-sequence construction,
///          and mock facilities used by unit tests to assert correct behaviour.

#include "tui/term/clipboard.hpp"
#include "tui/term/term_io.hpp"

#include <cstdlib>
#include <string>
#include <string_view>

namespace zanna::tui::term {
namespace {
static const char kB64Table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/// @brief Encode a byte string into base64 for OSC payloads.
/// @details Processes the input three bytes at a time, emitting four base64
///          characters per chunk and applying the canonical '=' padding
///          rules for trailing bytes.  The implementation avoids heap churn
///          by reserving the exact output size upfront, making it suitable
///          for latency-sensitive clipboard operations.
/// @param in UTF-8 text to encode.
/// @return Base64 string ready for insertion into an OSC 52 sequence.
std::string base64_encode(std::string_view in) {
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    std::size_t i = 0;
    while (i + 3 <= in.size()) {
        unsigned triple = (static_cast<unsigned char>(in[i]) << 16) |
                          (static_cast<unsigned char>(in[i + 1]) << 8) |
                          (static_cast<unsigned char>(in[i + 2]));
        out.push_back(kB64Table[(triple >> 18) & 0x3F]);
        out.push_back(kB64Table[(triple >> 12) & 0x3F]);
        out.push_back(kB64Table[(triple >> 6) & 0x3F]);
        out.push_back(kB64Table[triple & 0x3F]);
        i += 3;
    }
    if (i < in.size()) {
        unsigned triple = static_cast<unsigned char>(in[i]) << 16;
        bool two = false;
        if (i + 1 < in.size()) {
            triple |= static_cast<unsigned char>(in[i + 1]) << 8;
            two = true;
        }
        out.push_back(kB64Table[(triple >> 18) & 0x3F]);
        out.push_back(kB64Table[(triple >> 12) & 0x3F]);
        if (two) {
            out.push_back(kB64Table[(triple >> 6) & 0x3F]);
            out.push_back('=');
        } else {
            out.push_back('=');
            out.push_back('=');
        }
    }
    return out;
}

/// @brief Construct a complete OSC 52 sequence for the provided text.
/// @details Prepends the OSC introducer (`ESC ] 52 ; c ;`), appends the
///          base64-encoded payload, and terminates with BEL.  The helper
///          keeps the logic centralised so both production and mock
///          clipboards emit identical sequences.
/// @param text Clipboard text to transmit.
/// @return Escape sequence that copies @p text into the terminal clipboard.
std::string build_seq(std::string_view text) {
    std::string seq("\x1b]52;c;");
    seq += base64_encode(text);
    seq.push_back('\x07');
    return seq;
}

/// @brief Check whether OSC 52 clipboard integration is disabled.
/// @details Reads the `ZANNATUI_DISABLE_OSC52` environment variable,
///          treating a leading `1` as an affirmative disable signal.  This
///          allows developers to opt out of clipboard writes in environments
///          where OSC 52 is unsupported or undesirable.
/// @return @c true when OSC 52 emission should be skipped.
bool osc52_disabled() {
    const char *v = std::getenv("ZANNATUI_DISABLE_OSC52");
    return v && v[0] == '1';
}
} // namespace

/// @brief Bind the OSC 52 clipboard implementation to a terminal interface.
/// @details Stores a reference to the embedder-supplied @ref TermIO so that copy
///          operations can emit escape sequences without taking ownership of the
///          terminal backend.
/// @param io Terminal bridge used to send OSC sequences.
Osc52Clipboard::Osc52Clipboard(TermIO &io) : io_(io) {}

/// @brief Copy text to the terminal clipboard using OSC 52.
/// @details Respects @ref osc52_disabled() before emitting any data.  When
///          enabled, the routine builds the OSC 52 sequence, writes it to the
///          terminal, and flushes the output to ensure immediate delivery.
/// @param text UTF-8 payload to copy to the clipboard.
/// @return @c true when the copy succeeded; @c false if OSC 52 is disabled.
bool Osc52Clipboard::copy(std::string_view text) {
    if (osc52_disabled()) {
        return false;
    }
    io_.write(build_seq(text));
    io_.flush();
    return true;
}

/// @brief Terminals cannot read back OSC 52 contents, so paste is a stub.
/// @details OSC 52 provides a one-way interface in typical terminal emulators.
///          Returning an empty string communicates that no data can be fetched
///          programmatically while keeping the API symmetrical with the mock
///          clipboard used in tests.
/// @return Always returns an empty string.
std::string Osc52Clipboard::paste() {
    return {};
}

/// @brief Encode text as OSC 52 and record it for later inspection.
/// @details Mirrors @ref Osc52Clipboard::copy but stores the escape sequence in
///          @ref last_ so tests can verify exact terminal output.  Honour the
///          same disable flag to accurately simulate runtime behaviour.
/// @param text Clipboard text to encode.
/// @return @c true when the sequence was recorded; @c false when disabled.
bool MockClipboard::copy(std::string_view text) {
    if (osc52_disabled()) {
        last_.clear();
        return false;
    }
    last_ = build_seq(text);
    return true;
}

/// @brief Decode the previously captured OSC 52 sequence.
/// @details Searches for the final semicolon separating the payload, locates the
///          terminator (BEL or ST), and base64-decodes the intervening bytes.
///          Invalid or truncated sequences yield an empty string, mirroring the
///          behaviour of terminal implementations that ignore malformed
///          payloads.
/// @return Decoded clipboard contents, or an empty string on failure.
std::string MockClipboard::paste() {
    // Expect: ESC ] 52 ; c ; <base64> BEL   (or ST terminator)
    auto find_last_semicolon = last_.rfind(';');
    if (find_last_semicolon == std::string::npos)
        return {};

    // Find terminator: BEL (\x07) or ST (\x1b\\)
    std::size_t end = last_.find('\x07', find_last_semicolon + 1);
    if (end == std::string::npos) {
        // Look for ST: ESC \\ terminator
        std::size_t esc = last_.find('\x1b', find_last_semicolon + 1);
        if (esc != std::string::npos && esc + 1 < last_.size() && last_[esc + 1] == '\\') {
            end = esc;
        }
    }
    if (end == std::string::npos)
        return {};

    std::string_view b64 =
        std::string_view(last_).substr(find_last_semicolon + 1, end - (find_last_semicolon + 1));

    /// @brief Decode one Base64 alphabet character.
    /// @param c Character to decode.
    /// @return Sextet value, `-2` for padding, or `-1` for invalid input.
    auto dec = [](char c) -> int {
        if (c >= 'A' && c <= 'Z')
            return c - 'A';
        if (c >= 'a' && c <= 'z')
            return c - 'a' + 26;
        if (c >= '0' && c <= '9')
            return c - '0' + 52;
        if (c == '+')
            return 62;
        if (c == '/')
            return 63;
        if (c == '=')
            return -2; // padding
        return -1;     // invalid
    };

    std::string out;
    int val = 0;
    int valb = -8;
    for (char c : b64) {
        int d = dec(c);
        if (d < 0) {
            if (d == -2)
                break; // stop at padding
            continue;  // skip invalid
        }
        val = (val << 6) | d;
        valb += 6;
        if (valb >= 0) {
            out.push_back(char((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

/// @brief Inspect the raw OSC 52 sequence recorded by the mock clipboard.
/// @details Exposes @ref last_ so assertions can compare the emitted escape
///          sequence byte-for-byte against expected values.
/// @return Reference to the internally stored sequence.
const std::string &MockClipboard::last() const {
    return last_;
}

/// @brief Reset the captured OSC 52 sequence to an empty state.
/// @details Clears @ref last_ so subsequent test scenarios begin with a clean
///          slate and do not accidentally observe earlier clipboard operations.
void MockClipboard::clear() {
    last_.clear();
}

} // namespace zanna::tui::term
