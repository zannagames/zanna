//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/asmfmt/Format.hpp
// Purpose: Provide reusable helpers for formatting x86-64 assembly operands.
// Key invariants:
//   - Helpers are side-effect free and produce AT&T syntax.
//   - Negative register IDs denote virtual registers formatted as "%vN".
//   - escape_ascii handles embedded quotes and backslashes.
// Ownership/Lifetime:
//   - Functions allocate and return std::string values by copy; callers own
//     the returned strings.
// Links: src/codegen/x86_64/asmfmt/Format.cpp,
//        src/codegen/x86_64/AsmEmitter.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

/// @file
/// @brief Declares stateless AT&T-syntax operand and read-only-data formatters.

/// @brief Formatting helpers shared by x86-64 textual assembly emitters.
/// @details Functions in this namespace return self-contained strings and
///          retain no references to caller-owned views after returning.
namespace asmfmt {

/// @brief Escapes a run of characters for use inside an @c .ascii string.
/// @details Prefixes double quotes and backslashes with a backslash. Other
///          bytes are copied verbatim, so callers that need arbitrary binary
///          data should use @ref format_rodata_bytes.
/// @param bytes Input byte sequence, normally a printable ASCII run.
/// @return Escaped copy suitable between the quotes of an assembler directive.
[[nodiscard]] std::string escape_ascii(std::string_view bytes);

/// @brief Formats a signed immediate operand in AT&T syntax.
/// @param v Immediate value to render in base ten.
/// @return A dollar sign followed by the signed decimal representation of @p v.
[[nodiscard]] std::string format_imm(std::int64_t v);

/// @brief Formats a plain assembly symbol reference.
/// @details Applies the common label sanitizer, including replacement of
///          unsupported characters and protection against an empty or
///          digit-leading result.
/// @param name Unsanitized label name.
/// @return Assembler-safe symbol spelling.
[[nodiscard]] std::string format_label(std::string_view name);

/// @brief Formats a RIP-relative assembly symbol reference.
/// @param name Unsanitized label name.
/// @return Sanitized symbol followed by the @c "(%rip)" addressing suffix.
[[nodiscard]] std::string format_rip_label(std::string_view name);

/// @brief Describes the components of an x86-64 memory operand.
/// @details The formatter does not validate architectural addressing rules.
///          Register identifiers use the convention accepted by @ref fmt_reg,
///          and @c has_index determines whether @c index and @c scale are read.
struct MemAddr {
    /// @brief Physical id or negative virtual-register encoding for the base.
    int base{-1};          ///< Encoded base register; negative for virtual regs.
    /// @brief Physical id or negative virtual-register encoding for the index.
    int index{-1};         ///< Encoded index register; negative when absent.
    /// @brief Scale printed only when @c has_index is true.
    std::uint8_t scale{1}; ///< Scaling factor applied to the index register.
    /// @brief Signed byte displacement; a zero value is omitted.
    std::int32_t disp{0};  ///< Signed displacement.
    /// @brief Whether to emit the index and scale components.
    bool has_index{false}; ///< True when the index register participates.
};

/// @brief Formats a base/index/scale/displacement operand in AT&T syntax.
/// @details Emits @c disp(base) without an index or
///          @c disp(base,index,scale) with one. A zero displacement is omitted.
/// @param a Memory-address descriptor to render.
/// @return Textual effective-address expression.
[[nodiscard]] std::string format_mem(const MemAddr &a);

/// @brief Formats a physical or virtual register identifier.
/// @details Non-negative values are cast to the backend's physical-register
///          enum. A negative value @c -N-1 denotes virtual register @c N.
/// @param reg Encoded physical or virtual register identifier.
/// @return Physical spelling such as @c "%rax", virtual spelling such as
///         @c "%v0", or @c "%unknown" for an unrecognized physical value.
[[nodiscard]] std::string fmt_reg(int reg);

/// @brief Formats a byte buffer for insertion into a read-only-data section.
/// @details Coalesces printable ASCII into escaped @c .ascii directives and
///          emits non-printable bytes in decimal @c .byte groups of at most
///          sixteen. Empty input produces an explanatory assembly comment.
/// @param bytes Literal payload; embedded null bytes are preserved.
/// @return Complete newline-terminated directive sequence.
[[nodiscard]] std::string format_rodata_bytes(std::string_view bytes);

} // namespace asmfmt
