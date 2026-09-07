//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/peephole/DCE.cpp
// Purpose: Implement conservative block-local physical-register and EFLAGS
//          dead code elimination for the x86-64 backend.
// Key invariants:
//   - RSP modifications are never eliminated (stack frame changes).
//   - Uses a conservative backward sweep within each basic block.
//   - Division instructions (IDIV/DIV) are treated as side-effecting.
// Ownership/Lifetime:
//   - Stateless; mutates caller-owned instruction vectors.
// Links: src/codegen/x86_64/peephole/DCE.hpp,
//        src/codegen/x86_64/peephole/PeepholeCommon.hpp
//
//===----------------------------------------------------------------------===//

#include "DCE.hpp"

#include "codegen/x86_64/OperandRoles.hpp"

#include <cstdint>
#include <optional>

/// @file
/// @brief Implements physical-register and EFLAGS liveness-based block DCE.

namespace zanna::codegen::x64::peephole {
namespace {

/// @brief Bit set used to track the backend's physical-register identifiers.
using RegMask = uint64_t;

/// @brief Returns the liveness bit corresponding to a register identifier.
/// @param reg Physical-register numeric identifier.
/// @return One-bit mask, or zero when @p reg is 64 or greater.
[[nodiscard]] RegMask regBit(uint16_t reg) noexcept {
    return reg < 64 ? (RegMask{1} << reg) : RegMask{0};
}

/// @brief Adds a physical register to a liveness mask.
/// @param mask Mask updated in place.
/// @param reg Physical-register identifier; out-of-range values have no effect.
void addReg(RegMask &mask, uint16_t reg) noexcept {
    mask |= regBit(reg);
}

/// @brief Tests whether a physical register is present in a liveness mask.
/// @param mask Mask to query.
/// @param reg Physical-register identifier.
/// @return @c true when the corresponding in-range bit is set.
[[nodiscard]] bool containsReg(RegMask mask, uint16_t reg) noexcept {
    return (mask & regBit(reg)) != 0;
}

/// @brief Check if an instruction modifies RSP (the stack pointer).
/// @details Examines only a physical register in operand zero, matching the
///          destination convention of instructions accepted by this DCE pass.
/// @param instr Machine instruction to inspect.
/// @return @c true when operand zero names physical @c RSP.
[[nodiscard]] bool modifiesRSP(const MInstr &instr) noexcept {
    if (instr.operands.empty())
        return false;

    // Check if the first operand (destination) is RSP
    const auto *reg = std::get_if<OpReg>(&instr.operands[0]);
    if (!reg || !reg->isPhys)
        return false;

    return static_cast<PhysReg>(reg->idOrPhys) == PhysReg::RSP;
}

/// @brief Check if an instruction has observable side effects.
/// @param instr Machine instruction to classify.
/// @return @c true for an explicit side-effect opcode or an RSP destination.
[[nodiscard]] bool dceHasSideEffects(const MInstr &instr) noexcept {
    // RSP modifications are always significant - they affect the stack frame
    if (modifiesRSP(instr))
        return true;
    return hasObservableSideEffects(instr.opcode);
}

/// @brief Get the destination register from an instruction, if it defines one.
/// @details Consults target operand-role metadata and returns the first
///          physical register in a defining role.
/// @param instr Machine instruction to inspect.
/// @return Defined physical-register id, or @c std::nullopt.
[[nodiscard]] std::optional<uint16_t> getDefReg(const MInstr &instr) noexcept {
    if (instr.operands.empty())
        return std::nullopt;

    for (std::size_t idx = 0; idx < instr.operands.size(); ++idx) {
        const auto [isUse, isDef] = operandRoles(instr, idx);
        (void)isUse;
        if (!isDef)
            continue;
        const auto *reg = std::get_if<OpReg>(&instr.operands[idx]);
        if (reg && reg->isPhys)
            return reg->idOrPhys;
    }
    return std::nullopt;
}

/// @brief Collect all physical registers used by an instruction.
/// @details Includes physical register operands in use roles plus the physical
///          base and optional index of used memory operands.
/// @param instr Machine instruction to inspect.
/// @return Bit mask of explicit physical-register uses.
[[nodiscard]] RegMask collectUsedRegs(const MInstr &instr) {
    RegMask usedRegs = 0;

    // Helper to add a register if it's physical
    /// @brief Adds @p op when it contains a physical register.
    /// @param op Operand variant to inspect.
    auto addIfPhysReg = [&usedRegs](const Operand &op) {
        const auto *reg = std::get_if<OpReg>(&op);
        if (reg && reg->isPhys)
            addReg(usedRegs, reg->idOrPhys);
    };

    // Helper to add registers from memory operand
    /// @brief Adds a memory operand's physical base and active index.
    /// @param op Operand variant to inspect.
    auto addMemRegs = [&usedRegs](const Operand &op) {
        const auto *mem = std::get_if<OpMem>(&op);
        if (mem) {
            // Base register is always valid in OpMem
            if (mem->base.isPhys)
                addReg(usedRegs, mem->base.idOrPhys);
            // Index register is only valid when hasIndex is true
            if (mem->hasIndex && mem->index.isPhys)
                addReg(usedRegs, mem->index.idOrPhys);
        }
    };

    for (std::size_t idx = 0; idx < instr.operands.size(); ++idx) {
        const auto [isUse, isDef] = operandRoles(instr, idx);
        (void)isDef;
        if (!isUse)
            continue;
        addIfPhysReg(instr.operands[idx]);
        addMemRegs(instr.operands[idx]);
    }

    return usedRegs;
}

/// @brief Mark all registers a CALL implicitly uses as live.
/// @details Argument registers, plus RAX (vararg vector-arg count for SysV),
///          plus RSP must stay live across CALL points so DCE cannot drop
///          the instructions that populate them.
/// @param target ABI argument-register and count metadata.
/// @param usedRegs Liveness mask extended in place.
void addCallUsedRegs(const TargetInfo &target, RegMask &usedRegs) {
    for (std::size_t i = 0; i < target.maxGPRArgs && i < target.intArgOrder.size(); ++i)
        addReg(usedRegs, static_cast<uint16_t>(target.intArgOrder[i]));
    for (std::size_t i = 0; i < target.maxFPArgs && i < target.f64ArgOrder.size(); ++i)
        addReg(usedRegs, static_cast<uint16_t>(target.f64ArgOrder[i]));
    addReg(usedRegs, static_cast<uint16_t>(PhysReg::RSP));
    // SysV varargs use AL to carry the number of vector arguments. Keeping RAX
    // live at calls is conservative for non-varargs and required for varargs.
    addReg(usedRegs, static_cast<uint16_t>(PhysReg::RAX));
}

/// @brief Mark RET-implicit registers as live.
/// @details Return value registers (int + fp), the stack pointer, and all
///          callee-saved registers must survive to the function epilogue.
/// @param target ABI return and callee-saved register metadata.
/// @param usedRegs Liveness mask extended in place.
void addReturnUsedRegs(const TargetInfo &target, RegMask &usedRegs) {
    addReg(usedRegs, static_cast<uint16_t>(target.intReturnReg));
    addReg(usedRegs, static_cast<uint16_t>(target.f64ReturnReg));
    addReg(usedRegs, static_cast<uint16_t>(PhysReg::RSP));
    for (PhysReg reg : target.calleeSavedGPR)
        addReg(usedRegs, static_cast<uint16_t>(reg));
    for (PhysReg reg : target.calleeSavedFPR)
        addReg(usedRegs, static_cast<uint16_t>(reg));
}

/// @brief Seed @p liveRegs with the registers conservatively live at block exit.
/// @details Delegates to @ref addReturnUsedRegs and explicitly reinforces the
///          RSP invariant used by frame-manipulating blocks.
/// @param target ABI return and callee-saved register metadata.
/// @param liveRegs Liveness mask extended in place.
void addExitLiveRegs(const TargetInfo &target, RegMask &liveRegs) {
    addReturnUsedRegs(target, liveRegs);
    addReg(liveRegs, static_cast<uint16_t>(PhysReg::RSP));
}

/// @brief Marks every allocator-visible physical register live.
/// @param liveRegs Liveness mask extended in place.
void addAllAllocatableRegs(RegMask &liveRegs) {
    for (uint16_t reg : getAllAllocatableRegs())
        addReg(liveRegs, reg);
}

/// @brief Add implicit register uses for @p instr to @p liveRegs / @p flagsLive.
/// @details Some opcodes touch registers that do not appear in their operand
///          list — CALL implicitly reads arg registers, RET reads the return
///          regs, CQO and IDIV/DIV read the RAX/RDX pair, and any opcode in
///          the EFLAGS-using set marks flags as live.
/// @param instr Machine instruction whose implicit behavior is inspected.
/// @param target ABI metadata used for calls and returns.
/// @param liveRegs Register liveness mask extended in place.
/// @param flagsLive EFLAGS liveness flag set when @p instr consumes flags.
void collectImplicitUses(const MInstr &instr,
                         const TargetInfo &target,
                         RegMask &liveRegs,
                         bool &flagsLive) {
    if (usesEFlags(instr.opcode))
        flagsLive = true;

    switch (instr.opcode) {
        case MOpcode::CALL:
            addCallUsedRegs(target, liveRegs);
            break;
        case MOpcode::RET:
            addReturnUsedRegs(target, liveRegs);
            break;
        default:
            break;
    }
    // CQO / IDIV / DIV / MUL / IMUL / shift-by-CL implicit inputs.
    liveRegs |= implicitUseMask(instr.opcode);
}

} // namespace

/// @brief Run liveness-based dead-code elimination over a single block.
/// @details Performs a backward sweep maintaining a live-register set and a
///          flags-live flag. Instructions that define only dead registers
///          (and lack observable side effects) are marked for removal. The
///          single backward pass naturally propagates uses across earlier
///          definitions. @p preservePhysRegsAtExit seeds the initial live set
///          with every allocatable register for blocks whose successor
///          liveness is unavailable.
/// @param instrs Block instructions, mutated in place.
/// @param stats Pass-wide statistics accumulator.
/// @param target Calling-convention metadata for implicit-use computation.
/// @param preservePhysRegsAtExit Whether to seed every allocatable register live.
/// @return Number of instructions eliminated.
std::size_t runBlockDCE(std::vector<MInstr> &instrs,
                        PeepholeStats &stats,
                        const TargetInfo &target,
                        bool preservePhysRegsAtExit) {
    if (instrs.empty())
        return 0;

    std::size_t eliminated = 0;

    RegMask liveRegs = 0;
    if (preservePhysRegsAtExit) {
        addAllAllocatableRegs(liveRegs);
        addReg(liveRegs, static_cast<uint16_t>(PhysReg::RSP));
    } else {
        addExitLiveRegs(target, liveRegs);
    }
    bool flagsLive = false;

    std::vector<bool> toRemove(instrs.size(), false);

    for (std::size_t i = instrs.size(); i-- > 0;) {
        const auto &instr = instrs[i];
        if (instr.opcode == MOpcode::LABEL)
            addAllAllocatableRegs(liveRegs);

        const RegMask explicitUses = collectUsedRegs(instr);
        const bool explicitFlagsUse = usesEFlags(instr.opcode);

        const auto defReg = getDefReg(instr);
        const bool definesFlags = definesEFlags(instr.opcode);
        const bool hasTrackedDef = defReg.has_value() || definesFlags;
        const bool regResultLive = defReg && containsReg(liveRegs, *defReg);
        const bool flagsResultLive = definesFlags && flagsLive;
        const bool anyResultLive = regResultLive || flagsResultLive;

        if (hasTrackedDef && !dceHasSideEffects(instr) && !anyResultLive) {
            toRemove[i] = true;
            ++eliminated;
            continue;
        }

        if (defReg)
            liveRegs &= ~regBit(*defReg);
        liveRegs &= ~implicitDefMask(instr.opcode);
        if (definesFlags)
            flagsLive = false;

        liveRegs |= explicitUses;
        collectImplicitUses(instr, target, liveRegs, flagsLive);
        if (explicitFlagsUse)
            flagsLive = true;
    }

    if (eliminated != 0)
        removeMarkedInstructions(instrs, toRemove);

    stats.deadCodeEliminated += eliminated;
    return eliminated;
}

} // namespace zanna::codegen::x64::peephole
