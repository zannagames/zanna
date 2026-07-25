//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/peephole/CopyPropDCE.cpp
// Purpose: Copy propagation, dead code elimination, dead FP store elimination,
//          dead flag-setter removal, and compute-into-target folding for the
//          AArch64 peephole optimizer.
//
// Key invariants:
//   - Copy propagation rewrites an ABI-register use only when the copy origin
//     is a non-ABI register (disable with ZANNA_NO_ABI_COPYFWD=1); origins are
//     never chased through ABI registers.
//   - DCE conservatively marks callee-saved and ABI registers as live at exit.
//
// Ownership/Lifetime:
//   - Operates on mutable instruction vectors owned by the caller.
//
// Links: codegen/aarch64/Peephole.hpp
//
//===----------------------------------------------------------------------===//

#include "CopyPropDCE.hpp"

#include "PeepholeCommon.hpp"
#include "codegen/aarch64/Noreturn.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

/// @file
/// @brief Implements physical-copy propagation and several AArch64 DCE passes.

namespace zanna::codegen::aarch64::peephole {

namespace {

/// @brief Return true for instructions that end or escape the local straight-line scan.
/// @details Copy propagation and dead-store tracking are only valid within a
///          single fallthrough-free instruction range. Calls, traps, and branches
///          clear all local facts so later malformed or unreachable instructions
///          cannot inherit state from before a control-flow edge.
/// @param opc Opcode to classify.
/// @return `true` when local dataflow facts must not cross @p opc.
[[nodiscard]] bool isControlBoundary(MOpcode opc) noexcept {
    return opc == MOpcode::Br || opc == MOpcode::BCond || opc == MOpcode::Ret ||
           opc == MOpcode::Bl || opc == MOpcode::Blr || opc == MOpcode::Cbz ||
           opc == MOpcode::Cbnz || opc == MOpcode::Tbz || opc == MOpcode::Tbnz ||
           opc == MOpcode::JumpTable;
}

/// @brief Return true if @p opc is a memory access through an arbitrary base register.
/// @details FP-relative dead-store optimizations cannot prove that a base-relative
///          access does not alias an address-taken local or spill slot, so these
///          opcodes act as barriers for frame-slot store facts.
/// @param opc Opcode to classify.
/// @return `true` for recognized loads and stores through an arbitrary base GPR.
[[nodiscard]] bool isBaseRelativeMemory(MOpcode opc) noexcept {
    switch (opc) {
        case MOpcode::LdrRegBaseImm:
        case MOpcode::Ldr8RegBaseImm:
        case MOpcode::Ldr16RegBaseImm:
        case MOpcode::Ldr32RegBaseImm:
        case MOpcode::LdrFprBaseImm:
        case MOpcode::StrRegBaseImm:
        case MOpcode::Str8RegBaseImm:
        case MOpcode::Str16RegBaseImm:
        case MOpcode::Str32RegBaseImm:
        case MOpcode::StrFprBaseImm:
        case MOpcode::LdrRegBaseRegLsl:
        case MOpcode::Ldr32RegBaseRegLsl:
        case MOpcode::LdrFprBaseRegLsl:
        case MOpcode::StrRegBaseRegLsl:
        case MOpcode::Str32RegBaseRegLsl:
        case MOpcode::StrFprBaseRegLsl:
            return true;
        default:
            return false;
    }
}

/// @brief Add @p rhs to @p lhs while rejecting signed overflow.
/// @param lhs Base FP-relative byte offset.
/// @param rhs Positive access width or adjacent-slot delta.
/// @param out Receives the sum when it is representable.
/// @return True when the addition was representable in int64_t.
[[nodiscard]] bool checkedOffsetAdd(int64_t lhs, int64_t rhs, int64_t &out) noexcept {
    if ((rhs > 0 && lhs > std::numeric_limits<int64_t>::max() - rhs) ||
        (rhs < 0 && lhs < std::numeric_limits<int64_t>::min() - rhs))
        return false;
    out = lhs + rhs;
    return true;
}

} // namespace

/// @copydoc propagateCopies
std::size_t propagateCopies(std::vector<MInstr> &instrs, PeepholeStats &stats) {
    std::unordered_map<uint32_t, MOperand> copyOrigin;
    std::size_t propagated = 0;

    /// Erase every tracked copy whose recorded origin is being redefined.
    auto invalidateDependents = [&copyOrigin](uint32_t originKey) {
        std::vector<uint32_t> toErase;
        for (const auto &[key, origin] : copyOrigin) {
            if (regKey(origin) == originKey)
                toErase.push_back(key);
        }
        for (uint32_t key : toErase)
            copyOrigin.erase(key);
    };

    for (auto &instr : instrs) {
        if (isControlBoundary(instr.opc)) {
            copyOrigin.clear();
            continue;
        }

        // Shared copy-tracking for MovRR and FMovRR (same-class).
        /// Record a well-formed two-register copy, collapsing a safe existing
        /// origin chain and rewriting the copy source when that shortens it.
        auto trackCopy = [&](MInstr &mi) -> bool {
            const MOperand &dst = mi.ops[0];
            const MOperand &src = mi.ops[1];
            uint32_t dstKey = regKey(dst);

            invalidateDependents(dstKey);
            copyOrigin.erase(dstKey);

            MOperand origin = src;
            if (!isABIReg(src)) {
                auto it = copyOrigin.find(regKey(src));
                if (it != copyOrigin.end())
                    origin = it->second;
            }

            if (!samePhysReg(dst, origin)) {
                copyOrigin[dstKey] = origin;
                if (!samePhysReg(src, origin)) {
                    mi.ops[1] = origin;
                    ++propagated;
                }
            }
            return true;
        };

        // MovRR copy tracking
        if (instr.opc == MOpcode::MovRR && instr.ops.size() == 2 && isPhysReg(instr.ops[0]) &&
            isPhysReg(instr.ops[1])) {
            trackCopy(instr);
            continue;
        }

        // FMovRR copy tracking (same-class only)
        if (instr.opc == MOpcode::FMovRR && instr.ops.size() == 2 && isPhysReg(instr.ops[0]) &&
            isPhysReg(instr.ops[1]) && instr.ops[0].reg.cls == instr.ops[1].reg.cls) {
            trackCopy(instr);
            continue;
        }

        // Propagate uses BEFORE invalidating defs
        for (std::size_t i = 0; i < instr.ops.size(); ++i) {
            auto &op = instr.ops[i];
            if (!isPhysReg(op))
                continue;

            auto [isUse, isDef] = classifyOperand(instr, i);
            if (!isUse || isDef)
                continue;
            uint32_t key = regKey(op);
            auto it = copyOrigin.find(key);
            if (it == copyOrigin.end() || samePhysReg(op, it->second))
                continue;
            // ABI registers (x0-x7 / v0-v7) carry arguments and return values;
            // rewriting a use of one is safe only when the recorded origin is a
            // non-ABI register (calls already clear the copy map, so the origin
            // cannot have been clobbered by argument setup).
            if (isABIReg(op)) {
                static const bool noAbiCopyFwd =
                    std::getenv("ZANNA_NO_ABI_COPYFWD") != nullptr;
                if (noAbiCopyFwd || isABIReg(it->second))
                    continue;
            }
            op = it->second;
            ++propagated;
        }

        // Invalidate definitions
        for (std::size_t i = 0; i < instr.ops.size(); ++i) {
            const auto &op = instr.ops[i];
            if (!isPhysReg(op))
                continue;

            auto [isUse, isDef] = classifyOperand(instr, i);
            if (isDef) {
                uint32_t key = regKey(op);
                invalidateDependents(key);
                copyOrigin.erase(key);
            }
        }
    }

    stats.copiesPropagated += static_cast<int>(propagated);
    return propagated;
}

/// @copydoc removeDeadInstructions
std::size_t removeDeadInstructions(std::vector<MInstr> &instrs,
                                   PeepholeStats &stats,
                                   const std::vector<uint16_t> *carriedExitRegs) {
    if (instrs.empty())
        return 0;

    std::unordered_set<uint32_t> liveRegs;

    // Mark argument registers as live at block exit
    for (int i = 0; i <= 7; ++i) {
        liveRegs.insert((static_cast<uint32_t>(RegClass::GPR) << 16) |
                        static_cast<uint32_t>(PhysReg::X0) + i);
    }
    for (int i = 0; i <= 7; ++i) {
        liveRegs.insert((static_cast<uint32_t>(RegClass::FPR) << 16) |
                        static_cast<uint32_t>(PhysReg::V0) + i);
    }

    // Mark callee-saved GPRs (x19-x28) as live at block exit
    for (uint32_t r = static_cast<uint32_t>(PhysReg::X19); r <= static_cast<uint32_t>(PhysReg::X28);
         ++r) {
        liveRegs.insert((static_cast<uint32_t>(RegClass::GPR) << 16) | r);
    }

    // Registers carried across the block exit by the allocator have no
    // in-block use marking their liveness; treat them as live at exit.
    if (carriedExitRegs != nullptr) {
        for (uint16_t phys : *carriedExitRegs) {
            const RegClass cls = isGPR(static_cast<PhysReg>(phys)) ? RegClass::GPR : RegClass::FPR;
            liveRegs.insert((static_cast<uint32_t>(cls) << 16) | phys);
        }
    }

    std::vector<bool> toRemove(instrs.size(), false);
    std::size_t removed = 0;

    for (std::size_t i = instrs.size(); i > 0; --i) {
        const std::size_t idx = i - 1;
        const auto &instr = instrs[idx];

        if (hasSideEffects(instr)) {
            for (std::size_t j = 0; j < instr.ops.size(); ++j) {
                const auto &op = instr.ops[j];
                if (isPhysReg(op)) {
                    auto [isUse, isDef] = classifyOperand(instr, j);
                    if (isUse)
                        liveRegs.insert(regKey(op));
                }
            }
            continue;
        }

        auto defReg = getDefinedReg(instr);
        if (defReg) {
            uint32_t key = regKey(*defReg);
            if (liveRegs.find(key) == liveRegs.end()) {
                toRemove[idx] = true;
                ++removed;
                continue;
            }

            liveRegs.erase(key);
        }

        for (std::size_t j = 0; j < instr.ops.size(); ++j) {
            const auto &op = instr.ops[j];
            if (isPhysReg(op)) {
                auto [isUse, isDef] = classifyOperand(instr, j);
                if (isUse)
                    liveRegs.insert(regKey(op));
            }
        }
    }

    if (removed > 0) {
        removeMarkedInstructions(instrs, toRemove);
        stats.deadInstructionsRemoved += static_cast<int>(removed);
    }

    return removed;
}

namespace {

/// @brief Encode a physical register and its register class as a liveness key.
/// @param reg Physical GPR or FPR to encode.
/// @return Packed key compatible with @ref regKey.
[[nodiscard]] uint32_t physRegKey(PhysReg reg) noexcept {
    const RegClass cls = isGPR(reg) ? RegClass::GPR : RegClass::FPR;
    return (static_cast<uint32_t>(cls) << 16) | static_cast<uint32_t>(reg);
}

/// @brief Compact live-register set covering the complete AArch64 physical file.
///
/// GPR/SP identifiers occupy the low 32 bits and V0--V31 occupy the high
/// 32 bits. Invalid or unrecognized packed keys are ignored.
struct RegSet {
    /// One bit per tracked physical register.
    uint64_t bits{0};

