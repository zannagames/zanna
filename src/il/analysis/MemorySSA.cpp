//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/analysis/MemorySSA.cpp
// Purpose: Implements the MemorySSA analysis. See MemorySSA.hpp for design
//          documentation.
//
// Algorithm overview
// ------------------
//
// For each non-escaping alloca A we track memory def-use chains precisely:
//
//   - Store to A  →  MemoryDef   (defines a new version of A's memory)
//   - Load from A →  MemoryUse   (consumes the reaching MemoryDef)
//   - Call        →  transparent for A (calls cannot access non-escaping stack)
//
// The "reaching def" for a use is the most recent MemoryDef that dominates
// the use in the CFG.  Rather than building full SSA form with dominance
// frontiers, we use an RPO-order forward dataflow:
//
//   currentDef[A] starts as LiveOnEntry (id=0).
//   At each store to A: create MemoryDef, update currentDef[A].
//   At each load from A: create MemoryUse pointing at currentDef[A].
//   At block joins:      take the union of incoming currentDef[A]; if they
//                        differ, insert a MemoryPhi.
//
// Dead-store detection
// --------------------
//
// A MemoryDef D is dead iff, on every control-flow path from D to any exit:
//   - some later MemoryDef for the same location overwrites D, OR
//   - the exit is reached without any MemoryUse consuming D.
//
// Equivalently: D is dead iff D has no MemoryUse consumers reachable before
// the next overwriting MemoryDef on any path.
//
// Implementation: after building def-use links we propagate "liveness" of
// each MemoryDef backward through the users list.  A MemoryDef with zero
// users is unconditionally dead.
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Implements MemorySSA graph construction and non-escaping-alloca DSE facts.
 *
 * @details The implementation builds a forward, coarse memory-version stream
 *          over reverse post-order, inserts phis where predecessor versions
 *          disagree, and associates loads, stores, and relevant calls with
 *          dense access nodes. A location-sensitive path search then proves
 *          dead stores to non-escaping allocas while treating calls as
 *          transparent to that private stack storage.
 */

#include "il/analysis/MemorySSA.hpp"

#include "il/analysis/AllocaRoots.hpp"
#include "il/analysis/BasicAA.hpp"
#include "il/analysis/CFG.hpp"
#include "il/core/Function.hpp"
#include "il/core/Instr.hpp"
#include "il/core/Opcode.hpp"

#include <algorithm>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace il::core;

