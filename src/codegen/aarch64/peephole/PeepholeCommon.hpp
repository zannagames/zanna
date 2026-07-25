//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/peephole/PeepholeCommon.hpp
// Purpose: Shared utility functions and types for AArch64 peephole sub-passes.
//
// Key invariants:
//   - Small queries (3-30 LOC) live inline here for hot-path callsites.
//   - Switch-heavy classifiers (definesReg, usesReg, classifyOperand,
//     hasSideEffects, getDefinedReg, updateKnownConsts) are declared here and
//     defined in PeepholeCommon.cpp so sub-pass translation units don't pay
//     their compile cost on every include.
//   - Register classification must stay in sync with MachineIR opcode additions.
//
// Ownership/Lifetime:
//   - Free functions, no dynamic state.
//
// Links: codegen/aarch64/Peephole.hpp, docs/internals/architecture.md
//
//===----------------------------------------------------------------------===//

#pragma once

#include "../MachineIR.hpp"
#include "codegen/common/PeepholeUtil.hpp"

#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

/// @file
/// @brief Declares shared operand, liveness, and constant queries for peepholes.
///
/// The opcode-specific classifiers in this interface encode AArch64 MIR
/// operand conventions used by multiple post-allocation rewrites. They must be
/// reviewed whenever the machine-opcode set or an operand layout changes.

