//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/Peephole.cpp
// Purpose: Driver for conservative peephole optimizations over AArch64 MIR.
//          Orchestrates modular sub-passes and implements cross-block phi-join
//          load forwarding/coalescing over the shared MirCfg (edges,
//          dominators, natural loops).
// Key invariants:
//   - All rewrites preserve instruction semantics and ordering.
//   - Must be called after register allocation (physical registers required).
//   - Cross-block passes read edges only from MirCfg, never from a private
//     terminator scan, so they agree with the allocator and the verifier.
//   - Join-load coalescing skips loop headers whose natural loop contains calls.
//   - Block-local folds that reach the block end consult blockExitLive().
// Ownership/Lifetime:
//   - Mutates MFunction in place; borrows fn only during the call.
// Links: src/codegen/aarch64/Peephole.hpp,
//        src/codegen/aarch64/peephole/ (sub-pass implementations)
//
//===----------------------------------------------------------------------===//

#include "Peephole.hpp"

#include "MirCfg.hpp"
#include "TargetAArch64.hpp"
#include "peephole/BranchOpt.hpp"
#include "peephole/CopyPropDCE.hpp"
#include "peephole/IdentityElim.hpp"
#include "peephole/LoopOpt.hpp"
#include "peephole/MemoryOpt.hpp"
#include "peephole/PeepholeCommon.hpp"
#include "peephole/StrengthReduce.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>

/**
 * @file
 * @brief Implements the AArch64 post-allocation peephole pipeline.
 *
 * In addition to orchestrating modular local sub-passes, this file recognizes
 * stack-based phi transfers at block joins and replaces provably safe
 * store/load round trips with physical-register copies. Cross-block rewrites
 * maintain `carriedExitRegs` so subsequent local cleanup respects the extended
 * physical-register live ranges.
 */

