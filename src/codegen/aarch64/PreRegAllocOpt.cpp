//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/PreRegAllocOpt.cpp
// Purpose: Conservative AArch64 MIR cleanup before register allocation:
//          identity-copy removal, single-use copy forwarding. The traversal
//          and safety conditions live in the shared PreRAForwardCopy
//          template; this file supplies the AArch64 MIR queries (copy
//          shapes, def positions, boundary opcodes).
//
// Key invariants:
//   - Forwarding candidates are virtual-register copies; precolored physical
//     operands are treated as barriers rather than forwarded sources.
//   - Only eliminates copies that are provably safe (single use, no call crosses,
//     no aliasing in the def/use chain).
//
// Ownership/Lifetime:
//   - Borrows MFunction for the duration of the call; no persistent state.
//
// Links: src/codegen/common/PreRAForwardCopy.hpp,
//        src/codegen/aarch64/PreRegAllocOpt.hpp,
//        src/codegen/aarch64/passes/PreRegAllocOptPass.cpp
//
//===----------------------------------------------------------------------===//

#include "codegen/aarch64/PreRegAllocOpt.hpp"

#include "codegen/aarch64/ra/OpcodeClassify.hpp"
#include "codegen/aarch64/ra/OperandRoles.hpp"
#include "codegen/common/PreRAForwardCopy.hpp"

#include <cstdint>
#include <cstdlib>
#include <unordered_map>
#include <vector>

/**
 * @file
 * @brief Implements AArch64 pre-allocation copy forwarding and shifted-operand folding.
 *
 * `A64PreRATraits` adapts AArch64 operand shapes to the shared copy-forwarding
 * algorithm. A second whole-function analysis folds single-def/single-use
 * `LslRI` chains into scaled load/store addressing or shifted-register ALU
 * instructions without extending virtual-register live ranges.
 */

