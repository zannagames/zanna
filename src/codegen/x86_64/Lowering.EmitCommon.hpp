//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/Lowering.EmitCommon.hpp
// Purpose: Provide shared emission helpers used by the lowering rule
//          translation units. Centralises register materialisation and
//          instruction construction so opcode-specific emitters remain focused.
// Key invariants:
//   - Helper routines never mutate MIRBuilder state outside the provided block.
//   - Register classes requested by the caller are honoured.
//   - Immediate operands are materialised only when necessary for the opcode.
// Ownership/Lifetime:
//   - EmitCommon borrows the MIRBuilder reference supplied at construction;
//     no ownership of IL or MIR nodes is transferred.
// Links: src/codegen/x86_64/Lowering.EmitCommon.cpp,
//        src/codegen/x86_64/LowerILToMIR.hpp,
//        src/codegen/x86_64/MachineIR.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "LowerILToMIR.hpp"
#include "MachineIR.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

/**
 * @file
 * @brief Declares shared operand legalization and MIR emission primitives.
 *
 * Opcode-specific lowering rules use EmitCommon to enforce IL shape and
 * register-class requirements consistently, materialize constants and address
 * expressions, choose legal x86-64 operand forms, and build common control-flow,
 * comparison, memory, cast, division, and return sequences.
 */

namespace zanna::codegen::x64 {

/// @brief Test whether a 64-bit signed value fits in a sign-extended imm32 encoding.
/// @param value Candidate immediate.
/// @return @c true when x86-64 can encode @p value in a signed imm32 field.
[[nodiscard]] inline bool fitsImm32(int64_t value) noexcept {
    return value >= static_cast<int64_t>(std::numeric_limits<int32_t>::min()) &&
           value <= static_cast<int64_t>(std::numeric_limits<int32_t>::max());
}

/**
 * @brief Shared facade that consolidates common lowering and legalization utilities.
 *
 * The facade borrows one MIRBuilder and appends only to that builder's active
 * block. Methods report malformed IL through the backend's unsupported-form
 * diagnostic path.
 */
class EmitCommon {
  public:
    /// @brief Construct the helper façade bound to a concrete MIR builder.
    /// @details Stores the provided @p builder pointer so subsequent emission helpers can append
    ///          instructions to the current block without re-threading references through every
    ///          call.  The façade never takes ownership of the builder; callers must ensure the
    ///          builder outlives the helper.
    /// @param builder Borrowed active builder.
    explicit EmitCommon(MIRBuilder &builder) noexcept;

    /// @brief Access the underlying MIR builder used to append Machine IR.
    /// @details Provides a reference for opcode-specific emitters that need direct access to
    ///          builder primitives.  The helper asserts in debug builds when the façade has not
    ///          been initialised with a valid builder pointer.
    /// @return Mutable borrowed builder reference.
    [[nodiscard]] MIRBuilder &builder() const noexcept;

    /// @brief Duplicate an operand while preserving its kind and payload.
    /// @details Materialises a new operand instance, copying register identifiers, immediates,
    ///          or memory operands as needed so the caller can safely mutate the clone without
    ///          affecting the original instruction operand.
    /// @param operand Value-semantic operand to duplicate.
    /// @return Independent operand copy.
    [[nodiscard]] Operand clone(const Operand &operand) const;

    /// @brief Materialise an operand into a register suitable for the requested class.
    /// @details Reuses existing registers when @p operand already matches @p cls, otherwise emits
    ///          the appropriate move or constant materialisation so the result can be consumed by
    ///          register-only opcodes.
    /// @param operand Source operand consumed by the materialization operation.
    /// @param cls Required destination register class.
    /// @return Existing compatible register or newly allocated temporary.
    [[nodiscard]] Operand materialise(Operand operand, RegClass cls);

    /// @brief Convenience wrapper that materialises an operand into a general-purpose register.
    /// @details Invokes @ref materialise with @ref RegClass::GPR to keep call sites concise when
    ///          lowering integer arithmetic and address calculations.
    /// @param operand Source operand.
    /// @return GPR operand holding the same value.
    [[nodiscard]] Operand materialiseGpr(Operand operand);

