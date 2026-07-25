//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/ra/Allocator.cpp
// Purpose: Core linear-scan register allocator implementation for AArch64.
//          Integrates shared CFG liveness with slot pinning, spill logic,
//          register materialization, call handling, and per-block allocation.
//
// Key invariants:
//   - All virtual registers are resolved to physical registers after run().
//   - Spill/reload code is inserted in-place via prefix/suffix vectors.
//   - Cross-block persistence restores predecessor spill state before
//     re-adopting registers from single-predecessor exit states.
//
// Ownership/Lifetime:
//   - See Allocator.hpp.
//
// Links: codegen/aarch64/ra/Allocator.hpp,
//        codegen/aarch64/ra/InstrBuilders.hpp,
//        codegen/aarch64/ra/OpcodeClassify.hpp,
//        codegen/aarch64/ra/OperandRoles.hpp
//
//===----------------------------------------------------------------------===//

#include "Allocator.hpp"

#include "InstrBuilders.hpp"
#include "OpcodeClassify.hpp"
#include "OperandRoles.hpp"
#include "RegClassify.hpp"
#include "codegen/common/ra/GlobalPinning.hpp"
#include "il/runtime/RuntimeNameMap.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <climits>
#include <cstdlib>
#include <initializer_list>
#include <stdexcept>
#include <utility>

/// @file
/// @brief Implements stateful linear allocation and spill rewriting for AArch64 MIR.

