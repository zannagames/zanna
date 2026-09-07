//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/OperandRoles.cpp
// Purpose: Shared x86-64 Machine IR operand role classification.
// Key invariants:
//   - Covers all defined MIR opcodes; unknown future opcodes return conservative roles.
// Ownership/Lifetime:
//   - Stateless free functions; no dynamic allocation.
// Links: src/codegen/x86_64/OperandRoles.hpp,
//        src/codegen/x86_64/MachineIR.hpp
//
//===----------------------------------------------------------------------===//

#include "codegen/x86_64/OperandRoles.hpp"

#include <variant>

/**
 * @file
 * @brief Implements x86-64 MIR operand, EFLAGS, and side-effect classification.
 *
 * The opcode switches form a shared semantic contract for liveness, allocation,
 * scheduling, folding, and dead-code elimination. Unknown future opcodes fall
 * back conservatively to use-and-def operands and observable side effects.
 */

namespace zanna::codegen::x64 {

/// @brief Compute the (use, def) role pair for operand @p idx of @p instr.
/// @details Each MIR opcode declares fixed roles for its operands. Two-operand
///          arithmetic puts the destination at index 0 (use+def), the
///          right-hand value at index 1 (use only). Loads have dest at 0
///          (def), address operands at 1+ (use). Stores have address and
///          value both as uses. The fallback at the end conservatively
///          treats unknown operands as both use and def so liveness analysis
///          cannot accidentally drop live values or miss implicit writes.
/// @param instr MIR instruction whose operand is classified.
/// @param idx Zero-based index into @p instr's operand vector.
/// @return Pair whose first element denotes a use and whose second denotes a definition.
std::pair<bool, bool> operandRoles(const MInstr &instr, std::size_t idx) noexcept {
    switch (instr.opcode) {
        case MOpcode::PUSH:
            return {idx == 0, false};
        case MOpcode::POP:
            return {false, idx == 0};

        case MOpcode::MOVrr:
        case MOpcode::MOVri:
        case MOpcode::MOVmr:
        case MOpcode::LEA:
        case MOpcode::MOVZXrr8:
        case MOpcode::MOVZXrr32:
        case MOpcode::MOVSXD:
        case MOpcode::MOVSXrr16:
        case MOpcode::CVTSI2SD:
        case MOpcode::CVTTSD2SI:
        case MOpcode::MOVQrx:
        case MOpcode::MOVQxr:
        case MOpcode::MOVSDrr:
        case MOpcode::MOVSDmr:
        case MOpcode::MOVUPSmr:
            return {idx == 1, idx == 0};

        case MOpcode::XORrr32:
            return {false, idx == 0};

        case MOpcode::MOVrm:
        case MOpcode::MOVSDrm:
        case MOpcode::MOVUPSrm:
            return {idx == 0 || idx == 1, false};

        case MOpcode::ADDrr:
        case MOpcode::ADDri:
        case MOpcode::ANDrr:
        case MOpcode::ANDri:
        case MOpcode::ORrr:
        case MOpcode::ORri:
        case MOpcode::XORrr:
        case MOpcode::XORri:
        case MOpcode::SUBrr:
        case MOpcode::SHLri:
        case MOpcode::SHLrc:
        case MOpcode::SHRri:
        case MOpcode::SHRrc:
        case MOpcode::SARri:
        case MOpcode::SARrc:
        case MOpcode::IMULrr:
        case MOpcode::ADDrr32:
        case MOpcode::SUBrr32:
        case MOpcode::IMULrr32:
        case MOpcode::ADDri32:
        case MOpcode::ADDrr16:
        case MOpcode::SUBrr16:
        case MOpcode::IMULrr16:
        case MOpcode::ADDri16:
        case MOpcode::ADDrm:
        case MOpcode::SUBrm:
        case MOpcode::ANDrm:
        case MOpcode::ORrm:
        case MOpcode::XORrm:
        case MOpcode::IMULrm:
        case MOpcode::CMOVNErr:
        case MOpcode::FADD:
        case MOpcode::FSUB:
        case MOpcode::FMUL:
        case MOpcode::FDIV:
            if (idx == 0)
                return {true, true};
            if (idx == 1)
                return {true, false};
            return {false, false};

        case MOpcode::DIVS64rr:
        case MOpcode::REMS64rr:
        case MOpcode::DIVS64Chk0rr:
        case MOpcode::REMS64Chk0rr:
        case MOpcode::DIVU64rr:
        case MOpcode::REMU64rr:
        case MOpcode::DIVU64Chk0rr:
        case MOpcode::REMU64Chk0rr:
        case MOpcode::ADDOvfrr:
        case MOpcode::SUBOvfrr:
        case MOpcode::IMULOvfrr:
            if (idx == 0)
                return {false, true};
            if (idx == 1 || idx == 2)
                return {true, false};
            return {false, false};

        case MOpcode::CMPrr:
        case MOpcode::CMPrr32:
        case MOpcode::CMPrm:
        case MOpcode::TESTrr:
        case MOpcode::UCOMIS:
            return {idx <= 1, false};
        case MOpcode::CMPri:
            return {idx == 0, false};

        case MOpcode::SETcc:
            if (idx < instr.operands.size() &&
                (std::holds_alternative<OpReg>(instr.operands[idx]) ||
                 std::holds_alternative<OpMem>(instr.operands[idx]))) {
                return {false, true};
            }
            return {false, false};

        case MOpcode::IDIVrm:
        case MOpcode::DIVrm:
        case MOpcode::MULr:
        case MOpcode::IMULr:
            return {idx == 0, false};

        case MOpcode::CQO:
            return {false, false};

        case MOpcode::CALL:
            return {idx == 0, false};
        case MOpcode::JMP:
            // An indirect jump reads its register target; label targets have
            // no register semantics.
            return {idx == 0 && std::holds_alternative<OpReg>(instr.operands[idx]), false};

        case MOpcode::JUMPTABLE:
            // [0]=index (use); the remaining operands are labels. The
            // dispatch temps are the reserved R10/R11 scratch registers.
            return {idx == 0, false};

        case MOpcode::RET:
        case MOpcode::JCC:
        case MOpcode::LABEL:
        case MOpcode::UD2:
            return {false, false};

        case MOpcode::PX_COPY:
            return {(idx % 2) == 1, (idx % 2) == 0};

        case MOpcode::SELECT_GPR:
        case MOpcode::SELECT_XMM:
            if (idx == 0)
                return {false, true};
            if (idx == 1 || idx == 2 || idx == 3)
                return {true, false};
            return {false, false};
    }
    return {true, true};
}

/// @brief Predicate: does @p opcode read x86 EFLAGS?
/// @details Only branch/setcc/cmov family opcodes consume EFLAGS. Used by
///          peephole and scheduler passes to determine whether a flag-defining
///          instruction is still observable.
/// @param opcode MIR opcode to classify.
/// @return `true` when the opcode consumes the current EFLAGS value.
bool usesEFlags(MOpcode opcode) noexcept {
    switch (opcode) {
        case MOpcode::JCC:
        case MOpcode::SETcc:
        case MOpcode::CMOVNErr:
            return true;
        default:
            return false;
    }
}

/// @brief Predicate: does @p opcode unconditionally write x86 EFLAGS?
/// @details Almost every ALU instruction defines EFLAGS; MOVs and LEA do not.
///          Used to bound the "live EFLAGS" window scanned by peepholes.
/// @param opcode MIR opcode to classify.
/// @return `true` when executing the opcode overwrites EFLAGS.
bool definesEFlags(MOpcode opcode) noexcept {
    switch (opcode) {
        case MOpcode::ADDrr:
        case MOpcode::ADDri:
        case MOpcode::ANDrr:
        case MOpcode::ANDri:
        case MOpcode::ORrr:
        case MOpcode::ORri:
        case MOpcode::XORrr:
        case MOpcode::XORri:
        case MOpcode::XORrr32:
        case MOpcode::SUBrr:
        case MOpcode::ADDrr32:
        case MOpcode::SUBrr32:
        case MOpcode::IMULrr32:
        case MOpcode::ADDri32:
        case MOpcode::CMPrr32:
        case MOpcode::ADDrr16:
        case MOpcode::SUBrr16:
        case MOpcode::IMULrr16:
        case MOpcode::ADDri16:
        case MOpcode::ADDrm:
        case MOpcode::SUBrm:
        case MOpcode::ANDrm:
        case MOpcode::ORrm:
        case MOpcode::XORrm:
        case MOpcode::CMPrm:
        case MOpcode::IMULrm:
        case MOpcode::SHLri:
        case MOpcode::SHLrc:
        case MOpcode::SHRri:
        case MOpcode::SHRrc:
        case MOpcode::SARri:
        case MOpcode::SARrc:
        case MOpcode::IMULrr:
        case MOpcode::CMPrr:
        case MOpcode::CMPri:
        case MOpcode::TESTrr:
        case MOpcode::UCOMIS:
        case MOpcode::IDIVrm:
        case MOpcode::DIVrm:
        case MOpcode::MULr:
        case MOpcode::IMULr:
            return true;
        default:
            return false;
    }
}

/// @brief Predicate: does @p opcode have side effects beyond producing a value?
/// @details Memory writes, control-flow transfers, division traps and the
///          PX_COPY pseudo all qualify because dropping them could change
///          observable program behavior. Pure register-to-register data
///          movement instructions return false so DCE can remove them when
///          their destinations are dead.
/// @param opcode MIR opcode to classify.
/// @return `true` when deleting an otherwise dead instruction could change behavior.
bool hasObservableSideEffects(MOpcode opcode) noexcept {
    switch (opcode) {
        case MOpcode::PUSH:
        case MOpcode::POP:
        case MOpcode::MOVrm:
        case MOpcode::MOVSDrm:
        case MOpcode::MOVUPSrm:
        case MOpcode::CALL:
        case MOpcode::RET:
        case MOpcode::JMP:
        case MOpcode::JCC:
        case MOpcode::LABEL:
        case MOpcode::UD2:
        case MOpcode::IDIVrm:
        case MOpcode::DIVrm:
        case MOpcode::MULr:
        case MOpcode::IMULr:
        case MOpcode::DIVS64rr:
        case MOpcode::REMS64rr:
        case MOpcode::DIVS64Chk0rr:
        case MOpcode::REMS64Chk0rr:
        case MOpcode::DIVU64rr:
        case MOpcode::REMU64rr:
        case MOpcode::DIVU64Chk0rr:
        case MOpcode::REMU64Chk0rr:
        case MOpcode::CQO:
        case MOpcode::ADDOvfrr:
        case MOpcode::SUBOvfrr:
        case MOpcode::IMULOvfrr:
        case MOpcode::PX_COPY:
        case MOpcode::JUMPTABLE:
            return true;
        case MOpcode::MOVrr:
        case MOpcode::MOVri:
        case MOpcode::MOVmr:
        case MOpcode::CMOVNErr:
        case MOpcode::SELECT_GPR:
        case MOpcode::SELECT_XMM:
        case MOpcode::LEA:
        case MOpcode::ADDrr:
        case MOpcode::ADDri:
        case MOpcode::ANDrr:
        case MOpcode::ANDri:
        case MOpcode::ORrr:
        case MOpcode::ORri:
        case MOpcode::XORrr:
        case MOpcode::XORri:
        case MOpcode::SUBrr:
        case MOpcode::SHLri:
        case MOpcode::SHLrc:
        case MOpcode::SHRri:
        case MOpcode::SHRrc:
        case MOpcode::SARri:
        case MOpcode::SARrc:
        case MOpcode::IMULrr:
        case MOpcode::XORrr32:
        case MOpcode::CMPrr:
        case MOpcode::CMPri:
        case MOpcode::SETcc:
        case MOpcode::TESTrr:
        case MOpcode::MOVZXrr8:
        case MOpcode::MOVZXrr32:
        case MOpcode::ADDrr32:
        case MOpcode::SUBrr32:
        case MOpcode::IMULrr32:
        case MOpcode::ADDri32:
        case MOpcode::CMPrr32:
        case MOpcode::MOVSXD:
        case MOpcode::ADDrr16:
        case MOpcode::SUBrr16:
        case MOpcode::IMULrr16:
        case MOpcode::ADDri16:
        case MOpcode::MOVSXrr16:
        case MOpcode::ADDrm:
        case MOpcode::SUBrm:
        case MOpcode::ANDrm:
        case MOpcode::ORrm:
        case MOpcode::XORrm:
        case MOpcode::CMPrm:
        case MOpcode::IMULrm:
        case MOpcode::FADD:
        case MOpcode::FSUB:
        case MOpcode::FMUL:
        case MOpcode::FDIV:
        case MOpcode::UCOMIS:
        case MOpcode::CVTSI2SD:
        case MOpcode::CVTTSD2SI:
        case MOpcode::MOVQrx:
        case MOpcode::MOVQxr:
        case MOpcode::MOVSDrr:
        case MOpcode::MOVSDmr:
        case MOpcode::MOVUPSmr:
            return false;
    }
    return true;
}

PhysRegMask implicitDefMask(MOpcode opcode) noexcept {
    switch (opcode) {
        case MOpcode::CQO:
            return physRegBit(PhysReg::RDX);
        case MOpcode::IDIVrm:
        case MOpcode::DIVrm:
        case MOpcode::MULr:
        case MOpcode::IMULr:
            return physRegBit(PhysReg::RAX) | physRegBit(PhysReg::RDX);
        default:
            return 0;
    }
}

PhysRegMask implicitUseMask(MOpcode opcode) noexcept {
    switch (opcode) {
        case MOpcode::CQO:
        case MOpcode::MULr:
        case MOpcode::IMULr:
            return physRegBit(PhysReg::RAX);
        case MOpcode::IDIVrm:
        case MOpcode::DIVrm:
            return physRegBit(PhysReg::RAX) | physRegBit(PhysReg::RDX);
        case MOpcode::SHLrc:
        case MOpcode::SHRrc:
        case MOpcode::SARrc:
            return physRegBit(PhysReg::RCX);
        default:
            return 0;
    }
}

} // namespace zanna::codegen::x64