    /// @brief Emit a binary arithmetic operation with register or immediate forms.
    /// @details Selects between the register-register and register-immediate opcode variants,
    ///          materialising immediates when required and updating the MIR builder in-place with
    ///          the resulting instruction.
    /// @param instr Binary IL instruction.
    /// @param opcRR Register-register machine opcode.
    /// @param opcRI Register-immediate machine opcode.
    /// @param cls Result and operand register class.
    /// @param requireImm32 Require an immediate to fit signed 32 bits when @c true.
    void emitBinary(
        const ILInstr &instr, MOpcode opcRR, MOpcode opcRI, RegClass cls, bool requireImm32);

    /// @brief Emit a shift instruction handling both immediate and register counts.
    /// @details Uses @p opcImm when the shift amount is a constant that fits the encoding and
    ///          falls back to @p opcReg after materialising the count into CL when necessary.
    /// @param instr Shift IL instruction.
    /// @param opcImm Immediate-count machine opcode.
    /// @param opcReg RCX/CL-count machine opcode.
    void emitShift(const ILInstr &instr, MOpcode opcImm, MOpcode opcReg);

    /// @brief Emit an integer compare, mapping high-level predicates to X86 condition codes.
    /// @details Materialises operands into the requested register class, emits the compare, and
    ///          returns condition codes via @ref icmpConditionCode when the opcode encodes a
    ///          specific predicate.
    /// @param instr Compare IL instruction.
    /// @param cls Operand register class.
    /// @param defaultCond Condition code used when the opcode has no encoded predicate.
    void emitCmp(const ILInstr &instr, RegClass cls, int defaultCond);

    /// @brief Emit a SELECT instruction that implements ternary selection semantics.
    /// @details Materialises the condition and operands, emits the compare, and then builds the
    ///          conditional move or branch sequence required to produce the final value.
    /// @param instr Four-operand select instruction.
    void emitSelect(const ILInstr &instr);

    /// @brief Emit an unconditional branch to the instruction's successor block.
    /// @details Appends the jump to the MIR builder and records the target block so subsequent
    ///          passes can resolve edge metadata.
    /// @param instr Branch instruction containing a label operand.
    void emitBranch(const ILInstr &instr);

    /// @brief Emit a conditional branch mapping IL predicates onto X86 condition codes.
    /// @details Queries @ref icmpConditionCode or @ref fcmpConditionCode to translate the IL
    ///          opcode and then emits the branch pair (compare plus Jcc) while materialising
    ///          operands as required.
    /// @param instr Conditional branch instruction.
    void emitCondBranch(const ILInstr &instr);

    /// @brief Emit a return sequence for the current function.
    /// @details Materialises the return value into the ABI-designated register and appends the
    ///          RET instruction, leaving control-flow to the target lowering pipeline.
    /// @param instr Return instruction with an optional value operand.
    void emitReturn(const ILInstr &instr);

    /// @brief Emit a load from memory into the requested register class.
    /// @details Constructs the addressing mode from IL operands, materialises any index
    ///          registers, and emits the load instruction, returning the destination operand.
    /// @param instr Load instruction containing base and optional displacement.
    /// @param cls Register class of the loaded value.
    void emitLoad(const ILInstr &instr, RegClass cls);

    /// @brief Emit a store that writes an operand to memory.
    /// @details Materialises the value operand, constructs the memory address, and appends the
    ///          appropriate store instruction into the MIR builder.
    /// @param instr Store instruction containing address and value operands.
    void emitStore(const ILInstr &instr);

    /// @brief Emit a cast operation translating between register classes.
    /// @details Chooses the appropriate opcode @p opc, materialises the source operand into
    ///          @p srcCls, and produces a result in @p dstCls while handling sign/zero extension
    ///          requirements encoded by the opcode.
    /// @param instr Cast instruction.
    /// @param opc Machine conversion opcode.
    /// @param dstCls Destination register class.
    /// @param srcCls Source register class.
    void emitCast(const ILInstr &instr, MOpcode opc, RegClass dstCls, RegClass srcCls);