namespace zanna::codegen::aarch64 {
namespace {

/**
 * @brief Tests whether two register descriptors name the same typed register.
 *
 * @param lhs First physical or virtual register.
 * @param rhs Second physical or virtual register.
 * @return `true` when physicality, class, and numeric identity all match.
 */
[[nodiscard]] bool sameReg(const MReg &lhs, const MReg &rhs) noexcept {
    return lhs.isPhys == rhs.isPhys && lhs.cls == rhs.cls && lhs.idOrPhys == rhs.idOrPhys;
}

/**
 * @brief Tests whether an opcode is an integer or FP register copy.
 *
 * @param opcode MIR opcode to classify.
 * @return `true` for `MovRR` or `FMovRR`.
 */
[[nodiscard]] bool isCopyOpcode(MOpcode opcode) noexcept {
    return opcode == MOpcode::MovRR || opcode == MOpcode::FMovRR;
}

/**
 * @brief Tests whether an opcode forms a non-call block boundary.
 *
 * @param opcode MIR opcode to classify.
 * @return `true` for branch, jump-table, or return opcodes; calls deliberately
 *         return `false` because the shared algorithm handles them separately.
 */
[[nodiscard]] bool isNonCallBoundaryOpcode(MOpcode opcode) noexcept {
    switch (opcode) {
        case MOpcode::Br:
        case MOpcode::BCond:
        case MOpcode::Cbz:
        case MOpcode::Cbnz:
        case MOpcode::Tbz:
        case MOpcode::Tbnz:
        case MOpcode::JumpTable:
        case MOpcode::Ret:
            return true;
        default:
            return false;
    }
}

/**
 * @brief Tests whether an opcode defines its first operand.
 *
 * @param opcode MIR opcode to classify.
 * @return `true` for the explicitly enumerated single-destination operations.
 * @note Paired loads are handled separately because they define two operands.
 */
[[nodiscard]] bool definesFirstOperand(MOpcode opcode) noexcept {
    switch (opcode) {
        case MOpcode::MovRR:
        case MOpcode::MovRI:
        case MOpcode::FMovRR:
        case MOpcode::FMovRI:
        case MOpcode::FMovGR:
        case MOpcode::FAddRRR:
        case MOpcode::FSubRRR:
        case MOpcode::FMulRRR:
        case MOpcode::FDivRRR:
        case MOpcode::SCvtF:
        case MOpcode::FCvtZS:
        case MOpcode::UCvtF:
        case MOpcode::FCvtZU:
        case MOpcode::FRintN:
        case MOpcode::LdrRegFpImm:
        case MOpcode::Ldr8RegFpImm:
        case MOpcode::Ldr16RegFpImm:
        case MOpcode::Ldr32RegFpImm:
        case MOpcode::LdrFprFpImm:
        case MOpcode::LdrRegBaseImm:
        case MOpcode::Ldr8RegBaseImm:
        case MOpcode::Ldr16RegBaseImm:
        case MOpcode::Ldr32RegBaseImm:
        case MOpcode::LdrFprBaseImm:
        case MOpcode::LdrRegBaseRegLsl:
        case MOpcode::Ldr32RegBaseRegLsl:
        case MOpcode::LdrFprBaseRegLsl:
        case MOpcode::AddRRRLsl:
        case MOpcode::SubRRRLsl:
        case MOpcode::AndRRRLsl:
        case MOpcode::OrrRRRLsl:
        case MOpcode::EorRRRLsl:
        case MOpcode::AddFpImm:
        case MOpcode::AddRRR:
        case MOpcode::SubRRR:
        case MOpcode::MulRRR:
        case MOpcode::SmulhRRR:
        case MOpcode::UmulhRRR:
        case MOpcode::SDivRRR:
        case MOpcode::UDivRRR:
        case MOpcode::MSubRRRR:
        case MOpcode::AndRRR:
        case MOpcode::OrrRRR:
        case MOpcode::EorRRR:
        case MOpcode::AndRI:
        case MOpcode::OrrRI:
        case MOpcode::EorRI:
        case MOpcode::AddRI:
        case MOpcode::SubRI:
        case MOpcode::LslRI:
        case MOpcode::LsrRI:
        case MOpcode::AsrRI:
        case MOpcode::LslvRRR:
        case MOpcode::LsrvRRR:
        case MOpcode::AsrvRRR:
        case MOpcode::Cset:
        case MOpcode::AdrPage:
        case MOpcode::AddPageOff:
        case MOpcode::MAddRRRR:
        case MOpcode::Csel:
        case MOpcode::FCsel:
        case MOpcode::AddsRRR:
        case MOpcode::SubsRRR:
        case MOpcode::AddsRI:
        case MOpcode::SubsRI:
        case MOpcode::AddOvfRRR:
        case MOpcode::SubOvfRRR:
        case MOpcode::AddOvfRI:
        case MOpcode::SubOvfRI:
        case MOpcode::MulOvfRRR:
            return true;
        default:
            return false;
    }
}

/**
 * @brief Tests whether an operand names a particular register.
 *
 * @param operand Candidate MIR operand.
 * @param reg Register descriptor to match.
 * @return `true` only for a register operand accepted by `sameReg()`.
 */
[[nodiscard]] bool operandIsReg(const MOperand &operand, const MReg &reg) noexcept {
    return operand.kind == MOperand::Kind::Reg && sameReg(operand.reg, reg);
}

/**
 * @brief Classifies an indexed register operand as a use rather than a definition.
 *
 * @param instr Instruction whose operand roles are inspected.
 * @param operandIndex Valid index into `instr.ops`.
 * @return `false` for non-registers, first-operand definitions, and the two
 *         destination operands of paired loads; otherwise `true`.
 * @pre `operandIndex < instr.ops.size()`.
 */
[[nodiscard]] bool operandIsUse(const MInstr &instr, std::size_t operandIndex) noexcept {
    if (instr.ops[operandIndex].kind != MOperand::Kind::Reg)
        return false;
    if (definesFirstOperand(instr.opc) && operandIndex == 0)
        return false;
    if ((instr.opc == MOpcode::LdpRegFpImm || instr.opc == MOpcode::LdpFprFpImm) &&
        operandIndex < 2) {
        return false;
    }
    return true;
}

/**
 * @brief Adapts AArch64 MIR to the shared pre-register-allocation copy forwarder.
 *
 * The trait operations expose instruction storage, recognize copy shapes and
 * boundaries, count direct uses, and perform the final operand substitution.
 */
struct A64PreRATraits {
    using BlockT = MBasicBlock;
    using InstrT = MInstr;
    using RegT = MReg;

    /**
     * @brief Retrieves mutable instruction storage for a block.
     * @param block Block to adapt.
     * @return Reference to `block.instrs`.
     */
    static std::vector<MInstr> &instrs(MBasicBlock &block) {
        return block.instrs;
    }

