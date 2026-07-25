//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/PreRegAllocOpt.cpp
// Purpose: Implement conservative x86-64 MIR cleanup before register
//          allocation. The traversal and safety conditions live in the shared
//          PreRAForwardCopy template; this file supplies the x86-64 MIR
//          queries (copy shapes, operand roles, boundary opcodes).
// Key invariants:
//   - Operates on virtual-register MIR only; physical sources are not forwarded.
//   - Single-use copy elimination does not cross block boundaries.
// Ownership/Lifetime:
//   - Stateless; mutates caller-owned MFunction in place.
// Links: src/codegen/common/PreRAForwardCopy.hpp,
//        src/codegen/x86_64/PreRegAllocOpt.hpp,
//        src/codegen/x86_64/OperandRoles.hpp
//
//===----------------------------------------------------------------------===//

#include "codegen/x86_64/PreRegAllocOpt.hpp"

#include "codegen/common/PreRAForwardCopy.hpp"
#include "codegen/x86_64/OperandRoles.hpp"

#include <cstdlib>
#include <variant>

/**
 * @file
 * @brief Implements x86-64 traits and load fusion for pre-RA MIR cleanup.
 *
 * The shared forward-copy engine receives target-specific copy, boundary,
 * register-definition, and use-scan semantics through X64PreRATraits. A second
 * pass folds a provably single-use load into an ALU memory operand without
 * crossing stores, calls, control flow, or address-register clobbers.
 */

namespace zanna::codegen::x64 {
namespace {

/// @brief Compare two register operands for equality (phys flag, class, id).
/// @param lhs First register.
/// @param rhs Second register.
/// @return @c true for exact register identity.
[[nodiscard]] bool sameReg(const OpReg &lhs, const OpReg &rhs) noexcept {
    return lhs.isPhys == rhs.isPhys && lhs.cls == rhs.cls && lhs.idOrPhys == rhs.idOrPhys;
}

/// @brief Predicate: is @p opcode a pure register-to-register copy?
/// @param opcode Opcode to classify.
/// @return @c true for GPR or scalar-double register copies.
[[nodiscard]] bool isCopyOpcode(MOpcode opcode) noexcept {
    return opcode == MOpcode::MOVrr || opcode == MOpcode::MOVSDrr;
}

/// @brief Predicate: does @p opcode terminate a straight-line region (calls excluded)?
/// @details Implicit-register sequences (CQO/IDIV), pseudos expanded later
///          (PX_COPY, SELECT, checked arithmetic), and control flow all stop
///          the forwarding scan.
/// @param opcode Opcode to classify.
/// @return @c true when forwarding must stop before this operation.
[[nodiscard]] bool isNonCallBoundaryOpcode(MOpcode opcode) noexcept {
    switch (opcode) {
        case MOpcode::RET:
        case MOpcode::JMP:
        case MOpcode::JCC:
        case MOpcode::LABEL:
        case MOpcode::JUMPTABLE:
        case MOpcode::UD2:
        case MOpcode::PUSH:
        case MOpcode::POP:
        case MOpcode::CQO:
        case MOpcode::IDIVrm:
        case MOpcode::DIVrm:
        case MOpcode::MULr:
        case MOpcode::IMULr:
        case MOpcode::PX_COPY:
        case MOpcode::SELECT_GPR:
        case MOpcode::SELECT_XMM:
        case MOpcode::DIVS64rr:
        case MOpcode::REMS64rr:
        case MOpcode::DIVU64rr:
        case MOpcode::REMU64rr:
        case MOpcode::DIVS64Chk0rr:
        case MOpcode::REMS64Chk0rr:
        case MOpcode::DIVU64Chk0rr:
        case MOpcode::REMU64Chk0rr:
        case MOpcode::ADDOvfrr:
        case MOpcode::SUBOvfrr:
        case MOpcode::IMULOvfrr:
            return true;
        default:
            return false;
    }
}

/// @brief Predicate: does @p mem reference @p reg via base or index?
/// @param mem Address expression to inspect.
/// @param reg Register to find.
/// @return @c true when @p reg participates in the address.
[[nodiscard]] bool memUsesReg(const OpMem &mem, const OpReg &reg) noexcept {
    if (sameReg(mem.base, reg))
        return true;
    return mem.hasIndex && sameReg(mem.index, reg);
}

/// @brief x86-64 traits for the shared pre-RA copy forwarding template.
struct X64PreRATraits {
    using BlockT = MBasicBlock;
    using InstrT = MInstr;
    using RegT = OpReg;

    /// @brief Access a block's mutable instruction sequence.
    /// @param block Block to access.
    /// @return Mutable instruction vector.
    static std::vector<MInstr> &instrs(MBasicBlock &block) {
        return block.instructions;
    }

    /// @brief Access a block's read-only instruction sequence.
    /// @param block Block to access.
    /// @return Const instruction vector.
    static const std::vector<MInstr> &instrs(const MBasicBlock &block) {
        return block.instructions;
    }