    /// @brief Emit a division or remainder sequence.
    /// @details Recognises whether the IL opcode expects quotient or remainder results and
    ///          materialises operands into the registers mandated by the X86 IDIV/FDIV semantics.
    /// @param instr Division or remainder instruction.
    /// @param opcode IL mnemonic used to select the target pseudo.
    void emitDivRem(const ILInstr &instr, std::string_view opcode);

    /// @brief Map an integer-compare opcode to the corresponding X86 condition code.
    /// @details Returns `std::nullopt` when the opcode does not carry sufficient predicate
    ///          information, allowing callers to fall back to default codes.
    /// @param opcode Integer comparison mnemonic.
    /// @return x86-64 condition code or @c std::nullopt.
    [[nodiscard]] static std::optional<int> icmpConditionCode(std::string_view opcode) noexcept;

    /// @brief Map a floating-compare opcode to an X86 condition code.
    /// @details Provides the same semantics as @ref icmpConditionCode but operates over the
    ///          floating-point compare opcodes produced by the IL.
    /// @param opcode Floating comparison mnemonic.
    /// @return x86-64 condition code or @c std::nullopt.
    [[nodiscard]] static std::optional<int> fcmpConditionCode(std::string_view opcode) noexcept;

    /// @brief Emit a NaN-safe floating-point comparison.
    /// @details After UCOMISD with NaN operands, x86 sets ZF=1, PF=1, CF=1.  A
    ///          single SETcc cannot correctly implement IEEE 754 semantics for
    ///          fcmp_eq, fcmp_ne, fcmp_lt, or fcmp_le.  This helper emits the
    ///          multi-instruction sequences required:
    ///          - eq: SETE ∧ SETNP (ordered equal)
    ///          - ne: SETNE ∨ SETP (unordered not-equal)
    ///          - lt: swap operands + SETA (ordered less-than)
    ///          - le: swap operands + SETAE (ordered less-or-equal)
    /// @param instr IL fcmp instruction.
    /// @param suffix The comparison suffix (eq, ne, lt, le).
    void emitFCmpNanSafe(const ILInstr &instr, std::string_view suffix);

    /// @brief Try to recognise (base + (idx << shift) + disp) addressing.
    /// @details Best-effort pattern matcher that returns an OpMem operand when
    ///          the address producer encodes a base pointer plus a scaled index
    ///          and optional displacement. Future improvements can plumb IL def
    ///          chains; the current implementation conservatively declines when
    ///          insufficient information is available.
    /// @param addrProducer Instruction supplying the base/index expression.
    /// @param displacementOperandIndex Index of an optional displacement operand.
    /// @return Folded memory operand, or @c std::nullopt when the pattern is unsafe.
    [[nodiscard]] std::optional<Operand> tryMakeIndexedMem(const ILInstr &addrProducer,
                                                           std::size_t displacementOperandIndex);

    /// @brief Fold an oversized displacement into the base by materialising it.
    /// @details If @p disp does not fit a signed 32-bit immediate, emits the
    ///          three-instruction sequence (MOVri scratch, MOVrr addr, ADDrr
    ///          addr+scratch) into the builder and returns the new addr operand
    ///          paired with disp=0. Otherwise returns @p baseOp unchanged and
    ///          @p disp untouched. Used by @ref emitLoad and @ref emitStore so
    ///          the imm32 boundary is handled in one place.
    /// @param baseOp Input base register operand (will be cloned).
    /// @param disp Displacement in bytes; reset to 0 when folded.
    /// @return Effective base operand (either @p baseOp or a freshly allocated
    ///         GPR temp holding base + disp).
    [[nodiscard]] Operand materialiseDisplacement(const Operand &baseOp, int64_t &disp);

  private:
    /// Borrowed builder that must outlive this facade.
    MIRBuilder *builder_{nullptr};
};

} // namespace zanna::codegen::x64