    /**
     * @brief Retrieves read-only instruction storage for a block.
     * @param block Block to adapt.
     * @return Const reference to `block.instrs`.
     */
    static const std::vector<MInstr> &instrs(const MBasicBlock &block) {
        return block.instrs;
    }

    /**
     * @brief Tests for a well-formed copy whose source and destination are identical.
     * @param instr Instruction to classify.
     * @return `true` for a two-register `MovRR`/`FMovRR` identity.
     */
    static bool isIdentityCopy(const MInstr &instr) {
        if (!isCopyOpcode(instr.opc) || instr.ops.size() != 2)
            return false;
        if (instr.ops[0].kind != MOperand::Kind::Reg || instr.ops[1].kind != MOperand::Kind::Reg)
            return false;
        return sameReg(instr.ops[0].reg, instr.ops[1].reg);
    }

    /**
     * @brief Extracts a forwardable virtual-register copy.
     *
     * @param instr Candidate copy instruction.
     * @param[out] dst Destination register on success.
     * @param[out] src Source register on success.
     * @return `true` for a non-identity, same-class virtual-to-virtual copy.
     */
    static bool isForwardableCopy(const MInstr &instr, MReg &dst, MReg &src) {
        if (!isCopyOpcode(instr.opc) || instr.ops.size() != 2)
            return false;
        if (instr.ops[0].kind != MOperand::Kind::Reg || instr.ops[1].kind != MOperand::Kind::Reg)
            return false;
        const MReg &dstReg = instr.ops[0].reg;
        const MReg &srcReg = instr.ops[1].reg;
        // Physical sources such as ABI return registers are not tracked as
        // live ranges here. Forwarding them would let register allocation
        // reuse that physical register before the forwarded use.
        if (dstReg.isPhys || srcReg.isPhys || dstReg.cls != srcReg.cls || sameReg(dstReg, srcReg))
            return false;
        dst = dstReg;
        src = srcReg;
        return true;
    }

    /**
     * @brief Tests whether an instruction defines a specified register.
     * @param instr Instruction to inspect.
     * @param reg Register whose definition is sought.
     * @return `true` for a matching ordinary destination or either paired-load destination.
     */
    static bool definesReg(const MInstr &instr, const MReg &reg) {
        if (definesFirstOperand(instr.opc) && !instr.ops.empty() && operandIsReg(instr.ops[0], reg))
            return true;
        if ((instr.opc == MOpcode::LdpRegFpImm || instr.opc == MOpcode::LdpFprFpImm) &&
            instr.ops.size() >= 2) {
            return operandIsReg(instr.ops[0], reg) || operandIsReg(instr.ops[1], reg);
        }
        return false;
    }

    /**
     * @brief Tests whether an instruction is a direct or indirect call.
     * @param instr Instruction to classify.
     * @return `true` for `Bl` or `Blr`.
     */
    static bool isCall(const MInstr &instr) {
        return instr.opc == MOpcode::Bl || instr.opc == MOpcode::Blr;
    }

    /**
     * @brief Tests whether an instruction ends non-call sequential flow.
     * @param instr Instruction to classify.
     * @return Classification supplied by `isNonCallBoundaryOpcode()`.
     */
    static bool isNonCallBoundary(const MInstr &instr) {
        return isNonCallBoundaryOpcode(instr.opc);
    }

    /**
     * @brief Counts direct uses of a candidate copy destination in one instruction.
     * @param instr Instruction whose use operands are scanned.
     * @param dst Register to count.
     * @return Use count and the last matching direct operand index.
     */
    static common::PreRAUseScan scanUses(const MInstr &instr, const MReg &dst) {
        common::PreRAUseScan scan{};
        for (std::size_t opIdx = 0; opIdx < instr.ops.size(); ++opIdx) {
            if (!operandIsUse(instr, opIdx))
                continue;
            if (!operandIsReg(instr.ops[opIdx], dst))
                continue;
            ++scan.useCount;
            ++scan.directUseCount;
            scan.directOperand = opIdx;
        }
        return scan;
    }

    /**
     * @brief Replaces one use operand with a copy's source operand.
     * @param[in,out] use Instruction containing the operand to rewrite.
     * @param operandIndex Valid use index in `use.ops`.
     * @param copy Well-formed copy whose second operand is the replacement source.
     * @pre `operandIndex < use.ops.size()` and `copy.ops.size() >= 2`.
     */
    static void forwardUse(MInstr &use, std::size_t operandIndex, const MInstr &copy) {
        use.ops[operandIndex] = copy.ops[1];
    }

