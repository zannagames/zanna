//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tui/src/term/Utf8Decoder.cpp
// Purpose: Provide the incremental UTF-8 decoder that converts terminal byte
//          streams into Unicode code points for the input subsystem.
// Key invariants: Decoder tracks the number of expected continuation bytes,
//                 rejects overlong and surrogate-range encodings, and requests
//                 that callers replay bytes that begin a new sequence after an
//                 error.
// Ownership/Lifetime: Utf8Decoder stores only its internal state; callers
//                     manage the buffers that feed bytes into @ref feed().
// Links: docs/internals/architecture.md#zannatui-architecture, Unicode Standard §3.9
//
//===----------------------------------------------------------------------===//

/**
 * @file Utf8Decoder.cpp
 * @brief Implements incremental UTF-8 decoding for terminal input.
 * @details The state machine consumes one byte at a time, accumulates valid
 *          multibyte sequences, rejects invalid leaders, continuations,
 *          overlong encodings, surrogates, and out-of-range scalar values, and
 *          requests replay when an error byte may begin the next sequence.
 */

#include "tui/term/Utf8Decoder.hpp"

namespace zanna::tui::term {

/// @brief Consume a single byte from the terminal and produce decoding status.
/// @details Implements a small state machine that recognises one- through
///          four-byte UTF-8 sequences.  When the decoder is idle (no expected
///          continuation bytes), it classifies the incoming byte as either an
///          ASCII code point, the leading byte of a multibyte sequence, or an
///          error.  Continuation bytes update the accumulated code point; once
///          the sequence completes, the helper validates against overlong forms,
///          surrogate halves, and the Unicode maximum before reporting success.
///          Errors reset the state and request that the caller replay the byte
///          if it might begin a new sequence.
/// @param byte Next octet sourced from the terminal stream.
/// @return Struct describing whether a code point was produced, whether an
///          error occurred, and if the caller should replay the input byte.
Utf8Result Utf8Decoder::feed(unsigned char byte) noexcept {
    Utf8Result result{};
    if (expected_ == 0) {
        length_ = 0;
        if (byte < 0x80) {
            result.has_codepoint = true;
            result.codepoint = byte;
        } else if ((byte & 0xE0) == 0xC0) {
            cp_ = byte & 0x1F;
            expected_ = 1;
            length_ = 2;
        } else if ((byte & 0xF0) == 0xE0) {
            cp_ = byte & 0x0F;
            expected_ = 2;
            length_ = 3;
        } else if ((byte & 0xF8) == 0xF0) {
            cp_ = byte & 0x07;
            expected_ = 3;
            length_ = 4;
        } else {
            result.error = true;
            reset();
        }
    } else if ((byte & 0xC0) == 0x80) {
        cp_ = (cp_ << 6) | (byte & 0x3F);
        if (--expected_ == 0) {
            const uint32_t codepoint = cp_;
            const unsigned length = length_;
            length_ = 0;

            bool overlong = false;
            switch (length) {
                case 2:
                    overlong = codepoint < 0x80;
                    break;
                case 3:
                    overlong = codepoint < 0x800;
                    break;
                case 4:
                    overlong = codepoint < 0x10000;
                    break;
                default:
                    break;
            }

            if (overlong || codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
                result.error = true;
                reset();
            } else {
                result.has_codepoint = true;
                result.codepoint = codepoint;
                cp_ = 0;
            }
        }
    } else {
        result.error = true;
        result.replay = true;
        reset();
    }
    return result;
}

/// @brief Report whether the decoder awaits continuation bytes.
/// @details The decoder is considered idle when no continuation bytes are
///          outstanding.  Callers can use this to detect incomplete multibyte
///          sequences at the end of an input buffer.
/// @return @c true when the decoder is ready for a new leading byte.
bool Utf8Decoder::idle() const noexcept {
    return expected_ == 0;
}

/// @brief Clear the decoder state after errors or explicit restarts.
/// @details Resets the accumulated code point, the expected continuation count,
///          and the cached sequence length.  Callers typically invoke this in
///          response to fatal terminal protocol errors.
void Utf8Decoder::reset() noexcept {
    cp_ = 0;
    expected_ = 0;
    length_ = 0;
}

} // namespace zanna::tui::term
