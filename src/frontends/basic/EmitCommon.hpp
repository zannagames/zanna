//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/EmitCommon.hpp
// Purpose: Declare shared IR emission helpers to reduce boilerplate across the
//          BASIC front-end.
// Key invariants: Helpers honour the active Lowerer context and only append
//                 instructions to the current block when one is selected.
// Ownership/Lifetime: Emit borrows the Lowerer instance; IR objects remain
//                     owned by the lowering pipeline.
// Links: src/frontends/basic/EmitCommon.cpp,
//        src/frontends/basic/Lowerer.hpp,
//        docs/internals/architecture.md#cpp-overview
//
//===----------------------------------------------------------------------===//

/**
 * @file EmitCommon.hpp
 * @brief Declares shared typed integer-emission helpers for BASIC lowering.
 *
 * Emit stages an optional persistent source location and centralizes integer
 * width selection, checked narrowing, widening, arithmetic, and bitwise logic.
 */

#pragma once

#include "support/source_location.hpp"
#include "zanna/il/Module.hpp"

#include <cstdint>
#include <optional>

namespace il::frontends::basic {

class Lowerer;

/// @brief Selects how overflow should be handled for arithmetic helpers.
enum class OverflowPolicy {
    Checked, ///< Emit overflow-checking arithmetic.
    Wrap     ///< Emit wrapping arithmetic.
};

/// @brief Indicates whether widening preserves signedness or zero-extends.
enum class Signedness { Signed, Unsigned };

/// @brief Facade that centralises common IL emission patterns for the BASIC front end.
/// @invariant Helpers that emit instructions require an active Lowerer block.
/// @ownership Does not own IR objects; borrows the Lowerer state for instruction emission.
class Emit {
  public:
    /// IL SSA value type used by helper signatures.
    using Value = il::core::Value;
    /// IL result type used by helper signatures.
    using Type = il::core::Type;
    /// IL opcode type used by private emission adapters.
    using Opcode = il::core::Opcode;

    /// @brief Bind an emission facade to a Lowerer.
    /// @param lowerer Parent lowering context used by all operations.
    /// @warning @p lowerer must outlive this facade.
    explicit Emit(Lowerer &lowerer) noexcept;

    /// @brief Annotate subsequent emissions with @p loc.
    /// @details Replaces the previously staged location and retains it until
    ///          another call to at() or destruction; emission does not consume it.
    /// @param loc Source location forwarded before each emitted instruction.
    /// @return This facade for fluent use.
    Emit &at(il::support::SourceLoc loc) noexcept;

    /// @brief Convert @p value from @p fromBits to @p bits.
    /// @details Returns unchanged for equal widths, widens with @p signedness,
    ///          or performs checked signed narrowing.
    /// @param value Source IL value.
    /// @param bits Requested result width.
    /// @param fromBits Logical source width.
    /// @param signedness Extension behavior used only when widening.
    /// @return Original or emitted conversion result.
    [[nodiscard]] Value to_iN(Value value,
                              int bits,
                              int fromBits = 64,
                              Signedness signedness = Signedness::Signed) const;

    /// @brief Widen @p value from @p fromBits to @p toBits.
    /// @details Supports 1-, 16-, and 32-bit inputs widening to 64 bits. Invalid
    ///          combinations emit B9002 when diagnostics are available and
    ///          return @p value unchanged.
    /// @param value Source IL value.
    /// @param fromBits Logical source width.
    /// @param toBits Destination width; expected to be 64.
    /// @param signedness Sign- or zero-extension selection for 16/32-bit inputs.
    /// @return Widened value or the unchanged input on unsupported widths.
    /// @pre @p toBits is 64 in assertion-enabled builds.
    [[nodiscard]] Value widen_to(Value value,
                                 int fromBits,
                                 int toBits,
                                 Signedness signedness = Signedness::Signed) const;

    /// @brief Narrow @p value from @p fromBits to @p toBits with overflow checking.
    /// @param value Source IL value.
    /// @param fromBits Logical source width; used to detect a no-op conversion.
    /// @param toBits Destination integer width.
    /// @return Original value for equal widths, Trunc1 result for i1, or a
    ///         CastSiNarrowChk result for other widths.
    [[nodiscard]] Value narrow_to(Value value, int fromBits, int toBits) const;

    /// @brief Emit an integer addition following the requested overflow policy.
    /// @param lhs Left integer operand.
    /// @param rhs Right integer operand.
    /// @param policy Checked selects IAddOvf; Wrap selects Add.
    /// @param bits Result integer width.
    /// @return Emitted SSA result.
    [[nodiscard]] Value add_checked(Value lhs,
                                    Value rhs,
                                    OverflowPolicy policy,
                                    int bits = 64) const;

    /// @brief Emit a logical AND on BASIC logical masks.
    /// @param lhs Left mask operand.
    /// @param rhs Right mask operand.
    /// @param bits Result integer width.
    /// @return Bitwise AND SSA result.
    [[nodiscard]] Value logical_and(Value lhs, Value rhs, int bits = 64) const;

    /// @brief Emit a logical OR on BASIC logical masks.
    /// @param lhs Left mask operand.
    /// @param rhs Right mask operand.
    /// @param bits Result integer width.
    /// @return Bitwise OR SSA result.
    [[nodiscard]] Value logical_or(Value lhs, Value rhs, int bits = 64) const;

    /// @brief Emit a logical XOR on BASIC logical masks.
    /// @param lhs Left mask operand.
    /// @param rhs Right mask operand.
    /// @param bits Result integer width.
    /// @return Bitwise XOR SSA result.
    [[nodiscard]] Value logical_xor(Value lhs, Value rhs, int bits = 64) const;

  private:
    ///< Borrowed parent lowerer.
    Lowerer *lowerer_;
    ///< Persistent optional source location applied before emission.
    mutable std::optional<il::support::SourceLoc> loc_;

    /// @brief Map a supported bit width to an IL integer type.
    /// @param bits Requested width: 1, 16, 32, or 64.
    /// @return Matching type, or i64 after emitting B9003 for unsupported widths.
    [[nodiscard]] Type intType(int bits) const;
    /// @brief Forward the staged source location when one is present.
    void applyLoc() const;
    /// @brief Emit one unary instruction after applying staged location.
    /// @param op Unary opcode.
    /// @param ty Result type.
    /// @param val Operand.
    /// @return Emitted SSA result.
    [[nodiscard]] Value emitUnary(Opcode op, Type ty, Value val) const;
    /// @brief Emit one binary instruction after applying staged location.
    /// @param op Binary opcode.
    /// @param ty Result type.
    /// @param lhs Left operand.
    /// @param rhs Right operand.
    /// @return Emitted SSA result.
    [[nodiscard]] Value emitBinary(Opcode op, Type ty, Value lhs, Value rhs) const;
};

} // namespace il::frontends::basic