    /**
     * @brief Appends every virtual register the instruction mentions.
     *
     * Role-agnostic on purpose: the caller only needs to know which blocks
     * touch a register, so definitions and uses alike count.
     *
     * @param instr Instruction to scan.
     * @param[out] out Vector receiving the virtual registers.
     */
    static void collectVRegs(const MInstr &instr, std::vector<MReg> &out) {
        for (const auto &operand : instr.ops) {
            if (operand.kind == MOperand::Kind::Reg && !operand.reg.isPhys)
                out.push_back(operand.reg);
        }
    }

    /**
     * @brief Produces a stable identity for a virtual register.
     * @param reg Virtual register to key.
     * @return Class-qualified key, so GPR and FPR ids never collide.
     */
    static uint32_t vregKey(const MReg &reg) {
        return (static_cast<uint32_t>(reg.cls) << 16) | reg.idOrPhys;
    }
};

/**
 * @brief Folds shifted virtual-register chains into AArch64 addressing and ALU forms.
 *
 * Within each block, a single-use `lsl` plus `add` address can be absorbed by
 * a zero-offset scalar load/store when the scale is legal for its access size.
 * A single-use shifted operand may also be absorbed by add/sub/and/orr/eor.
 * Whole-function definition/use counts ensure removed producers have no other
 * consumers.
 *
 * @param[in,out] fn Pre-allocation MIR function to rewrite.
 * @return Number of load/store or ALU patterns folded.
 */
std::size_t runAddressingFolds(MFunction &fn) {
    // Whole-function def/use counts per GPR vreg.
    std::unordered_map<uint16_t, unsigned> defCount;
    std::unordered_map<uint16_t, unsigned> useCount;
    for (const auto &bb : fn.blocks) {
        for (const auto &mi : bb.instrs) {
            for (std::size_t oi = 0; oi < mi.ops.size(); ++oi) {
                const auto &op = mi.ops[oi];
                if (op.kind != MOperand::Kind::Reg || op.reg.isPhys ||
                    op.reg.cls != RegClass::GPR)
                    continue;
                const auto [isUse, isDef] = ra::operandRoles(mi, oi);
                if (isUse)
                    ++useCount[op.reg.idOrPhys];
                if (isDef)
                    ++defCount[op.reg.idOrPhys];
            }
        }
    }
    /**
     * @brief Tests whether a GPR virtual register has exactly one definition and use.
     * @param vreg Virtual-register ID to query.
     * @return `true` when both whole-function counts equal one.
     */
    const auto singleDefUse = [&](uint16_t vreg) {
        auto d = defCount.find(vreg);
        auto u = useCount.find(vreg);
        return d != defCount.end() && d->second == 1 && u != useCount.end() && u->second == 1;
    };
    /**
     * @brief Extracts the numeric ID from a known register operand.
     * @param op Register operand.
     * @return Stored register ID.
     */
    const auto vregOf = [](const MOperand &op) -> uint16_t {
        return op.reg.idOrPhys;
    };
    /**
     * @brief Tests whether an operand names a virtual GPR.
     * @param op Operand to classify.
     * @return `true` for a non-physical GPR register operand.
     */
    const auto isVGpr = [](const MOperand &op) {
        return op.kind == MOperand::Kind::Reg && !op.reg.isPhys && op.reg.cls == RegClass::GPR;
    };

    std::size_t folded = 0;
    for (auto &bb : fn.blocks) {
        struct ShiftDef {
            std::size_t idx{};
            MOperand src{};
            long long amount{};
        };
        struct AddrDef {
            std::size_t idx{};
            std::size_t shiftIdx{};
            MOperand base{};
            MOperand index{};
            long long amount{};
        };
        std::unordered_map<uint16_t, ShiftDef> shiftDefs;
        std::unordered_map<uint16_t, AddrDef> addrDefs;
        std::vector<char> removed(bb.instrs.size(), 0);

        /**
         * @brief Invalidates recorded patterns affected by an instruction's definitions.
         *
         * @param mi Instruction whose defining operands may overwrite a
         *        tracked result, base, index, or shifted source.
         */
        const auto invalidateOnDef = [&](const MInstr &mi) {
            for (std::size_t oi = 0; oi < mi.ops.size(); ++oi) {
                const auto [isUse, isDef] = ra::operandRoles(mi, oi);
                (void)isUse;
                if (!isDef || mi.ops[oi].kind != MOperand::Kind::Reg)
                    continue;
                const auto &defReg = mi.ops[oi].reg;
                for (auto it = shiftDefs.begin(); it != shiftDefs.end();) {
                    if ((defReg.isPhys == it->second.src.reg.isPhys &&
                         defReg.cls == it->second.src.reg.cls &&
                         defReg.idOrPhys == it->second.src.reg.idOrPhys) ||
                        (!defReg.isPhys && defReg.cls == RegClass::GPR &&
                         defReg.idOrPhys == it->first)) {
                        it = shiftDefs.erase(it);
                    } else {
                        ++it;
                    }
                }
                for (auto it = addrDefs.begin(); it != addrDefs.end();) {
                    const auto &entry = it->second;
                    /// @brief Tests whether the current definition overwrites a tracked operand.
                    /// @param src Tracked register operand.
                    /// @return `true` when `defReg` and `src` name the same register.
                    const auto matches = [&](const MOperand &src) {
                        return defReg.isPhys == src.reg.isPhys && defReg.cls == src.reg.cls &&
                               defReg.idOrPhys == src.reg.idOrPhys;
                    };
                    if (matches(entry.base) || matches(entry.index) ||
                        (!defReg.isPhys && defReg.cls == RegClass::GPR &&
                         defReg.idOrPhys == it->first)) {
                        it = addrDefs.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
        };

        for (std::size_t i = 0; i < bb.instrs.size(); ++i) {
            MInstr &mi = bb.instrs[i];

            // Load/store through a single-use shift+add address: fold to the
            // scaled register-offset form when the amount is legal.
            const bool isBaseLd = mi.opc == MOpcode::LdrRegBaseImm ||
                                  mi.opc == MOpcode::Ldr32RegBaseImm ||
                                  mi.opc == MOpcode::LdrFprBaseImm;
            const bool isBaseSt = mi.opc == MOpcode::StrRegBaseImm ||
                                  mi.opc == MOpcode::Str32RegBaseImm ||
                                  mi.opc == MOpcode::StrFprBaseImm;
            if ((isBaseLd || isBaseSt) && mi.ops.size() >= 3 && isVGpr(mi.ops[1]) &&
                mi.ops[2].kind == MOperand::Kind::Imm && mi.ops[2].imm == 0) {
                auto addrIt = addrDefs.find(vregOf(mi.ops[1]));
                if (addrIt != addrDefs.end()) {
                    const bool is32 = mi.opc == MOpcode::Ldr32RegBaseImm ||
                                      mi.opc == MOpcode::Str32RegBaseImm;
                    const long long legal = is32 ? 2 : 3;
                    const long long amount = addrIt->second.amount;
                    if (amount == 0 || amount == legal) {
                        MOpcode replacement = MOpcode::LdrRegBaseRegLsl;
                        if (mi.opc == MOpcode::StrRegBaseImm)
                            replacement = MOpcode::StrRegBaseRegLsl;
                        else if (mi.opc == MOpcode::Ldr32RegBaseImm)
                            replacement = MOpcode::Ldr32RegBaseRegLsl;
                        else if (mi.opc == MOpcode::Str32RegBaseImm)
                            replacement = MOpcode::Str32RegBaseRegLsl;
                        else if (mi.opc == MOpcode::LdrFprBaseImm)
                            replacement = MOpcode::LdrFprBaseRegLsl;
                        else if (mi.opc == MOpcode::StrFprBaseImm)
                            replacement = MOpcode::StrFprBaseRegLsl;
                        const AddrDef entry = addrIt->second;
                        mi.opc = replacement;
                        mi.ops = {mi.ops[0], entry.base, entry.index, MOperand::immOp(entry.amount)};
                        removed[entry.idx] = 1;
                        removed[entry.shiftIdx] = 1;
                        addrDefs.erase(addrIt);
                        ++folded;
                        invalidateOnDef(mi);
                        continue;
                    }
                }
            }

            // Shift feeding an add: remember the pair as an address candidate
            // (a later load/store may absorb it) — or fold the ALU directly.
            if (mi.opc == MOpcode::AddRRR && mi.ops.size() >= 3 && isVGpr(mi.ops[0]) &&
                mi.ops[1].kind == MOperand::Kind::Reg && isVGpr(mi.ops[2])) {
                auto shiftIt = shiftDefs.find(vregOf(mi.ops[2]));
                if (shiftIt != shiftDefs.end() && singleDefUse(vregOf(mi.ops[2]))) {
                    const ShiftDef entry = shiftIt->second;
                    shiftDefs.erase(shiftIt);
                    // Invalidate BEFORE recording: this add's def must clear
                    // stale entries keyed by its destination, not the fresh
                    // candidate recorded below.
                    invalidateOnDef(mi);
                    if (singleDefUse(vregOf(mi.ops[0]))) {
                        // Candidate address computation; defer to a load fold.
                        addrDefs[vregOf(mi.ops[0])] =
                            AddrDef{i, entry.idx, mi.ops[1], entry.src, entry.amount};
                    } else {
                        // Fold the shift into the add's second operand.
                        mi.opc = MOpcode::AddRRRLsl;
                        mi.ops = {mi.ops[0], mi.ops[1], entry.src, MOperand::immOp(entry.amount)};
                        removed[entry.idx] = 1;
                        ++folded;
                    }
                    continue;
                }
            }

            // Sub/And/Orr/Eor with a single-use shifted second operand.
            if ((mi.opc == MOpcode::SubRRR || mi.opc == MOpcode::AndRRR ||
                 mi.opc == MOpcode::OrrRRR || mi.opc == MOpcode::EorRRR) &&
                mi.ops.size() >= 3 && isVGpr(mi.ops[2])) {
                auto shiftIt = shiftDefs.find(vregOf(mi.ops[2]));
                if (shiftIt != shiftDefs.end() && singleDefUse(vregOf(mi.ops[2]))) {
                    const ShiftDef entry = shiftIt->second;
                    mi.opc = mi.opc == MOpcode::SubRRR   ? MOpcode::SubRRRLsl
                             : mi.opc == MOpcode::AndRRR ? MOpcode::AndRRRLsl
                             : mi.opc == MOpcode::OrrRRR ? MOpcode::OrrRRRLsl
                                                         : MOpcode::EorRRRLsl;
                    mi.ops = {mi.ops[0], mi.ops[1], entry.src, MOperand::immOp(entry.amount)};
                    removed[entry.idx] = 1;
                    ++folded;
                    shiftDefs.erase(shiftIt);
                    invalidateOnDef(mi);
                    continue;
                }
            }

            // Record fresh shift defs AFTER matching so a shift never feeds
            // itself; calls and other clobbers invalidate via operand defs.
            if (ra::isCall(mi.opc)) {
                shiftDefs.clear();
                addrDefs.clear();
                continue;
            }
            invalidateOnDef(mi);
            if (mi.opc == MOpcode::LslRI && mi.ops.size() >= 3 && isVGpr(mi.ops[0]) &&
                mi.ops[1].kind == MOperand::Kind::Reg &&
                mi.ops[2].kind == MOperand::Kind::Imm && mi.ops[2].imm >= 0 &&
                mi.ops[2].imm < 64) {
                shiftDefs[vregOf(mi.ops[0])] = ShiftDef{i, mi.ops[1], mi.ops[2].imm};
            }
        }

        if (folded > 0) {
            std::vector<MInstr> kept;
            kept.reserve(bb.instrs.size());
            for (std::size_t i = 0; i < bb.instrs.size(); ++i) {
                if (!removed[i])
                    kept.push_back(std::move(bb.instrs[i]));
            }
            bb.instrs = std::move(kept);
        }
    }
    return folded;
}

} // namespace

/**
 * @brief Runs shared copy forwarding followed by optional addressing folds.
 *
 * @param[in,out] fn Legalized pre-allocation function to optimize.
 * @return Sum of transformations reported by copy forwarding and addressing folding.
 */
std::size_t runPreRegAllocOpt(MFunction &fn) {
    const std::size_t forwarded = common::runPreRAForwardCopy<A64PreRATraits>(fn);
    // ZANNA_NO_ADDR_FOLDS=1 disables the addressing folds for triage.
    if (std::getenv("ZANNA_NO_ADDR_FOLDS") != nullptr)
        return forwarded;
    return forwarded + runAddressingFolds(fn);
}

} // namespace zanna::codegen::aarch64
