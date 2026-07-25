//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/EmitCommon.cpp
// Purpose: Define shared IR emission helpers for BASIC lowering to eliminate
//          repetitive instruction construction.
// Key invariants: Helpers update the Lowerer source location before emitting
//                 instructions, preserving diagnostic fidelity.
// Ownership/Lifetime: Emit borrows the Lowerer and never owns IR objects.
// Links: src/frontends/basic/EmitCommon.hpp,
//        src/frontends/basic/Lowerer.hpp,
//        src/frontends/basic/lower/Emitter.hpp
//
//===----------------------------------------------------------------------===//

#include "frontends/basic/EmitCommon.hpp"

#include "frontends/basic/DiagnosticEmitter.hpp"
#include "frontends/basic/Lowerer.hpp"
#include "frontends/basic/lower/Emitter.hpp"
#include "support/diagnostics.hpp"

#include <cassert>
#include <cstdint>
#include <string>

namespace il::frontends::basic {

/// @brief Construct an Emit helper that forwards to the provided Lowerer.
/// @details Stores a pointer to the lowering façade so subsequent helper calls
///          can emit instructions while automatically maintaining source
///          locations.  The helper borrows the Lowerer and therefore assumes it
///          outlives the Emit instance.
/// @param lowerer Lowering façade that ultimately owns instruction emission.
Emit::Emit(Lowerer &lowerer) noexcept : lowerer_(&lowerer) {}

/// @brief Set the source location applied to the next emitted instruction.
/// @details Caches @p loc so subsequent calls to @ref emitUnary or
///          @ref emitBinary update the Lowerer's current location before
///          delegating. The location remains staged after emission. The helper
///          returns @c *this to allow fluent chaining.
/// @param loc Source location to apply.
/// @return Reference to the helper for fluent chaining.
Emit &Emit::at(il::support::SourceLoc loc) noexcept {
    loc_ = loc;
    return *this;
}

/// @brief Convert an integer value to a specific bit width.
/// @details Dispatches to @ref widen_to when the target width exceeds the
///          source and to @ref narrow_to otherwise.  When the widths already
///          match the value is returned unchanged, avoiding redundant IR.
/// @param value Operand being converted.
/// @param bits Target integer width in bits.
/// @param fromBits Source integer width in bits.
/// @param signedness Indicates how widening should treat the incoming value.
/// @return Converted value with the requested width.
Emit::Value Emit::to_iN(Value value, int bits, int fromBits, Signedness signedness) const {
    if (bits == fromBits) {
        return value;
    }
    if (bits > fromBits) {
        return widen_to(value, fromBits, bits, signedness);
    }
    return narrow_to(value, fromBits, bits);
}

/// @brief Widen an integer to a larger bit width while preserving semantics.
/// @details Handles boolean extension via explicit zero-extend, masks down
///          16- and 32-bit values before performing arithmetic shifts for sign
///          extension, and reports unsupported widths.  Unsigned values are
///          masked to guarantee upper bits are zero.
/// @param value Operand to widen.
/// @param fromBits Source width in bits.
/// @param toBits Destination width in bits (currently only 64).
/// @param signedness Determines whether sign extension should be applied.
/// @return Widened value ready for insertion into the IR.
Emit::Value Emit::widen_to(Value value, int fromBits, int toBits, Signedness signedness) const {
    assert(toBits == 64 && "widen_to currently supports widening to i64");
    if (fromBits == toBits) {
        return value;
    }
    if (fromBits == 1) {
        return emitUnary(il::core::Opcode::Zext1, intType(toBits), value);
    }
    if (fromBits == 16 || fromBits == 32) {
        const std::int64_t mask = (fromBits == 16) ? 0xFFFFll : 0xFFFFFFFFll;
        if (signedness == Signedness::Unsigned) {
            return emitBinary(
                il::core::Opcode::And, intType(toBits), value, il::core::Value::constInt(mask));
        }
        const int shift = (fromBits == 32) ? 32 : 48;
        Value masked = emitBinary(
            il::core::Opcode::And, intType(toBits), value, il::core::Value::constInt(mask));
        Value shl = emitBinary(
            il::core::Opcode::Shl, intType(toBits), masked, il::core::Value::constInt(shift));
        return emitBinary(
            il::core::Opcode::AShr, intType(toBits), shl, il::core::Value::constInt(shift));
    }

    if (auto *diag = lowerer_->diagnosticEmitter()) {
        diag->emit(il::support::Severity::Error,
                   "B9002",
                   loc_.value_or(il::support::SourceLoc{}),
                   1,
                   "unsupported integer widening from " + std::to_string(fromBits) + " to " +
                       std::to_string(toBits) + " bits");
    }
    return value;
}

/// @brief Narrow an integer value to a smaller bit width.
/// @details Returns the value unchanged when the widths match, emits a boolean
///          truncation for @c i1 results, and otherwise emits the checked
///          narrowing opcode that raises a trap when the value exceeds the
///          target width.
/// @param value Operand to narrow.
/// @param fromBits Source width in bits.
/// @param toBits Destination width in bits.
/// @return Narrowed value conforming to the requested width.
Emit::Value Emit::narrow_to(Value value, int fromBits, int toBits) const {
    if (fromBits == toBits) {
        return value;
    }
    if (toBits == 1) {
        return emitUnary(il::core::Opcode::Trunc1, intType(toBits), value);
    }
    return emitUnary(il::core::Opcode::CastSiNarrowChk, intType(toBits), value);
}

/// @brief Emit an addition with optional overflow trapping.
/// @details Chooses between the checked @c iadd.ovf opcode and the plain add
///          instruction based on @p policy, producing an integer of the supplied
///          width.  Callers use this to share the overflow policy logic across
///          the lowering pipeline.
/// @param lhs Left-hand operand.
/// @param rhs Right-hand operand.
/// @param policy Overflow handling strategy.
/// @param bits Bit width of the result.
/// @return Result value representing the addition.
Emit::Value Emit::add_checked(Value lhs, Value rhs, OverflowPolicy policy, int bits) const {
    const il::core::Opcode op =
        (policy == OverflowPolicy::Checked) ? il::core::Opcode::IAddOvf : il::core::Opcode::Add;
    return emitBinary(op, intType(bits), lhs, rhs);
}

/// @brief Emit a bitwise AND across two operands.
/// @details Delegates to @ref emitBinary with the appropriate opcode while
///          ensuring the result uses the supplied integer width.
/// @param lhs Left operand.
/// @param rhs Right operand.
/// @param bits Bit width of the result.
/// @return Value representing the AND operation.
Emit::Value Emit::logical_and(Value lhs, Value rhs, int bits) const {
    return emitBinary(il::core::Opcode::And, intType(bits), lhs, rhs);
}

/// @brief Emit a bitwise OR between two operands.
/// @details Shares the plumbing with other helpers to keep location updates and
///          type construction centralised.
/// @param lhs Left operand.
/// @param rhs Right operand.
/// @param bits Bit width of the result.
/// @return Value representing the OR operation.
Emit::Value Emit::logical_or(Value lhs, Value rhs, int bits) const {
    return emitBinary(il::core::Opcode::Or, intType(bits), lhs, rhs);
}

/// @brief Emit a bitwise XOR between two operands.
/// @details Thin wrapper around @ref emitBinary that ensures the opcode, type,
///          and location bookkeeping remain consistent with other helpers.
/// @param lhs Left operand.
/// @param rhs Right operand.
/// @param bits Bit width of the result.
/// @return Value representing the XOR operation.
Emit::Value Emit::logical_xor(Value lhs, Value rhs, int bits) const {
    return emitBinary(il::core::Opcode::Xor, intType(bits), lhs, rhs);
}

/// @brief Construct an IL integer type object for the given width.
/// @details Maps the supported bit widths to their corresponding IL type
///          variants and reports an error if an unsupported width is requested.  The
///          helper centralises the mapping so call sites remain concise.
/// @param bits Desired integer width.
/// @return Type descriptor matching the width.
Emit::Type Emit::intType(int bits) const {
    switch (bits) {
        case 1:
            return Type(Type::Kind::I1);
        case 16:
            return Type(Type::Kind::I16);
        case 32:
            return Type(Type::Kind::I32);
        case 64:
            return Type(Type::Kind::I64);
        default:
            break;
    }
    if (auto *diag = lowerer_->diagnosticEmitter()) {
        diag->emit(il::support::Severity::Error,
                   "B9003",
                   loc_.value_or(il::support::SourceLoc{}),
                   1,
                   "unsupported integer width " + std::to_string(bits));
    }
    return Type(Type::Kind::I64);
}

/// @brief Apply the cached source location to the underlying Lowerer.
/// @details Uses the public setSourceLocation() accessor when a location has
///          been staged via @ref at. This eliminates the need for Emit to be a
///          friend of Lowerer. Called automatically by emission helpers to keep
///          diagnostics in sync with the original AST nodes.
void Emit::applyLoc() const {
    if (loc_) {
        lowerer_->setSourceLocation(*loc_);
    }
}

/// @brief Forward a unary operation to the Lowerer with location bookkeeping.
/// @details Ensures @ref applyLoc runs before delegating to the Lowerer so the
///          resulting instruction carries the correct source metadata.
/// @param op Opcode to emit.
/// @param ty Resulting type of the operation.
/// @param val Operand consumed by the unary instruction.
/// @return Value produced by the emitted instruction.
Emit::Value Emit::emitUnary(Opcode op, Type ty, Value val) const {
    applyLoc();
    return lowerer_->emitUnary(op, ty, val);
}

/// @brief Forward a binary operation to the Lowerer with location bookkeeping.
/// @details Mirrors @ref emitUnary but accepts two operands, keeping the
///          emission path uniform across arithmetic helpers.
/// @param op Opcode to emit.
/// @param ty Result type produced by the instruction.
/// @param lhs Left operand.
/// @param rhs Right operand.
/// @return Value produced by the emitted instruction.
Emit::Value Emit::emitBinary(Opcode op, Type ty, Value lhs, Value rhs) const {
    applyLoc();
    return lowerer_->emitBinary(op, ty, lhs, rhs);
}

} // namespace il::frontends::basic