namespace zanna::analysis {

// -------------------------------------------------------------------------
// MemorySSA query implementation
// -------------------------------------------------------------------------

/// @brief Query the precomputed dead-store set by source instruction position.
/// @param block Block containing the candidate instruction.
/// @param instrIdx Zero-based instruction index.
/// @return True when the mapped MemoryAccess ID was proven to be a dead store.
bool MemorySSA::isDeadStore(const Block *block, size_t instrIdx) const {
    auto bit = instrToAccess_.find(block);
    if (bit == instrToAccess_.end())
        return false;
    auto iit = bit->second.find(instrIdx);
    if (iit == bit->second.end())
        return false;
    return deadStoreIds_.count(iit->second) != 0;
}

/// @brief Look up a dense MemoryAccess node by source instruction position.
/// @param block Block containing the instruction or phi.
/// @param instrIdx Instruction index, with `size_t(-1)` reserved for a block phi.
/// @return Borrowed node pointer, or nullptr for an absent/invalid mapping.
const MemoryAccess *MemorySSA::accessFor(const Block *block, size_t instrIdx) const {
    auto bit = instrToAccess_.find(block);
    if (bit == instrToAccess_.end())
        return nullptr;
    auto iit = bit->second.find(instrIdx);
    if (iit == bit->second.end())
        return nullptr;
    uint32_t idx = iit->second;
    if (idx >= accesses_.size())
        return nullptr;
    return &accesses_[idx];
}

// -------------------------------------------------------------------------
// computeMemorySSA
// -------------------------------------------------------------------------

namespace {

/// @brief Test whether a pointer has exactly one proven non-escaping alloca root.
/// @param ptr Pointer value to classify.
/// @param nonEsc Set of non-escaping alloca result IDs.
/// @param roots Temporary-to-alloca-root propagation map.
/// @return True for a temporary whose unique root belongs to @p nonEsc.
inline bool isNonEscapingAlloca(const Value &ptr,
                                const std::unordered_set<unsigned> &nonEsc,
                                const AllocaRootMap &roots) {
    if (ptr.kind != Value::Kind::Temp)
        return false;
    auto rootIt = roots.find(ptr.id);
    if (rootIt == roots.end() || rootIt->second.size() != 1)
        return false;
    return nonEsc.count(*rootIt->second.begin()) != 0;
}

/// @brief Determine whether a later store completely covers an earlier store.
/// @param laterPtr Address written by the later store.
/// @param laterSize Known later-store width.
/// @param earlierPtr Address written by the earlier store.
/// @param earlierSize Known earlier-store width.
/// @param AA Alias analysis used to prove identical bases/offsets.
/// @return True when both sizes are known, the later write is at least as wide,
///         and the locations must alias.
bool fullyOverwrites(const Value &laterPtr,
                     std::optional<unsigned> laterSize,
                     const Value &earlierPtr,
                     std::optional<unsigned> earlierSize,
                     BasicAA &AA) {
    if (!laterSize || !earlierSize)
        return false;
    if (*laterSize < *earlierSize)
        return false;
    return AA.alias(laterPtr, earlierPtr, laterSize, earlierSize) == AliasResult::MustAlias;
}

/// @brief Detect exception-handling opcodes unsupported by this MemorySSA model.
/// @param F Function to scan.
/// @return True when any block contains an EH stack, entry, or resume opcode.
bool hasExceptionHandling(const Function &F) {
    for (const auto &B : F.blocks) {
        for (const auto &I : B.instructions) {
            switch (I.op) {
                case Opcode::EhPush:
                case Opcode::EhPop:
                case Opcode::EhEntry:
                case Opcode::ResumeSame:
                case Opcode::ResumeNext:
                case Opcode::ResumeLabel:
                    return true;
                default:
                    break;
            }
        }
    }
    return false;
}

/// @brief Compute a reverse-post-order block listing via DFS over the CFG
///        (successors follow terminator labels). Used to drive forward dataflow.
/// @param F Function whose blocks are ordered; must be non-empty.
/// @param labelToBlock Label lookup used to resolve terminator targets.
/// @return Every block once, with entry-reachable blocks in reverse post-order
///         followed by deterministic DFS coverage of unreachable components.
std::vector<Block *> buildReversePostOrder(
    Function &F, const std::unordered_map<std::string, Block *> &labelToBlock) {
    std::vector<Block *> rpo;
    std::unordered_set<Block *> visited;
    std::vector<Block *> postOrder;
    /// @brief Traverse one not-yet-covered CFG component in iterative depth-first order.
    /// @param start First block in the component to traverse.
    auto dfs = [&](Block *start) {
        if (!visited.insert(start).second)
            return;
        struct Frame {
            Block *block;
            std::size_t successorIndex;
        };
        std::vector<Frame> stack{{start, 0}};
        while (!stack.empty()) {
            Frame &frame = stack.back();
            const auto *labels = frame.block->instructions.empty()
                                     ? nullptr
                                     : &frame.block->instructions.back().labels;
            bool advanced = false;
            while (labels && frame.successorIndex < labels->size()) {
                const auto &label = (*labels)[frame.successorIndex++];
                auto succIt = labelToBlock.find(label);
                if (succIt != labelToBlock.end() && visited.insert(succIt->second).second) {
                    stack.push_back({succIt->second, 0});
                    advanced = true;
                    break;
                }
            }
            if (advanced)
                continue;
            postOrder.push_back(frame.block);
            stack.pop_back();
        }
    };
    dfs(&F.blocks.front());
    for (auto &block : F.blocks)
        dfs(&block);
    rpo.assign(postOrder.rbegin(), postOrder.rend());
    return rpo;
}

} // namespace

/// @brief Build coarse memory def-use nodes and precise non-escaping-alloca dead-store facts.
/// @param F Function to analyze; exception-handling functions produce an empty result.
/// @param AA Function-compatible alias and ModRef analysis.
/// @return Value-owned MemorySSA result referencing stable blocks in @p F.
MemorySSA computeMemorySSA(Function &F, BasicAA &AA) {
    MemorySSA mssa;

    if (F.blocks.empty() || hasExceptionHandling(F))
        return mssa;

    // LiveOnEntry sentinel at index 0.
    mssa.accesses_.push_back(MemoryAccess{MemAccessKind::LiveOnEntry, 0, nullptr, -1, 0, {}, {}});

    /// @brief Determine the dense identifier assigned to the next access node.
    /// @return Current node-table size converted to an access identifier.
    auto nextId = [&]() -> uint32_t { return static_cast<uint32_t>(mssa.accesses_.size()); };

    /// @brief Append and index an instruction-backed memory access.
    /// @param kind Role of the new graph node.
    /// @param block Block containing the represented instruction.
    /// @param instrIdx Zero-based instruction index, or -1 for a synthetic access.
    /// @param definingAccess Reaching memory version consumed by the new access.
    /// @return Dense identifier assigned to the appended node.
    auto makeAccess =
        [&](MemAccessKind kind, Block *block, int instrIdx, uint32_t definingAccess) -> uint32_t {
        uint32_t id = nextId();
        mssa.accesses_.push_back(MemoryAccess{kind, id, block, instrIdx, definingAccess, {}, {}});
        mssa.instrToAccess_[block][static_cast<size_t>(instrIdx)] = id;
        return id;
    };

    /// @brief Find the nearest possibly aliasing store earlier in one block.
    /// @param block Block containing the memory operation being analyzed.
    /// @param beforeIdx Exclusive upper instruction index for the backward search.
    /// @param ptr Address whose reaching definition is requested.
    /// @param size Known access width, or no value when the width is unknown.
    /// @return Indexed access ID for the nearest usable store, or no value when
    ///         no local definition can safely be selected.
    auto findLocalReachingDef = [&](Block *block,
                                    size_t beforeIdx,
                                    const Value &ptr,
                                    std::optional<unsigned> size) -> std::optional<uint32_t> {
        for (size_t j = beforeIdx; j-- > 0;) {
            const Instr &prev = block->instructions[j];
            if (prev.op == Opcode::Store && !prev.operands.empty()) {
                auto prevSize = BasicAA::typeSizeBytes(prev.type);
                if (AA.alias(prev.operands[0], ptr, prevSize, size) != AliasResult::NoAlias) {
                    auto bit = mssa.instrToAccess_.find(block);
                    if (bit == mssa.instrToAccess_.end())
                        return std::nullopt;
                    auto ait = bit->second.find(j);
                    if (ait == bit->second.end())
                        return std::nullopt;
                    return ait->second;
                }
            }
            if (prev.op == Opcode::Call || prev.op == Opcode::CallIndirect) {
                if (AA.modRef(prev) != ModRefResult::NoModRef)
                    return std::nullopt;
            }
        }
        return std::nullopt;
    };

    const auto defs = collectAllocaRootDefs(F);
    const auto roots = computeAllocaRoots(F, defs);

    // Collect non-escaping allocas — calls are transparent for these.
    const std::unordered_set<unsigned> nonEsc = nonEscapingAllocas(F, defs, roots);

    // Build label→Block* map for successor lookup.
    std::unordered_map<std::string, Block *> labelToBlock;
    for (auto &B : F.blocks)
        labelToBlock[B.label] = &B;

    // -----------------------------------------------------------------------
    // Phase 1: Compute RPO order for forward dataflow.
    // -----------------------------------------------------------------------
    std::vector<Block *> rpo = buildReversePostOrder(F, labelToBlock);

    // Build predecessor/successor maps for join-point phi insertion and
    // forward fixpoint propagation.
    std::unordered_map<Block *, std::vector<Block *>> preds;
    std::unordered_map<Block *, std::vector<Block *>> succs;
    // Cross-block path results depend only on the queried pointer/width and
    // successor block entry, not on the originating store. Reuse them for
    // repeated stores to the same location instead of traversing the CFG once
    // per store (a common quadratic case after frontend lowering).
    using PathMemo = std::unordered_map<std::string, bool>;
    std::unordered_map<std::uint64_t, PathMemo> pathMemoByLocation;

    for (auto &B : F.blocks) {
        if (!B.instructions.empty()) {
            for (const auto &label : B.instructions.back().labels) {
                auto it = labelToBlock.find(label);
                if (it != labelToBlock.end()) {
                    preds[it->second].push_back(&B);
                    succs[&B].push_back(it->second);
                }
            }
        }
    }

    // -----------------------------------------------------------------------
    // Phase 2: Forward dataflow — assign MemoryDef/Use, inserting Phis.
    //
    // currentDef[block] = the MemoryAccess id that represents the current
    // memory definition at the EXIT of that block.
    //
    // At the start of each block, we take the meet (phi-insert if needed)
    // of incoming currentDefs.
    // -----------------------------------------------------------------------

    // outDef[B] = id of the last MemoryAccess at the end of block B.
    std::unordered_map<Block *, uint32_t> outDef;

    // Initialize all blocks to LiveOnEntry (id=0).
    for (auto &B : F.blocks)
        outDef[&B] = 0;

    std::vector<Block *> worklist = rpo;
    std::unordered_set<Block *> queued;
    queued.reserve(rpo.size());
    for (Block *B : rpo)
        queued.insert(B);

    for (std::size_t wi = 0; wi < worklist.size(); ++wi) {
        Block *B = worklist[wi];
        queued.erase(B);
        // Determine the incoming def at the start of B.
        uint32_t inDef = 0; // LiveOnEntry default

        const auto &predList = preds[B];
        if (!predList.empty()) {
            // Collect outDefs from all predecessors.
            uint32_t first = outDef[predList[0]];
            bool allSame = true;
            for (size_t pi = 1; pi < predList.size(); ++pi) {
                if (outDef[predList[pi]] != first) {
                    allSame = false;
                    break;
                }
            }

            if (allSame) {
                inDef = first;
            } else {
                // Need a Phi. Look for an existing Phi at the start of B.
                uint32_t phiId = 0;
                auto bit = mssa.instrToAccess_.find(B);
                if (bit != mssa.instrToAccess_.end()) {
                    // Phi is stored at instrIdx = -1 (represented as SIZE_MAX).
                    auto pit = bit->second.find(static_cast<size_t>(-1));
                    if (pit != bit->second.end())
                        phiId = pit->second;
                }

                if (phiId == 0) {
                    // Create new Phi.
                    phiId = nextId();
                    std::vector<uint32_t> incoming;
                    incoming.reserve(predList.size());
                    for (Block *pred : predList)
                        incoming.push_back(outDef[pred]);
                    mssa.accesses_.push_back(
                        MemoryAccess{MemAccessKind::Phi, phiId, B, -1, 0, std::move(incoming), {}});
                    mssa.instrToAccess_[B][static_cast<size_t>(-1)] = phiId;
                } else {
                    // Update existing Phi's incoming arms.
                    MemoryAccess &phi = mssa.accesses_[phiId];
                    for (size_t pi = 0; pi < predList.size(); ++pi) {
                        uint32_t newArm = outDef[predList[pi]];
                        if (pi >= phi.incoming.size()) {
                            phi.incoming.push_back(newArm);
                        } else {
                            phi.incoming[pi] = newArm;
                        }
                    }
                }
                inDef = phiId;
            }
        }

        // Walk instructions in B, updating inDef as we encounter defs/uses.
        uint32_t curDef = inDef;

        for (size_t i = 0; i < B->instructions.size(); ++i) {
            const Instr &I = B->instructions[i];

            // Check if this instruction already has an access (from a prior iter).
            uint32_t existingId = 0;
            auto bit = mssa.instrToAccess_.find(B);
            if (bit != mssa.instrToAccess_.end()) {
                auto iit = bit->second.find(i);
                if (iit != bit->second.end())
                    existingId = iit->second;
            }

            // For calls touching non-escaping allocas: transparent (skip).
            // We check this at the Use/Def determination step.

            if (I.op == Opcode::Store) {
                const Value &ptr = I.operands.empty() ? Value{} : I.operands[0];
                bool nonEscaping = isNonEscapingAlloca(ptr, nonEsc, roots);
                auto storeSize = BasicAA::typeSizeBytes(I.type);
                uint32_t reachingDef = findLocalReachingDef(B, i, ptr, storeSize).value_or(curDef);

                // Create or update MemoryDef.
                if (existingId == 0) {
                    uint32_t defId = makeAccess(MemAccessKind::Def, B, (int)i, reachingDef);
                    // Link curDef's users to include this new def.
                    if (reachingDef < mssa.accesses_.size()) {
                        // Only link if the store potentially reads curDef
                        // (i.e., reading first then writing). For stores we only
                        // link as Def; use consumers are separate.
                        (void)nonEscaping; // noted but not needed for linkage logic
                    }
                    curDef = defId;
                } else {
                    // Update definingAccess if it changed.
                    MemoryAccess &acc = mssa.accesses_[existingId];
                    acc.definingAccess = reachingDef;
                    curDef = existingId;
                }
            } else if (I.op == Opcode::Load) {
                const Value &ptr = I.operands.empty() ? Value{} : I.operands[0];
                bool nonEscaping = isNonEscapingAlloca(ptr, nonEsc, roots);
                (void)nonEscaping;
                auto loadSize = BasicAA::typeSizeBytes(I.type);
                uint32_t reachingDef = findLocalReachingDef(B, i, ptr, loadSize).value_or(curDef);

                // Create or update MemoryUse.
                if (existingId == 0) {
                    uint32_t useId = makeAccess(MemAccessKind::Use, B, (int)i, reachingDef);
                    // Register this use in the def's users list.
                    if (reachingDef < mssa.accesses_.size()) {
                        mssa.accesses_[reachingDef].users.push_back(useId);
                    }
                } else {
                    MemoryAccess &acc = mssa.accesses_[existingId];
                    if (acc.definingAccess != reachingDef) {
                        // Remove from old def's users, add to new.
                        uint32_t oldDef = acc.definingAccess;
                        if (oldDef < mssa.accesses_.size()) {
                            auto &users = mssa.accesses_[oldDef].users;
                            users.erase(std::remove(users.begin(), users.end(), existingId),
                                        users.end());
                        }
                        acc.definingAccess = reachingDef;
                        if (reachingDef < mssa.accesses_.size()) {
                            mssa.accesses_[reachingDef].users.push_back(existingId);
                        }
                    }
                    // curDef unchanged by loads.
                }
            } else if (I.op == Opcode::Call || I.op == Opcode::CallIndirect) {
                // Calls are transparent for non-escaping allocas.
                // For the global memory state they may Def or Use.
                // We model them as global Defs if they Mod, and Uses if they Ref.
                // This is conservative for heap/global accesses.
                auto mr = AA.modRef(I);
                if (mr == ModRefResult::NoModRef)
                    continue;

                // Def: call modifies global memory.
                if (mr == ModRefResult::Mod || mr == ModRefResult::ModRef) {
                    if (existingId == 0) {
                        uint32_t defId = makeAccess(MemAccessKind::Def, B, (int)i, curDef);
                        curDef = defId;
                    } else {
                        MemoryAccess &acc = mssa.accesses_[existingId];
                        acc.definingAccess = curDef;
                        curDef = existingId;
                    }
                }
                // Use: call reads global memory (register use of curDef).
                if (mr == ModRefResult::Ref || mr == ModRefResult::ModRef) {
                    // Register the call as a user of curDef.
                    // For ModRef: the Def we just created reads the prior curDef.
                    // We don't create a separate Use node; the Def implicitly reads.
                }
            }
        }

        uint32_t newOutDef = curDef;
        if (outDef[B] == newOutDef)
            continue;
        outDef[B] = newOutDef;
        for (Block *succ : succs[B]) {
            if (queued.insert(succ).second)
                worklist.push_back(succ);
        }
    }

    // -----------------------------------------------------------------------
    // Phase 3: Dead-store detection.
    //
    // A MemoryDef at (block, instrIdx) that corresponds to a Store is dead if
    // no MemoryUse transitively reachable from it reads from the same location
    // before another MemoryDef kills it.
    //
    // Precise per-location dead-store check using BFS forward from each store.
    // Unlike the previous runCrossBlockDSE:
    //   - Calls are NOT treated as reads for non-escaping allocas.
    //   - This is the key precision improvement over the conservative BFS.
    // -----------------------------------------------------------------------

    // Rebuild label→Block* (for successor traversal).
    // (labelToBlock already exists in this scope.)

    for (auto &B : F.blocks) {
        for (size_t i = 0; i < B.instructions.size(); ++i) {
            const Instr &I = B.instructions[i];
            if (I.op != Opcode::Store || I.operands.empty())
                continue;

            const Value &ptr = I.operands[0];
            if (!isNonEscapingAlloca(ptr, nonEsc, roots))
                continue;

            auto storeSize = BasicAA::typeSizeBytes(I.type);

            // Determine if this store is dead using a forward BFS that is
            // precise about calls: since the alloca doesn't escape, calls
            // cannot read or modify it.
            bool isDead = true;

            // Intra-block check: scan instructions AFTER the store in same block.
            bool killedInSameBlock = false;
            for (size_t j = i + 1; j < B.instructions.size(); ++j) {
                const Instr &next = B.instructions[j];

                if (next.op == Opcode::Load && !next.operands.empty()) {
                    auto loadSize = BasicAA::typeSizeBytes(next.type);
                    if (AA.alias(next.operands[0], ptr, loadSize, storeSize) !=
                        AliasResult::NoAlias) {
                        isDead = false;
                        break;
                    }
                }
                if (next.op == Opcode::Store && !next.operands.empty()) {
                    auto nextSize = BasicAA::typeSizeBytes(next.type);
                    if (fullyOverwrites(next.operands[0], nextSize, ptr, storeSize, AA)) {
                        // Fully overwritten before any read in this block.
                        killedInSameBlock = true;
                        break;
                    }
                }
                // KEY PRECISION IMPROVEMENT: calls do NOT read non-escaping allocas.
                // Do NOT treat calls as read barriers here.
                // (Calls with Mod: don't modify non-escaping allocas either.)
            }

            if (!isDead)
                continue;
            if (killedInSameBlock) {
                auto bit = mssa.instrToAccess_.find(&B);
                if (bit != mssa.instrToAccess_.end()) {
                    auto ait = bit->second.find(i);
                    if (ait != bit->second.end())
                        mssa.deadStoreIds_.insert(ait->second);
                }
                continue;
            }

            const std::uint64_t locationKey =
                (static_cast<std::uint64_t>(ptr.id) << 32U) |
                (storeSize ? static_cast<std::uint64_t>(*storeSize) + 1U : 0U);
            PathMemo &livePathMemo = pathMemoByLocation[locationKey];
            std::unordered_set<std::string> visiting;
            /// @brief Prove that every continuation from a successor kills the store or exits.
            /// @param label Label of the successor block at which to begin the proof.
            /// @return True when all paths overwrite the location or terminate before a read.
            std::function<bool(const std::string &)> allLivePathsKillOrExit =
                [&](const std::string &label) -> bool {
                if (auto memoIt = livePathMemo.find(label); memoIt != livePathMemo.end())
                    return memoIt->second;

                if (!visiting.insert(label).second)
                    return false; // A live loop can keep the old store reachable.

                /// @brief Complete and memoize the proof result for the current label.
                /// @param value Result to cache and return.
                /// @return The unchanged proof result.
                auto finish = [&](bool value) {
                    visiting.erase(label);
                    livePathMemo[label] = value;
                    return value;
                };

                auto it = labelToBlock.find(label);
                if (it == labelToBlock.end())
                    return finish(false);

                Block *succ = it->second;
                for (const auto &next : succ->instructions) {
                    if (next.op == Opcode::Load && !next.operands.empty()) {
                        auto loadSize = BasicAA::typeSizeBytes(next.type);
                        if (AA.alias(next.operands[0], ptr, loadSize, storeSize) !=
                            AliasResult::NoAlias)
                            return finish(false);
                    }
                    if (next.op == Opcode::Store && !next.operands.empty()) {
                        auto nextSize = BasicAA::typeSizeBytes(next.type);
                        if (fullyOverwrites(next.operands[0], nextSize, ptr, storeSize, AA))
                            return finish(true);
                    }
                    // Calls cannot read or write non-escaping stack memory.
                }

                if (succ->instructions.empty())
                    return finish(false);

                const Instr &term = succ->instructions.back();
                if (term.op == Opcode::Ret || term.op == Opcode::Trap ||
                    term.op == Opcode::TrapFromErr)
                    return finish(true);
                if (term.labels.empty())
                    return finish(false);

                for (const auto &succLabel : term.labels)
                    if (!allLivePathsKillOrExit(succLabel))
                        return finish(false);
                return finish(true);
            };

            bool allPathsKillOrExit = true;
            if (!B.instructions.empty()) {
                for (const auto &label : B.instructions.back().labels) {
                    if (!allLivePathsKillOrExit(label)) {
                        allPathsKillOrExit = false;
                        break;
                    }
                }
            }

            if (allPathsKillOrExit) {
                // Look up the MemoryAccess id for this store.
                auto bit = mssa.instrToAccess_.find(&B);
                if (bit != mssa.instrToAccess_.end()) {
                    auto iit = bit->second.find(i);
                    if (iit != bit->second.end())
                        mssa.deadStoreIds_.insert(iit->second);
                }
            }
        }
    }

    return mssa;
}

} // namespace zanna::analysis
