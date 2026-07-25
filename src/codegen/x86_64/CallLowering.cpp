//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/CallLowering.cpp
// Purpose: Implements the call-lowering phase for the x86-64 backend, mapping
//          abstract call plans into concrete SysV or Win64 argument-setup MIR.
// Key invariants:
//   - Argument registers are populated in ABI order before the CALL instruction.
//   - Outgoing argument stack areas are 8-byte aligned.
//   - Frame metadata is updated with new stack argument slots.
// Ownership/Lifetime:
//   - Mutates the caller-provided MBasicBlock and FrameInfo in-place; does not
//     retain references after lowerCall() returns.
// Links: src/codegen/x86_64/CallLowering.hpp,
//        src/codegen/x86_64/FrameLowering.hpp,
//        src/codegen/x86_64/TargetX64.hpp
//
//===----------------------------------------------------------------------===//

#include "CallLowering.hpp"

#include "FrameLowering.hpp"
#include "OperandRoles.hpp"
#include "OperandUtils.hpp"
#include "TargetX64.hpp"

#include <algorithm>
#include <cassert>
#include <limits>
#include <stdexcept>
#include <string>

/**
 * @file
 * @brief Implements ABI-specific x86-64 call-argument materialization.
 *
 * The implementation delegates placement decisions to the common call layout
 * planner, then translates those decisions into MIR moves and stack stores.
 * It also handles aggregate chunks, boolean extension, SysV's variadic XMM
 * count, Win64's variadic floating-point duplication, and checked frame sizing.
 */