    /// @brief Test whether a copy reads and writes the same register.
    /// @param instr Candidate instruction.
    /// @return @c true for an identity MOVrr or MOVSDrr.
    static bool isIdentityCopy(const MInstr &instr) {
        if (!isCopyOpcode(instr.opcode) || instr.operands.size() != 2)
            return false;
        const auto *dst = std::get_if<OpReg>(&instr.operands[0]);
        const auto *src = std::get_if<OpReg>(&instr.operands[1]);
        return dst && src && sameReg(*dst, *src);
    }

    /// @brief Decode a safe virtual-to-virtual forwarding copy.
    /// @param instr Candidate copy instruction.
    /// @param dst Destination register output.
    /// @param src Source register output.
    /// @return @c true when the copy may participate in forwarding.
    static bool isForwardableCopy(const MInstr &instr, OpReg &dst, OpReg &src) {
        if (!isCopyOpcode(instr.opcode) || instr.operands.size() != 2)
            return false;
        const auto *dstReg = std::get_if<OpReg>(&instr.operands[0]);
        const auto *srcReg = std::get_if<OpReg>(&instr.operands[1]);
        // Physical sources such as ABI return registers are not tracked as
        // live ranges here. Forwarding them would let register allocation
        // reuse that physical register before the forwarded use.
        if (!dstReg || !srcReg || dstReg->isPhys || srcReg->isPhys ||
            dstReg->cls != srcReg->cls || sameReg(*dstReg, *srcReg))
            return false;
        dst = *dstReg;
        src = *srcReg;
        return true;
    }

    /// @brief Query role-aware definitions of one register.
    /// @param instr Instruction to inspect.
    /// @param reg Register to find.
    /// @return @c true when an explicit def operand names @p reg.
    static bool definesReg(const MInstr &instr, const OpReg &reg) {
        for (std::size_t idx = 0; idx < instr.operands.size(); ++idx) {
            const auto [isUse, isDef] = operandRoles(instr, idx);
            (void)isUse;
            if (!isDef)
                continue;
            const auto *opReg = std::get_if<OpReg>(&instr.operands[idx]);
            if (opReg && sameReg(*opReg, reg))
                return true;
        }
        return false;
    }

    /// @brief Recognize a CALL boundary.
    /// @param instr Instruction to classify.
    /// @return @c true for CALL.
    static bool isCall(const MInstr &instr) {
        return instr.opcode == MOpcode::CALL;
    }

    /// @brief Recognize a non-call straight-line forwarding boundary.
    /// @param instr Instruction to classify.
    /// @return Result of the target opcode boundary classifier.
    static bool isNonCallBoundary(const MInstr &instr) {
        return isNonCallBoundaryOpcode(instr.opcode);
    }

    /// @brief Count direct and address uses of a copy destination.
    /// @param instr Instruction whose use operands are scanned.
    /// @param dst Copy destination register.
    /// @return Use counts and the direct operand index when applicable.
    static common::PreRAUseScan scanUses(const MInstr &instr, const OpReg &dst) {
        common::PreRAUseScan scan{};
        for (std::size_t opIdx = 0; opIdx < instr.operands.size(); ++opIdx) {
            const auto [isUse, isDef] = operandRoles(instr, opIdx);
            if (!isUse)
                continue;

            const auto &operand = instr.operands[opIdx];
            bool usesDst = false;
            bool direct = false;
            if (const auto *opReg = std::get_if<OpReg>(&operand)) {
                usesDst = sameReg(*opReg, dst);
                direct = usesDst && !isDef;
            } else if (const auto *mem = std::get_if<OpMem>(&operand)) {
                usesDst = memUsesReg(*mem, dst);
            }
            if (!usesDst)
                continue;

            ++scan.useCount;
            if (direct) {
                ++scan.directUseCount;
                scan.directOperand = opIdx;
            }
        }
        return scan;
    }