namespace zanna::codegen::aarch64 {

// Import sub-pass functions into local scope for concise call sites.
namespace ph = peephole;

namespace {

/// @brief Descriptor for an FP-relative load at the start of a join block.
struct JoinLoad {
    std::size_t instrIndex{0};   ///< Index of the load instruction in the block.
    int64_t offset{0};           ///< FP-relative byte offset loaded from.
    MOperand dstReg{};           ///< Destination register operand.
    RegClass cls{RegClass::GPR}; ///< GPR or FPR.
};

/// @brief Descriptor for an FP-relative store at the end of a predecessor block.
struct JoinStore {
    std::size_t instrIndex{0};   ///< Index of the store instruction in the predecessor.
    int64_t offset{0};           ///< FP-relative byte offset stored to.
    MOperand srcReg{};           ///< Source register operand.
    RegClass cls{RegClass::GPR}; ///< GPR or FPR.
};

/// @brief A single register-to-register copy to be inserted on a pred→join edge.
struct JoinCopy {
    MOperand srcReg{};           ///< Source (value stored in predecessor).
    MOperand dstReg{};           ///< Destination (value loaded in join block).
    RegClass cls{RegClass::GPR}; ///< GPR or FPR.
};

/**
 * @brief Encodes a register class and register ID into a set/map key.
 *
 * @param op Register operand whose `reg` payload is read.
 * @return Class in the upper 16 bits and `idOrPhys` in the lower 16 bits.
 * @pre @p op is a register operand.
 */
static std::uint32_t regKey(const MOperand &op) {
    return (static_cast<std::uint32_t>(op.reg.cls) << 16) |
           static_cast<std::uint32_t>(op.reg.idOrPhys);
}

/**
 * @brief Records a physical register newly carried across a block exit.
 *
 * Virtual/non-register operands are ignored. Physical IDs are inserted once
 * while preserving the sorted `carriedExitRegs` invariant.
 *
 * @param[in,out] block Predecessor block whose live-out metadata is extended.
 * @param reg Candidate physical-register operand.
 */
static void markCarriedExitReg(MBasicBlock &block, const MOperand &reg) {
    if (!ph::isPhysReg(reg))
        return;
    const uint16_t phys = reg.reg.idOrPhys;
    const auto insertion =
        std::lower_bound(block.carriedExitRegs.begin(), block.carriedExitRegs.end(), phys);
    if (insertion == block.carriedExitRegs.end() || *insertion != phys)
        block.carriedExitRegs.insert(insertion, phys);
}

/**
 * @brief Collects valid FP-relative loads at the beginning of a join block.
 *
 * Scalar loads contribute one descriptor and paired loads contribute two
 * descriptors sharing an instruction index. Scanning stops at the first
 * non-load or malformed load.
 *
 * @param block Candidate join block.
 * @param[out] loads Cleared, then populated in instruction/operand order.
 * @return `true` when at least one valid leading load was collected.
 */
static bool collectJoinPrefixLoads(const MBasicBlock &block, std::vector<JoinLoad> &loads) {
    loads.clear();
    for (std::size_t i = 0; i < block.instrs.size(); ++i) {
        const auto &instr = block.instrs[i];
        switch (instr.opc) {
            case MOpcode::LdrRegFpImm:
            case MOpcode::LdrFprFpImm:
                if (instr.ops.size() < 2 || !ph::isPhysReg(instr.ops[0]) ||
                    instr.ops[1].kind != MOperand::Kind::Imm)
                    return !loads.empty();
                loads.push_back(
                    {i,
                     instr.ops[1].imm,
                     instr.ops[0],
                     instr.opc == MOpcode::LdrFprFpImm ? RegClass::FPR : RegClass::GPR});
                continue;
            case MOpcode::LdpRegFpImm:
            case MOpcode::LdpFprFpImm:
                if (instr.ops.size() < 3 || !ph::isPhysReg(instr.ops[0]) ||
                    !ph::isPhysReg(instr.ops[1]) || instr.ops[2].kind != MOperand::Kind::Imm)
                    return !loads.empty();
                loads.push_back(
                    {i,
                     instr.ops[2].imm,
                     instr.ops[0],
                     instr.opc == MOpcode::LdpFprFpImm ? RegClass::FPR : RegClass::GPR});
                loads.push_back(
                    {i,
                     instr.ops[2].imm + 8,
                     instr.ops[1],
                     instr.opc == MOpcode::LdpFprFpImm ? RegClass::FPR : RegClass::GPR});
                continue;
            default:
                return !loads.empty();
        }
    }
    return !loads.empty();
}

/**
 * @brief Collects usable FP-relative stores from a predecessor's trailing transfer region.
 *
 * The backward scan skips a direct branch to the join and crosses only loads,
 * moves, and stores. A store is excluded if its source register was clobbered
 * later in the scanned region. Paired stores contribute one entry per offset.
 *
 * @param fn Function containing the predecessor and successor.
 * @param cfg Control-flow graph of @p fn in its current layout.
 * @param predIndex Predecessor block index.
 * @param succIndex Required direct successor block index.
 * @param[out] stores Cleared map populated by FP-relative byte offset.
 * @param[out] storeInstrs Cleared set of instruction indices supplying entries.
 * @return `true` when at least one safe store is collected.
 * @pre `predIndex < fn.blocks.size()` and `succIndex < fn.blocks.size()`.
 */
static bool collectJoinSuffixStores(const MFunction &fn,
                                    const MirCfg &cfg,
                                    std::size_t predIndex,
                                    std::size_t succIndex,
                                    std::unordered_map<int64_t, JoinStore> &stores,
                                    std::unordered_set<std::size_t> &storeInstrs) {
    stores.clear();
    storeInstrs.clear();
    if (!cfg.exitsDirectlyTo(fn, predIndex, succIndex))
        return false;

    const std::string &succName = fn.blocks[succIndex].name;
    const auto &instrs = fn.blocks[predIndex].instrs;
    std::size_t scanEnd = instrs.size();
    if (!instrs.empty()) {
        const auto &last = instrs.back();
        if (last.opc == MOpcode::Br && !last.ops.empty() &&
            last.ops[0].kind == MOperand::Kind::Label && last.ops[0].label == succName)
            scanEnd = instrs.size() - 1;
    }

    std::unordered_set<std::uint32_t> clobbered;
    std::unordered_set<int64_t> blockedOffsets;
    for (std::size_t i = scanEnd; i-- > 0;) {
        const auto &instr = instrs[i];
        if ((instr.opc == MOpcode::LdrRegFpImm || instr.opc == MOpcode::LdrFprFpImm ||
             instr.opc == MOpcode::LdpRegFpImm || instr.opc == MOpcode::LdpFprFpImm ||
             instr.opc == MOpcode::MovRR || instr.opc == MOpcode::FMovRR) &&
            i + 1 <= scanEnd) {
            if (instr.opc == MOpcode::LdpRegFpImm || instr.opc == MOpcode::LdpFprFpImm) {
                if (instr.ops.size() >= 1 && ph::isPhysReg(instr.ops[0]))
                    clobbered.insert(regKey(instr.ops[0]));
                if (instr.ops.size() >= 2 && ph::isPhysReg(instr.ops[1]))
                    clobbered.insert(regKey(instr.ops[1]));
            } else if (auto def = ph::getDefinedReg(instr); def && ph::isPhysReg(*def)) {
                clobbered.insert(regKey(*def));
            }
            continue;
        }

        if ((instr.opc == MOpcode::StrRegFpImm || instr.opc == MOpcode::StrFprFpImm) &&
            instr.ops.size() >= 2 && ph::isPhysReg(instr.ops[0]) &&
            instr.ops[1].kind == MOperand::Kind::Imm) {
            const auto key = regKey(instr.ops[0]);
            const int64_t off = instr.ops[1].imm;
            if (stores.find(off) == stores.end() && !blockedOffsets.count(off)) {
                if (clobbered.count(key)) {
                    blockedOffsets.insert(off);
                    continue;
                }
                stores.emplace(
                    off,
                    JoinStore{i,
                              off,
                              instr.ops[0],
                              instr.opc == MOpcode::StrFprFpImm ? RegClass::FPR : RegClass::GPR});
                storeInstrs.insert(i);
            }
            continue;
        }

        if ((instr.opc == MOpcode::StpRegFpImm || instr.opc == MOpcode::StpFprFpImm) &&
            instr.ops.size() >= 3 && ph::isPhysReg(instr.ops[0]) && ph::isPhysReg(instr.ops[1]) &&
            instr.ops[2].kind == MOperand::Kind::Imm) {
            const RegClass cls = instr.opc == MOpcode::StpFprFpImm ? RegClass::FPR : RegClass::GPR;
            const int64_t baseOff = instr.ops[2].imm;
            const std::array<std::pair<int64_t, MOperand>, 2> pairStores = {
                {{baseOff, instr.ops[0]}, {baseOff + 8, instr.ops[1]}}};
            for (const auto &[off, src] : pairStores) {
                if (stores.count(off) || blockedOffsets.count(off))
                    continue;
                if (clobbered.count(regKey(src))) {
                    blockedOffsets.insert(off);
                    continue;
                }
                stores.emplace(off, JoinStore{i, off, src, cls});
                storeInstrs.insert(i);
            }
            continue;
        }

        break;
    }

    return !stores.empty();
}

/**
 * @brief Topologically orders register copies to avoid overwriting pending sources.
 *
 * Identity copies are omitted. A copy is ready only when its destination is
 * not the source of another pending copy in the same register class.
 *
 * @param copies Candidate edge copies.
 * @param[out] ordered Cleared and populated with a safe execution order.
 * @return `true` when every non-identity copy can be ordered; `false` for a
 *         dependency cycle.
 */
static bool orderJoinCopies(const std::vector<JoinCopy> &copies, std::vector<JoinCopy> &ordered) {
    ordered.clear();
    ordered.reserve(copies.size());

    std::vector<JoinCopy> pending;
    pending.reserve(copies.size());
    for (const auto &copy : copies) {
        if (copy.srcReg.reg.idOrPhys == copy.dstReg.reg.idOrPhys)
            continue;
        pending.push_back(copy);
    }

    while (!pending.empty()) {
        auto readyIt = pending.end();
        for (auto it = pending.begin(); it != pending.end(); ++it) {
            const auto dstPhys = it->dstReg.reg.idOrPhys;
            bool dstUsedAsSource = false;
            for (auto jt = pending.begin(); jt != pending.end(); ++jt) {
                if (it == jt)
                    continue;
                if (jt->cls == it->cls && jt->srcReg.reg.idOrPhys == dstPhys) {
                    dstUsedAsSource = true;
                    break;
                }
            }
            if (!dstUsedAsSource) {
                readyIt = it;
                break;
            }
        }
        if (readyIt == pending.end())
            return false;
        ordered.push_back(*readyIt);
        pending.erase(readyIt);
    }

    return true;
}

/**
 * @brief Removes forwarded join loads while preserving unforwarded halves of pairs.
 *
 * A fully forwarded scalar or pair instruction is dropped. When exactly one
 * half of an `LDP` remains, it is rewritten as the corresponding scalar load
 * at the original or `+8` offset.
 *
 * @param[in,out] block Join block whose prefix is rebuilt in place.
 * @param loads Descriptors previously collected from @p block.
 * @param forwardedLoads Indices into @p loads selected for forwarding.
 */
static void removeForwardedJoinLoads(MBasicBlock &block,
                                     const std::vector<JoinLoad> &loads,
                                     const std::unordered_set<std::size_t> &forwardedLoads) {
    std::unordered_map<std::size_t, std::vector<std::size_t>> loadsByInstr;
    for (std::size_t loadIndex = 0; loadIndex < loads.size(); ++loadIndex)
        loadsByInstr[loads[loadIndex].instrIndex].push_back(loadIndex);

    std::vector<MInstr> rewritten;
    rewritten.reserve(block.instrs.size());
    for (std::size_t instrIndex = 0; instrIndex < block.instrs.size(); ++instrIndex) {
        const auto loadIt = loadsByInstr.find(instrIndex);
        if (loadIt == loadsByInstr.end()) {
            rewritten.push_back(block.instrs[instrIndex]);
            continue;
        }

        const auto &instr = block.instrs[instrIndex];
        const auto &indices = loadIt->second;
        if (instr.opc == MOpcode::LdrRegFpImm || instr.opc == MOpcode::LdrFprFpImm) {
            if (indices.empty() || forwardedLoads.count(indices.front()) == 0)
                rewritten.push_back(instr);
            continue;
        }

        if ((instr.opc == MOpcode::LdpRegFpImm || instr.opc == MOpcode::LdpFprFpImm) &&
            indices.size() == 2 && instr.ops.size() >= 3 &&
            instr.ops[2].kind == MOperand::Kind::Imm) {
            const bool firstForwarded = forwardedLoads.count(indices[0]) != 0;
            const bool secondForwarded = forwardedLoads.count(indices[1]) != 0;
            if (firstForwarded && secondForwarded)
                continue;
            if (!firstForwarded && !secondForwarded) {
                rewritten.push_back(instr);
                continue;
            }

            const bool keepSecond = firstForwarded;
            const MOpcode loadOpcode =
                instr.opc == MOpcode::LdpFprFpImm ? MOpcode::LdrFprFpImm : MOpcode::LdrRegFpImm;
            const MOperand dst = instr.ops[keepSecond ? 1 : 0];
            const int64_t offset = instr.ops[2].imm + (keepSecond ? 8 : 0);
            rewritten.push_back(
                MInstr{loadOpcode, {dst, MOperand::immOp(static_cast<long long>(offset))}});
            continue;
        }

        rewritten.push_back(instr);
    }
    block.instrs.swap(rewritten);
}

/**
 * @brief Adds every physical register clobbered by an instruction to a set.
 *
 * Paired loads define two registers, calls conservatively define all
 * caller-saved GPRs and FPRs, and ordinary instructions use the common
 * defined-register query.
 *
 * @param instr Instruction to classify.
 * @param[in,out] clobbered Encoded register-key set to extend.
 */
[[maybe_unused]] static void markInstructionDefs(const MInstr &instr,
                                                 std::unordered_set<std::uint32_t> &clobbered) {
    if (instr.opc == MOpcode::LdpRegFpImm || instr.opc == MOpcode::LdpFprFpImm) {
        if (instr.ops.size() >= 1 && ph::isPhysReg(instr.ops[0]))
            clobbered.insert(regKey(instr.ops[0]));
        if (instr.ops.size() >= 2 && ph::isPhysReg(instr.ops[1]))
            clobbered.insert(regKey(instr.ops[1]));
        return;
    }

    if (instr.opc == MOpcode::Bl || instr.opc == MOpcode::Blr) {
        for (uint16_t reg = static_cast<uint16_t>(PhysReg::X0);
             reg <= static_cast<uint16_t>(PhysReg::X18);
             ++reg)
            clobbered.insert((static_cast<std::uint32_t>(RegClass::GPR) << 16) | reg);
        for (uint16_t reg = static_cast<uint16_t>(PhysReg::V0);
             reg <= static_cast<uint16_t>(PhysReg::V31);
             ++reg)
            clobbered.insert((static_cast<std::uint32_t>(RegClass::FPR) << 16) | reg);
        return;
    }

    if (auto def = ph::getDefinedReg(instr); def && ph::isPhysReg(*def))
        clobbered.insert(regKey(*def));
}

/**
 * @brief Tests whether a block set contains a direct or indirect call.
 *
 * @param fn Function containing the indexed blocks.
 * @param blocks Block indices to inspect.
 * @return `true` on a `Bl`/`Blr` or on any out-of-range index, which is treated
 *         conservatively as unsafe; otherwise `false`.
 */
static bool blocksContainCall(const MFunction &fn, const std::vector<std::size_t> &blocks) {
    for (std::size_t blockIndex : blocks) {
        if (blockIndex >= fn.blocks.size())
            return true;
        for (const auto &instr : fn.blocks[blockIndex].instrs) {
            if (instr.opc == MOpcode::Bl || instr.opc == MOpcode::Blr)
                return true;
        }
    }
    return false;
}

/**
 * @brief Forwards complete join-load prefixes from a unique acyclic predecessor.
 *
 * Every leading join load must match a same-class trailing store. The pass
 * orders replacement copies, records their sources as carried live-out values,
 * removes the loads, and updates dead-instruction statistics.
 *
 * @param[in,out] fn Function whose predecessor/join pairs are rewritten.
 * @param[in,out] stats Counters updated for removed load instructions.
 * @return `true` if at least one join block changed.
 */
static bool forwardSinglePredPhiLoads(MFunction &fn, PeepholeStats &stats) {
    bool changed = false;
    // The rewrites below replace join loads with moves and append no
    // terminators, so one CFG snapshot stays valid for the whole pass.
    const MirCfg cfg(fn);

    for (std::size_t blockIndex = 0; blockIndex < fn.blocks.size(); ++blockIndex) {
        auto &block = fn.blocks[blockIndex];
        std::vector<JoinLoad> loads;
        if (!collectJoinPrefixLoads(block, loads))
            continue;

        const auto &preds = cfg.preds(blockIndex);
        if (preds.size() != 1)
            continue;

        const std::size_t predIndex = preds.front();
        if (predIndex >= blockIndex)
            continue;
        if (!cfg.exitsDirectlyTo(fn, predIndex, blockIndex))
            continue;

        std::unordered_map<int64_t, JoinStore> stores;
        std::unordered_set<std::size_t> ignoredStoreInstrs;
        if (!collectJoinSuffixStores(fn, cfg, predIndex, blockIndex, stores, ignoredStoreInstrs))
            continue;

        std::vector<JoinCopy> copies;
        copies.reserve(loads.size());
        for (const auto &load : loads) {
            auto storeIt = stores.find(load.offset);
            if (storeIt == stores.end() || storeIt->second.cls != load.cls) {
                copies.clear();
                break;
            }
            copies.push_back({storeIt->second.srcReg, load.dstReg, load.cls});
        }
        if (copies.empty())
            continue;

        std::vector<JoinCopy> ordered;
        if (!orderJoinCopies(copies, ordered))
            continue;

        // Removing the successor loads makes every stored source register a
        // live-in to this block, including identity copies omitted by the move
        // ordering helper.
        for (const auto &copy : copies)
            markCarriedExitReg(fn.blocks[predIndex], copy.srcReg);

        std::vector<bool> removeLoadInstr(block.instrs.size(), false);
        for (const auto &load : loads)
            removeLoadInstr[load.instrIndex] = true;

        std::vector<MInstr> rewritten;
        rewritten.reserve(block.instrs.size() -
                          std::count(removeLoadInstr.begin(), removeLoadInstr.end(), true) +
                          ordered.size());
        bool insertedMoves = false;
        for (std::size_t ii = 0; ii < block.instrs.size(); ++ii) {
            if (removeLoadInstr[ii]) {
                if (!insertedMoves) {
                    for (const auto &copy : ordered) {
                        rewritten.push_back(
                            MInstr{copy.cls == RegClass::FPR ? MOpcode::FMovRR : MOpcode::MovRR,
                                   {copy.dstReg, copy.srcReg}});
                    }
                    insertedMoves = true;
                }
                continue;
            }
            rewritten.push_back(block.instrs[ii]);
        }
        block.instrs.swap(rewritten);
        stats.deadInstructionsRemoved +=
            static_cast<int>(std::count(removeLoadInstr.begin(), removeLoadInstr.end(), true));
        changed = true;
    }

    return changed;
}

/**
 * @brief Coalesces selected phi-entry loads across every predecessor of a join.
 *
 * A load is eligible only when all incoming edges provide a same-class store
 * for its frame offset and permit edge-local copy insertion. Copy cycles cause
 * progressively fewer loads to be selected. Call-containing natural-loop
 * headers are skipped because their extended physical live ranges are unsafe.
 *
 * @param[in,out] fn Function whose predecessor edges and join prefixes are rewritten.
 * @param[in,out] stats Counters updated for eliminated join loads.
 * @return `true` if at least one multi-predecessor join changed.
 */
static bool coalesceJoinPhiLoads(MFunction &fn, PeepholeStats &stats) {
    bool changed = false;
    // Copies are inserted before predecessor terminators and join loads are
    // replaced in place; no edge changes, so one CFG snapshot serves the pass.
    const MirCfg cfg(fn);

    for (std::size_t blockIndex = 0; blockIndex < fn.blocks.size(); ++blockIndex) {
        auto &block = fn.blocks[blockIndex];
        std::vector<JoinLoad> loads;
        if (!collectJoinPrefixLoads(block, loads))
            continue;

        const auto &preds = cfg.preds(blockIndex);
        if (preds.size() < 2)
            continue;

        // Loop headers are safe to coalesce only while their natural loop is
        // call-free. If the loop body calls out, the generic join rewrite can
        // keep loop-carried values only in physical registers across complex
        // call-heavy paths without proving every rewritten register live range.
        bool unsafeLoopHeader = false;
        for (std::size_t predIndex : preds) {
            if (predIndex < blockIndex)
                continue;
            if (!cfg.dominates(blockIndex, predIndex))
                continue;
            const NaturalLoop loop = cfg.naturalLoop(BackEdge{predIndex, blockIndex});
            if (blocksContainCall(fn, loop.blocks)) {
                unsafeLoopHeader = true;
                break;
            }
        }
        if (unsafeLoopHeader)
            continue;

        struct PredStorePlan {
            std::size_t predIndex{0};                      ///< Predecessor block index.
            std::unordered_map<int64_t, JoinStore> stores; ///< Stores keyed by FP offset.
        };

        std::vector<PredStorePlan> predStorePlans;
        predStorePlans.reserve(preds.size());
        bool allPredsConvertible = true;
        for (std::size_t predIndex : preds) {
            if (!cfg.exitsDirectlyTo(fn, predIndex, blockIndex)) {
                allPredsConvertible = false;
                break;
            }

            std::unordered_map<int64_t, JoinStore> stores;
            std::unordered_set<std::size_t> ignoredStoreInstrs;
            if (!collectJoinSuffixStores(
                    fn, cfg, predIndex, blockIndex, stores, ignoredStoreInstrs)) {
                allPredsConvertible = false;
                break;
            }
            predStorePlans.push_back({predIndex, std::move(stores)});
        }

        if (!allPredsConvertible || predStorePlans.empty())
            continue;

        std::vector<std::size_t> selectedLoads;
        selectedLoads.reserve(loads.size());
        for (std::size_t loadIndex = 0; loadIndex < loads.size(); ++loadIndex) {
            const auto &load = loads[loadIndex];
            bool availableOnEveryPred = true;
            for (const auto &plan : predStorePlans) {
                const auto storeIt = plan.stores.find(load.offset);
                if (storeIt == plan.stores.end() || storeIt->second.cls != load.cls) {
                    availableOnEveryPred = false;
                    break;
                }
            }
            if (availableOnEveryPred)
                selectedLoads.push_back(loadIndex);
        }

        if (selectedLoads.empty())
            continue;

        std::vector<std::vector<JoinCopy>> predCopies;
        predCopies.reserve(predStorePlans.size());
        /**
         * @brief Builds and orders the selected copies independently for every predecessor.
         *
         * @param loadIndices Indices into `loads` to attempt on all edges.
         * @return `true` when every edge provides and safely orders every copy.
         */
        const auto buildPredCopies = [&](const std::vector<std::size_t> &loadIndices) {
            predCopies.clear();
            predCopies.reserve(predStorePlans.size());
            if (loadIndices.empty())
                return false;
            for (const auto &plan : predStorePlans) {
                std::vector<JoinCopy> copies;
                copies.reserve(loadIndices.size());
                for (std::size_t loadIndex : loadIndices) {
                    const auto &load = loads[loadIndex];
                    auto storeIt = plan.stores.find(load.offset);
                    if (storeIt == plan.stores.end() || storeIt->second.cls != load.cls)
                        return false;
                    copies.push_back({storeIt->second.srcReg, load.dstReg, load.cls});
                }

                std::vector<JoinCopy> ordered;
                if (!orderJoinCopies(copies, ordered))
                    return false;
                predCopies.push_back(std::move(ordered));
            }
            return true;
        };

        while (!selectedLoads.empty() && !buildPredCopies(selectedLoads))
            selectedLoads.pop_back();

        if (selectedLoads.empty() || predCopies.empty())
            continue;

        for (std::size_t pi = 0; pi < predStorePlans.size(); ++pi) {
            auto &predBlock = fn.blocks[predStorePlans[pi].predIndex];
            const bool branchesDirectly =
                !predBlock.instrs.empty() && predBlock.instrs.back().opc == MOpcode::Br &&
                predBlock.instrs.back().ops.size() == 1 &&
                predBlock.instrs.back().ops[0].kind == MOperand::Kind::Label &&
                predBlock.instrs.back().ops[0].label == block.name;
            const std::size_t insertAt =
                branchesDirectly ? predBlock.instrs.size() - 1 : predBlock.instrs.size();

            std::vector<MInstr> rewritten;
            rewritten.reserve(predBlock.instrs.size() + predCopies[pi].size());
            for (std::size_t ii = 0; ii < predBlock.instrs.size(); ++ii) {
                if (ii == insertAt) {
                    for (const auto &copy : predCopies[pi]) {
                        rewritten.push_back(
                            MInstr{copy.cls == RegClass::FPR ? MOpcode::FMovRR : MOpcode::MovRR,
                                   {copy.dstReg, copy.srcReg}});
                    }
                }
                rewritten.push_back(predBlock.instrs[ii]);
            }
            if (insertAt == predBlock.instrs.size()) {
                for (const auto &copy : predCopies[pi]) {
                    rewritten.push_back(
                        MInstr{copy.cls == RegClass::FPR ? MOpcode::FMovRR : MOpcode::MovRR,
                               {copy.dstReg, copy.srcReg}});
                }
            }
            predBlock.instrs.swap(rewritten);

            // The join now consumes each load destination directly from this
            // edge. Preserve the copy (or an identity-carried value) through
            // post-schedule block-local cleanup.
            for (std::size_t loadIndex : selectedLoads)
                markCarriedExitReg(predBlock, loads[loadIndex].dstReg);
        }

        const std::unordered_set<std::size_t> forwardedLoadSet(selectedLoads.begin(),
                                                               selectedLoads.end());
        removeForwardedJoinLoads(block, loads, forwardedLoadSet);
        stats.deadInstructionsRemoved += static_cast<int>(forwardedLoadSet.size());
        changed = true;
    }

    return changed;
}

/**
 * @brief Forwards matching scalar GPR stores/loads between adjacent layout blocks.
 *
 * For a uniquely reached successor, matching trailing predecessor stores and
 * leading successor loads are replaced by a topologically safe move prefix.
 * Cyclic subsets retain their loads. Forwarded source registers are recorded
 * as carried across the predecessor exit.
 *
 * @param[in,out] fn Function whose adjacent blocks are examined and rewritten.
 * @param[in,out] stats Counters updated for non-identity replacement moves.
 */
static void forwardLayoutSuccessorStoreLoad(MFunction &fn, PeepholeStats &stats) {
    // Loads become moves and stores stay; no edge changes, so one CFG
    // snapshot serves the pass.
    const MirCfg cfg(fn);

    for (std::size_t bi = 0; bi + 1 < fn.blocks.size(); ++bi) {
        auto &predInstrs = fn.blocks[bi].instrs;
        auto &succBlock = fn.blocks[bi + 1];
        auto &succInstrs = succBlock.instrs;

        if (predInstrs.empty() || succInstrs.empty())
            continue;

        // Only forward to blocks with exactly one real predecessor — including
        // fallthrough edges — otherwise short-circuit boolean joins can be
        // miscompiled by forwarding only one incoming value. The layout
        // predecessor must actually reach the successor (a block ending in a
        // branch elsewhere, a return, a jump table, or a no-return call does
        // not).
        const auto &preds = cfg.preds(bi + 1);
        if (preds.size() != 1 || preds.front() != bi || !cfg.hasEdge(bi, bi + 1))
            continue;

        // Collect trailing FP-relative stores in the predecessor.
        struct StoreInfo {
            std::size_t idx{0}; ///< Store instruction index in the predecessor block.
            MOperand srcReg{};  ///< Stored source register.
        };

        std::unordered_map<int64_t, StoreInfo> endStores;

        for (std::size_t i = predInstrs.size(); i-- > 0;) {
            const auto &instr = predInstrs[i];

            if (instr.opc == MOpcode::Br || instr.opc == MOpcode::BCond ||
                instr.opc == MOpcode::Ret || instr.opc == MOpcode::Cbz ||
                instr.opc == MOpcode::Cbnz || instr.opc == MOpcode::Tbz ||
                instr.opc == MOpcode::Tbnz || instr.opc == MOpcode::JumpTable)
                continue;

            if (instr.opc == MOpcode::StrRegFpImm && instr.ops.size() >= 2 &&
                ph::isPhysReg(instr.ops[0]) && instr.ops[1].kind == MOperand::Kind::Imm) {
                const int64_t off = instr.ops[1].imm;
                endStores.try_emplace(off, StoreInfo{i, instr.ops[0]});
                continue;
            }

            break; // Non-store, non-terminator: stop scanning backward.
        }

        if (endStores.empty())
            continue;

        struct PrefixLoad {
            std::size_t idx{0}; ///< Load instruction index in the successor block.
            MInstr instr{};     ///< Load instruction to replace when forwarding succeeds.
        };

        struct ForwardPair {
            std::size_t idx{0}; ///< Successor load index to rewrite.
            MOperand dstReg{};  ///< Destination loaded register.
            MOperand srcReg{};  ///< Predecessor stored register.
        };

        std::vector<PrefixLoad> prefixLoads;
        for (std::size_t j = 0; j < succInstrs.size(); ++j) {
            const auto &instr = succInstrs[j];
            if (instr.opc != MOpcode::LdrRegFpImm)
                break;
            if (instr.ops.size() < 2 || !ph::isPhysReg(instr.ops[0]) ||
                instr.ops[1].kind != MOperand::Kind::Imm)
                break;
            prefixLoads.push_back({j, instr});
        }
        if (prefixLoads.empty())
            continue;

        std::vector<ForwardPair> pending;
        std::unordered_set<std::size_t> forwardedIdx;
        pending.reserve(prefixLoads.size());
        for (const auto &load : prefixLoads) {
            const int64_t off = load.instr.ops[1].imm;
            auto it = endStores.find(off);
            if (it == endStores.end())
                continue;
            pending.push_back({load.idx, load.instr.ops[0], it->second.srcReg});
        }
        if (pending.empty())
            continue;

        // Greedy topological ordering — a destination cannot be overwritten
        // before its value has been read elsewhere in the move set.
        std::vector<ForwardPair> ordered;
        ordered.reserve(pending.size());
        while (!pending.empty()) {
            auto readyIt = pending.end();
            for (auto it = pending.begin(); it != pending.end(); ++it) {
                const auto dstPhys = it->dstReg.reg.idOrPhys;
                bool dstUsedAsSource = false;
                for (auto jt = pending.begin(); jt != pending.end(); ++jt) {
                    if (it == jt)
                        continue;
                    if (jt->srcReg.reg.idOrPhys == dstPhys) {
                        dstUsedAsSource = true;
                        break;
                    }
                }
                if (!dstUsedAsSource) {
                    readyIt = it;
                    break;
                }
            }
            if (readyIt == pending.end())
                break; // Remaining copies form a cycle; keep their loads.
            forwardedIdx.insert(readyIt->idx);
            ordered.push_back(*readyIt);
            pending.erase(readyIt);
        }

        if (ordered.empty())
            continue;

        // The replacement moves execute in the successor, so their source
        // registers are newly live across the predecessor's exit. This also
        // covers identity pairs, for which no explicit move is emitted.
        for (const auto &pair : ordered)
            markCarriedExitReg(fn.blocks[bi], pair.srcReg);

        std::vector<MInstr> newPrefix;
        newPrefix.reserve(prefixLoads.size());
        for (const auto &pair : ordered) {
            if (pair.dstReg.reg.idOrPhys == pair.srcReg.reg.idOrPhys)
                continue;
            newPrefix.push_back(MInstr{MOpcode::MovRR, {pair.dstReg, pair.srcReg}});
            ++stats.deadInstructionsRemoved;
        }
        for (const auto &load : prefixLoads) {
            if (forwardedIdx.count(load.idx))
                continue;
            newPrefix.push_back(load.instr);
        }

        succInstrs.erase(succInstrs.begin(),
                         succInstrs.begin() +
                             static_cast<std::ptrdiff_t>(prefixLoads.back().idx + 1));
        succInstrs.insert(succInstrs.begin(), newPrefix.begin(), newPrefix.end());
    }
}

/**
 * @brief Runs the ordered local rewrite sequence over every non-empty block.
 *
 * The sequence covers division/remainder strength reduction, constant-aware
 * rewrites, copy propagation, branch/arithmetic/load-store fusion, local
 * memory forwarding, move folding, identity removal, optional local DCE, and
 * dead flag-setter elimination.
 *
 * @param[in,out] fn Physical-register MIR function to rewrite.
 * @param[in,out] stats Transformation counters shared across sub-passes.
 * @param target Non-null when CFG-aware DCE will run separately; `nullptr`
 *        enables the legacy per-block dead-instruction pass here.
 */
static void runPerBlockRewrites(MFunction &fn, PeepholeStats &stats, const TargetInfo *target) {
    // Folds that redirect or drop a definition reaching the block end need
    // the ABI (call/return effects) and the block's exit-live set. Direct
    // unit tests may run without a target; AAPCS64 (the Darwin singleton) is
    // the common denominator.
    const TargetInfo &effectiveTarget = target != nullptr ? *target : darwinTarget();

    for (std::size_t bi = 0; bi < fn.blocks.size(); ++bi) {
        auto &block = fn.blocks[bi];
        auto &instrs = block.instrs;
        if (instrs.empty())
            continue;

        const PhysRegSet exitLive = blockExitLive(fn, bi, effectiveTarget);

        // Pass 0.9: Division/remainder strength reduction (multi-instruction patterns).
        // Must run BEFORE Pass 1's single-instruction strength reduction, because
        // Pass 1 converts UDIV->LSR which would break the UDIV+MSUB remainder
        // pattern. Remainder fusion must see the original UDIV/SDIV.
        {
            bool changed = true;
            while (changed) {
                changed = false;
                ph::RegConstMap divConsts;
                for (std::size_t i = 0; i + 1 < instrs.size(); ++i) {
                    if (ph::tryRemainderFusion(
                            instrs, i, divConsts, stats, &block.carriedExitRegs)) {
                        changed = true;
                        break;
                    }
                    ph::updateKnownConsts(instrs[i], divConsts);
                }
            }

            changed = true;
            while (changed) {
                changed = false;
                ph::RegConstMap divConsts;
                for (std::size_t i = 0; i < instrs.size(); ++i) {
                    bool localChange = false;
                    if (instrs[i].opc == MOpcode::UDivRRR)
                        localChange = ph::tryUDivStrengthReduction(
                            instrs, i, divConsts, stats, &block.carriedExitRegs);
                    else if (instrs[i].opc == MOpcode::SDivRRR)
                        localChange = ph::trySDivStrengthReduction(
                            instrs, i, divConsts, stats, &block.carriedExitRegs);

                    if (localChange) {
                        changed = true;
                        break;
                    }
                    ph::updateKnownConsts(instrs[i], divConsts);
                }
            }
        }

        // Pass 1: Single-instruction constant-aware rewrites.
        ph::RegConstMap knownConsts;
        for (auto &instr : instrs) {
            ph::updateKnownConsts(instr, knownConsts);
            if (ph::tryCmpZeroToTst(instr, stats))
                continue;
            if (ph::tryArithmeticIdentity(instr, stats))
                continue;
            (void)ph::tryStrengthReduction(instr, knownConsts, stats);
            (void)ph::tryDivStrengthReduction(instr, knownConsts, stats);
            (void)ph::tryImmediateFolding(instr, knownConsts, stats);
        }

        // Pass 1.5: Copy propagation.
        ph::propagateCopies(instrs, stats);

        // Pass 1.6 / 1.65 / 1.7 / 1.8: instruction-level fusions.
        for (std::size_t i = 0; i + 1 < instrs.size(); ++i) {
            if (ph::tryCbzCbnzFusion(instrs, i, stats) && i > 0)
                --i;
        }
        for (std::size_t i = 0; i + 1 < instrs.size(); ++i) {
            if (ph::tryTbzTbnzFusion(instrs, i, stats, &block.carriedExitRegs) && i > 0)
                --i;
        }
        for (std::size_t i = 0; i < instrs.size(); ++i) {
            if (ph::tryCsetBranchFusion(instrs, i, stats, &block.carriedExitRegs) && i > 0)
                --i;
        }
        for (std::size_t i = 0; i + 1 < instrs.size(); ++i) {
            if (ph::tryMaddFusion(instrs, i, stats, effectiveTarget, exitLive) && i > 0)
                --i;
        }
        for (std::size_t i = 0; i + 1 < instrs.size(); ++i) {
            if (ph::tryLdpStpMerge(instrs, i, stats) && i > 0)
                --i;
        }

        // Pass 1.85 / 1.9 / 1.97: local store/load shuffling.
        ph::eliminateDeadFpStores(instrs, stats);
        ph::forwardStoreLoads(instrs, stats);
        ph::foldComputeIntoTarget(instrs, stats, effectiveTarget, exitLive);

        // Pass 2: Fold consecutive moves.
        for (std::size_t i = 0; i + 1 < instrs.size(); ++i) {
            if (!ph::tryFoldImmThenMove(instrs, i, stats, &block.carriedExitRegs))
                (void)ph::tryFoldConsecutiveMoves(instrs, i, stats, &block.carriedExitRegs);
        }

        // Pass 3+4: Mark and remove identity moves.
        std::vector<bool> toRemove(instrs.size(), false);
        for (std::size_t i = 0; i < instrs.size(); ++i) {
            if (ph::isIdentityMovRR(instrs[i])) {
                toRemove[i] = true;
                ++stats.identityMovesRemoved;
            } else if (ph::isIdentityFMovRR(instrs[i])) {
                toRemove[i] = true;
                ++stats.identityFMovesRemoved;
            }
        }
        /// @brief Tests whether an instruction has been marked for removal.
        /// @param v Removal marker.
        /// @return The marker value.
        if (std::any_of(toRemove.begin(), toRemove.end(), [](bool v) { return v; }))
            ph::removeMarkedInstructions(instrs, toRemove);

        // Pass 4.5: Local DCE. The modular pipeline runs the CFG-aware variant
        // post-block; direct unit tests (no target) keep the legacy per-block path.
        if (target == nullptr)
            ph::removeDeadInstructions(instrs, stats, &block.carriedExitRegs);

        // Pass 4.6: Dead flag-setter elimination AFTER DCE so dead readers of
        // flags are gone before we judge a flag-setter unused.
        ph::removeDeadFlagSetters(instrs, stats);
    }
}

/**
 * @brief Applies final branch inversion and branch-to-next cleanup.
 *
 * A conditional branch to the next layout block followed by an unconditional
 * branch is inverted and collapsed when the condition has a known inverse.
 * Remaining unconditional branches to the next block are removed.
 *
 * @param[in,out] fn Function whose block tails are simplified.
 * @param[in,out] stats Branch inversion/removal counters to update.
 */
static void runBranchInversionAndCleanup(MFunction &fn, PeepholeStats &stats) {
    for (std::size_t bi = 0; bi + 1 < fn.blocks.size(); ++bi) {
        auto &block = fn.blocks[bi];
        const auto &nextBlock = fn.blocks[bi + 1];

        if (block.instrs.empty())
            continue;

        // Branch inversion: b.cond .Ltarget; b .Lfallthrough
        // when .Ltarget == next block -> b.!cond .Lfallthrough (remove b .Lfallthrough)
        if (block.instrs.size() >= 2) {
            auto &secondLast = block.instrs[block.instrs.size() - 2];
            auto &last = block.instrs[block.instrs.size() - 1];

            if (secondLast.opc == MOpcode::BCond && secondLast.ops.size() == 2 &&
                secondLast.ops[0].kind == MOperand::Kind::Cond &&
                secondLast.ops[1].kind == MOperand::Kind::Label && last.opc == MOpcode::Br &&
                last.ops.size() == 1 && last.ops[0].kind == MOperand::Kind::Label) {
                if (secondLast.ops[1].label == nextBlock.name) {
                    const char *inv = ph::invertCondition(secondLast.ops[0].cond);
                    if (inv) {
                        secondLast.ops[0] = MOperand::condOp(inv);
                        secondLast.ops[1] = last.ops[0];
                        block.instrs.pop_back();
                        ++stats.branchInversions;
                        continue;
                    }
                }
            }
        }

        // Remove branches to the immediately following block.
        if (ph::isBranchTo(block.instrs.back(), nextBlock.name)) {
            block.instrs.pop_back();
            ++stats.branchesToNextRemoved;
        }
    }
}

} // namespace

/**
 * @brief Runs the full ordered peephole pipeline on allocated AArch64 MIR.
 *
 * @param[in,out] fn Function to optimize in place.
 * @param target Optional target metadata enabling CFG-aware dead-code removal.
 * @return Aggregate counters populated by every invoked sub-pass.
 * @pre Register operands have been assigned physical registers.
 */
/// @brief Triage kill switch for one peephole sub-stage (`ZANNA_NO_PH_<NAME>=1`).
/// @details Names: REORDER, LOOPHOIST, PERBLOCK, DCE_CFG, FPSTORES,
///          STORELOAD_FWD, PHI_LOADS, PHI_SPILLS, BRANCH. A bisection aid
///          against a program-level oracle; the stage-level switches live in
///          CodegenPipeline.cpp (backendStageDisabled). Never consulted at -O0.
static bool peepholeStageDisabled(const char *name) {
    std::string key = "ZANNA_NO_PH_";
    key += name;
    return std::getenv(key.c_str()) != nullptr;
}

PeepholeStats runPeephole(MFunction &fn, const TargetInfo *target) {
    PeepholeStats stats;

    // Pass 0: Reorder blocks for better code layout
    if (!peepholeStageDisabled("REORDER"))
        stats.blocksReordered = static_cast<int>(ph::reorderBlocks(fn));

    // Pass 0.5: Hoist loop-invariant MovRI out of loop bodies.
    // LoopOpt now rejects merge-like headers, non-preheader entries, and uses
    // that can observe the value before a dominating definition inside the loop.
    if (!peepholeStageDisabled("LOOPHOIST"))
        stats.loopConstsHoisted = static_cast<int>(ph::hoistLoopConstants(fn));

    // Passes 0.9 through 4.6: local per-block rewrites (division strength reduction,
    // constant-aware rewrites, fusions, identity removal, local DCE/flag DCE).
    // (Pass 0.6 — loop phi spill elimination — runs after Pass 4.8 below.)
    if (!peepholeStageDisabled("PERBLOCK"))
        runPerBlockRewrites(fn, stats, target);

    if (target != nullptr && !peepholeStageDisabled("DCE_CFG")) {
        ph::removeDeadInstructionsCFG(fn, stats, *target);
        for (auto &block : fn.blocks)
            ph::removeDeadFlagSetters(block.instrs, stats);
    }

    const bool fpStores = !peepholeStageDisabled("FPSTORES");
    if (fpStores)
        ph::eliminateDeadFpStoresCrossBlock(fn, stats);

    // Pass 4.8: Cross-block store-load forwarding for phi stores/loads.
    // Forwards single-predecessor join blocks; multi-predecessor joins handled
    // by passes 4.86 / 4.88 below.
    if (!peepholeStageDisabled("STORELOAD_FWD"))
        forwardLayoutSuccessorStoreLoad(fn, stats);

    // Pass 4.86: Forward single-predecessor phi-entry loads from predecessor
    // edge stores when the edge is acyclic and the source register survives to
    // the successor. This collapses direct join reloads without touching
    // loop-carried back-edges.
    if (!peepholeStageDisabled("PHI_LOADS") && forwardSinglePredPhiLoads(fn, stats) && fpStores)
        ph::eliminateDeadFpStoresCrossBlock(fn, stats);

    // Pass 4.88: Coalesce multi-predecessor join-entry phi loads into
    // predecessor register moves when every incoming edge already materializes
    // the values in physical registers before branching to the join. This cuts
    // stack round-trips that remain after the single-predecessor forwarding pass.
    if (!peepholeStageDisabled("PHI_LOADS") && coalesceJoinPhiLoads(fn, stats) && fpStores)
        ph::eliminateDeadFpStoresCrossBlock(fn, stats);

    // (Pass 4.85 moved to Pass 0.7 — runs before per-block loop above)

    // Pass 4.9: Eliminate redundant phi-slot spill/reload cycles in loop back-edges.
    // Must run AFTER Pass 4.8 (cross-block store-load forwarding) because that pass
    // may convert phi loads to register movs, changing the header's instruction mix.
    // Running after 4.8 ensures we see the final form of the header instructions.
    // Loop phi-spill elimination reasons about call clobbers, so it needs an
    // ABI description; direct unit tests may run without one, in which case
    // AAPCS64 (the Darwin singleton) is the common denominator.
    const TargetInfo &effectiveTarget = target != nullptr ? *target : darwinTarget();
    for (int iter = 0; iter < 16 && !peepholeStageDisabled("PHI_SPILLS"); ++iter) {
        const auto eliminated = ph::eliminateLoopPhiSpills(fn, effectiveTarget);
        if (eliminated == 0)
            break;
        stats.loopConstsHoisted += static_cast<int>(eliminated);
        if (fpStores)
            ph::eliminateDeadFpStoresCrossBlock(fn, stats);
    }

    // Pass 4.95: Re-run cross-block dead spill-store elimination after forwarding
    // and loop phi cleanup. Pass 4.8 can replace the only remaining reload from a
    // phi slot with a register move, which leaves the predecessor spill dead only
    // after the earlier Pass 0.7 has already run.
    if (fpStores)
        ph::eliminateDeadFpStoresCrossBlock(fn, stats);

    // Pass 5: Branch inversion and branch-to-next removal.
    if (!peepholeStageDisabled("BRANCH"))
        runBranchInversionAndCleanup(fn, stats);

    return stats;
}

/**
 * @brief Runs local cleanup and branch simplification after instruction scheduling.
 *
 * @param[in,out] fn Scheduled function to clean without cross-block reshaping.
 * @param target Optional target metadata; when null, local DCE is performed.
 * @return Counters for transformations applied by this reduced pass set.
 */
PeepholeStats runPostSchedulePeephole(MFunction &fn, const TargetInfo *target) {
    PeepholeStats stats;

    for (auto &block : fn.blocks) {
        auto &instrs = block.instrs;
        if (instrs.empty())
            continue;

        ph::propagateCopies(instrs, stats);

        for (std::size_t i = 0; i + 1 < instrs.size(); ++i) {
            if (!ph::tryFoldImmThenMove(instrs, i, stats, &block.carriedExitRegs))
                (void)ph::tryFoldConsecutiveMoves(instrs, i, stats, &block.carriedExitRegs);
        }

        std::vector<bool> toRemove(instrs.size(), false);
        for (std::size_t i = 0; i < instrs.size(); ++i) {
            if (ph::isIdentityMovRR(instrs[i])) {
                toRemove[i] = true;
                ++stats.identityMovesRemoved;
            } else if (ph::isIdentityFMovRR(instrs[i])) {
                toRemove[i] = true;
                ++stats.identityFMovesRemoved;
            }
        }
        /// @brief Tests whether an instruction has been marked for removal.
        /// @param v Removal marker.
        /// @return The marker value.
        if (std::any_of(toRemove.begin(), toRemove.end(), [](bool v) { return v; }))
            ph::removeMarkedInstructions(instrs, toRemove);

        if (target == nullptr)
            ph::removeDeadInstructions(instrs, stats, &block.carriedExitRegs);
        ph::removeDeadFlagSetters(instrs, stats);
    }

    runBranchInversionAndCleanup(fn, stats);

    return stats;
}

/**
 * @brief Prunes saved-register metadata to physical registers still referenced by MIR.
 *
 * @param[in,out] fn Function whose `savedGPRs` and `savedFPRs` vectors are filtered.
 * @post Each retained saved register occurs as a physical register operand.
 */
void pruneUnusedCalleeSaved(MFunction &fn) {
    // Build a set of all physical registers actually referenced in the MIR.
    std::unordered_set<uint16_t> usedRegs;
    for (const auto &bb : fn.blocks) {
        for (const auto &mi : bb.instrs) {
            for (const auto &op : mi.ops) {
                if (op.kind == MOperand::Kind::Reg && op.reg.isPhys)
                    usedRegs.insert(op.reg.idOrPhys);
            }
        }
    }

    // Prune savedGPRs: remove any callee-saved register not referenced.
    fn.savedGPRs.erase(std::remove_if(fn.savedGPRs.begin(),
                                      fn.savedGPRs.end(),
                                      /// @brief Tests whether a saved GPR is unused.
                                      /// @param r Physical register to inspect.
                                      /// @return `true` when no MIR operand references `r`.
                                      [&](PhysReg r) {
                                          return usedRegs.find(static_cast<uint16_t>(r)) ==
                                                 usedRegs.end();
                                      }),
                       fn.savedGPRs.end());

    // Prune savedFPRs: same logic.
    fn.savedFPRs.erase(std::remove_if(fn.savedFPRs.begin(),
                                      fn.savedFPRs.end(),
                                      /// @brief Tests whether a saved FPR is unused.
                                      /// @param r Physical register to inspect.
                                      /// @return `true` when no MIR operand references `r`.
                                      [&](PhysReg r) {
                                          return usedRegs.find(static_cast<uint16_t>(r)) ==
                                                 usedRegs.end();
                                      }),
                       fn.savedFPRs.end());
}

} // namespace zanna::codegen::aarch64
