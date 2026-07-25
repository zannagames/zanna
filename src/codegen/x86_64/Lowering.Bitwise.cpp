//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE in the project root for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/Lowering.Bitwise.cpp
// Purpose: Implement bitwise opcode lowering rules for the x86-64 backend.
//          Emitters rely on EmitCommon to manage register materialisation and
//          operand cloning.
// Key invariants:
//   - Emitters only trigger when operands are valid.
//   - Resulting machine instructions operate on GPR registers only.
// Ownership/Lifetime:
//   - Operates on borrowed IL instructions and MIR builders; no ownership transfer.
// Links: src/codegen/x86_64/LoweringRules.hpp,
//        src/codegen/x86_64/Lowering.EmitCommon.hpp,
//        src/codegen/x86_64/MachineIR.hpp
//
//===----------------------------------------------------------------------===//

#include "LoweringRuleTable.hpp"

#include "LowerILToMIR.hpp"
#include "Lowering.EmitCommon.hpp"
#include "Unsupported.hpp"

/**
 * @file
 * @brief Implements GPR bitwise and shift rules for the x86-64 IL bridge.
 *
 * AND, OR, and XOR select legal register/immediate forms through EmitCommon.
 * Shift rules delegate count materialization so constants use immediate forms
 * and variable counts satisfy x86-64's fixed RCX/CL constraint.
 */

namespace zanna::codegen::x64::lowering {
namespace {

/// @brief True if @p kind is integer-like (I64/I1/PTR) and thus GPR-eligible.
/// @param kind IL value kind to classify.
/// @return @c true for values represented in the integer register bank.
[[nodiscard]] bool isIntegerLikeKind(ILValue::Kind kind) noexcept {
    return kind == ILValue::Kind::I64 || kind == ILValue::Kind::I1 || kind == ILValue::Kind::PTR;
}

/// @brief Assert a bitwise op's result is integer-like and maps to a GPR.
/// @details Bitwise opcodes only have GPR encodings; a non-integer or
///          non-GPR result raises phaseAUnsupported(@p context).
/// @param instr Instruction whose result type is validated.
/// @param builder Builder used to resolve the target register class.
/// @param context Diagnostic text reported on mismatch.
/// @throws std::runtime_error Through @c phaseAUnsupported for an invalid result.
void requireBitwiseResult(const ILInstr &instr, MIRBuilder &builder, const char *context) {
    if (!isIntegerLikeKind(instr.resultKind) ||
        builder.regClassFor(instr.resultKind) != RegClass::GPR) {
        phaseAUnsupported(context);
    }
}

} // namespace

/// @brief Lower a bitwise AND IL instruction.
/// @details Emits an `AND` binary instruction when the IL result type maps to a
///          general-purpose register, folding immediates through
///          @ref EmitCommon::emitBinary.
/// @param instr IL bitwise AND instruction.
/// @param builder Machine IR builder receiving the lowered sequence.
void emitAnd(const ILInstr &instr, MIRBuilder &builder) {
    requireBitwiseResult(instr, builder, "bitwise and: expected integer-like result");
    EmitCommon(builder).emitBinary(instr, MOpcode::ANDrr, MOpcode::ANDri, RegClass::GPR, true);
}

/// @brief Lower a bitwise OR IL instruction.
/// @details Restricts lowering to general-purpose registers and emits either the
///          register or immediate form of the OR instruction via
///          @ref EmitCommon::emitBinary.
/// @param instr IL bitwise OR instruction.
/// @param builder Machine IR builder receiving the lowered sequence.
void emitOr(const ILInstr &instr, MIRBuilder &builder) {
    requireBitwiseResult(instr, builder, "bitwise or: expected integer-like result");
    EmitCommon(builder).emitBinary(instr, MOpcode::ORrr, MOpcode::ORri, RegClass::GPR, true);
}

/// @brief Lower a bitwise XOR IL instruction.
/// @details Emits XOR register or immediate forms when the result type maps to
///          a general-purpose register, using @ref EmitCommon::emitBinary to
///          keep operand handling consistent.
/// @param instr IL bitwise XOR instruction.
/// @param builder Machine IR builder receiving the lowered sequence.
void emitXor(const ILInstr &instr, MIRBuilder &builder) {
    requireBitwiseResult(instr, builder, "bitwise xor: expected integer-like result");
    EmitCommon(builder).emitBinary(instr, MOpcode::XORrr, MOpcode::XORri, RegClass::GPR, true);
}

/// @brief Lower a shift-left IL instruction.
/// @details Delegates to @ref EmitCommon::emitShift so the helper can choose
///          between immediate and RCX-based shift encodings.
/// @param instr IL shift-left instruction.
/// @param builder Machine IR builder receiving the lowered sequence.
void emitShiftLeft(const ILInstr &instr, MIRBuilder &builder) {
    EmitCommon(builder).emitShift(instr, MOpcode::SHLri, MOpcode::SHLrc);
}

/// @brief Lower a logical right-shift IL instruction.
/// @details Uses @ref EmitCommon::emitShift to emit either the immediate or
///          variable shift form corresponding to the SHR opcode family.
/// @param instr IL logical right-shift instruction.
/// @param builder Machine IR builder receiving the lowered sequence.
void emitShiftLshr(const ILInstr &instr, MIRBuilder &builder) {
    EmitCommon(builder).emitShift(instr, MOpcode::SHRri, MOpcode::SHRrc);
}

/// @brief Lower an arithmetic right-shift IL instruction.
/// @details Invokes @ref EmitCommon::emitShift with SAR opcodes so signed shifts
///          normalise their operand handling across immediate and register
///          counts.
/// @param instr IL arithmetic right-shift instruction.
/// @param builder Machine IR builder receiving the lowered sequence.
void emitShiftAshr(const ILInstr &instr, MIRBuilder &builder) {
    EmitCommon(builder).emitShift(instr, MOpcode::SARri, MOpcode::SARrc);
}

} // namespace zanna::codegen::x64::lowering