namespace zanna::codegen::aarch64::peephole {

// Bring the shared compaction helper into this namespace scope.
using zanna::codegen::common::removeMarkedInstructions;

// ---- Register query helpers ------------------------------------------------

/// @brief Test whether an operand names an allocated physical register.
/// @param op Machine operand to inspect.
/// @return `true` only for a register operand whose `isPhys` flag is set.
[[nodiscard]] inline bool isPhysReg(const MOperand &op) noexcept {
    return op.kind == MOperand::Kind::Reg && op.reg.isPhys;
}

/// @brief Test whether two operands name the same classed physical register.
/// @param a First operand.
/// @param b Second operand.
/// @return `true` when both operands are physical registers with equal register
///         classes and identifiers.
[[nodiscard]] inline bool samePhysReg(const MOperand &a, const MOperand &b) noexcept {
    if (!isPhysReg(a) || !isPhysReg(b))
        return false;
    return a.reg.cls == b.reg.cls && a.reg.idOrPhys == b.reg.idOrPhys;
}

/// @brief Test whether an operand names an integer argument register.
/// @param reg Operand to inspect.
/// @return `true` for physical GPRs X0--X7.
[[nodiscard]] inline bool isArgReg(const MOperand &reg) noexcept {
    if (!isPhysReg(reg) || reg.reg.cls != RegClass::GPR)
        return false;
    const auto pr = static_cast<PhysReg>(reg.reg.idOrPhys);
    return pr <= PhysReg::X7;
}

/// @brief Test whether an operand names a modeled ABI argument/result register.
/// @param reg Operand to inspect.
/// @return `true` for physical GPRs X0--X7 or FPRs V0--V7.
[[nodiscard]] inline bool isABIReg(const MOperand &reg) noexcept {
    if (!isPhysReg(reg))
        return false;
    const auto pr = static_cast<PhysReg>(reg.reg.idOrPhys);
    if (reg.reg.cls == RegClass::GPR)
        return pr <= PhysReg::X7;
    if (reg.reg.cls == RegClass::FPR)
        return pr >= PhysReg::V0 && pr <= PhysReg::V7;
    return false;
}

/// @brief Test whether an operand is an immediate equal to a requested value.
/// @param op Operand to inspect.
/// @param value Signed immediate value to compare.
/// @return `true` when @p op is an immediate whose payload equals @p value.
[[nodiscard]] inline bool isImmValue(const MOperand &op, long long value) noexcept {
    return op.kind == MOperand::Kind::Imm && op.imm == value;
}

/// @brief Pack a physical register's class and identifier into a map key.
/// @param op Operand expected to name a physical register.
/// @return A key with the register class in the high 16 bits and physical
///         identifier in the low 16 bits, or `UINT32_MAX` for other operands.
[[nodiscard]] inline uint32_t regKey(const MOperand &op) noexcept {
    if (op.kind != MOperand::Kind::Reg || !op.reg.isPhys)
        return UINT32_MAX;
    return (static_cast<uint32_t>(op.reg.cls) << 16) | op.reg.idOrPhys;
}

// ---- Instruction query helpers (defined in PeepholeCommon.cpp) -------------

/// @brief Check if an instruction defines (writes to) a given physical register.
///
/// @details Manually maintained opcode switch (kept out-of-line to avoid bloat
///          in every translation unit that includes this header). Each opcode
///          group has different operand-index semantics (e.g., LDP defines both
///          ops[0] and ops[1]), which is why a generic "dest is ops[0]" table
///          is insufficient.
/// @param instr Instruction whose explicit definitions are inspected.
/// @param reg Physical register to query.
/// @return `true` when a recognized explicit destination of @p instr matches
///         @p reg. Implicit call clobbers are not modeled here.
[[nodiscard]] bool definesReg(const MInstr &instr, const MOperand &reg) noexcept;

/// @brief Test whether an instruction explicitly reads a physical register.
/// @param instr Instruction whose explicit source operands are inspected.
/// @param reg Physical register to query.
/// @return `true` when a recognized source operand of @p instr matches @p reg.
/// @note Implicit ABI operands of calls and returns are not modeled.
[[nodiscard]] bool usesReg(const MInstr &instr, const MOperand &reg) noexcept;

/// @brief Classify one opcode operand position as an explicit use and/or definition.
/// @param instr Instruction supplying the opcode-specific operand convention.
/// @param idx Zero-based operand index to classify.
/// @return Pair `{isUse, isDef}`. Unknown opcodes use the conventional
///         destination-at-zero fallback.
[[nodiscard]] std::pair<bool, bool> classifyOperand(const MInstr &instr, std::size_t idx) noexcept;

// ---- Constant tracking -----------------------------------------------------

/// @brief Maps physical GPR identifiers to values established by `MovRI`.
using RegConstMap = std::unordered_map<uint16_t, long long>;

/// @brief Transfer one instruction through the local known-constant state.
///
/// A well-formed `MovRI` records its immediate. Recognized explicit
/// destinations, jump-table scratch clobbers, and caller-saved GPRs at calls
/// invalidate affected entries.
///
/// @param instr Instruction whose definitions update the state.
/// @param[in,out] knownConsts Physical-GPR constants valid before and then
///                            after @p instr.
void updateKnownConsts(const MInstr &instr, RegConstMap &knownConsts);

/// @brief Query a tracked constant for a physical GPR operand.
/// @param reg Operand whose value is requested.
/// @param knownConsts Current physical-GPR constant map.
/// @return The known signed value, or `std::nullopt` for non-GPR, virtual,
///         untracked, or unknown operands.
[[nodiscard]] inline std::optional<long long> getConstValue(const MOperand &reg,
                                                            const RegConstMap &knownConsts) {
    if (!isPhysReg(reg) || reg.reg.cls != RegClass::GPR)
        return std::nullopt;
    auto it = knownConsts.find(reg.reg.idOrPhys);
    if (it != knownConsts.end())
        return it->second;
    return std::nullopt;
}

// ---- Side-effect / def queries ---------------------------------------------

/// @brief Test whether the peephole DCE policy must retain an instruction.
///
/// This is deliberately broader than language-level side effects: memory
/// loads, address materialization, comparisons, control flow, stack changes,
/// and moves into ABI registers are all treated as observable for conservative
/// post-allocation elimination.
///
/// @param instr Instruction to classify.
/// @return `true` when local DCE must not remove @p instr solely because its
///         explicit destination is dead.
[[nodiscard]] bool hasSideEffects(const MInstr &instr) noexcept;

/// @brief Return the primary explicit physical-register destination.
/// @param instr Instruction whose destination is requested.
/// @return Operand zero for recognized destination-producing opcodes when it
///         is physical; `std::nullopt` otherwise.
/// @note Pair loads define two registers, but this convenience query returns
///       only their first destination. Use @ref classifyOperand for all defs.
[[nodiscard]] std::optional<MOperand> getDefinedReg(const MInstr &instr) noexcept;

} // namespace zanna::codegen::aarch64::peephole
