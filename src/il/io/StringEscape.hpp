//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/io/StringEscape.hpp
// Purpose: Encoding and decoding of C-style escape sequences in IL string
//          literals (\n, \t, \r, \0, \\, \", \xNN). Used by the parser and
//          serializer to maintain ASCII-safe, round-trippable string constants.
// Key invariants:
//   - Round-trip: decode(encode(s)) == s for all valid UTF-8 strings.
//   - Encoder is stateless and preserves printable ASCII / multi-byte UTF-8.
//   - Decoder rejects malformed hex escapes with a descriptive error message.
// Ownership/Lifetime: Stateless free functions; no persistent state between
//          encode/decode calls.
// Links: il/io/Parser.hpp, il/io/Serializer.hpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares round-trippable escaping helpers for textual IL strings.

#pragma once

#include <cstdio>
#include <string>
#include <string_view>

namespace il::io {

/// @brief Decode common C-style escape sequences from @p input.
/// @param input String containing escape sequences like `\n`, `\t`, `\\`, `\"`, or `\xNN`.
/// @param output Destination for the decoded UTF-8 string.
/// @param error Optional pointer receiving a human-readable error message on failure.
/// @return True on success; false if @p input contains a malformed escape sequence.
bool decodeEscapedString(std::string_view input, std::string &output, std::string *error = nullptr);

/// @brief Encode control characters in @p input using C-style escape sequences.
/// @details Printable ASCII bytes are copied unchanged. Control, delete, and
///          non-ASCII bytes are emitted independently as uppercase `\xNN`
///          escapes, making the result safe inside a quoted IL literal.
/// @param input Raw UTF-8 string to encode.
/// @return Escaped representation safe for inclusion in IL text.
inline std::string encodeEscapedString(std::string_view input) {
    std::string out;
    out.reserve(input.size());
    for (unsigned char c : input) {
        switch (c) {
            case '\\':
                out.append("\\\\");
                break;
            case '\"':
                out.append("\\\"");
                break;
            case '\n':
                out.append("\\n");
                break;
            case '\r':
                out.append("\\r");
                break;
            case '\t':
                out.append("\\t");
                break;
            case '\0':
                out.append("\\0");
                break;
            default:
                if (c < 0x20 || c == 0x7F || c >= 0x80) {
                    char buf[5];
                    std::snprintf(buf, sizeof(buf), "\\x%02X", c);
                    out.append(buf);
                } else {
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    return out;
}

} // namespace il::io