    /// @brief Replace a direct destination use with the copy's source operand.
    /// @param use Consumer instruction to rewrite.
    /// @param operandIndex Direct-use operand index.
    /// @param copy Two-operand source copy.
    static void forwardUse(MInstr &use, std::size_t operandIndex, const MInstr &copy) {
        use.operands[operandIndex] = copy.operands[1];
    }
};

} // namespace

namespace {

/// @brief Map a reg-reg ALU opcode onto its memory-operand (rm) form.
/// @param opcode Register-register opcode to map.
/// @return Memory-source equivalent, or LABEL as a no-form sentinel.
[[nodiscard]] MOpcode memFormFor(MOpcode opcode) noexcept {
    switch (opcode) {
        case MOpcode::ADDrr:
            return MOpcode::ADDrm;
        case MOpcode::SUBrr:
            return MOpcode::SUBrm;
        case MOpcode::ANDrr:
            return MOpcode::ANDrm;
        case MOpcode::ORrr:
            return MOpcode::ORrm;
        case MOpcode::XORrr:
            return MOpcode::XORrm;
        case MOpcode::IMULrr:
            return MOpcode::IMULrm;
        case MOpcode::CMPrr:
            return MOpcode::CMPrm;
        default:
            return MOpcode::LABEL; // sentinel: no memory form
    }
}

/// @brief Predicate: does @p opcode write to memory?
/// @param opcode Opcode to classify.
/// @return @c true for GPR, scalar-double, or full-XMM stores.
[[nodiscard]] bool isStoreOpcode(MOpcode opcode) noexcept {
    return opcode == MOpcode::MOVrm || opcode == MOpcode::MOVSDrm || opcode == MOpcode::MOVUPSrm;
}

/// @brief Predicate: does @p op mention virtual GPR @p reg (directly or via a memory operand)?
/// @param op Operand to inspect.
/// @param reg Register to find.
/// @return @c true for a direct register or address-component mention.
[[nodiscard]] bool operandMentionsReg(const Operand &op, const OpReg &reg) noexcept {
    if (const auto *r = std::get_if<OpReg>(&op))
        return sameReg(*r, reg);
    if (const auto *m = std::get_if<OpMem>(&op))
        return memUsesReg(*m, reg);
    return false;
}

/// @brief Count every mention of @p reg across @p fn except @p skip.
/// @param fn Function to scan.
/// @param reg Register whose mentions are counted.
/// @param skip Instruction excluded by address identity.
/// @return Number of direct and memory-address mentions.
[[nodiscard]] std::size_t countMentionsExcept(const MFunction &fn,
                                              const OpReg &reg,
                                              const MInstr *skip) noexcept {
    std::size_t count = 0;
    for (const auto &block : fn.blocks) {
        for (const auto &instr : block.instructions) {
            if (&instr == skip)
                continue;
            for (const auto &op : instr.operands) {
                if (operandMentionsReg(op, reg))
                    ++count;
            }
        }
    }
    return count;
}

/// @brief Fuse a single-use MOVmr load into a following reg-reg ALU consumer.
/// @details `MOVmr vX, [mem]` + `ALUrr vD, vX` becomes `ALUrm vD, [mem]` when
///          the load result has no other use and nothing between the pair can
///          change the loaded value: any store, call, or boundary opcode
///          aborts the scan, as does a redefinition of the loaded register or
///          of the address's base/index registers.
/// @param fn Function rewritten in place.
/// @return Number of loads folded away.
std::size_t runLoadAluFusion(MFunction &fn) {
    if (std::getenv("ZANNA_NO_LOAD_FUSE") != nullptr)
        return 0;

    std::size_t fused = 0;
    for (auto &block : fn.blocks) {
        auto &instrs = block.instructions;
        for (std::size_t i = 0; i < instrs.size(); ++i) {
            const MInstr &load = instrs[i];
            if (load.opcode != MOpcode::MOVmr || load.operands.size() < 2)
                continue;
            const auto *dst = std::get_if<OpReg>(&load.operands[0]);
            const auto *mem = std::get_if<OpMem>(&load.operands[1]);
            if (!dst || !mem || dst->isPhys || dst->cls != RegClass::GPR)
                continue;

            // The loaded value must have exactly one mention beyond its own
            // definition: the consumer's use.
            if (countMentionsExcept(fn, *dst, &load) != 1)
                continue;

            for (std::size_t j = i + 1; j < instrs.size(); ++j) {
                MInstr &consumer = instrs[j];
                const MOpcode memForm = memFormFor(consumer.opcode);
                if (memForm != MOpcode::LABEL && consumer.operands.size() == 2) {
                    const auto *src = std::get_if<OpReg>(&consumer.operands[1]);
                    if (src != nullptr && sameReg(*src, *dst)) {
                        consumer.opcode = memForm;
                        consumer.operands[1] = *mem;
                        instrs.erase(instrs.begin() + static_cast<std::ptrdiff_t>(i));
                        ++fused;
                        --i;
                        break;
                    }
                }

                if (isStoreOpcode(consumer.opcode) || consumer.opcode == MOpcode::CALL ||
                    isNonCallBoundaryOpcode(consumer.opcode))
                    break;

                // A redefinition of the loaded register or of the address's
                // base/index registers invalidates the pending fold.
                bool clobbered = false;
                for (std::size_t oi = 0; oi < consumer.operands.size() && !clobbered; ++oi) {
                    if (!operandRoles(consumer, oi).second)
                        continue;
                    const auto *defReg = std::get_if<OpReg>(&consumer.operands[oi]);
                    if (defReg == nullptr)
                        continue;
                    if (sameReg(*defReg, *dst) || sameReg(*defReg, mem->base) ||
                        (mem->hasIndex && sameReg(*defReg, mem->index)))
                        clobbered = true;
                }
                if (clobbered)
                    break;
            }
        }
    }
    return fused;
}

} // namespace

/// @brief Top-level entry point for pre-register-allocation MIR cleanup.
/// @details Delegates to the shared template: identity copies are removed and
///          single-use virtual-to-virtual copies are forwarded, then
///          single-use loads are folded into their ALU consumers. Returning a
///          count lets callers report the reduction in statistics output.
/// @param fn Function to rewrite in place.
/// @return Number of MIR instructions removed.
std::size_t runPreRegAllocOpt(MFunction &fn) {
    std::size_t removed = common::runPreRAForwardCopy<X64PreRATraits>(fn);
    removed += runLoadAluFusion(fn);
    return removed;
}

} // namespace zanna::codegen::x64
