//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/peephole/PeepholeCommon.hpp
// Purpose: Shared utility functions and types for x86-64 peephole sub-passes.
// Key invariants:
//   - Helpers are pure queries, local state updates, or immutable static-set accessors.
//   - Register classification must stay in sync with MachineIR opcode additions.
// Ownership/Lifetime:
//   - Header-only; returned register vectors live for the process duration.
// Links: src/codegen/x86_64/Peephole.hpp,
//        src/codegen/x86_64/MachineIR.hpp,
//        src/codegen/common/PeepholeUtil.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "../MachineIR.hpp"
#include "codegen/common/PeepholeUtil.hpp"
#include "codegen/x86_64/OperandRoles.hpp"

#include <array>
#include <optional>
#include <string>
#include <vector>

/// @file
/// @brief Defines shared x86-64 peephole statistics, queries, and tracking helpers.

namespace zanna::codegen::x64::peephole {

/// @brief Exposes the target-independent marked-instruction compactor locally.
using zanna::codegen::common::removeMarkedInstructions;

// ---- Statistics -------------------------------------------------------------

/// @brief Statistics tracking for peephole optimizations.
struct PeepholeStats {
    /// @brief Immediate-zero moves replaced by self-XOR.
    std::size_t movZeroToXor{0};
    /// @brief Compare-with-zero instructions replaced by self-TEST.
    std::size_t cmpZeroToTest{0};
    /// @brief Flag-dead arithmetic identities selected for removal.
    std::size_t arithmeticIdentities{0};
    /// @brief Constant-power-of-two multiplies replaced by shifts.
    std::size_t strengthReductions{0};
    /// @brief Integer and scalar-double self-moves removed.
    std::size_t identityMovesRemoved{0};
    /// @brief Adjacent move chains folded.
    std::size_t consecutiveMovsFolded{0};
    /// @brief Jumps to physically adjacent blocks removed.
    std::size_t branchesToNextRemoved{0};
    /// @brief Dead instructions and forwarded memory-load operations counted.
    std::size_t deadCodeEliminated{0};
    /// @brief Cold blocks appended to the cold partition.
    std::size_t coldBlocksMoved{0};
    /// @brief Conditional branches inverted while removing a companion jump.
    std::size_t branchesInverted{0};
    /// @brief Blocks participating in a changed trace layout.
    std::size_t blocksReordered{0};
    /// @brief Branch instructions retargeted through forwarding blocks.
    std::size_t branchChainsEliminated{0};