namespace zanna::codegen::aarch64::ra {

namespace {

/// @brief Return true if @p pr is one of the reserved emergency scratch registers
///        (kScratchGPR/GPR2/GPR3 or kScratchFPR/FPR2) that must not be released to the pool.
/// @param pr Physical register to classify.
/// @return `true` when @p pr belongs to the permanently reserved emergency set.
bool isReservedScratch(PhysReg pr) {
    return pr == kScratchGPR || pr == kScratchGPR2 || pr == kScratchGPR3 || pr == kScratchFPR ||
           pr == kScratchFPR2;
}

/// @brief Test whether a scratch register was already acquired for this instruction.
/// @param scratch Current instruction's acquired scratch list.
/// @param pr Physical register to query.
/// @return `true` when @p pr already appears in @p scratch.
bool scratchAlreadyUsed(const std::vector<PhysReg> &scratch, PhysReg pr) {
    return std::find(scratch.begin(), scratch.end(), pr) != scratch.end();
}

/// @brief Rewrite same-instruction virtual uses to an already-resident physical register.
/// @details Physical call-argument marshalling often has the shape `mov xN, v`.
///          If `v` already lives in `xN`, clobber eviction must not discard the
///          value before the source operand is materialized. Rewriting the use
///          keeps the current instruction correct while allowing the old vreg
///          state to be retired or spilled.
/// @param[in,out] ins Instruction whose matching virtual use operands are rewritten.
/// @param vreg Virtual-register identifier to replace.
/// @param cls Required register class of a matching operand.
/// @param phys Resident physical register substituted for the virtual use.
/// @return `true` when at least one use operand was rewritten.
bool rewriteVirtualUseToResidentPhys(MInstr &ins, uint16_t vreg, RegClass cls, PhysReg phys) {
    bool rewritten = false;
    for (std::size_t opIdx = 0; opIdx < ins.ops.size(); ++opIdx) {
        auto &operand = ins.ops[opIdx];
        if (operand.kind != MOperand::Kind::Reg || operand.reg.isPhys)
            continue;
        if (operand.reg.cls != cls || operand.reg.idOrPhys != vreg)
            continue;
        const auto [isUse, isDef] = operandRoles(ins, opIdx);
        (void)isDef;
        if (!isUse)
            continue;
        operand = MOperand::regOp(phys);
        rewritten = true;
    }
    return rewritten;
}

/// @brief Pick the first available scratch register from @p candidates.
/// @details If @p canReuseDefScratch is true, prefers a register already in @p scratch
///          (safe when the use and def are the same operand slot). Otherwise picks one
///          not yet in @p scratch. Registers listed in @p blocked are never selected,
///          which prevents emergency scratch reloads from clobbering explicit physical
///          operands on the current instruction. Throws if all candidates conflict.
///
/// @par Reserved-scratch invariant
/// The candidate count per class is sized to the maximum number of *simultaneous*
/// register operands any single MIR instruction of that class can present:
///   - GPR: 3 (kScratchGPR/2/3) — covers the 4-operand `MAddRRRR`/`MSubRRRR`
///     (rd is def-only, so 3 distinct use reloads + the def reusing one).
///   - FPR: 2 (kScratchFPR/2) — covers the widest FP op, the 3-operand
///     `FAddRRR`/`FSubRRR`/`FMulRRR`/`FDivRRR` (def-only rd reuses a use slot).
/// Because every current opcode honours this bound, the throw below is a guard
/// against a *future* opcode that violates it — e.g. a 3-source read-modify-write
/// FP op (`FMLA`: rd is use+def, defeating def-reuse) would present 3 simultaneous
/// FPR uses and require adding `kScratchFPR3`. Keep the scratch sets in sync with
/// the maximum-arity instruction of each class.
///
/// @param candidates Ordered reserved-register set for one register class.
/// @param canReuseDefScratch Whether an already acquired scratch may also serve
///                           this def-only operand.
/// @param scratch Scratch registers already acquired for the instruction.
/// @param blocked Explicit physical operands that candidates must not clobber.
/// @return Selected reserved scratch register.
/// @throws std::runtime_error when no candidate satisfies the constraints.
PhysReg chooseFromScratchSet(std::initializer_list<PhysReg> candidates,
                             bool canReuseDefScratch,
                             const std::vector<PhysReg> &scratch,
                             const std::unordered_set<PhysReg> &blocked) {
    if (canReuseDefScratch) {
        for (PhysReg candidate : candidates) {
            if (blocked.find(candidate) == blocked.end() &&
                scratchAlreadyUsed(scratch, candidate)) {
                return candidate;
            }
        }
    }
    for (PhysReg candidate : candidates) {
        if (blocked.find(candidate) == blocked.end() && !scratchAlreadyUsed(scratch, candidate)) {
            return candidate;
        }
    }
    throw std::runtime_error(
        "AArch64 register allocator: reserved emergency scratch exhausted for a spilled-operand "
        "reload (" +
        std::to_string(candidates.size()) + " scratch reg(s), " + std::to_string(scratch.size()) +
        " already taken this instruction). The instruction presents more simultaneous "
        "spilled register operands than the class reserves — add another kScratch* register for "
        "this class (see the reserved-scratch invariant on chooseFromScratchSet).");
}

/// @brief Select an emergency scratch register when the normal pool is exhausted.
/// @param fprClass         True to pick an FPR scratch; false for GPR.
/// @param canReuseDefScratch True when the spilled operand is def-only (safe to reuse).
/// @param scratch          Registers already acquired this instruction.
/// @param blocked          Explicit physical operands that must not be clobbered.
/// @return A reserved scratch register suitable for the operand class.
/// @throws std::runtime_error when every class-specific candidate conflicts.
PhysReg chooseEmergencyScratch(bool fprClass,
                               bool canReuseDefScratch,
                               const std::vector<PhysReg> &scratch,
                               const std::unordered_set<PhysReg> &blocked) {
    if (fprClass)
        return chooseFromScratchSet(
            {kScratchFPR, kScratchFPR2}, canReuseDefScratch, scratch, blocked);
    return chooseFromScratchSet(
        {kScratchGPR, kScratchGPR2, kScratchGPR3}, canReuseDefScratch, scratch, blocked);
}

} // namespace

// =========================================================================
// Construction
// =========================================================================

/// @copydoc LinearAllocator::LinearAllocator
LinearAllocator::LinearAllocator(MFunction &fn, const TargetInfo &ti) : fn_(fn), ti_(ti), fb_(fn) {
    // Argument registers are allocatable, except those whose INCOMING value is
    // still consumed by the lowered code: a physical-register USE that is not
    // preceded by a def in the same block reads ABI state (entry parameters,
    // fast-path bodies), so handing that register to a vreg would clobber it.
    // The scan is conservative (function-wide exclusion) but cheap and safe.
    std::array<bool, 64> abiLiveIn{};
    for (const auto &bb : fn.blocks) {
        std::array<bool, 64> definedInBlock{};
        for (const auto &mi : bb.instrs) {
            for (std::size_t opIdx = 0; opIdx < mi.ops.size(); ++opIdx) {
                const auto &op = mi.ops[opIdx];
                if (op.kind != MOperand::Kind::Reg || !op.reg.isPhys)
                    continue;
                const auto ordinal = static_cast<std::size_t>(op.reg.idOrPhys);
                if (ordinal >= abiLiveIn.size())
                    continue;
                const auto [isUse, isDef] = operandRoles(mi, opIdx);
                if (isUse && !definedInBlock[ordinal] &&
                    isArgRegister(static_cast<PhysReg>(op.reg.idOrPhys), ti))
                    abiLiveIn[ordinal] = true;
                if (isDef)
                    definedInBlock[ordinal] = true;
            }
        }
    }

    pools_.build(ti, abiLiveIn);
    liveness_.run(fn);
}

// =========================================================================
// Entry point
// =========================================================================

/// @copydoc LinearAllocator::run
AllocationResult LinearAllocator::run() {
    blockExitStates_.resize(fn_.blocks.size());

    // Tier 1: pin the hottest phi/cross-block frame slots to callee-saved
    // registers so their loads and stores become register moves.
    // ZANNA_NO_GLOBAL_RA=1 disables the tier for triage.
    if (std::getenv("ZANNA_NO_GLOBAL_RA") == nullptr) {
        assignPinnedSlots();
    }

    for (std::size_t bi = 0; bi < fn_.blocks.size(); ++bi) {
        currentBlockIdx_ = bi;
        restoreFromPredecessor(bi);
        allocateBlock(fn_.blocks[bi]);
        releaseBlockState();
    }

    fb_.finalize();
    recordCalleeSavedUsage();

    return AllocationResult{};
}

/// @copydoc LinearAllocator::assignPinnedSlots
void LinearAllocator::assignPinnedSlots() {
    const std::size_t blockCount = fn_.blocks.size();
    if (blockCount == 0) {
        return;
    }

    // Loop depths weight slot traffic (a slot touched in a nested loop beats
    // one touched at function scope).
    std::vector<std::vector<std::size_t>> succs(blockCount);
    for (std::size_t bi = 0; bi < blockCount; ++bi) {
        succs[bi] = liveness_.successors(bi);
    }
    const std::vector<unsigned> loopDepth = zanna::codegen::ra::computeLoopDepths(succs);

    // Calls inside a loop defeat pinning: every callee activation (and every
    // iteration around the call) pays the enlarged prologue/epilogue for the
    // pinned callee-saved registers, which costs more than the slot traffic
    // it saves. Calls outside loops (entry-block runtime setup) are fine.
    //
    // Exception frames defeat pinning entirely: rt_native_eh_push is a
    // setjmp-style snapshot, so a throw restores callee-saved registers to
    // their push-time values while frame memory keeps its current values.
    // Values pinned to registers after the push would be silently rolled
    // back on the exception path.
    for (std::size_t bi = 0; bi < blockCount; ++bi) {
        const bool inLoop = loopDepth[bi] != 0;
        for (const auto &ins : fn_.blocks[bi].instrs) {
            if (!isCall(ins.opc)) {
                continue;
            }
            if (inLoop) {
                return;
            }
            for (const auto &op : ins.ops) {
                if (op.kind == MOperand::Kind::Label &&
                    op.label.find("rt_native_eh_push") != std::string::npos) {
                    return;
                }
            }
        }
    }

    // Gather per-offset access statistics for the slot-shaped opcodes. Any
    // candidate offset that also appears as an immediate anywhere else
    // (address-of via AddFpImm, unrelated constants — conservatively assume
    // the worst) is disqualified: pinning requires that every access to the
    // slot flows through the rewritable forms below. Slots never written by
    // this function (caller-populated stack params) are also excluded.
    /// @brief Weighted access evidence for one frame-pointer-relative slot.
    struct SlotStats {
        /// Sum of loop-depth-weighted supported accesses.
        double weight{0.0};

        /// Whether the function supplies the slot's value.
        bool written{false};

        /// Whether the most recently classified access is FPR-class.
        bool fpr{false};
    };

    std::unordered_map<int, SlotStats> stats;
    std::unordered_set<int> disqualified;
    // Taking a frame address (AddFpImm at offset B) lets derived pointers
    // reach every slot between the object's base and fp ([B, 0)): aggregate
    // fields and array elements are addressed at non-negative deltas from
    // their base, and those accesses bypass the rewritable FpImm forms. Any
    // candidate at or above the lowest address-taken offset is poisoned.
    int minAddrTakenOff = INT_MAX;
    for (std::size_t bi = 0; bi < blockCount; ++bi) {
        double blockWeight = 1.0;
        for (unsigned d = 0; d < loopDepth[bi]; ++d) {
            blockWeight *= 10.0;
        }
        for (const auto &ins : fn_.blocks[bi].instrs) {
            const bool isSlotAccess =
                (ins.opc == MOpcode::LdrRegFpImm || ins.opc == MOpcode::StrRegFpImm ||
                 ins.opc == MOpcode::LdrFprFpImm || ins.opc == MOpcode::StrFprFpImm ||
                 ins.opc == MOpcode::PhiStoreGPR || ins.opc == MOpcode::PhiStoreFPR) &&
                ins.ops.size() >= 2 && ins.ops[0].kind == MOperand::Kind::Reg &&
                ins.ops[1].kind == MOperand::Kind::Imm;
            if (isSlotAccess) {
                const int off = static_cast<int>(ins.ops[1].imm);
                auto &entry = stats[off];
                entry.weight += blockWeight;
                const bool isStore =
                    ins.opc == MOpcode::StrRegFpImm || ins.opc == MOpcode::StrFprFpImm ||
                    ins.opc == MOpcode::PhiStoreGPR || ins.opc == MOpcode::PhiStoreFPR;
                if (isStore) {
                    entry.written = true;
                }
                entry.fpr = ins.opc == MOpcode::LdrFprFpImm || ins.opc == MOpcode::StrFprFpImm ||
                            ins.opc == MOpcode::PhiStoreFPR;
                continue;
            }
            // Only other FP-relative forms disqualify an offset: sub-word
            // accesses, address-of (AddFpImm), and pair forms reach the slot
            // in ways the rewriter cannot redirect to a register.
            switch (ins.opc) {
                case MOpcode::Ldr8RegFpImm:
                case MOpcode::Str8RegFpImm:
                case MOpcode::Ldr16RegFpImm:
                case MOpcode::Str16RegFpImm:
                case MOpcode::Ldr32RegFpImm:
                case MOpcode::Str32RegFpImm:
                    for (const auto &op : ins.ops) {
                        if (op.kind == MOperand::Kind::Imm) {
                            disqualified.insert(static_cast<int>(op.imm));
                        }
                    }
                    break;
                case MOpcode::AddFpImm:
                    for (const auto &op : ins.ops) {
                        if (op.kind == MOperand::Kind::Imm) {
                            const int base = static_cast<int>(op.imm);
                            disqualified.insert(base);
                            minAddrTakenOff = std::min(minAddrTakenOff, base);
                        }
                    }
                    break;
                case MOpcode::LdpRegFpImm:
                case MOpcode::StpRegFpImm:
                case MOpcode::LdpFprFpImm:
                case MOpcode::StpFprFpImm:
                    for (const auto &op : ins.ops) {
                        if (op.kind == MOperand::Kind::Imm) {
                            disqualified.insert(static_cast<int>(op.imm));
                            disqualified.insert(static_cast<int>(op.imm) + 8);
                        }
                    }
                    break;
                default:
                    break;
            }
        }
    }

    // Only loop-resident slots are worth a callee-saved register: pinning a
    // straight-line slot trades two memory ops for a bigger prologue and a
    // burned register. Weight >= 10 requires at least one access at loop
    // depth >= 1.
    constexpr double kMinPinWeight = 10.0;
    std::vector<std::pair<int, const SlotStats *>> ranked;
    for (const auto &[off, entry] : stats) {
        if (off == 0 || !entry.written || entry.weight < kMinPinWeight || disqualified.count(off) ||
            off >= minAddrTakenOff) {
            continue;
        }
        ranked.emplace_back(off, &entry);
    }
    if (ranked.empty()) {
        return;
    }
    /// Rank hotter slots first with frame offset as a deterministic tie break.
    std::sort(ranked.begin(), ranked.end(), [](const auto &a, const auto &b) {
        if (a.second->weight != b.second->weight)
            return a.second->weight > b.second->weight;
        return a.first < b.first; // deterministic tie-break
    });

    // Pinnable pools: callee-saved registers currently present in the free
    // pools (never the reserved scratch registers, which are not pool
    // members). Each pinned register is removed from its pool for the whole
    // function and recorded as used so the prologue saves it.
    /// Remove the first target-designated callee-saved register from a pool.
    ///
    /// @param[in,out] pool Class-specific free register deque.
    /// @param calleeSaved Target's ordered saved-register set for the class.
    /// @param off Candidate slot offset, retained for diagnostic/debug symmetry.
    /// @param[out] outReg Selected physical register.
    /// @return `true` when a suitable pool member was found.
    auto pinFrom = [&](std::deque<PhysReg> &pool,
                       const std::vector<PhysReg> &calleeSaved,
                       int off,
                       PhysReg &outReg) {
        for (auto it = pool.begin(); it != pool.end(); ++it) {
            if (std::find(calleeSaved.begin(), calleeSaved.end(), *it) == calleeSaved.end()) {
                continue;
            }
            outReg = *it;
            pool.erase(it);
            (void)off;
            return true;
        }
        return false;
    };

    for (const auto &[off, entry] : ranked) {
        PhysReg reg{};
        if (entry->fpr) {
            if (!pinFrom(pools_.fprFree, ti_.calleeSavedFPR, off, reg)) {
                continue;
            }
            pinnedSlotFPR_.emplace(off, reg);
            pools_.calleeUsedFPR[static_cast<std::size_t>(reg)] = true;
        } else {
            if (!pinFrom(pools_.gprFree, ti_.calleeSavedGPR, off, reg)) {
                continue;
            }
            pinnedSlotGPR_.emplace(off, reg);
            pools_.calleeUsed[static_cast<std::size_t>(reg)] = true;
        }
    }

    if (std::getenv("ZANNA_DEBUG_PINSLOTS")) {
        std::fprintf(stderr, "[pin-slots] fn=%s\n", fn_.name.c_str());
        for (const auto &[off, entry] : ranked) {
            const bool pinnedG = pinnedSlotGPR_.count(off) != 0;
            const bool pinnedF = pinnedSlotFPR_.count(off) != 0;
            std::fprintf(stderr,
                         "  off=%d weight=%.0f fpr=%d pinned=%d\n",
                         off,
                         entry->weight,
                         entry->fpr ? 1 : 0,
                         (pinnedG || pinnedF) ? 1 : 0);
        }
        for (int off : disqualified) {
            if (stats.count(off))
                std::fprintf(stderr, "  off=%d DISQUALIFIED\n", off);
        }
    }
}

/// @copydoc LinearAllocator::rewritePinnedSlotAccess
bool LinearAllocator::rewritePinnedSlotAccess(MInstr &ins) {
    if (ins.ops.size() < 2 || ins.ops[1].kind != MOperand::Kind::Imm) {
        return false;
    }
    const int off = static_cast<int>(ins.ops[1].imm);

    switch (ins.opc) {
        case MOpcode::LdrRegFpImm: {
            auto it = pinnedSlotGPR_.find(off);
            if (it == pinnedSlotGPR_.end())
                return false;
            // ldr dst, [fp, #off]  ->  mov dst, pin
            ins.opc = MOpcode::MovRR;
            ins.ops[1] = MOperand::regOp(it->second);
            return true;
        }
        case MOpcode::StrRegFpImm:
        case MOpcode::PhiStoreGPR: {
            auto it = pinnedSlotGPR_.find(off);
            if (it == pinnedSlotGPR_.end())
                return false;
            // str src, [fp, #off]  ->  mov pin, src
            ins.opc = MOpcode::MovRR;
            ins.ops[1] = ins.ops[0];
            ins.ops[0] = MOperand::regOp(it->second);
            return true;
        }
        case MOpcode::LdrFprFpImm: {
            auto it = pinnedSlotFPR_.find(off);
            if (it == pinnedSlotFPR_.end())
                return false;
            ins.opc = MOpcode::FMovRR;
            ins.ops[1] = MOperand::regOp(it->second);
            return true;
        }
        case MOpcode::StrFprFpImm:
        case MOpcode::PhiStoreFPR: {
            auto it = pinnedSlotFPR_.find(off);
            if (it == pinnedSlotFPR_.end())
                return false;
            ins.opc = MOpcode::FMovRR;
            ins.ops[1] = ins.ops[0];
            ins.ops[0] = MOperand::regOp(it->second);
            return true;
        }
        default:
            return false;
    }
}

// =========================================================================
// CFG / liveness — delegated to LivenessAnalysis (uses shared solver)
// =========================================================================

// =========================================================================
// Cross-block register persistence
// =========================================================================

/// @copydoc LinearAllocator::restoreFromPredecessor
void LinearAllocator::restoreFromPredecessor(std::size_t bi) {
    const auto &preds = liveness_.predecessors(bi);
    if (preds.size() != 1)
        return;

    const std::size_t pred = preds[0];
    if (pred >= bi)
        return;

    const auto &es = blockExitStates_[pred];

    // Allocation of an earlier sibling successor may have consumed and then
    // retired these same live-ins. Re-establish the predecessor's canonical
    // spill state before restoring any register-carried values; allocator
    // state from whichever block happened to be visited last is not valid on
    // a different CFG edge.
    /// Reset each live-out state to the predecessor's canonical spill-backed form.
    const auto restoreSpills = [](auto &states, const auto &liveOut) {
        for (const uint16_t vid : liveOut) {
            auto it = states.find(vid);
            if (it == states.end())
                continue;
            auto &st = it->second;
            st.hasPhys = false;
            st.spilled = st.fpOffset != 0;
            st.dirty = false;
        }
    };
    restoreSpills(gprStates_, liveness_.liveOutGPR(pred));
    restoreSpills(fprStates_, liveness_.liveOutFPR(pred));

    // Restore in sorted vreg order so register-pool mutation order is
    // independent of hash-map iteration order (deterministic codegen).
    /// Return map keys in ascending order for deterministic pool mutation.
    const auto sortedKeys = [](const std::unordered_map<uint16_t, PhysReg> &map) {
        std::vector<uint16_t> keys;
        keys.reserve(map.size());
        for (const auto &kv : map)
            keys.push_back(kv.first);
        std::sort(keys.begin(), keys.end());
        return keys;
    };

    for (const uint16_t vid : sortedKeys(es.gpr)) {
        const PhysReg phys = es.gpr.at(vid);

        auto it = std::find(pools_.gprFree.begin(), pools_.gprFree.end(), phys);
        if (it == pools_.gprFree.end()) {
            auto &st = gprStates_[vid];
            st.hasPhys = false;
            st.spilled = true;
            continue;
        }
        pools_.gprFree.erase(it);

        auto &st = gprStates_[vid];
        st.hasPhys = true;
        st.phys = phys;
        st.spilled = false;
        st.dirty = false;
    }

    for (const uint16_t vid : sortedKeys(es.fpr)) {
        const PhysReg phys = es.fpr.at(vid);

        auto it = std::find(pools_.fprFree.begin(), pools_.fprFree.end(), phys);
        if (it == pools_.fprFree.end()) {
            auto &st = fprStates_[vid];
            st.hasPhys = false;
            st.spilled = true;
            continue;
        }
        pools_.fprFree.erase(it);

        auto &st = fprStates_[vid];
        st.hasPhys = true;
        st.phys = phys;
        st.spilled = false;
        st.dirty = false;
    }
}

// =========================================================================
// Next-use analysis
// =========================================================================

/// @copydoc LinearAllocator::computeNextUses
void LinearAllocator::computeNextUses(const MBasicBlock &bb) {
    usePositionsGPR_.clear();
    usePositionsFPR_.clear();
    callPositions_.clear();

    unsigned idx = 0;
    for (const auto &mi : bb.instrs) {
        if (isCall(mi.opc))
            callPositions_.push_back(idx);

        for (std::size_t opIdx = 0; opIdx < mi.ops.size(); ++opIdx) {
            const auto &op = mi.ops[opIdx];
            if (op.kind != MOperand::Kind::Reg || op.reg.isPhys)
                continue;
            const auto [isUse, isDef] = operandRoles(mi, opIdx);
            (void)isDef;
            if (!isUse)
                continue;
            if (op.reg.cls == RegClass::GPR)
                usePositionsGPR_[op.reg.idOrPhys].push_back(idx);
            else
                usePositionsFPR_[op.reg.idOrPhys].push_back(idx);
        }
        ++idx;
    }
}

/// @copydoc LinearAllocator::isProtectedOperand
bool LinearAllocator::isProtectedOperand(uint16_t vreg, RegClass cls) const {
    if (cls == RegClass::GPR)
        return protectedOperandGPR_.find(vreg) != protectedOperandGPR_.end();
    return protectedOperandFPR_.find(vreg) != protectedOperandFPR_.end();
}

/// @copydoc LinearAllocator::isProtectedPhys
bool LinearAllocator::isProtectedPhys(PhysReg phys, RegClass cls) const {
    if (cls == RegClass::GPR)
        return protectedPhysGPR_.find(phys) != protectedPhysGPR_.end();
    return protectedPhysFPR_.find(phys) != protectedPhysFPR_.end();
}

/// @copydoc LinearAllocator::isLiveOut
bool LinearAllocator::isLiveOut(uint16_t vreg, RegClass cls) const {
    if (cls == RegClass::GPR)
        return liveness_.liveOutGPR(currentBlockIdx_).contains(vreg);
    return liveness_.liveOutFPR(currentBlockIdx_).contains(vreg);
}

/// @copydoc LinearAllocator::nextUseAfterCall
bool LinearAllocator::nextUseAfterCall(uint16_t vreg, RegClass cls) const {
    const auto &map = (cls == RegClass::GPR) ? usePositionsGPR_ : usePositionsFPR_;
    auto it = map.find(vreg);
    if (it == map.end() || it->second.empty())
        return false;

    unsigned lastUse = 0;
    for (auto posIt = it->second.rbegin(); posIt != it->second.rend(); ++posIt) {
        if (*posIt > currentInstrIdx_) {
            lastUse = *posIt;
            break;
        }
    }
    if (lastUse == 0)
        return false;

    for (unsigned callIdx : callPositions_) {
        if (callIdx > currentInstrIdx_ && callIdx < lastUse)
            return true;
    }
    return false;
}

/// @copydoc LinearAllocator::getNextUseDistance
unsigned LinearAllocator::getNextUseDistance(uint16_t vreg, RegClass cls) const {
    const auto &map = (cls == RegClass::GPR) ? usePositionsGPR_ : usePositionsFPR_;
    auto it = map.find(vreg);
    if (it == map.end())
        return UINT_MAX;

    const auto &positions = it->second;
    auto pos = std::upper_bound(positions.begin(), positions.end(), currentInstrIdx_);
    if (pos == positions.end())
        return UINT_MAX;
    return *pos - currentInstrIdx_;
}

/// @copydoc LinearAllocator::computeSpillLastUse
unsigned LinearAllocator::computeSpillLastUse(uint16_t vreg,
                                              RegClass cls,
                                              bool forceLiveOut) const {
    const auto &posMap = (cls == RegClass::GPR) ? usePositionsGPR_ : usePositionsFPR_;
    unsigned trueLastUse = currentInstrIdx_;
    auto posIt = posMap.find(vreg);
    if (posIt != posMap.end() && !posIt->second.empty())
        trueLastUse = std::max(trueLastUse, posIt->second.back());
    if (forceLiveOut || isLiveOut(vreg, cls))
        trueLastUse = std::max(trueLastUse, currentBlockInstrCount_);
    return trueLastUse;
}

/// @copydoc LinearAllocator::ensureCurrentSpillSlot
int LinearAllocator::ensureCurrentSpillSlot(uint16_t vreg, RegClass cls, bool forceLiveOut) {
    return fb_.ensureSpillWithReuse(
        vreg, computeSpillLastUse(vreg, cls, forceLiveOut), currentInstrIdx_);
}

// =========================================================================
// Spilling
// =========================================================================

/// @copydoc LinearAllocator::spillVictim
void LinearAllocator::spillVictim(RegClass cls, uint16_t id, std::vector<MInstr> &prefix) {
    auto &st = (cls == RegClass::GPR) ? gprStates_[id] : fprStates_[id];
    if (!st.hasPhys)
        return;

    const unsigned nextUseDist = getNextUseDistance(id, cls);
    const bool liveOut = isLiveOut(id, cls);
    if (nextUseDist == UINT_MAX && !liveOut) {
        if (cls == RegClass::GPR)
            pools_.releaseGPR(st.phys, ti_);
        else
            pools_.releaseFPR(st.phys, ti_);
        st.hasPhys = false;
        st.spilled = false;
        st.dirty = false;
        return;
    }

    if (st.dirty || st.fpOffset == 0) {
        const int off = ensureCurrentSpillSlot(id, cls);
        st.fpOffset = off;
        if (cls == RegClass::GPR)
            prefix.push_back(makeStrFp(st.phys, off));
        else
            prefix.push_back(
                MInstr{MOpcode::StrFprFpImm, {MOperand::regOp(st.phys), MOperand::immOp(off)}});
        st.dirty = false;
    }

    if (cls == RegClass::GPR)
        pools_.releaseGPR(st.phys, ti_);
    else
        pools_.releaseFPR(st.phys, ti_);
    st.hasPhys = false;
    st.spilled = true;
}

/// @copydoc LinearAllocator::selectLRUVictim
uint16_t LinearAllocator::selectLRUVictim(RegClass cls) {
    auto &states = (cls == RegClass::GPR) ? gprStates_ : fprStates_;
    uint16_t victim = UINT16_MAX;
    unsigned oldestUse = UINT_MAX;

    // Tie-break equal lastUse values on the smaller vreg id: hash-map
    // iteration order differs between STL implementations, and the victim
    // choice must not (deterministic codegen across platforms).
    for (auto &kv : states) {
        if (isProtectedOperand(kv.first, cls))
            continue;
        if (kv.second.hasPhys && isProtectedPhys(kv.second.phys, cls))
            continue;
        if (!kv.second.hasPhys)
            continue;
        if (kv.second.lastUse < oldestUse ||
            (kv.second.lastUse == oldestUse && kv.first < victim)) {
            oldestUse = kv.second.lastUse;
            victim = kv.first;
        }
    }
    return victim;
}

/// @copydoc LinearAllocator::selectFurthestVictim
uint16_t LinearAllocator::selectFurthestVictim(RegClass cls) {
    auto &states = (cls == RegClass::GPR) ? gprStates_ : fprStates_;
    uint16_t victim = UINT16_MAX;
    unsigned furthestDist = 0;

    // Tie-break equal next-use distances on the smaller vreg id so the
    // selection is independent of hash-map iteration order.
    for (auto &kv : states) {
        if (!kv.second.hasPhys)
            continue;
        if (isProtectedOperand(kv.first, cls))
            continue;
        if (isProtectedPhys(kv.second.phys, cls))
            continue;

        unsigned dist = getNextUseDistance(kv.first, cls);
        if (dist > furthestDist ||
            (dist == furthestDist && victim != UINT16_MAX && kv.first < victim)) {
            furthestDist = dist;
            victim = kv.first;
        }
    }

    if (victim == UINT16_MAX)
        return selectLRUVictim(cls);

    return victim;
}

/// @copydoc LinearAllocator::maybeSpillForPressure
void LinearAllocator::maybeSpillForPressure(RegClass cls, std::vector<MInstr> &prefix) {
    if (cls == RegClass::GPR) {
        if (!pools_.gprFree.empty())
            return;
        uint16_t victim = selectFurthestVictim(RegClass::GPR);
        if (victim != UINT16_MAX)
            spillVictim(RegClass::GPR, victim, prefix);
    } else {
        if (!pools_.fprFree.empty())
            return;
        uint16_t victim = selectFurthestVictim(RegClass::FPR);
        if (victim != UINT16_MAX)
            spillVictim(RegClass::FPR, victim, prefix);
    }
}

// =========================================================================
// Register materialization
// =========================================================================

/// @copydoc LinearAllocator::materialize
void LinearAllocator::materialize(MReg &r,
                                  bool isUse,
                                  bool isDef,
                                  std::vector<MInstr> &prefix,
                                  std::vector<MInstr> &suffix,
                                  std::vector<PhysReg> &scratch) {
    if (r.isPhys) {
        trackCalleeSavedPhys(static_cast<PhysReg>(r.idOrPhys));
        return;
    }

    const bool fprClass = (r.cls == RegClass::FPR);
    auto &st = fprClass ? fprStates_[r.idOrPhys] : gprStates_[r.idOrPhys];

    if (isUse || isDef)
        st.lastUse = currentInstrIdx_;

    if (st.spilled) {
        handleSpilledOperand(r, fprClass, isUse, isDef, prefix, suffix, scratch);
        return;
    }

    if (!st.hasPhys) {
        assignNewPhysReg(st, r.idOrPhys, fprClass);
    }

    if (isDef)
        st.dirty = true;

    r.isPhys = true;
    r.idOrPhys = static_cast<uint16_t>(st.phys);
}

/// @copydoc LinearAllocator::handleSpilledOperand
void LinearAllocator::handleSpilledOperand(MReg &r,
                                           bool fprClass,
                                           bool isUse,
                                           bool isDef,
                                           std::vector<MInstr> &prefix,
                                           std::vector<MInstr> &suffix,
                                           std::vector<PhysReg> &scratch) {
    auto &st = fprClass ? fprStates_[r.idOrPhys] : gprStates_[r.idOrPhys];
    const int off = st.fpOffset != 0 ? st.fpOffset : fb_.ensureSpill(r.idOrPhys);
    st.fpOffset = off;

    // Preferred path: adopt a free pool register as the vreg's home so later
    // uses in this block reuse it instead of reloading through scratch every
    // time. assignNewPhysReg keeps the callee-saved-across-calls preference
    // and the calleeUsed bookkeeping. Eviction is not attempted here — the
    // caller's maybeSpillForPressure already ran per operand — so an empty
    // pool falls back to the reserved emergency scratch registers exactly as
    // before (one-shot load/store, register released after the instruction).
    const RegClass cls = fprClass ? RegClass::FPR : RegClass::GPR;
    const bool poolHasFree = fprClass ? !pools_.fprFree.empty() : !pools_.gprFree.empty();
    if (poolHasFree) {
        assignNewPhysReg(st, r.idOrPhys, fprClass);
        st.spilled = false;
        st.dirty = isDef;

        if (isUse) {
            if (fprClass)
                prefix.push_back(
                    MInstr{MOpcode::LdrFprFpImm, {MOperand::regOp(st.phys), MOperand::immOp(off)}});
            else
                prefix.push_back(makeLdrFp(st.phys, off));
        }

        r.isPhys = true;
        r.idOrPhys = static_cast<uint16_t>(st.phys);
        return;
    }
    (void)cls;

    PhysReg tmp{};
    const auto &blocked = fprClass ? protectedPhysFPR_ : protectedPhysGPR_;
    tmp = chooseEmergencyScratch(fprClass, isDef && !isUse, scratch, blocked);

    if (isUse) {
        if (fprClass)
            prefix.push_back(
                MInstr{MOpcode::LdrFprFpImm, {MOperand::regOp(tmp), MOperand::immOp(off)}});
        else
            prefix.push_back(makeLdrFp(tmp, off));
    }

    if (isDef) {
        if (fprClass)
            suffix.push_back(
                MInstr{MOpcode::StrFprFpImm, {MOperand::regOp(tmp), MOperand::immOp(off)}});
        else
            suffix.push_back(makeStrFp(tmp, off));
    }

    r.isPhys = true;
    r.idOrPhys = static_cast<uint16_t>(tmp);
    scratch.push_back(tmp);
}

/// @copydoc LinearAllocator::assignNewPhysReg
void LinearAllocator::assignNewPhysReg(VState &st, uint16_t vregId, bool fprClass) {
    PhysReg phys;
    if (fprClass) {
        if (!fn_.isLeaf && nextUseAfterCall(vregId, RegClass::FPR))
            phys = pools_.takeFPRPreferCalleeSaved(ti_);
        else
            phys = pools_.takeFPR();
    } else {
        if (!fn_.isLeaf && nextUseAfterCall(vregId, RegClass::GPR))
            phys = pools_.takeGPRPreferCalleeSaved(ti_);
        else
            phys = pools_.takeGPR();
    }

    st.hasPhys = true;
    st.phys = phys;

    if (!fprClass) {
        if (std::find(ti_.calleeSavedGPR.begin(), ti_.calleeSavedGPR.end(), phys) !=
            ti_.calleeSavedGPR.end()) {
            pools_.calleeUsed[static_cast<std::size_t>(phys)] = true;
        }
    } else {
        if (std::find(ti_.calleeSavedFPR.begin(), ti_.calleeSavedFPR.end(), phys) !=
            ti_.calleeSavedFPR.end()) {
            pools_.calleeUsedFPR[static_cast<std::size_t>(phys)] = true;
        }
    }
}

/// @copydoc LinearAllocator::trackCalleeSavedPhys
void LinearAllocator::trackCalleeSavedPhys(PhysReg pr) {
    if (isGPR(pr)) {
        if (std::find(ti_.calleeSavedGPR.begin(), ti_.calleeSavedGPR.end(), pr) !=
            ti_.calleeSavedGPR.end()) {
            pools_.calleeUsed[static_cast<std::size_t>(pr)] = true;
        }
    } else if (isFPR(pr)) {
        if (std::find(ti_.calleeSavedFPR.begin(), ti_.calleeSavedFPR.end(), pr) !=
            ti_.calleeSavedFPR.end()) {
            pools_.calleeUsedFPR[static_cast<std::size_t>(pr)] = true;
        }
    }
}

/// @copydoc LinearAllocator::isCalleeSaved
bool LinearAllocator::isCalleeSaved(PhysReg pr, RegClass cls) const noexcept {
    if (cls == RegClass::GPR) {
        return std::find(ti_.calleeSavedGPR.begin(), ti_.calleeSavedGPR.end(), pr) !=
               ti_.calleeSavedGPR.end();
    } else {
        return std::find(ti_.calleeSavedFPR.begin(), ti_.calleeSavedFPR.end(), pr) !=
               ti_.calleeSavedFPR.end();
    }
}

// =========================================================================
// Block allocation
// =========================================================================

/// @copydoc LinearAllocator::allocateBlock
void LinearAllocator::allocateBlock(MBasicBlock &bb) {
    fb_.beginNewBlock();

    computeNextUses(bb);
    currentInstrIdx_ = 0;
    currentBlockInstrCount_ = static_cast<unsigned>(bb.instrs.size());

    std::vector<MInstr> rewritten;
    rewritten.reserve(bb.instrs.size());

    for (auto &ins : bb.instrs) {
        if (isSpAdj(ins.opc) || isBranch(ins.opc)) {
            rewritten.push_back(ins);
            ++currentInstrIdx_;
            continue;
        }

        if (isCall(ins.opc)) {
            handleCall(ins, rewritten);
            ++currentInstrIdx_;
            continue;
        }

        allocateInstruction(ins, rewritten);
        ++currentInstrIdx_;
    }

    if (!rewritten.empty()) {
        std::vector<MInstr> endSpills;
        const std::size_t bi = currentBlockIdx_;

        // liveOut sets are unordered; process them in sorted vreg order so the
        // emitted spill sequence and the register-pool release order do not
        // depend on the STL implementation's hash iteration order.
        /// Copy a live-out set into deterministic virtual-register order.
        const auto sortedLiveOut = [](const std::unordered_set<uint16_t> &set) {
            std::vector<uint16_t> sorted(set.begin(), set.end());
            std::sort(sorted.begin(), sorted.end());
            return sorted;
        };
        const std::vector<uint16_t> loGPR = sortedLiveOut(liveness_.liveOutGPR(bi));
        const std::vector<uint16_t> loFPR = sortedLiveOut(liveness_.liveOutFPR(bi));

        if (bi < blockExitStates_.size()) {
            auto &es = blockExitStates_[bi];
            for (auto vid : loGPR) {
                auto it = gprStates_.find(vid);
                if (it != gprStates_.end() && it->second.hasPhys)
                    es.gpr[vid] = it->second.phys;
            }
            for (auto vid : loFPR) {
                auto it = fprStates_.find(vid);
                if (it != fprStates_.end() && it->second.hasPhys)
                    es.fpr[vid] = it->second.phys;
            }

            // Publish the carried registers on the block itself: a
            // single-predecessor successor may re-adopt these values straight
            // from the registers without any instruction marking the carry,
            // so post-RA block-local rewrites must treat them as live-out.
            bb.carriedExitRegs.clear();
            bb.carriedExitRegs.reserve(es.gpr.size() + es.fpr.size());
            for (const auto &kv : es.gpr)
                bb.carriedExitRegs.push_back(static_cast<uint16_t>(kv.second));
            for (const auto &kv : es.fpr)
                bb.carriedExitRegs.push_back(static_cast<uint16_t>(kv.second));
            std::sort(bb.carriedExitRegs.begin(), bb.carriedExitRegs.end());
        }

        for (auto vid : loGPR) {
            auto it = gprStates_.find(vid);
            if (it != gprStates_.end() && it->second.hasPhys) {
                if (it->second.dirty || it->second.fpOffset == 0) {
                    const int off =
                        ensureCurrentSpillSlot(vid, RegClass::GPR, /*forceLiveOut=*/true);
                    it->second.fpOffset = off;
                    endSpills.push_back(makeStrFp(it->second.phys, off));
                    it->second.dirty = false;
                }
                pools_.releaseGPR(it->second.phys, ti_);
                it->second.hasPhys = false;
                it->second.spilled = true;
            }
        }
        for (auto vid : loFPR) {
            auto it = fprStates_.find(vid);
            if (it != fprStates_.end() && it->second.hasPhys) {
                if (it->second.dirty || it->second.fpOffset == 0) {
                    const int off =
                        ensureCurrentSpillSlot(vid, RegClass::FPR, /*forceLiveOut=*/true);
                    it->second.fpOffset = off;
                    endSpills.push_back(
                        MInstr{MOpcode::StrFprFpImm,
                               {MOperand::regOp(it->second.phys), MOperand::immOp(off)}});
                    it->second.dirty = false;
                }
                pools_.releaseFPR(it->second.phys, ti_);
                it->second.hasPhys = false;
                it->second.spilled = true;
            }
        }
        if (!endSpills.empty()) {
            std::size_t insertPos = rewritten.size();
            while (insertPos > 0 && isTerminator(rewritten[insertPos - 1].opc))
                --insertPos;
            rewritten.insert(rewritten.begin() + static_cast<long>(insertPos),
                             endSpills.begin(),
                             endSpills.end());
        }
    }
    bb.instrs = std::move(rewritten);
}

/// @copydoc LinearAllocator::handleCall
void LinearAllocator::handleCall(MInstr &ins, std::vector<MInstr> &rewritten) {
    bool isArrayObjGet = false;
    if (!ins.ops.empty() && ins.ops[0].kind == MOperand::Kind::Label) {
        std::string label = ins.ops[0].label;
        if (auto mapped = il::runtime::mapCanonicalRuntimeName(label))
            label = std::string(*mapped);
        if (label == "rt_arr_obj_get")
            isArrayObjGet = true;
    }

    // Spill order is part of the emitted instruction stream, so iterate vregs
    // in sorted order rather than hash-map order: iteration order differs
    // between STL implementations and codegen must stay byte-identical
    // across platforms.
    std::vector<MInstr> preCall;
    /// Spill or retire every resident value held in a caller-saved register.
    const auto spillCallerSavedResidents = [&](RegClass cls) {
        auto &states = (cls == RegClass::GPR) ? gprStates_ : fprStates_;
        std::vector<uint16_t> resident;
        resident.reserve(states.size());
        for (auto &kv : states) {
            if (kv.second.hasPhys && !isCalleeSaved(kv.second.phys, cls))
                resident.push_back(kv.first);
        }
        std::sort(resident.begin(), resident.end());
        for (uint16_t vid : resident) {
            auto &st = states[vid];
            const bool liveOut = isLiveOut(vid, cls);
            const unsigned dist = getNextUseDistance(vid, cls);
            if (dist == UINT_MAX && !liveOut) {
                if (cls == RegClass::GPR)
                    pools_.releaseGPR(st.phys, ti_);
                else
                    pools_.releaseFPR(st.phys, ti_);
                st.hasPhys = false;
            } else {
                spillVictim(cls, vid, preCall);
            }
        }
    };
    spillCallerSavedResidents(RegClass::GPR);
    spillCallerSavedResidents(RegClass::FPR);
    for (auto &mi : preCall)
        rewritten.push_back(std::move(mi));

    std::vector<MInstr> callPrefix;
    std::vector<MInstr> callSuffix;
    std::vector<PhysReg> callScratch;
    std::vector<std::pair<uint16_t, RegClass>> virtualCallOperands;

    protectedOperandGPR_.clear();
    protectedOperandFPR_.clear();
    protectedPhysGPR_.clear();
    protectedPhysFPR_.clear();
    for (std::size_t i = 0; i < ins.ops.size(); ++i) {
        auto &op = ins.ops[i];
        const auto [isUse, isDef] = operandRoles(ins, i);
        if ((!isUse && !isDef) || op.kind != MOperand::Kind::Reg)
            continue;
        if (op.reg.isPhys) {
            const PhysReg phys = static_cast<PhysReg>(op.reg.idOrPhys);
            if (op.reg.cls == RegClass::GPR)
                protectedPhysGPR_.insert(phys);
            else
                protectedPhysFPR_.insert(phys);
            trackCalleeSavedPhys(phys);
            continue;
        }

        virtualCallOperands.push_back({op.reg.idOrPhys, op.reg.cls});
        if (op.reg.cls == RegClass::GPR)
            protectedOperandGPR_.insert(op.reg.idOrPhys);
        else
            protectedOperandFPR_.insert(op.reg.idOrPhys);
    }

    for (std::size_t i = 0; i < ins.ops.size(); ++i) {
        auto &op = ins.ops[i];
        const auto [isUse, isDef] = operandRoles(ins, i);
        if (op.kind == MOperand::Kind::Reg && !op.reg.isPhys) {
            maybeSpillForPressure(op.reg.cls, callPrefix);
            materialize(op.reg, isUse, isDef, callPrefix, callSuffix, callScratch);
        }
    }
    for (const auto &[vreg, cls] : virtualCallOperands) {
        auto &states = cls == RegClass::GPR ? gprStates_ : fprStates_;
        auto it = states.find(vreg);
        if (it == states.end() || !it->second.hasPhys || isCalleeSaved(it->second.phys, cls))
            continue;
        const bool neededLater = isLiveOut(vreg, cls) || getNextUseDistance(vreg, cls) != UINT_MAX;
        if (!neededLater)
            continue;
        if (it->second.fpOffset == 0 || it->second.dirty) {
            const int off = ensureCurrentSpillSlot(vreg, cls);
            it->second.fpOffset = off;
            if (cls == RegClass::GPR)
                callPrefix.push_back(makeStrFp(it->second.phys, off));
            else
                callPrefix.push_back(
                    MInstr{MOpcode::StrFprFpImm,
                           {MOperand::regOp(it->second.phys), MOperand::immOp(off)}});
            it->second.dirty = false;
        }
    }
    for (auto &mi : callPrefix)
        rewritten.push_back(std::move(mi));
    rewritten.push_back(ins);
    for (auto &mi : callSuffix)
        rewritten.push_back(std::move(mi));
    for (const auto &[vreg, cls] : virtualCallOperands)
        retireCallOperandAfterCall(vreg, cls);
    // Marshalled argument registers were held out of the pools while the call
    // was in flight; the call has consumed them now.
    releaseCallReserved();
    releaseScratch(callScratch);
    protectedOperandGPR_.clear();
    protectedOperandFPR_.clear();
    protectedPhysGPR_.clear();
    protectedPhysFPR_.clear();
    if (isArrayObjGet)
        pendingGetBarrier_ = true;
}

/// @copydoc LinearAllocator::evictPhysDefClobbers
void LinearAllocator::evictPhysDefClobbers(MInstr &ins, std::vector<MInstr> &prefix) {
    for (std::size_t opIdx = 0; opIdx < ins.ops.size(); ++opIdx) {
        const auto &op = ins.ops[opIdx];
        if (op.kind != MOperand::Kind::Reg || !op.reg.isPhys)
            continue;
        const auto [isUse, isDef] = operandRoles(ins, opIdx);
        (void)isUse;
        if (!isDef)
            continue;

        const auto phys = static_cast<PhysReg>(op.reg.idOrPhys);
        const bool fprClass = isFPR(phys);
        auto &states = fprClass ? fprStates_ : gprStates_;
        const RegClass cls = fprClass ? RegClass::FPR : RegClass::GPR;

        // Evict a resident vreg before its register is overwritten. Spill when
        // the value is still needed; otherwise just release.
        for (auto &kv : states) {
            if (!kv.second.hasPhys || kv.second.phys != phys)
                continue;
            if (rewriteVirtualUseToResidentPhys(ins, kv.first, cls, phys)) {
                const bool liveOut = isLiveOut(kv.first, cls);
                const unsigned dist = getNextUseDistance(kv.first, cls);
                const bool neededLater = liveOut || dist != UINT_MAX;
                if (neededLater && (kv.second.dirty || kv.second.fpOffset == 0)) {
                    const int off = ensureCurrentSpillSlot(kv.first, cls);
                    kv.second.fpOffset = off;
                    if (fprClass)
                        prefix.push_back(
                            MInstr{MOpcode::StrFprFpImm,
                                   {MOperand::regOp(kv.second.phys), MOperand::immOp(off)}});
                    else
                        prefix.push_back(makeStrFp(kv.second.phys, off));
                    kv.second.dirty = false;
                }
                kv.second.hasPhys = false;
                kv.second.spilled = neededLater;
                break;
            }
            const bool liveOut = isLiveOut(kv.first, cls);
            const unsigned dist = getNextUseDistance(kv.first, cls);
            if (dist == UINT_MAX && !liveOut) {
                if (fprClass)
                    pools_.releaseFPR(kv.second.phys, ti_);
                else
                    pools_.releaseGPR(kv.second.phys, ti_);
                kv.second.hasPhys = false;
            } else {
                spillVictim(cls, kv.first, prefix);
            }
            break;
        }

        // A def of an argument register is call marshalling in flight: keep the
        // register out of the free pool until the call consumes it so reloads
        // and fresh allocations cannot clobber the marshalled value.
        if (isArgRegister(phys, ti_) && pools_.poolEligible[static_cast<std::size_t>(phys)]) {
            auto &pool = fprClass ? pools_.fprFree : pools_.gprFree;
            auto it = std::find(pool.begin(), pool.end(), phys);
            if (it != pool.end())
                pool.erase(it);
            if (std::find(reservedForCall_.begin(), reservedForCall_.end(), phys) ==
                reservedForCall_.end())
                reservedForCall_.push_back(phys);
        }
    }
}

/// @copydoc LinearAllocator::releaseCallReserved
void LinearAllocator::releaseCallReserved() {
    for (PhysReg phys : reservedForCall_) {
        if (isFPR(phys))
            pools_.releaseFPR(phys, ti_);
        else
            pools_.releaseGPR(phys, ti_);
    }
    reservedForCall_.clear();
}

/// @copydoc LinearAllocator::retireCallOperandAfterCall
void LinearAllocator::retireCallOperandAfterCall(uint16_t vreg, RegClass cls) {
    auto &states = cls == RegClass::GPR ? gprStates_ : fprStates_;
    auto it = states.find(vreg);
    if (it == states.end() || !it->second.hasPhys)
        return;

    auto &st = it->second;
    if (isCalleeSaved(st.phys, cls))
        return;

    const bool neededLater = isLiveOut(vreg, cls) || getNextUseDistance(vreg, cls) != UINT_MAX;
    if (cls == RegClass::GPR)
        pools_.releaseGPR(st.phys, ti_);
    else
        pools_.releaseFPR(st.phys, ti_);
    st.hasPhys = false;
    st.spilled = neededLater;
    st.dirty = false;
}

/// @copydoc LinearAllocator::allocateInstruction
void LinearAllocator::allocateInstruction(MInstr &ins, std::vector<MInstr> &rewritten) {
    // Pinned-slot accesses become register moves before any other handling;
    // the move's vreg operand then materializes through the normal path.
    rewritePinnedSlotAccess(ins);

    const bool isPhiStore = (ins.opc == MOpcode::PhiStoreGPR || ins.opc == MOpcode::PhiStoreFPR);
    const bool phiSrcIsFPR = (ins.opc == MOpcode::PhiStoreFPR);

    std::vector<MInstr> prefix;
    evictPhysDefClobbers(ins, prefix);
    bool applyGetBarrier = false;
    uint16_t getBarrierDstVreg = 0;
    if (pendingGetBarrier_ && ins.opc == MOpcode::MovRR && ins.ops.size() >= 2) {
        const auto &dst = ins.ops[0];
        const auto &src = ins.ops[1];
        if (dst.kind == MOperand::Kind::Reg && !dst.reg.isPhys && src.kind == MOperand::Kind::Reg &&
            src.reg.isPhys && static_cast<PhysReg>(src.reg.idOrPhys) == PhysReg::X0) {
            applyGetBarrier = true;
            getBarrierDstVreg = dst.reg.idOrPhys;
            pendingGetBarrier_ = false;
        } else {
            pendingGetBarrier_ = false;
        }
    }
    std::vector<MInstr> suffix;
    std::vector<PhysReg> scratch;

    protectedOperandGPR_.clear();
    protectedOperandFPR_.clear();
    protectedPhysGPR_.clear();
    protectedPhysFPR_.clear();
    for (std::size_t i = 0; i < ins.ops.size(); ++i) {
        const auto &op = ins.ops[i];
        const auto [isUse, isDef] = operandRoles(ins, i);
        if ((!isUse && !isDef) || op.kind != MOperand::Kind::Reg)
            continue;
        if (op.reg.isPhys) {
            const PhysReg phys = static_cast<PhysReg>(op.reg.idOrPhys);
            if (op.reg.cls == RegClass::GPR)
                protectedPhysGPR_.insert(phys);
            else
                protectedPhysFPR_.insert(phys);
            continue;
        }
        if (op.reg.cls == RegClass::GPR)
            protectedOperandGPR_.insert(op.reg.idOrPhys);
        else
            protectedOperandFPR_.insert(op.reg.idOrPhys);
    }

    for (std::size_t i = 0; i < ins.ops.size(); ++i) {
        auto &op = ins.ops[i];
        auto [isUse, isDef] = operandRoles(ins, i);
        if (op.kind == MOperand::Kind::Reg) {
            if (op.reg.isPhys) {
                materialize(op.reg, isUse, isDef, prefix, suffix, scratch);
                continue;
            }
            maybeSpillForPressure(op.reg.cls, prefix);
            materialize(op.reg, isUse, isDef, prefix, suffix, scratch);
        }
    }

    if (applyGetBarrier) {
        auto it = gprStates_.find(getBarrierDstVreg);
        if (it != gprStates_.end() && it->second.hasPhys) {
            const int off = ensureCurrentSpillSlot(getBarrierDstVreg, RegClass::GPR);
            it->second.fpOffset = off;
            suffix.push_back(makeStrFp(it->second.phys, off));
            pools_.releaseGPR(it->second.phys, ti_);
            it->second.hasPhys = false;
            it->second.spilled = true;
        }
    }

    if (isPhiStore) {
        // PhiStore writes the source register into the phi DESTINATION's
        // slot. It says nothing about the source vreg's own spill slot, so
        // the source's dirty bit must be left untouched: clearing it here
        // would let a later spill skip the store and leave the source's slot
        // stale. (Today lowering mints single-def per-block vregs, which
        // masks the hazard, but the invariant should not depend on that.)
        ins.opc = phiSrcIsFPR ? MOpcode::StrFprFpImm : MOpcode::StrRegFpImm;
    }

    for (auto &pre : prefix)
        rewritten.push_back(std::move(pre));
    rewritten.push_back(std::move(ins));
    for (auto &suf : suffix)
        rewritten.push_back(std::move(suf));

    releaseScratch(scratch);
    protectedOperandGPR_.clear();
    protectedOperandFPR_.clear();
    protectedPhysGPR_.clear();
    protectedPhysFPR_.clear();
}

// =========================================================================
// Cleanup
// =========================================================================

/// @copydoc LinearAllocator::releaseScratch
void LinearAllocator::releaseScratch(std::vector<PhysReg> &scratch) {
    for (auto pr : scratch) {
        if (isReservedScratch(pr))
            continue;
        if (isGPR(pr)) {
            if (std::find(ti_.calleeSavedGPR.begin(), ti_.calleeSavedGPR.end(), pr) !=
                ti_.calleeSavedGPR.end())
                pools_.calleeUsed[static_cast<std::size_t>(pr)] = true;
            pools_.releaseGPR(pr, ti_);
        } else {
            if (std::find(ti_.calleeSavedFPR.begin(), ti_.calleeSavedFPR.end(), pr) !=
                ti_.calleeSavedFPR.end())
                pools_.calleeUsedFPR[static_cast<std::size_t>(pr)] = true;
            pools_.releaseFPR(pr, ti_);
        }
    }
}

/// @copydoc LinearAllocator::releaseBlockState
void LinearAllocator::releaseBlockState() {
    // Defensive: a marshalling def without a following call in the same block
    // must not leak reserved argument registers into the next block.
    releaseCallReserved();

    // Release in sorted vreg order: the pool is a FIFO, so the order registers
    // return to it shapes every later allocation. Hash-map iteration order
    // would make the emitted code differ between STL implementations.
    /// Return all residents of one class in deterministic vreg order.
    const auto releaseAll = [&](RegClass cls) {
        auto &states = (cls == RegClass::GPR) ? gprStates_ : fprStates_;
        std::vector<uint16_t> resident;
        resident.reserve(states.size());
        for (auto &kv : states) {
            if (kv.second.hasPhys)
                resident.push_back(kv.first);
        }
        std::sort(resident.begin(), resident.end());
        for (uint16_t vid : resident) {
            auto &st = states[vid];
            if (cls == RegClass::GPR)
                pools_.releaseGPR(st.phys, ti_);
            else
                pools_.releaseFPR(st.phys, ti_);
            st.hasPhys = false;
            if (st.dirty) {
                st.spilled = false;
            } else if (st.fpOffset != 0) {
                st.spilled = true;
            }
        }
    };
    releaseAll(RegClass::GPR);
    releaseAll(RegClass::FPR);
}

/// @copydoc LinearAllocator::recordCalleeSavedUsage
void LinearAllocator::recordCalleeSavedUsage() {
    fn_.savedGPRs.clear();
    fn_.savedFPRs.clear();
    for (auto r : ti_.calleeSavedGPR) {
        if (pools_.calleeUsed[static_cast<std::size_t>(r)])
            fn_.savedGPRs.push_back(r);
    }
    for (auto r : ti_.calleeSavedFPR) {
        if (pools_.calleeUsedFPR[static_cast<std::size_t>(r)])
            fn_.savedFPRs.push_back(r);
    }
}

} // namespace zanna::codegen::aarch64::ra
