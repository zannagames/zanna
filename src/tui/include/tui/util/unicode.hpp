//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Declares Unicode display-width and UTF-8 decoding utilities.
/// @details Provides allocation-free code-point width classification and an
///          owning UTF-32 decoder that substitutes U+FFFD for malformed input.
//
// char_width() determines how many terminal columns a Unicode code point
// occupies, following the Unicode East Asian Width property:
//   - Combining marks and zero-width characters return 0
//   - CJK ideographs and other wide characters return 2
//   - All other printable characters return 1
//
// decode_utf8() converts a UTF-8 byte sequence into a UTF-32 string,
// replacing invalid sequences with the Unicode replacement character U+FFFD.
//
// Key invariants:
//   - char_width() returns 0, 1, or 2 for any valid code point.
//   - decode_utf8() never produces surrogate code points.
//   - Invalid UTF-8 sequences are replaced, not skipped.
//
// Ownership: Functions are stateless. decode_utf8() returns an owned string.
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace zanna::tui::util {
/// @brief Determine the terminal display width of a Unicode code point.
/// @details Returns the number of terminal columns the character occupies:
///          0 for combining marks and zero-width characters, 2 for wide CJK
///          ideographs and fullwidth forms, and 1 for all other printable
///          characters. Based on the Unicode East Asian Width property.
/// @param cp Unicode scalar value (U+0000 to U+10FFFF).
/// @return Display width: 0, 1, or 2 columns.
[[nodiscard]] int char_width(char32_t cp);

/// @brief Decode a UTF-8 byte sequence into a UTF-32 string.
/// @details Processes each byte of the input, assembling multi-byte UTF-8
///          sequences into Unicode code points. Invalid or incomplete sequences
///          are replaced with U+FFFD (Unicode Replacement Character).
/// @param in UTF-8 encoded byte sequence.
/// @return UTF-32 string of decoded code points.
[[nodiscard]] std::u32string decode_utf8(std::string_view in);

} // namespace zanna::tui::util