    /// @brief Add an already-mapped bit position.
    /// @param bit Bit index in `[0, 63]`.
    void insertBit(unsigned bit) noexcept {
        bits |= (uint64_t{1} << bit);
    }

    /// @brief Add the physical register represented by a packed liveness key.
    /// @param key Register-class and physical-identifier key.
    void insertKey(uint32_t key) noexcept {
        const uint32_t cls = key >> 16;
        const uint32_t phys = key & 0xFFFFu;
        if (cls == static_cast<uint32_t>(RegClass::GPR)) {
            if (phys <= static_cast<uint32_t>(PhysReg::SP))
                insertBit(phys);
            return;
        }
        const uint32_t v0 = static_cast<uint32_t>(PhysReg::V0);
        if (phys >= v0 && phys <= static_cast<uint32_t>(PhysReg::V31))
            insertBit(32u + (phys - v0));
    }

    /// @brief Remove the physical register represented by a packed key.
    /// @param key Register-class and physical-identifier key.
    [[maybe_unused]] void eraseKey(uint32_t key) noexcept {
        const uint32_t cls = key >> 16;
        const uint32_t phys = key & 0xFFFFu;
        unsigned bit = 64;
        if (cls == static_cast<uint32_t>(RegClass::GPR)) {
            if (phys <= static_cast<uint32_t>(PhysReg::SP))
                bit = phys;
        } else {
            const uint32_t v0 = static_cast<uint32_t>(PhysReg::V0);
            if (phys >= v0 && phys <= static_cast<uint32_t>(PhysReg::V31))
                bit = 32u + (phys - v0);
        }
        if (bit < 64)
            bits &= ~(uint64_t{1} << bit);
    }