namespace zanna::codegen::x64 {

namespace {

/// Volatile integer register used to stage arguments without consuming a vreg.
constexpr PhysReg kScratchGPR = PhysReg::R11;
/// Volatile vector register used to stage floating-point stack arguments.
constexpr PhysReg kScratchXMM = PhysReg::XMM15;

/// @brief Compute outgoing-call stack bytes with overflow checking.
/// @details The call layout reports stack slots in 8-byte units and Win64 adds
///          a fixed shadow-space prefix. This helper performs the multiplication
///          and addition in size_t while rejecting values that would wrap.
/// @param shadowSpace ABI-mandated prefix bytes before stack arguments.
/// @param stackSlots Number of 8-byte outgoing stack slots.
/// @return Total outgoing-call byte count before frame-size rounding.
/// @throws std::length_error If the multiplication or addition would overflow.
static std::size_t checkedOutgoingStackBytes(std::size_t shadowSpace, std::size_t stackSlots) {
    constexpr std::size_t kMax = std::numeric_limits<std::size_t>::max();
    if (stackSlots > (kMax - shadowSpace) / kSlotSizeBytes)
        throw std::length_error("x86-64 call lowering: outgoing argument area exceeds size_t");
    return shadowSpace + stackSlots * kSlotSizeBytes;
}

/// @brief Compute a frame-resident outgoing stack slot offset.
/// @details Returns the byte offset used by stack-slot MIR operands, rejecting
///          layouts that cannot be represented in the signed 32-bit displacement
///          carried by the downstream frame and encoding paths.
/// @param shadowSpace ABI-mandated prefix bytes before stack arguments.
/// @param stackSlotIndex Zero-based outgoing stack slot index.
/// @return Signed 32-bit byte offset from the outgoing-argument area base.
static int32_t checkedStackSlotOffset(std::size_t shadowSpace, std::size_t stackSlotIndex) {
    const std::size_t offset = checkedOutgoingStackBytes(shadowSpace, stackSlotIndex);
    if (offset > static_cast<std::size_t>(std::numeric_limits<int32_t>::max()))
        throw std::length_error("x86-64 call lowering: stack argument offset exceeds int32_t");
    return static_cast<int32_t>(offset);
}

/// @brief Narrow an aggregate byte offset to the memory-operand displacement type.
/// @param byteOffset Offset of an eight-byte aggregate chunk from its source base.
/// @return @p byteOffset represented as a signed 32-bit displacement.
/// @throws std::length_error If the offset exceeds the displacement range.
static int32_t checkedAggregateChunkOffset(std::size_t byteOffset) {
    if (byteOffset > static_cast<std::size_t>(std::numeric_limits<int32_t>::max()))
        throw std::length_error("x86-64 call lowering: aggregate chunk offset exceeds int32_t");
    return static_cast<int32_t>(byteOffset);
}

/// @brief Round and narrow outgoing-call frame bytes to FrameInfo's int field.
/// @details FrameInfo stores byte counts as signed ints, so this helper rejects
///          valid size_t layouts that would not survive the existing field type.
/// @param stackBytes Raw outgoing-call byte count.
/// @return Rounded byte count representable as int.
/// @throws std::length_error If rounding overflows or the result exceeds @c int.
static int checkedOutgoingAreaForFrame(std::size_t stackBytes) {
    const std::size_t rounded = roundUpSize(stackBytes, kSlotSizeBytes);
    if (rounded > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw std::length_error("x86-64 call lowering: outgoing argument area exceeds int");
    return static_cast<int>(rounded);
}

/// @brief Decide whether an instruction produces the boolean SSA value @p vreg.
///
/// @details Walks the instruction's defined operands to see if it writes the
///          provided virtual register via a @c SETcc opcode. SETcc carries its
///          condition code before the destination operand, so the generic
///          operand-role table is used instead of assuming operand zero is the
///          def.
///
/// @param instr Instruction whose operands are inspected.
/// @param vreg Virtual register identifier associated with the SSA value.
/// @return True when the instruction writes @p vreg as a boolean result.
[[nodiscard]] bool isBoolProducer(const MInstr &instr, uint16_t vreg) {
    if (instr.opcode != MOpcode::SETcc) {
        return false;
    }
    for (std::size_t idx = 0; idx < instr.operands.size(); ++idx) {
        const auto [isUse, isDef] = operandRoles(instr, idx);
        (void)isUse;
        if (!isDef) {
            continue;
        }
        if (const auto *reg = std::get_if<OpReg>(&instr.operands[idx]); reg) {
            if (!reg->isPhys && reg->idOrPhys == vreg) {
                return true;
            }
        }
    }
    return false;
}

/// @brief Test whether @p instr writes the virtual register @p vreg through any def-role operand.
/// @details Used by @ref isI1Value to terminate the backward search as soon as
///          another definition shadows the candidate boolean producer.
[[nodiscard]] bool definesVReg(const MInstr &instr, uint16_t vreg) {
    for (std::size_t idx = 0; idx < instr.operands.size(); ++idx) {
        const auto [isUse, isDef] = operandRoles(instr, idx);
        (void)isUse;
        if (!isDef) {
            continue;
        }
        if (const auto *reg = std::get_if<OpReg>(&instr.operands[idx]); reg) {
            if (!reg->isPhys && reg->idOrPhys == vreg) {
                return true;
            }
        }
    }
    return false;
}

/// @brief Determine whether a virtual register carries boolean semantics.
///
/// @details Scans backwards through @p block up to @p searchLimit instructions
///          looking for a defining @c SETcc.  If it encounters another
///          definition of the virtual register the search stops, treating the
///          value as non-boolean.  This allows call lowering to avoid emitting
///          redundant boolean materialisation instructions.
///
/// @param block Basic block containing the candidate value.
/// @param searchLimit Number of instructions to inspect starting from the end
///        of the block.
/// @param vreg Virtual register identifier being classified.
/// @return True when @p vreg is produced by a recognised boolean pattern.
[[nodiscard]] bool isI1Value(const MBasicBlock &block, std::size_t searchLimit, uint16_t vreg) {
    if (searchLimit > block.instructions.size()) {
        searchLimit = block.instructions.size();
    }
    while (searchLimit > 0) {
        --searchLimit;
        const auto &instr = block.instructions[searchLimit];
        if (isBoolProducer(instr, vreg)) {
            return true;
        }
        if (definesVReg(instr, vreg)) {
            return false;
        }
    }
    return false;
}

/// @brief Create a stack-relative memory operand at the supplied offset.
///
/// @details Emits an operand that indexes from @c RSP using the canonical stack
///          frame base chosen by the ABI.  The helper centralises the base
///          register choice so updates to stack layout behaviour remain local.
///
/// @param offset Byte offset from the stack pointer where the slot begins.
/// @return Memory operand pointing at the requested stack slot.
[[nodiscard]] Operand makeStackSlot(int32_t offset) {
    return makeMemOperand(makePhysBase(PhysReg::RSP), offset);
}

} // namespace

/// @copydoc lowerCall
void lowerCall(MBasicBlock &block,
               std::size_t insertIdx,
               const CallLoweringPlan &plan,
               const TargetInfo &target,
               FrameInfo &frame) {
    if (insertIdx > block.instructions.size())
        throw std::out_of_range("x86-64 call lowering: insert index is out of range");

    auto insertIt = block.instructions.begin();
    std::advance(insertIt, static_cast<std::ptrdiff_t>(insertIdx));

    /// Insert one instruction at the moving cursor and advance past it.
    auto insertInstr = [&](MInstr instr) {
        insertIt = block.instructions.insert(insertIt, std::move(instr));
        ++insertIt;
    };

    const bool isWin64 = target.shadowSpace != 0;
    const CallArgLayout layout = common::planCallArgs(
        plan.args,
        CallArgLayoutConfig{.maxGPRArgs = target.maxGPRArgs,
                            .maxFPRArgs = target.maxFPArgs,
                            .slotModel = isWin64 ? CallSlotModel::UnifiedRegisterPositions
                                                 : CallSlotModel::IndependentRegisterBanks,
                            .variadicTailOnStack = false,
                            .numNamedArgs = plan.numNamedArgs});
    /**
     * @brief Mirror a Win64 variadic FP argument into its positional GPR.
     *
     * @param loc Planned floating-point argument location.
     * @param xmmReg XMM register already containing the argument bits.
     */
    auto maybeDuplicateWin64VarArgFpr = [&](const CallArgLocation &loc, PhysReg xmmReg) {
        if (!isWin64 || !plan.isVarArg || !loc.inRegister || loc.cls != CallArgClass::FPR)
            return;
        if (loc.regIndex >= target.intArgOrder.size())
            return;

        // Win64 variadic/unprototyped calls must mirror FP register arguments
        // into the corresponding positional integer register slot as raw bits.
        insertInstr(MInstr::make(MOpcode::MOVQxr,
                                 {makePhysOperand(RegClass::GPR, target.intArgOrder[loc.regIndex]),
                                  makePhysOperand(RegClass::XMM, xmmReg)}));
    };

    // Stack alignment is handled statically by FrameLowering which folds
    // outgoingArgArea into frameSize and rounds up to kStackAlignment (16).
    // No dynamic padding subtraction is needed here — the frame-resident
    // outgoing arg space is already correctly aligned.

    // Two-pass approach to avoid clobbering vreg values during argument setup:
    // Pass 1: Copy all vreg arguments to their destinations (reading vregs first)
    // Pass 2: Set up all immediate arguments (can safely overwrite registers now)
    // For vreg args going to registers, use scratch to avoid reading clobbered values.

    // Pass 1: Handle all vreg (non-immediate) arguments first
    for (const auto &loc : layout.locations) {
        const auto &arg = plan.args[loc.argIndex];
        if (loc.isAggregatePart && arg.isImm)
            throw std::invalid_argument("x86-64 call lowering: aggregate argument cannot be immediate");
        if (arg.isImm)
            continue;
        const auto currentIdx =
            static_cast<std::size_t>(std::distance(block.instructions.begin(), insertIt));

        if (loc.isAggregatePart) {
            const Operand base = makeVRegOperand(RegClass::GPR, arg.vreg);
            const Operand scratch = makePhysOperand(RegClass::GPR, kScratchGPR);
            insertInstr(MInstr::make(
                MOpcode::MOVmr,
                {scratch, makeMemOperand(std::get<OpReg>(base),
                                         checkedAggregateChunkOffset(loc.byteOffset))}));

            if (loc.inRegister) {
                const PhysReg destReg = target.intArgOrder[loc.regIndex];
                insertInstr(MInstr::make(MOpcode::MOVrr,
                                         {makePhysOperand(RegClass::GPR, destReg), scratch}));
            } else {
                const auto slotOffset =
                    checkedStackSlotOffset(target.shadowSpace, loc.stackSlotIndex);
                insertInstr(MInstr::make(MOpcode::MOVrm, {makeStackSlot(slotOffset), scratch}));
            }
            continue;
        }

        if (loc.cls == CallArgClass::GPR) {
            const Operand src = makeVRegOperand(RegClass::GPR, arg.vreg);
            const Operand scratch = makePhysOperand(RegClass::GPR, kScratchGPR);
            if (isI1Value(block, currentIdx, arg.vreg)) {
                insertInstr(MInstr::make(MOpcode::MOVZXrr8, {scratch, src}));
            } else {
                insertInstr(MInstr::make(MOpcode::MOVrr, {scratch, src}));
            }

            if (loc.inRegister) {
                const PhysReg destReg = target.intArgOrder[loc.regIndex];
                insertInstr(MInstr::make(MOpcode::MOVrr,
                                         {makePhysOperand(RegClass::GPR, destReg), scratch}));
            } else {
                const auto slotOffset =
                    checkedStackSlotOffset(target.shadowSpace, loc.stackSlotIndex);
                insertInstr(MInstr::make(MOpcode::MOVrm, {makeStackSlot(slotOffset), scratch}));
            }
        } else {
            if (loc.inRegister) {
                const PhysReg destReg = target.f64ArgOrder[loc.regIndex];
                insertInstr(MInstr::make(MOpcode::MOVSDrr,
                                         {makePhysOperand(RegClass::XMM, destReg),
                                          makeVRegOperand(RegClass::XMM, arg.vreg)}));
                maybeDuplicateWin64VarArgFpr(loc, destReg);
            } else {
                const auto slotOffset =
                    checkedStackSlotOffset(target.shadowSpace, loc.stackSlotIndex);
                const Operand scratchXmm = makePhysOperand(RegClass::XMM, kScratchXMM);
                insertInstr(MInstr::make(MOpcode::MOVSDrr,
                                         {scratchXmm, makeVRegOperand(RegClass::XMM, arg.vreg)}));
                insertInstr(
                    MInstr::make(MOpcode::MOVSDrm, {makeStackSlot(slotOffset), scratchXmm}));
            }
        }
    }

    // Pass 2: Handle all immediate arguments (now safe to overwrite registers)
    for (const auto &loc : layout.locations) {
        const auto &arg = plan.args[loc.argIndex];
        if (loc.isAggregatePart)
            continue;
        if (!arg.isImm)
            continue;

        if (loc.cls == CallArgClass::GPR) {
            if (loc.inRegister) {
                const PhysReg destReg = target.intArgOrder[loc.regIndex];
                insertInstr(MInstr::make(
                    MOpcode::MOVri,
                    {makePhysOperand(RegClass::GPR, destReg), makeImmOperand(arg.imm)}));
            } else {
                const auto slotOffset =
                    checkedStackSlotOffset(target.shadowSpace, loc.stackSlotIndex);
                const Operand scratch = makePhysOperand(RegClass::GPR, kScratchGPR);
                insertInstr(MInstr::make(MOpcode::MOVri, {scratch, makeImmOperand(arg.imm)}));
                insertInstr(MInstr::make(MOpcode::MOVrm, {makeStackSlot(slotOffset), scratch}));
            }
        } else {
            if (loc.inRegister) {
                const PhysReg destReg = target.f64ArgOrder[loc.regIndex];
                const Operand scratchGpr = makePhysOperand(RegClass::GPR, kScratchGPR);
                insertInstr(MInstr::make(MOpcode::MOVri, {scratchGpr, makeImmOperand(arg.imm)}));
                insertInstr(MInstr::make(MOpcode::MOVQrx,
                                         {makePhysOperand(RegClass::XMM, destReg), scratchGpr}));
                maybeDuplicateWin64VarArgFpr(loc, destReg);
            } else {
                const auto slotOffset =
                    checkedStackSlotOffset(target.shadowSpace, loc.stackSlotIndex);
                const Operand scratchGpr = makePhysOperand(RegClass::GPR, kScratchGPR);
                const Operand scratchXmm = makePhysOperand(RegClass::XMM, kScratchXMM);
                insertInstr(MInstr::make(MOpcode::MOVri, {scratchGpr, makeImmOperand(arg.imm)}));
                insertInstr(MInstr::make(MOpcode::MOVQrx, {scratchXmm, scratchGpr}));
                insertInstr(
                    MInstr::make(MOpcode::MOVSDrm, {makeStackSlot(slotOffset), scratchXmm}));
            }
        }
    }

    const std::size_t stackBytes =
        checkedOutgoingStackBytes(target.shadowSpace, layout.stackSlotsUsed);
    frame.outgoingArgArea =
        std::max(frame.outgoingArgArea, checkedOutgoingAreaForFrame(stackBytes));

    // SysV AMD64 varargs: %al must carry the number of XMM registers used.
    // Win64 instead mirrors FP register args into the corresponding integer
    // lanes earlier in lowering and does not use %al.
    if (plan.isVarArg && target.shadowSpace == 0) {
        const Operand rax = makePhysOperand(RegClass::GPR, PhysReg::RAX);
        insertInstr(MInstr::make(MOpcode::MOVri,
                                 {rax, makeImmOperand(static_cast<int64_t>(layout.fprRegsUsed))}));
    }
}

} // namespace zanna::codegen::x64