    /// @brief Sums every optimization counter in this record.
    /// @return Aggregate transformation/counting units accumulated by all sub-passes.
    [[nodiscard]] std::size_t total() const noexcept {
        return movZeroToXor + cmpZeroToTest + arithmeticIdentities + strengthReductions +
               identityMovesRemoved + consecutiveMovsFolded + branchesToNextRemoved +
               deadCodeEliminated + coldBlocksMoved + branchesInverted + blocksReordered +
               branchChainsEliminated;
    }
};

// ---- Pre-computed register sets ---------------------------------------------

/// @brief Returns the legacy fixed conservative block-exit register set.
/// @details Contains RAX, RBX, RBP, RDI, RSI, RSP, R12-R15, and XMM0.
///          The vector is precomputed once and is not derived from a selected
///          target ABI.
/// @return Process-lifetime immutable register-id vector.
inline const std::vector<uint16_t> &getBlockExitLiveRegs() {
    static const std::vector<uint16_t> regs = {
        static_cast<uint16_t>(PhysReg::RAX),
        static_cast<uint16_t>(PhysReg::RBX),
        static_cast<uint16_t>(PhysReg::RBP),
        static_cast<uint16_t>(PhysReg::RDI),
        static_cast<uint16_t>(PhysReg::RSI),
        static_cast<uint16_t>(PhysReg::RSP),
        static_cast<uint16_t>(PhysReg::R12),
        static_cast<uint16_t>(PhysReg::R13),
        static_cast<uint16_t>(PhysReg::R14),
        static_cast<uint16_t>(PhysReg::R15),
        static_cast<uint16_t>(PhysReg::XMM0),
    };
    return regs;
}

/// @brief Returns all 32 physical-register ids, marked live at labels.
/// @details Despite the legacy function name, the vector includes fixed RBP
///          and RSP as well as every GPR and XMM enumerator.
/// @return Process-lifetime immutable register-id vector.
inline const std::vector<uint16_t> &getAllAllocatableRegs() {
    static const std::vector<uint16_t> regs = {
        // GPRs
        static_cast<uint16_t>(PhysReg::RAX),
        static_cast<uint16_t>(PhysReg::RBX),
        static_cast<uint16_t>(PhysReg::RCX),
        static_cast<uint16_t>(PhysReg::RDX),
        static_cast<uint16_t>(PhysReg::RSI),
        static_cast<uint16_t>(PhysReg::RDI),
        static_cast<uint16_t>(PhysReg::R8),
        static_cast<uint16_t>(PhysReg::R9),
        static_cast<uint16_t>(PhysReg::R10),
        static_cast<uint16_t>(PhysReg::R11),
        static_cast<uint16_t>(PhysReg::R12),
        static_cast<uint16_t>(PhysReg::R13),
        static_cast<uint16_t>(PhysReg::R14),
        static_cast<uint16_t>(PhysReg::R15),
        static_cast<uint16_t>(PhysReg::RBP),
        static_cast<uint16_t>(PhysReg::RSP),
        // XMM registers
        static_cast<uint16_t>(PhysReg::XMM0),
        static_cast<uint16_t>(PhysReg::XMM1),
        static_cast<uint16_t>(PhysReg::XMM2),
        static_cast<uint16_t>(PhysReg::XMM3),
        static_cast<uint16_t>(PhysReg::XMM4),
        static_cast<uint16_t>(PhysReg::XMM5),
        static_cast<uint16_t>(PhysReg::XMM6),
        static_cast<uint16_t>(PhysReg::XMM7),
        static_cast<uint16_t>(PhysReg::XMM8),
        static_cast<uint16_t>(PhysReg::XMM9),
        static_cast<uint16_t>(PhysReg::XMM10),
        static_cast<uint16_t>(PhysReg::XMM11),
        static_cast<uint16_t>(PhysReg::XMM12),
        static_cast<uint16_t>(PhysReg::XMM13),
        static_cast<uint16_t>(PhysReg::XMM14),
        static_cast<uint16_t>(PhysReg::XMM15),
    };
    return regs;
}

// ---- Operand query helpers --------------------------------------------------

/// @brief Test whether an operand is the immediate integer zero.
/// @param operand Operand variant to inspect.
/// @return @c true only for @c OpImm carrying value zero.
[[nodiscard]] inline bool isZeroImm(const Operand &operand) noexcept {
    const auto *imm = std::get_if<OpImm>(&operand);
    return imm != nullptr && imm->val == 0;
}

/// @brief Check whether an operand refers to a general-purpose register.
/// @param operand Operand variant to inspect.
/// @return @c true for an @c OpReg classified as GPR, physical or virtual.
[[nodiscard]] inline bool isGprReg(const Operand &operand) noexcept {
    const auto *reg = std::get_if<OpReg>(&operand);
    return reg != nullptr && reg->cls == RegClass::GPR;
}

/// @brief Check if an operand is a physical register.
/// @param operand Operand variant to inspect.
/// @return @c true only for a physical @c OpReg.
[[nodiscard]] inline bool isPhysReg(const Operand &operand) noexcept {
    const auto *reg = std::get_if<OpReg>(&operand);
    return reg != nullptr && reg->isPhys;
}

/// @brief Check if two register operands refer to the same physical register.
/// @param a First operand variant.
/// @param b Second operand variant.
/// @return @c true when both are physical registers with equal class and id.
[[nodiscard]] inline bool samePhysReg(const Operand &a, const Operand &b) noexcept {
    const auto *regA = std::get_if<OpReg>(&a);
    const auto *regB = std::get_if<OpReg>(&b);
    if (!regA || !regB)
        return false;
    if (!regA->isPhys || !regB->isPhys)
        return false;
    return regA->cls == regB->cls && regA->idOrPhys == regB->idOrPhys;
}

/// @brief Check if an instruction is an identity move (mov r, r).
/// @param instr Machine instruction to inspect.
/// @return @c true for a two-operand @c MOVrr whose physical registers match.
[[nodiscard]] inline bool isIdentityMovRR(const MInstr &instr) noexcept {
    if (instr.opcode != MOpcode::MOVrr)
        return false;
    if (instr.operands.size() != 2)
        return false;
    return samePhysReg(instr.operands[0], instr.operands[1]);
}

/// @brief Check if an instruction is an identity FPR move (movsd d, d).
/// @param instr Machine instruction to inspect.
/// @return @c true for a two-operand @c MOVSDrr whose physical registers match.
[[nodiscard]] inline bool isIdentityMovSDRR(const MInstr &instr) noexcept {
    if (instr.opcode != MOpcode::MOVSDrr)
        return false;
    if (instr.operands.size() != 2)
        return false;
    return samePhysReg(instr.operands[0], instr.operands[1]);
}

/// @brief Get immediate value from an operand if it is an immediate.
/// @param operand Operand variant to inspect.
/// @return Contained integer value, or @c std::nullopt.
[[nodiscard]] inline std::optional<int64_t> getImmValue(const Operand &operand) noexcept {
    const auto *imm = std::get_if<OpImm>(&operand);
    if (imm)
        return imm->val;
    return std::nullopt;
}

/// @brief Check whether a memory operand references a physical register.
/// @param mem Memory address whose base and active index are inspected.
/// @param reg Candidate physical-register operand.
/// @return @c true when @p reg matches the base or enabled index.
[[nodiscard]] inline bool memUsesPhysReg(const OpMem &mem, const Operand &reg) noexcept {
    return samePhysReg(Operand{mem.base}, reg) ||
           (mem.hasIndex && samePhysReg(Operand{mem.index}, reg));
}

/// @brief Check if a value is a power of 2 and return the log2, or -1 if not.
/// @param value Signed integer to classify.
/// @return Exact base-two logarithm for a positive power of two; otherwise -1.
[[nodiscard]] inline int log2IfPowerOf2(int64_t value) noexcept {
    if (value <= 0)
        return -1;
    if ((value & (value - 1)) != 0)
        return -1; // not a power of 2
    int log = 0;
    while ((1LL << log) < value)
        ++log;
    return log;
}

// ---- Constant tracking ------------------------------------------------------

/// @brief Array of known constant values indexed by physical register ID.
///
/// Each entry holds the constant loaded into that physical register by a
/// recent MOVri, or nullopt if the register's value is unknown.  Indexed
/// directly by static_cast<uint16_t>(PhysReg); the x86-64 PhysReg enum
/// has 32 entries (RAX=0 ... XMM15=31) so the array is always in-bounds.
using RegConstMap = std::array<std::optional<int64_t>, 32>;

/// @brief Updates physical-GPR constant facts after one instruction.
/// @details Records immediate moves and the zero-XOR idiom, invalidates
///          explicitly defined registers through operand-role metadata, handles
///          implicit divide/sign-extension definitions, and clears the fixed
///          conservative caller-saved GPR set at calls.
/// @param instr Instruction whose effects are applied.
/// @param knownConsts Register-indexed facts mutated in place.
inline void updateKnownConsts(const MInstr &instr, RegConstMap &knownConsts) {
    // MOVri loads a constant into a register
    if (instr.opcode == MOpcode::MOVri && instr.operands.size() == 2) {
        const auto *dst = std::get_if<OpReg>(&instr.operands[0]);
        const auto *imm = std::get_if<OpImm>(&instr.operands[1]);
        if (dst && dst->isPhys && dst->idOrPhys < 32 && imm) {
            knownConsts[dst->idOrPhys] = imm->val;
            return;
        }
    }

    if (instr.opcode == MOpcode::XORrr32 && instr.operands.size() >= 2 &&
        samePhysReg(instr.operands[0], instr.operands[1])) {
        const auto *dst = std::get_if<OpReg>(&instr.operands[0]);
        if (dst && dst->isPhys && dst->idOrPhys < knownConsts.size()) {
            knownConsts[dst->idOrPhys] = 0;
            return;
        }
    }

    // Any physical register definition invalidates the tracked constant. Use
    // shared operand roles so POP, read-modify-write instructions, and new
    // opcodes cannot leave stale constants behind.
    for (std::size_t idx = 0; idx < instr.operands.size(); ++idx) {
        const auto [isUse, isDef] = operandRoles(instr, idx);
        (void)isUse;
        if (!isDef)
            continue;
        const auto *dst = std::get_if<OpReg>(&instr.operands[idx]);
        if (dst && dst->isPhys && dst->idOrPhys < knownConsts.size()) {
            knownConsts[dst->idOrPhys].reset();
        }
    }

    if (instr.opcode == MOpcode::CQO) {
        knownConsts[static_cast<uint16_t>(PhysReg::RDX)].reset();
    } else if (instr.opcode == MOpcode::IDIVrm || instr.opcode == MOpcode::DIVrm) {
        knownConsts[static_cast<uint16_t>(PhysReg::RAX)].reset();
        knownConsts[static_cast<uint16_t>(PhysReg::RDX)].reset();
    }

    // Calls invalidate all caller-saved registers
    if (instr.opcode == MOpcode::CALL) {
        // x86-64 caller-saved: RAX, RCX, RDX, RSI, RDI, R8-R11
        knownConsts[static_cast<uint16_t>(PhysReg::RAX)].reset();
        knownConsts[static_cast<uint16_t>(PhysReg::RCX)].reset();
        knownConsts[static_cast<uint16_t>(PhysReg::RDX)].reset();
        knownConsts[static_cast<uint16_t>(PhysReg::RSI)].reset();
        knownConsts[static_cast<uint16_t>(PhysReg::RDI)].reset();
        knownConsts[static_cast<uint16_t>(PhysReg::R8)].reset();
        knownConsts[static_cast<uint16_t>(PhysReg::R9)].reset();
        knownConsts[static_cast<uint16_t>(PhysReg::R10)].reset();
        knownConsts[static_cast<uint16_t>(PhysReg::R11)].reset();
    }
}

/// @brief Get constant value for a register if known.
/// @param operand Candidate physical GPR operand.
/// @param knownConsts Current register-indexed constant facts.
/// @return Tracked value, or @c std::nullopt for another operand kind, virtual
///         register, non-GPR class, out-of-range id, or unknown value.
[[nodiscard]] inline std::optional<int64_t> getConstValue(const Operand &operand,
                                                          const RegConstMap &knownConsts) {
    const auto *reg = std::get_if<OpReg>(&operand);
    if (!reg || !reg->isPhys || reg->cls != RegClass::GPR || reg->idOrPhys >= 32)
        return std::nullopt;
    return knownConsts[reg->idOrPhys];
}

// ---- Register def/use classification ----------------------------------------

/// @brief Check if an instruction defines a given physical register.
/// @details Consults explicit operand roles and selected implicit CQO/divide
///          definitions.
/// @param instr Machine instruction to inspect.
/// @param reg Candidate physical-register operand.
/// @return @c true when @p instr defines @p reg.
[[nodiscard]] inline bool definesReg(const MInstr &instr, const Operand &reg) noexcept {
    if (!isPhysReg(reg))
        return false;

    for (std::size_t idx = 0; idx < instr.operands.size(); ++idx) {
        const auto [isUse, isDef] = operandRoles(instr, idx);
        (void)isUse;
        if (isDef && samePhysReg(instr.operands[idx], reg))
            return true;
    }

    const auto *wanted = std::get_if<OpReg>(&reg);
    if (!wanted || !wanted->isPhys)
        return false;
    const auto wantedPhys = static_cast<PhysReg>(wanted->idOrPhys);
    if (instr.opcode == MOpcode::CQO)
        return wantedPhys == PhysReg::RDX;
    if (instr.opcode == MOpcode::IDIVrm || instr.opcode == MOpcode::DIVrm)
        return wantedPhys == PhysReg::RAX || wantedPhys == PhysReg::RDX;
    return false;
}

/// @brief Check if an instruction uses a given physical register as a source.
/// @details Consults explicit operand roles, used memory-address registers, and
///          selected implicit return/sign-extension/division inputs.
/// @param instr Machine instruction to inspect.
/// @param reg Candidate physical-register operand.
/// @return @c true when @p instr reads @p reg.
[[nodiscard]] inline bool usesReg(const MInstr &instr, const Operand &reg) noexcept {
    if (!isPhysReg(reg))
        return false;

    for (std::size_t idx = 0; idx < instr.operands.size(); ++idx) {
        const auto [isUse, isDef] = operandRoles(instr, idx);
        (void)isDef;
        if (!isUse)
            continue;
        if (samePhysReg(instr.operands[idx], reg))
            return true;
        if (const auto *mem = std::get_if<OpMem>(&instr.operands[idx])) {
            if (memUsesPhysReg(*mem, reg))
                return true;
        }
    }

    const auto *wanted = std::get_if<OpReg>(&reg);
    if (!wanted || !wanted->isPhys)
        return false;
    const auto wantedPhys = static_cast<PhysReg>(wanted->idOrPhys);
    if (instr.opcode == MOpcode::RET) {
        return wantedPhys == PhysReg::RAX || wantedPhys == PhysReg::XMM0 ||
               wantedPhys == PhysReg::RSP;
    }
    if (instr.opcode == MOpcode::CQO)
        return wantedPhys == PhysReg::RAX;
    if (instr.opcode == MOpcode::IDIVrm || instr.opcode == MOpcode::DIVrm)
        return wantedPhys == PhysReg::RAX || wantedPhys == PhysReg::RDX;
    return false;
}

// ---- Misc helpers -----------------------------------------------------------

/// @brief Check if a register is an argument-passing register (RDI, RSI, RDX, RCX, R8, R9).
/// @details The fixed set covers all SysV integer argument registers and is a
///          conservative superset of the Win64 set.
/// @param operand Candidate register operand.
/// @return @c true for a physical GPR in the fixed set.
[[nodiscard]] inline bool isArgReg(const Operand &operand) noexcept {
    const auto *reg = std::get_if<OpReg>(&operand);
    if (!reg || !reg->isPhys || reg->cls != RegClass::GPR)
        return false;
    const auto pr = static_cast<PhysReg>(reg->idOrPhys);
    return pr == PhysReg::RDI || pr == PhysReg::RSI || pr == PhysReg::RDX || pr == PhysReg::RCX ||
           pr == PhysReg::R8 || pr == PhysReg::R9;
}

/// @brief Check if an instruction is an unconditional jump to a specific label.
/// @param instr Candidate instruction; only operand zero is inspected.
/// @param label Exact target label spelling.
/// @return @c true for @c JMP with an operand-zero label equal to @p label.
[[nodiscard]] inline bool isJumpTo(const MInstr &instr, const std::string &label) noexcept {
    if (instr.opcode != MOpcode::JMP)
        return false;
    if (instr.operands.empty())
        return false;
    const auto *lbl = std::get_if<OpLabel>(&instr.operands[0]);
    return lbl && lbl->name == label;
}

/// @brief Check if any following instruction in this block reads flags.
/// @details IMUL and SHL set flags differently (IMUL sets CF/OF for overflow,
///          SHL sets CF to last shifted bit and OF based on sign change).
///          Transforming IMUL to SHL is only safe when no subsequent instruction
///          reads the flags before they are overwritten by another flag-setting
///          instruction or a block boundary.
/// @param instrs Instruction vector for the basic block.
/// @param idx Index of the instruction being considered for rewrite.
/// @return @c true if a subsequent instruction reads flags before they are
///         overwritten, or if a label boundary is encountered first.
/// @pre @p idx is less than @c instrs.size().
[[nodiscard]] inline bool nextInstrReadsFlags(const std::vector<MInstr> &instrs,
                                              std::size_t idx) noexcept {
    for (std::size_t j = idx + 1; j < instrs.size(); ++j) {
        const auto opc = instrs[j].opcode;
        if (usesEFlags(opc))
            return true;
        if (definesEFlags(opc))
            return false;
        // LABEL is a potential branch target — conservatively assume flags are read
        if (opc == MOpcode::LABEL)
            return true;
    }
    return false;
}

} // namespace zanna::codegen::x64::peephole