    /// @brief Test membership of the physical register represented by a key.
    /// @param key Register-class and physical-identifier key.
    /// @return `true` when the mapped register bit is set.
    [[maybe_unused]] [[nodiscard]] bool containsKey(uint32_t key) const noexcept {
        const uint32_t cls = key >> 16;
        const uint32_t phys = key & 0xFFFFu;
        unsigned bit = 64;
        if (cls == static_cast<uint32_t>(RegClass::GPR)) {
            if (phys <= static_cast<uint32_t>(PhysReg::SP))
                bit = phys;
        } else {
            const uint32_t v0 = static_cast<uint32_t>(PhysReg::V0);
            if (phys >= v0 && phys <= static_cast<uint32_t>(PhysReg::V31))
                bit = 32u + (phys - v0);
        }
        return bit < 64 && (bits & (uint64_t{1} << bit)) != 0;
    }

    /// @brief Test whether the set contains no registers.
    /// @return `true` when every bit is clear.
    [[nodiscard]] bool empty() const noexcept {
        return bits == 0;
    }
};

/// @brief `RegSet` overload of @ref addTargetExitLive for the bitset live-set path.
/// @param target Target ABI providing return + callee-saved register sets.
/// @param regs   RegSet to which the live-at-exit register keys are added.
void addTargetExitLive(const TargetInfo &target, RegSet &regs) {
    regs.insertKey(physRegKey(target.intReturnReg));
    regs.insertKey(physRegKey(target.f64ReturnReg));
    regs.insertKey(physRegKey(PhysReg::SP));
    for (PhysReg reg : target.calleeSavedGPR)
        regs.insertKey(physRegKey(reg));
    for (PhysReg reg : target.calleeSavedFPR)
        regs.insertKey(physRegKey(reg));
}

/// @brief `RegSet` overload of @ref addCallImplicitUses.
/// @param target Target ABI providing the arg-register order.
/// @param uses   RegSet to which the implicit-use register keys are added.
void addCallImplicitUses(const TargetInfo &target, RegSet &uses) {
    for (PhysReg reg : target.intArgOrder)
        uses.insertKey(physRegKey(reg));
    for (PhysReg reg : target.f64ArgOrder)
        uses.insertKey(physRegKey(reg));
    uses.insertKey(physRegKey(PhysReg::SP));
}

/// @brief `RegSet` overload of @ref addCallClobbers.
/// @param target Target ABI providing the caller-saved sets.
/// @param defs   RegSet to which the clobbered register keys are added.
void addCallClobbers(const TargetInfo &target, RegSet &defs) {
    for (PhysReg reg : target.callerSavedGPR)
        defs.insertKey(physRegKey(reg));
    for (PhysReg reg : target.callerSavedFPR)
        defs.insertKey(physRegKey(reg));
}

/// @brief `RegSet` overload of @ref collectUsesDefs for the bitset live-set path.
/// @param instr  Machine instruction whose live-set contribution is computed.
/// @param target Target ABI for call-implicit handling.
/// @param uses   RegSet receiving the use register keys.
/// @param defs   RegSet receiving the def register keys.
void collectUsesDefs(const MInstr &instr, const TargetInfo &target, RegSet &uses, RegSet &defs) {
    for (std::size_t idx = 0; idx < instr.ops.size(); ++idx) {
        const auto [isUse, isDef] = classifyOperand(instr, idx);
        const auto &op = instr.ops[idx];
        if (!isPhysReg(op))
            continue;
        const uint32_t key = regKey(op);
        if (isUse)
            uses.insertKey(key);
        if (isDef)
            defs.insertKey(key);
    }

    if (instr.opc == MOpcode::Bl || instr.opc == MOpcode::Blr) {
        addCallImplicitUses(target, uses);
        addCallClobbers(target, defs);
    }

    // A Ret reads the function's return value from the ABI return registers even though
    // the instruction carries no explicit operand. Mark them used so the chain computing
    // the return value is not treated as dead when the Ret's block also has other
    // successors (e.g. idx.chk's bounds-trap branches that precede the final Ret),
    // because then the block's live-out is taken from successors that omit x0/v0.
    if (instr.opc == MOpcode::Ret) {
        uses.insertKey(physRegKey(target.intReturnReg));
        uses.insertKey(physRegKey(target.f64ReturnReg));
    }
}

/// @brief Append the block index for @p label to @p succs if not already present.
/// @details Looks @p label up in the label-to-index map and appends the
///          corresponding block index; duplicates are skipped. Used while
///          building per-block successor lists for the liveness CFG walk.
/// @param succs        Successor list being built (modified in place).
/// @param labelToIndex Pre-built map from block name to block index.
/// @param label        Branch target label to add as a successor.
/// @return Nothing. Unknown labels and duplicate successors leave @p succs unchanged.
void addUniqueSucc(std::vector<std::size_t> &succs,
                   const std::unordered_map<std::string, std::size_t> &labelToIndex,
                   const std::string &label) {
    const auto it = labelToIndex.find(label);
    if (it == labelToIndex.end())
        return;
    if (std::find(succs.begin(), succs.end(), it->second) == succs.end())
        succs.push_back(it->second);
}

/// @brief Test whether @p instr is a conditional branch (`B.cond`/`CBZ`/`CBNZ`/`TBZ`/`TBNZ`).
/// @param instr Machine instruction to classify.
/// @return True if @p instr's opcode is one of the conditional-branch forms.
[[nodiscard]] bool isConditionalBranch(const MInstr &instr) noexcept {
    return instr.opc == MOpcode::BCond || instr.opc == MOpcode::Cbz ||
           instr.opc == MOpcode::Cbnz || instr.opc == MOpcode::Tbz || instr.opc == MOpcode::Tbnz;
}

/// @brief Test whether @p opcode writes the NZCV flags.
/// @details Used by DCE to refuse to delete instructions whose only observable
///          side effect is the flag write that a downstream conditional branch
///          will consume. Covers the comparison family and the flag-setting
///          arithmetic variants (`ADDS`/`SUBS`/`ANDS`).
/// @param opcode Opcode to classify.
/// @return True if @p opcode sets the NZCV flags.
[[nodiscard]] bool setsFlagsForDCE(MOpcode opcode) noexcept {
    switch (opcode) {
        case MOpcode::CmpRR:
        case MOpcode::CmpRI:
        case MOpcode::TstRR:
        case MOpcode::FCmpRR:
        case MOpcode::AddsRRR:
        case MOpcode::SubsRRR:
        case MOpcode::AddsRI:
        case MOpcode::SubsRI:
            return true;
        default:
            return false;
    }
}

/// @brief Build the successor list used by whole-function physical liveness.
///
/// Conditional branch targets are collected wherever they occur in a block.
/// The final instruction determines direct-branch, jump-table, return, or
/// no-return-call behavior; all other endings retain layout fallthrough.
///
/// @param fn Function whose block labels and terminators define the CFG.
/// @return One deduplicated successor-index vector per basic block.
[[nodiscard]] std::vector<std::vector<std::size_t>> buildSuccessors(const MFunction &fn) {
    std::unordered_map<std::string, std::size_t> labelToIndex;
    for (std::size_t i = 0; i < fn.blocks.size(); ++i)
        labelToIndex.emplace(fn.blocks[i].name, i);

    std::vector<std::vector<std::size_t>> succs(fn.blocks.size());
    for (std::size_t bi = 0; bi < fn.blocks.size(); ++bi) {
        /// Add the next layout block as this block's implicit fallthrough.
        const auto addFallthrough = [&]() {
            if (bi + 1 < fn.blocks.size())
                succs[bi].push_back(bi + 1);
        };

        const auto &instrs = fn.blocks[bi].instrs;
        if (instrs.empty()) {
            addFallthrough();
            continue;
        }

        for (const auto &instr : instrs) {
            if (isConditionalBranch(instr) && instr.ops.size() >= 2 &&
                instr.ops[1].kind == MOperand::Kind::Label)
                addUniqueSucc(succs[bi], labelToIndex, instr.ops[1].label);
        }

        const auto &last = instrs.back();
        if (last.opc == MOpcode::Br && !last.ops.empty() &&
            last.ops[0].kind == MOperand::Kind::Label)
            addUniqueSucc(succs[bi], labelToIndex, last.ops[0].label);
        else if (last.opc == MOpcode::JumpTable) {
            // Case labels start at operand 2; no fallthrough.
            for (std::size_t k = 2; k < last.ops.size(); ++k) {
                if (last.ops[k].kind == MOperand::Kind::Label)
                    addUniqueSucc(succs[bi], labelToIndex, last.ops[k].label);
            }
        } else if (last.opc != MOpcode::Ret && !isNoReturnCall(last))
            addFallthrough();
    }
    return succs;
}

} // namespace

/// @copydoc removeDeadInstructionsCFG
std::size_t removeDeadInstructionsCFG(MFunction &fn,
                                      PeepholeStats &stats,
                                      const TargetInfo &target) {
    if (fn.blocks.empty())
        return 0;

    const auto successors = buildSuccessors(fn);
    const std::size_t blockCount = fn.blocks.size();

    std::vector<RegSet> gen(blockCount);
    std::vector<RegSet> kill(blockCount);
    std::vector<RegSet> liveIn(blockCount);
    std::vector<RegSet> liveOut(blockCount);

    for (std::size_t bi = 0; bi < blockCount; ++bi) {
        for (const auto &instr : fn.blocks[bi].instrs) {
            RegSet uses;
            RegSet defs;
            collectUsesDefs(instr, target, uses, defs);

            gen[bi].bits |= uses.bits & ~kill[bi].bits;
            kill[bi].bits |= defs.bits;
        }
    }

    bool changed = true;
    while (changed) {
        changed = false;
        for (std::size_t bi = blockCount; bi-- > 0;) {
            RegSet newOut;
            if (successors[bi].empty()) {
                addTargetExitLive(target, newOut);
            } else {
                for (std::size_t succ : successors[bi])
                    newOut.bits |= liveIn[succ].bits;
            }

            RegSet newIn;
            newIn.bits = gen[bi].bits | (newOut.bits & ~kill[bi].bits);

            if (newOut.bits != liveOut[bi].bits || newIn.bits != liveIn[bi].bits) {
                liveOut[bi] = newOut;
                liveIn[bi] = newIn;
                changed = true;
            }
        }
    }

    std::size_t removed = 0;
    for (std::size_t bi = 0; bi < blockCount; ++bi) {
        auto &instrs = fn.blocks[bi].instrs;
        if (instrs.empty())
            continue;

        RegSet live = liveOut[bi];
        std::vector<bool> toRemove(instrs.size(), false);

        for (std::size_t i = instrs.size(); i-- > 0;) {
            const auto &instr = instrs[i];
            RegSet uses;
            RegSet defs;
            collectUsesDefs(instr, target, uses, defs);

            const bool defLive = (defs.bits & live.bits) != 0;

            if (!defs.empty() && !defLive && !hasSideEffects(instr) &&
                !setsFlagsForDCE(instr.opc)) {
                toRemove[i] = true;
                ++removed;
                continue;
            }

            live.bits &= ~defs.bits;
            live.bits |= uses.bits;
        }

        /// Test whether the block has at least one marked instruction.
        if (std::any_of(toRemove.begin(), toRemove.end(), [](bool value) { return value; }))
            removeMarkedInstructions(instrs, toRemove);
    }

    stats.deadInstructionsRemoved += static_cast<int>(removed);
    return removed;
}

/// @copydoc eliminateDeadFpStores
std::size_t eliminateDeadFpStores(std::vector<MInstr> &instrs, PeepholeStats &stats) {
    std::unordered_map<int64_t, std::size_t> lastStore;
    std::vector<bool> toRemove(instrs.size(), false);
    std::size_t removed = 0;

    for (std::size_t i = 0; i < instrs.size(); ++i) {
        const auto &instr = instrs[i];

        if ((instr.opc == MOpcode::StrRegFpImm || instr.opc == MOpcode::StrFprFpImm) &&
            instr.ops.size() >= 2 && instr.ops[1].kind == MOperand::Kind::Imm) {
            const int64_t offset = instr.ops[1].imm;
            auto it = lastStore.find(offset);
            if (it != lastStore.end()) {
                toRemove[it->second] = true;
                ++removed;
            }
            lastStore[offset] = i;
            continue;
        }

        if ((instr.opc == MOpcode::LdrRegFpImm || instr.opc == MOpcode::LdrFprFpImm) &&
            instr.ops.size() >= 2 && instr.ops[1].kind == MOperand::Kind::Imm) {
            lastStore.erase(instr.ops[1].imm);
            continue;
        }

        if ((instr.opc == MOpcode::StpRegFpImm || instr.opc == MOpcode::StpFprFpImm) &&
            instr.ops.size() >= 3 && instr.ops[2].kind == MOperand::Kind::Imm) {
            const int64_t off = instr.ops[2].imm;
            lastStore.erase(off);
            int64_t highOff = 0;
            if (!checkedOffsetAdd(off, 8, highOff)) {
                lastStore.clear();
                continue;
            }
            lastStore.erase(highOff);
            continue;
        }
        if ((instr.opc == MOpcode::LdpRegFpImm || instr.opc == MOpcode::LdpFprFpImm) &&
            instr.ops.size() >= 3 && instr.ops[2].kind == MOperand::Kind::Imm) {
            lastStore.erase(instr.ops[2].imm);
            int64_t highOff = 0;
            if (!checkedOffsetAdd(instr.ops[2].imm, 8, highOff)) {
                lastStore.clear();
                continue;
            }
            lastStore.erase(highOff);
            continue;
        }

        if (isControlBoundary(instr.opc) || isBaseRelativeMemory(instr.opc)) {
            lastStore.clear();
            continue;
        }
    }

    if (removed > 0) {
        removeMarkedInstructions(instrs, toRemove);
        stats.deadInstructionsRemoved += static_cast<int>(removed);
    }
    return removed;
}

/// @copydoc removeDeadFlagSetters
std::size_t removeDeadFlagSetters(std::vector<MInstr> &instrs, PeepholeStats &stats) {
    /// Test whether an instruction produces a new NZCV value.
    auto setsFlags = [](MOpcode opc) -> bool {
        switch (opc) {
            case MOpcode::CmpRR:
            case MOpcode::CmpRI:
            case MOpcode::TstRR:
            case MOpcode::FCmpRR:
            case MOpcode::AddsRRR:
            case MOpcode::SubsRRR:
            case MOpcode::AddsRI:
            case MOpcode::SubsRI:
                return true;
            default:
                return false;
        }
    };

    /// Test whether an instruction consumes the current NZCV value.
    auto readsFlags = [](MOpcode opc) -> bool {
        switch (opc) {
            case MOpcode::BCond:
            case MOpcode::Cset:
            case MOpcode::Csel:
            case MOpcode::FCsel:
                return true;
            default:
                return false;
        }
    };

    std::vector<bool> toRemove(instrs.size(), false);
    std::size_t removed = 0;

    for (std::size_t i = 0; i + 1 < instrs.size(); ++i) {
        if (!setsFlags(instrs[i].opc))
            continue;

        bool isDead = false;
        for (std::size_t j = i + 1; j < instrs.size(); ++j) {
            if (isControlBoundary(instrs[j].opc))
                break;
            if (readsFlags(instrs[j].opc))
                break;
            if (setsFlags(instrs[j].opc)) {
                if (instrs[i].opc == MOpcode::CmpRI || instrs[i].opc == MOpcode::CmpRR ||
                    instrs[i].opc == MOpcode::TstRR || instrs[i].opc == MOpcode::FCmpRR) {
                    isDead = true;
                }
                break;
            }
        }
        if (isDead) {
            toRemove[i] = true;
            ++removed;
        }
    }

    if (removed > 0) {
        removeMarkedInstructions(instrs, toRemove);
        stats.deadInstructionsRemoved += static_cast<int>(removed);
    }
    return removed;
}

/// @copydoc foldComputeIntoTarget
std::size_t foldComputeIntoTarget(std::vector<MInstr> &instrs, PeepholeStats &stats) {
    /// Test whether an opcode's explicit destination may be redirected safely.
    auto isSimpleALU = [](MOpcode opc) -> bool {
        switch (opc) {
            case MOpcode::AddRRR:
            case MOpcode::SubRRR:
            case MOpcode::AddRI:
            case MOpcode::SubRI:
            case MOpcode::AddsRRR:
            case MOpcode::SubsRRR:
            case MOpcode::AddsRI:
            case MOpcode::SubsRI:
            case MOpcode::MulRRR:
            case MOpcode::SmulhRRR:
            case MOpcode::UmulhRRR:
            case MOpcode::AndRRR:
            case MOpcode::OrrRRR:
            case MOpcode::EorRRR:
            case MOpcode::LslRI:
            case MOpcode::LsrRI:
            case MOpcode::AsrRI:
                return true;
            default:
                return false;
        }
    };

    std::size_t folded = 0;
    for (std::size_t i = 0; i + 1 < instrs.size(); ++i) {
        if (!isSimpleALU(instrs[i].opc))
            continue;
        if (instrs[i].ops.empty() || !isPhysReg(instrs[i].ops[0]))
            continue;

        const MOperand aluDst = instrs[i].ops[0];

        const std::size_t movIdx = i + 1;
        if (instrs[movIdx].opc != MOpcode::MovRR)
            continue;
        if (instrs[movIdx].ops.size() != 2)
            continue;
        if (!samePhysReg(instrs[movIdx].ops[1], aluDst))
            continue;
        if (!isPhysReg(instrs[movIdx].ops[0]))
            continue;

        const MOperand movDst = instrs[movIdx].ops[0];

        bool interveningUse = false;
        for (std::size_t k = i + 1; k < movIdx; ++k) {
            if (usesReg(instrs[k], movDst)) {
                interveningUse = true;
                break;
            }
        }
        if (interveningUse)
            continue;

        bool aluDstDead = true;
        for (std::size_t j = movIdx + 1; j < instrs.size(); ++j) {
            if (isControlBoundary(instrs[j].opc)) {
                // A conditional branch (b.cc trap guard, cbz/cbnz) falls
                // through within this instruction list, so instructions after
                // it may still read the ALU destination — the boundary proves
                // nothing. Only give up the scan for unconditional transfers.
                const bool conditional =
                    instrs[j].opc == MOpcode::BCond || instrs[j].opc == MOpcode::Cbz ||
                    instrs[j].opc == MOpcode::Cbnz || instrs[j].opc == MOpcode::Tbz ||
                    instrs[j].opc == MOpcode::Tbnz;
                if (!conditional)
                    break;
                if (usesReg(instrs[j], aluDst)) {
                    aluDstDead = false;
                    break;
                }
                continue;
            }
            if (usesReg(instrs[j], aluDst)) {
                aluDstDead = false;
                break;
            }
            if (definesReg(instrs[j], aluDst))
                break;
        }
        if (!aluDstDead)
            continue;

        instrs[i].ops[0] = movDst;
        instrs.erase(instrs.begin() + static_cast<std::ptrdiff_t>(movIdx));
        ++folded;
        ++stats.deadInstructionsRemoved;

        if (i > 0)
            --i;
    }
    return folded;
}

/// @copydoc eliminateDeadFpStoresCrossBlock
std::size_t eliminateDeadFpStoresCrossBlock(MFunction &fn, PeepholeStats &stats) {
    // Only compiler-created spill slots are safe for whole-function dead-store
    // removal. FP-relative locals/allocas can be observed through address-derived
    // base-register loads even when no direct LdrRegFpImm remains.
    std::unordered_set<int64_t> eligibleOffsets;
    for (const auto &slot : fn.frame.spills) {
        for (int byte = 0; byte < slot.size; byte += 8)
            eligibleOffsets.insert(static_cast<int64_t>(slot.offset + byte));
    }
    if (eligibleOffsets.empty())
        return 0;

    // Step 1: Collect all FP offsets that are LOADED anywhere in the function.
    std::unordered_set<int64_t> loadedOffsets;
    for (const auto &bb : fn.blocks) {
        for (const auto &mi : bb.instrs) {
            if ((mi.opc == MOpcode::LdrRegFpImm || mi.opc == MOpcode::LdrFprFpImm) &&
                mi.ops.size() >= 2 && mi.ops[1].kind == MOperand::Kind::Imm) {
                loadedOffsets.insert(mi.ops[1].imm);
            }
            if ((mi.opc == MOpcode::LdpRegFpImm || mi.opc == MOpcode::LdpFprFpImm) &&
                mi.ops.size() >= 3 && mi.ops[2].kind == MOperand::Kind::Imm) {
                loadedOffsets.insert(mi.ops[2].imm);
                loadedOffsets.insert(mi.ops[2].imm + 8);
            }
        }
    }

    // Step 2: Remove stores to offsets that are never loaded. Pair stores
    // (stp) qualify only when BOTH covered slots are eligible and unloaded —
    // spill stores frequently reach this pass already merged into pairs.
    std::size_t removed = 0;
    /// Test whether one eight-byte spill-slot offset is eligible and unread.
    const auto deadSingle = [&](int64_t off) {
        return eligibleOffsets.count(off) != 0 && loadedOffsets.count(off) == 0;
    };
    for (auto &bb : fn.blocks) {
        bb.instrs.erase(std::remove_if(bb.instrs.begin(),
                                       bb.instrs.end(),
                                       /// Select scalar and paired spill stores
                                       /// whose complete covered range is dead.
                                       [&](const MInstr &mi) {
                                           if ((mi.opc == MOpcode::StrRegFpImm ||
                                                mi.opc == MOpcode::StrFprFpImm) &&
                                               mi.ops.size() >= 2 &&
                                               mi.ops[1].kind == MOperand::Kind::Imm &&
                                               deadSingle(mi.ops[1].imm)) {
                                               ++removed;
                                               return true;
                                           }
                                           if ((mi.opc == MOpcode::StpRegFpImm ||
                                                mi.opc == MOpcode::StpFprFpImm) &&
                                               mi.ops.size() >= 3 &&
                                               mi.ops[2].kind == MOperand::Kind::Imm &&
                                               deadSingle(mi.ops[2].imm) &&
                                               deadSingle(mi.ops[2].imm + 8)) {
                                               ++removed;
                                               return true;
                                           }
                                           return false;
                                       }),
                        bb.instrs.end());
    }
    stats.deadInstructionsRemoved += static_cast<int>(removed);
    return removed;
}

} // namespace zanna::codegen::aarch64::peephole
