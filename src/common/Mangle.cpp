//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/common/Mangle.cpp
// Purpose: Implement unified mangling for linkable symbols derived from
//          dot-qualified names used across frontends and OOP emission.
// Key invariants:
//   - Output is lowercase ASCII and uses only [a-z0-9_].
//   - Plain C-safe identifiers remain readable unless they use the reserved prefix.
//   - Qualified or otherwise unsafe names are encoded with reversible escapes.
// Ownership/Lifetime: Stateless helpers returning std::string by value.
// Links: src/common/Mangle.hpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements symbol mangling helpers for linkable names.
/// @details Provides deterministic conversions between user-facing qualified
///          identifiers and ASCII-safe linker symbols. The mapping is stable
///          across platforms and is reversible for names emitted with the
///          reserved prefix.

#include "common/Mangle.hpp"

#include <string>

namespace zanna::common {
namespace {

/// Prefix reserving the reversible escaped-symbol namespace.
constexpr std::string_view kReservedPrefix = "vpr_";

/// @brief Return whether @p ch is an ASCII decimal digit.
/// @param ch Byte to classify.
/// @return True when @p ch is in the range @c '0' through @c '9'.
[[nodiscard]] bool is_ascii_digit(unsigned char ch) noexcept {
    return ch >= '0' && ch <= '9';
}

/// @brief Return whether @p ch is an ASCII lowercase letter.
/// @param ch Byte to classify.
/// @return True when @p ch is in the range @c 'a' through @c 'z'.
[[nodiscard]] bool is_ascii_lower(unsigned char ch) noexcept {
    return ch >= 'a' && ch <= 'z';
}

/// @brief Convert one ASCII byte to lowercase without locale dependence.
/// @param ch Byte to normalize.
/// @return Lowercase ASCII byte when @p ch is @c A-Z, otherwise @p ch unchanged.
[[nodiscard]] unsigned char ascii_lower(unsigned char ch) noexcept {
    if (ch >= 'A' && ch <= 'Z') {
        return static_cast<unsigned char>(ch - 'A' + 'a');
    }
    return ch;
}

/// @brief Return whether @p normalized is a plain symbol that can remain unescaped.
/// @details Plain symbols are readable linker identifiers whose lowercase spelling
///          does not begin with the reserved escape prefix.
/// @param normalized Candidate after ASCII case folding; non-ASCII and other
///                   unsupported bytes cause the check to fail.
/// @return True when @p normalized can be emitted without the reserved encoding.
[[nodiscard]] bool can_emit_plain(std::string_view normalized) noexcept {
    if (normalized.empty() || normalized.rfind(kReservedPrefix, 0) == 0) {
        return false;
    }
    const auto first = static_cast<unsigned char>(normalized.front());
    if (!is_ascii_lower(first) && first != '_') {
        return false;
    }
    for (const unsigned char ch : normalized) {
        if (!is_ascii_lower(ch) && !is_ascii_digit(ch) && ch != '_') {
            return false;
        }
    }
    return true;
}

/// @brief Append a two-character lowercase hexadecimal byte escape.
/// @param out Destination symbol buffer.
/// @param ch Byte to encode after an @c _x escape introducer.
/// @post Exactly two lowercase hexadecimal characters are appended to @p out.
void append_hex_byte(std::string &out, unsigned char ch) {
    constexpr char digits[] = "0123456789abcdef";
    out.push_back(digits[(ch >> 4U) & 0x0FU]);
    out.push_back(digits[ch & 0x0FU]);
}

/// @brief Decode one lowercase hexadecimal digit.
/// @param ch ASCII byte to decode.
/// @return Value in the range 0-15, or -1 when @p ch is not hexadecimal.
[[nodiscard]] int decode_hex_digit(unsigned char ch) noexcept {
    if (ch >= '0' && ch <= '9') {
        return static_cast<int>(ch - '0');
    }
    if (ch >= 'a' && ch <= 'f') {
        return static_cast<int>(ch - 'a' + 10);
    }
    if (ch >= 'A' && ch <= 'F') {
        return static_cast<int>(ch - 'A' + 10);
    }
    return -1;
}

} // namespace

/// @copydoc MangleLink()
std::string MangleLink(std::string_view qualified) {
    std::string normalized;
    normalized.reserve(qualified.size());
    for (unsigned char ch : qualified) {
        normalized.push_back(static_cast<char>(ascii_lower(ch)));
    }

    if (can_emit_plain(normalized)) {
        return normalized;
    }

    std::string out;
    out.reserve(kReservedPrefix.size() + normalized.size() * 4U);
    out.append(kReservedPrefix);
    for (unsigned char ch : normalized) {
        if (is_ascii_lower(ch) || is_ascii_digit(ch)) {
            out.push_back(static_cast<char>(ch));
        } else if (ch == '.') {
            out.append("_d");
        } else if (ch == '_') {
            out.append("_u");
        } else {
            out.append("_x");
            append_hex_byte(out, ch);
        }
    }
    return out;
}

/// @copydoc DemangleLink()
std::string DemangleLink(std::string_view symbol) {
    if (symbol.rfind(kReservedPrefix, 0) == 0) {
        std::string out;
        std::string_view body = symbol.substr(kReservedPrefix.size());
        out.reserve(body.size());
        for (std::size_t i = 0; i < body.size(); ++i) {
            const unsigned char ch = static_cast<unsigned char>(body[i]);
            if (ch != '_') {
                out.push_back(static_cast<char>(ch));
                continue;
            }
            if (i + 1 >= body.size()) {
                out.push_back('_');
                continue;
            }
            const unsigned char code = static_cast<unsigned char>(body[++i]);
            if (code == 'd') {
                out.push_back('.');
            } else if (code == 'u') {
                out.push_back('_');
            } else if (code == 'x' && i + 2 < body.size()) {
                const int hi = decode_hex_digit(static_cast<unsigned char>(body[i + 1]));
                const int lo = decode_hex_digit(static_cast<unsigned char>(body[i + 2]));
                if (hi >= 0 && lo >= 0) {
                    out.push_back(static_cast<char>((hi << 4) | lo));
                    i += 2;
                } else {
                    out.append("_x");
                }
            } else {
                out.push_back('_');
                out.push_back(static_cast<char>(code));
            }
        }
        return out;
    }

    std::string_view body = symbol;
    if (!body.empty() && body.front() == '@') {
        body.remove_prefix(1);
        std::string out;
        out.reserve(body.size());
        for (unsigned char ch : body) {
            out.push_back(ch == '_' ? '.' : static_cast<char>(ch));
        }
        return out;
    }

    std::string out;
    out.reserve(body.size());
    for (unsigned char ch : body) {
        out.push_back(static_cast<char>(ch));
    }
    return out;
}

} // namespace zanna::common
