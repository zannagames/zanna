//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/asmfmt/Format.cpp
// Purpose: Format operands and data blobs for x86-64 assembly emission.
// Key invariants:
//   - Helpers are side-effect free and avoid relying on global state so
//     emitters can reuse them across passes.
// Ownership/Lifetime:
//   - Operates solely on caller-provided views and returns newly constructed
//     strings; no persistent state.
// Links: src/codegen/x86_64/asmfmt/Format.hpp,
//        src/codegen/x86_64/TargetX64.hpp
//
//===----------------------------------------------------------------------===//

#include "Format.hpp"

#include "../TargetX64.hpp"
#include "codegen/common/LabelUtil.hpp"

#include <cstddef>
#include <string>

/// @file
/// @brief Implements stateless x86-64 assembly text-formatting helpers.

namespace asmfmt {
namespace {
/// @brief Determine whether a byte is printable ASCII.
/// @details Uses the locale-independent inclusive range from space through
///          tilde, making read-only-data output deterministic on every host.
/// @param ch Byte to inspect.
/// @return @c true when @p ch is in the range @c 0x20 through @c 0x7e.
[[nodiscard]] bool is_printable(unsigned char ch) noexcept {
    return ch >= 0x20U && ch <= 0x7EU;
}
} // namespace

/// @brief Escape embedded quotes and backslashes in an ASCII string.
/// @details Walks the input bytes and prefixes `"` and `\\` characters with a
///          backslash so the result can be embedded in `.ascii` directives.
///          It intentionally performs no printable-byte validation.
/// @param bytes Input character sequence to escape.
/// @return Escaped string suitable for assembly output.
std::string escape_ascii(std::string_view bytes) {
    std::string out{};
    out.reserve(bytes.size());
    for (const unsigned char ch : bytes) {
        if (ch == static_cast<unsigned char>('\\') || ch == static_cast<unsigned char>('"')) {
            out.push_back('\\');
        }
        out.push_back(static_cast<char>(ch));
    }
    return out;
}

/// @brief Format an immediate value using AT&T syntax.
/// @param v Integer value to print.
/// @return String containing the `$`-prefixed decimal literal.
std::string format_imm(std::int64_t v) {
    return std::string{"$"} + std::to_string(v);
}

/// @brief Convert a label name into an assembly operand.
/// @details Delegates all character replacement and leading-character handling
///          to the target-independent label sanitizer.
/// @param name Label identifier to emit.
/// @return Sanitized copy of @p name as a standard symbol reference.
std::string format_label(std::string_view name) {
    return zanna::codegen::common::sanitizeLabel(name);
}

/// @brief Emit a RIP-relative reference to a label.
/// @details Sanitizes the symbol before attaching the x86-64 program-counter
///          relative addressing suffix.
/// @param name Label identifier appearing in the operand.
/// @return Formatted string using the `symbol(%rip)` syntax.
std::string format_rip_label(std::string_view name) {
    std::string result = zanna::codegen::common::sanitizeLabel(name);
    result += "(%rip)";
    return result;
}

/// @brief Format either a physical or virtual register name.
/// @details Non-negative values are interpreted as physical registers using the
///          backend's name table, while negative values produce virtual register
///          mnemonics of the form `%vN`, where @c N equals @c -reg-1.
/// @param reg Encoded register identifier (physical or virtual).
/// @return Textual register representation for assembly output.
std::string fmt_reg(int reg) {
    if (reg >= 0) {
        const auto phys =
            zanna::codegen::x64::regName(static_cast<zanna::codegen::x64::PhysReg>(reg));
        return std::string{phys != nullptr ? phys : "%r?"};
    }

    const unsigned virt = static_cast<unsigned>(-reg - 1);
    std::string result;
    result.reserve(16); // Pre-size for typical virtual register names
    result += "%v";
    result += std::to_string(virt);
    return result;
}

/// @brief Render a memory addressing expression.
/// @details Emits the displacement, base register, and optional index/scale in
///          canonical AT&T order.  Missing fields are omitted to avoid
///          redundant commas. The descriptor is assumed to contain a valid
///          scale and register combination; this presentation helper does not
///          enforce x86-64 addressing restrictions.
/// @param a Structured memory operand describing the address.
/// @return Assembly string representing the effective address.
std::string format_mem(const MemAddr &a) {
    std::string result;
    result.reserve(64); // Pre-size for typical memory operand

    if (a.disp != 0) {
        result += std::to_string(a.disp);
    }

    result += '(';
    result += fmt_reg(a.base);
    if (a.has_index) {
        result += ',';
        result += fmt_reg(a.index);
        result += ',';
        result += std::to_string(static_cast<unsigned>(a.scale));
    }
    result += ')';
    return result;
}

/// @brief Format raw data bytes into `.ascii` and `.byte` directives.
/// @details Groups printable runs into `.ascii` directives with escaped
///          content and emits up to 16 non-printable bytes per `.byte` line.
///          Printable bytes end a byte group so subsequent printable runs can
///          return to the more compact string representation.
/// @param bytes Raw data blob to render.
/// @return Multi-line assembly listing representing the data section.
std::string format_rodata_bytes(std::string_view bytes) {
    std::string result;
    // Reserve approximately 3x the input size to accommodate formatting overhead
    result.reserve(bytes.size() * 3 + 32);

    if (bytes.empty()) {
        result += "  # empty literal\n";
        return result;
    }

    std::size_t index = 0;
    while (index < bytes.size()) {
        const unsigned char current = static_cast<unsigned char>(bytes[index]);
        if (is_printable(current)) {
            const std::size_t begin = index;
            while (index < bytes.size() && is_printable(static_cast<unsigned char>(bytes[index]))) {
                ++index;
            }
            const std::string_view run{bytes.data() + begin, index - begin};
            result += "  .ascii \"";
            result += escape_ascii(run);
            result += "\"\n";
            continue;
        }

        result += "  .byte ";
        std::size_t emitted = 0;
        while (index < bytes.size() && emitted < 16U) {
            const unsigned char byte_val = static_cast<unsigned char>(bytes[index]);
            if (is_printable(byte_val)) {
                break;
            }
            if (emitted != 0) {
                result += ", ";
            }
            result += std::to_string(static_cast<unsigned>(byte_val));
            ++index;
            ++emitted;
        }
        result += '\n';
    }

    return result;
}

} // namespace asmfmt
