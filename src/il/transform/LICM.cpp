//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/transform/LICM.cpp
// Purpose: Hoist loop-invariant, ownership-neutral instructions into dedicated
//          loop preheaders.
// Key invariants:
//   - Hoisted instructions are non-trapping and preserve memory ordering.
//   - String loads stay inside loops because every load creates a distinct
//     owned reference that a consuming use may spend.
// Ownership/Lifetime: Rewrites functions in place; analysis results are borrowed
//                     from the pass manager and invalidated after mutation.
// Links: il/transform/LICM.hpp, il/transform/LoadSafety.hpp,
//        docs/il/il-passes.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements a lightweight loop-invariant code motion pass.
/// @details Identifies trivially safe, loop-invariant instructions and hoists
///          them to the loop preheader. The pass is conservative, relying on
///          opcode metadata, verifier properties, and basic alias analysis to
///          avoid changing observable program behavior.

#include "il/transform/LICM.hpp"

#include "il/transform/AnalysisIDs.hpp"
#include "il/transform/AnalysisManager.hpp"
#include "il/transform/CallEffects.hpp"
#include "il/transform/LoadSafety.hpp"
#include "il/transform/analysis/Liveness.hpp"
#include "il/transform/analysis/LoopInfo.hpp"

#include "il/analysis/AllocaRoots.hpp"
#include "il/analysis/BasicAA.hpp"
#include "il/analysis/Dominators.hpp"
#include "il/core/BasicBlock.hpp"
#include "il/core/Function.hpp"
#include "il/core/Instr.hpp"
#include "il/core/Module.hpp"
#include "il/core/OpcodeInfo.hpp"
#include "il/core/Value.hpp"
#include "il/utils/Utils.hpp"
#include "il/verify/VerifierTable.hpp"

#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace il::core;

namespace il::transform {
namespace {
/// @brief Track a store within a loop for alias-checking load hoisting.
/// @details Captures the pointer operand and its byte size (when known) so
///          load instructions can be compared against loop stores safely.
struct StoreSite {
    Value ptr;                    ///< Pointer operand written by a store.
    std::optional<unsigned> size; ///< Size in bytes of the store, if known.
};

/// @brief Stable summary of a value-defining instruction.
/// @details LICM mutates instruction storage while scanning loops, so borrowed
///          instruction pointers are unsafe for helper caches. This summary
///          copies only the opcode and operands needed by pointer-derivation
///          checks.
using DefMap = std::unordered_map<unsigned, zanna::analysis::AllocaRootDefInfo>;

/// @brief Classify how a call instruction interacts with memory for hoisting.
/// @details Uses the shared call-effects classifier so runtime metadata and
///          verified call attributes are interpreted consistently across
///          optimizers.
enum class CallHoistKind {
    NotHoistable, ///< Call may write memory or has other side effects.
    Pure,         ///< Call has no memory effects (pure math, etc.).
    ReadOnly,     ///< Call only reads memory (safe if no aliasing stores).
};

/// @brief Determine whether a direct call is pure or read-only enough to hoist.
/// @param module Module used to resolve callee and runtime effect metadata.
/// @param instr Candidate call instruction.
/// @return Hoisting classification, with @ref CallHoistKind::NotHoistable for
///         non-calls, throwing calls, and calls that may mutate memory.
CallHoistKind classifyCallForHoist(const Module &module, const Instr &instr) {
    if (instr.op != Opcode::Call)
        return CallHoistKind::NotHoistable;

    const CallEffects effects = classifyCallEffects(instr, &module);
    if (!effects.nothrow)
        return CallHoistKind::NotHoistable;

    if (effects.pure)
        return CallHoistKind::Pure;
    if (effects.readonly)
        return CallHoistKind::ReadOnly;

    return CallHoistKind::NotHoistable;
}

/// @brief Build a temp-definition map for pointer-derivation queries.
/// @param function Function whose result-producing instructions are indexed.
/// @return Map from temp id to copied definition summary.
DefMap buildDefMap(const Function &function) {
    DefMap defs;
    for (const auto &block : function.blocks) {
        for (const auto &instr : block.instructions) {
            if (!instr.result)
                continue;
            defs.emplace(*instr.result,
                         zanna::analysis::AllocaRootDefInfo{instr.op, instr.operands});
        }
    }
    return defs;
}

/// @brief Determine whether @p ptr is rooted at a non-escaping alloca.
/// @details Uses the shared iterative alloca-root resolver, which detects
///          malformed cycles without truncating valid deep GEP chains.
/// @param defs Temp definition summaries for the function.
/// @param aa Alias analysis carrying non-escaping alloca facts.
/// @param ptr Pointer value to inspect.
/// @return True when @p ptr is an alloca or GEP chain rooted at a non-escaping alloca.
bool isDerivedFromNonEscapingAlloca(const DefMap &defs,
                                    const zanna::analysis::BasicAA &aa,
                                    const Value &ptr) {
    const auto root = zanna::analysis::getAllocaId(ptr, defs);
    return root && aa.isNonEscapingAlloca(*root);
}

/// @brief Determine whether an instruction can be hoisted out of the loop.
/// @details Rejects terminators, side-effecting instructions, and any opcode
///          marked as trapping by the verifier. Memory operations are only
///          allowed when they are explicitly safe, such as loads with proven
///          non-aliasing when @p allowLoadHoist is true.  Pure calls (no memory
///          effects) are always hoistable; readonly calls are hoistable under
///          the same alias conditions as loads.
/// @param function Function containing @p instr and its call context.
/// @param module Module used to resolve direct callee effects.
/// @param instr Instruction to test.
/// @param allowLoadHoist Whether loads/readonly calls may be hoisted.
/// @param callHoist Output: set to the call hoisting classification when the
///                  instruction is a call.
/// @return True if the instruction is safe to move to the preheader.
bool isSafeToHoist(const Function &function,
                   const Module &module,
                   const Instr &instr,
                   bool allowLoadHoist,
                   CallHoistKind &callHoist) {
    callHoist = CallHoistKind::NotHoistable;

    if (!instr.labels.empty() || !instr.brArgs.empty())
        return false;

    // A string load is not an ownership-neutral memory read: native and VM
    // execution both mint a fresh owned reference for every load. Hoisting it
    // would collapse the per-iteration retains while consuming uses remain in
    // the loop, allowing the first iteration to free a value reused by later
    // iterations.
    if (instr.op == Opcode::Load && instr.type.kind == Type::Kind::Str)
        return false;

    // Calls are marked side-effecting in opcode metadata by default, so classify
    // them before the generic side-effect rejection.
    if (instr.op == Opcode::Call) {
        callHoist = classifyCallForHoist(module, instr);
        if (callHoist == CallHoistKind::Pure)
            return true;
        if (callHoist == CallHoistKind::ReadOnly && allowLoadHoist)
            return true;
        return false;
    }

    const auto &info = getOpcodeInfo(instr.op);
    if (info.isTerminator || info.hasSideEffects)
        return false;

    if (auto spec = verify::lookupSpec(instr.op)) {
        if (spec->hasSideEffects)
            return false;
    }

    if (auto props = verify::lookup(instr.op)) {
        if (props->canTrap)
            return false;
    }

    const auto effects = memoryEffects(instr.op);
    if (effects == MemoryEffects::None)
        return true;

    if (allowLoadHoist && effects == MemoryEffects::Read && instr.op == Opcode::Load &&
        isLoadKnownNonTrapping(function, instr))
        return true;

    return false;
}

/// @brief Seed the invariant set with values defined outside the loop.
/// @details Inserts function parameters, block parameters, and results of
///          instructions from blocks not in the loop so the invariant set
///          starts with all values known to be loop-external.
/// @param loop Loop being analyzed.
/// @param function Function containing the loop.
/// @param invariants Output set of invariant temporary ids to populate.
void seedInvariants(const Loop &loop,
                    Function &function,
                    std::unordered_set<unsigned> &invariants) {
    for (const auto &param : function.params)
        invariants.insert(param.id);

    for (auto &block : function.blocks) {
        if (loop.contains(block.label))
            continue;
        for (const auto &param : block.params)
            invariants.insert(param.id);
        for (const auto &instr : block.instructions)
            if (instr.result)
                invariants.insert(*instr.result);
    }
}

/// @brief Check whether all operands of @p instr are loop-invariant.
/// @details Treats non-temporary values as inherently invariant, and consults
///          the supplied invariant set for temporaries. Branch argument lists
///          are checked as well to avoid hoisting instructions that feed loop-
///          internal control flow edges.
/// @param instr Instruction whose operands should be validated.
/// @param invariants Set of invariant temporary ids.
/// @return True if every operand and branch argument is invariant.
bool operandsInvariant(const Instr &instr, const std::unordered_set<unsigned> &invariants) {
    /// Return whether a literal or known loop-external temporary is invariant.
    auto isInvariantValue = [&invariants](const Value &value) {
        if (value.kind != Value::Kind::Temp)
            return true;
        return invariants.contains(value.id);
    };

    for (const auto &operand : instr.operands) {
        if (!isInvariantValue(operand))
            return false;
    }

    for (const auto &argList : instr.brArgs)
        for (const auto &arg : argList)
            if (!isInvariantValue(arg))
                return false;

    return true;
}

/// @brief Collect loop blocks in dominator-tree preorder.
/// @details Walks the dominator tree starting at the loop header and appends
///          only blocks that are part of the loop. This produces a stable order
///          for scanning instructions while preserving dominance relationships.
/// @param block Current dominator-tree node to visit.
/// @param loop Loop being analyzed.
/// @param domTree Dominator tree for the function.
/// @param order Output list of blocks in visitation order.
void collectDominanceOrder(BasicBlock *block,
                           const Loop &loop,
                           const zanna::analysis::DomTree &domTree,
                           std::vector<BasicBlock *> &order) {
    if (!block)
        return;

    order.push_back(block);

    auto it = domTree.children.find(block);
    if (it == domTree.children.end())
        return;

    for (auto *child : it->second) {
        if (!loop.contains(child->label))
            continue;
        collectDominanceOrder(child, loop, domTree, order);
    }
}

} // namespace

/// @brief Look up a basic block by label in a pre-built map.
/// @details Returns nullptr if the label is unknown, avoiding accidental
///          insertion into the map and keeping lookup O(1).
/// @param blocks Map from label to BasicBlock pointer.
/// @param label Label to look up.
/// @return Pointer to the block, or nullptr if not found.
BasicBlock *lookupBlock(const std::unordered_map<std::string, BasicBlock *> &blocks,
                        const std::string &label) {
    auto it = blocks.find(label);
    return it == blocks.end() ? nullptr : it->second;
}

/// @brief Find the unique loop preheader for a loop header.
/// @details Scans CFG predecessors of the header and looks for exactly one
///          predecessor that is outside the loop. If multiple external preds
///          exist or none are found, the loop is treated as missing a valid
///          preheader and the function returns nullptr.
/// @param loop Loop whose preheader is being queried.
/// @param header Header block of the loop.
/// @param cfg CFG analysis providing predecessor edges.
/// @param blocks Label-to-block map for resolving CFG nodes.
/// @return Pointer to the unique preheader block, or nullptr if ambiguous.
BasicBlock *findPreheader(const Loop &loop,
                          BasicBlock &header,
                          const CFGInfo &cfg,
                          const std::unordered_map<std::string, BasicBlock *> &blocks) {
    auto predsIt = cfg.predecessors.find(&header);
    if (predsIt == cfg.predecessors.end())
        return nullptr;
    BasicBlock *preheader = nullptr;
    for (const auto *pred : predsIt->second) {
        if (!pred || loop.contains(pred->label))
            continue;
        auto *mutablePred = lookupBlock(blocks, pred->label);
        if (!mutablePred)
            continue;
        if (preheader && preheader != mutablePred)
            return nullptr;
        preheader = mutablePred;
    }
    return preheader;
}

/// @copydoc LICM::LICM()
LICM::LICM(bool allowMemoryHoisting) : allowMemoryHoisting_(allowMemoryHoisting) {}

/// @brief Return the unique identifier for this pass.
/// @details Used when registering and invoking LICM via the pass registry.
/// @return The canonical pass id string "licm".
std::string_view LICM::id() const {
    return allowMemoryHoisting_ ? "licm" : "licm-safe";
}

/// @brief Execute loop-invariant code motion on a function.
/// @details For each loop with a unique preheader, the pass collects invariant
///          values, scans loop blocks in dominator order, and hoists instructions
///          that are safe and invariant. Load hoisting is permitted only when
///          no memory writes are observed in the loop or alias analysis proves
///          the load does not alias any store. Preserved analyses are returned
///          conservatively when changes are made.
/// @param function Function to optimize.
/// @param analysis Analysis manager providing CFG, dominators, loop info, and AA.
/// @return Preserved analysis set describing which analyses remain valid.
PreservedAnalyses LICM::run(Function &function, AnalysisManager &analysis) {
    auto &domTree =
        analysis.getFunctionResult<zanna::analysis::DomTree>(kAnalysisDominators, function);
    auto &loopInfo = analysis.getFunctionResult<LoopInfo>(kAnalysisLoopInfo, function);
    auto &aa = analysis.getFunctionResult<zanna::analysis::BasicAA>(kAnalysisBasicAA, function);
    auto &cfg = analysis.getFunctionResult<CFGInfo>(kAnalysisCFG, function);

    std::unordered_map<std::string, BasicBlock *> blockLookup;
    blockLookup.reserve(function.blocks.size());
    for (auto &blk : function.blocks)
        blockLookup.emplace(blk.label, &blk);

    const DefMap defs = buildDefMap(function);
    bool changed = false;

    for (const Loop &loop : loopInfo.loops()) {
        BasicBlock *header = lookupBlock(blockLookup, loop.headerLabel);
        if (!header)
            continue;

        BasicBlock *preheader = findPreheader(loop, *header, cfg, blockLookup);
        if (!preheader)
            continue;

        std::unordered_set<unsigned> invariants;
        invariants.reserve(function.params.size() + header->params.size() + 32);
        seedInvariants(loop, function, invariants);

        bool loopHasMod = false;
        std::vector<StoreSite> loopStores;
        for (const auto &label : loop.blockLabels) {
            BasicBlock *blk = lookupBlock(blockLookup, label);
            if (!blk)
                continue;
            for (const auto &ins : blk->instructions) {
                if (ins.op == Opcode::Store && !ins.operands.empty()) {
                    loopStores.push_back(
                        {ins.operands[0], zanna::analysis::BasicAA::typeSizeBytes(ins.type)});
                    continue;
                }

                if (ins.op == Opcode::Call || ins.op == Opcode::CallIndirect) {
                    auto mr = aa.modRef(ins);
                    if (mr == zanna::analysis::ModRefResult::Mod ||
                        mr == zanna::analysis::ModRefResult::ModRef) {
                        loopHasMod = true;
                    }
                }
                auto me = memoryEffects(ins.op);
                if (me == MemoryEffects::Write || me == MemoryEffects::ReadWrite ||
                    me == MemoryEffects::Unknown) {
                    loopHasMod = true;
                }
            }
        }

        std::vector<BasicBlock *> blockOrder;
        blockOrder.reserve(loop.blockLabels.size());
        collectDominanceOrder(header, loop, domTree, blockOrder);

        for (BasicBlock *block : blockOrder) {
            for (std::size_t idx = 0; idx < block->instructions.size();) {
                Instr &instr = block->instructions[idx];
                bool allowLoads = allowMemoryHoisting_;
                if (instr.op == Opcode::Load) {
                    // When the loop contains memory-modifying calls, loads from
                    // non-escaping allocas are still safe: the call cannot
                    // observe or modify stack memory whose address never left
                    // the function.
                    if (loopHasMod) {
                        allowLoads = !instr.operands.empty() &&
                                     isDerivedFromNonEscapingAlloca(defs, aa, instr.operands[0]);
                    }
                    if (allowLoads && !instr.operands.empty()) {
                        auto loadSize = zanna::analysis::BasicAA::typeSizeBytes(instr.type);
                        for (const auto &store : loopStores) {
                            if (aa.alias(instr.operands[0], store.ptr, loadSize, store.size) !=
                                zanna::analysis::AliasResult::NoAlias) {
                                allowLoads = false;
                                break;
                            }
                        }
                    }
                }

                CallHoistKind callHoist = CallHoistKind::NotHoistable;
                if (!isSafeToHoist(function, analysis.module(), instr, allowLoads, callHoist) ||
                    !operandsInvariant(instr, invariants)) {
                    ++idx;
                    continue;
                }

                // Readonly calls need the same memory-safety guard as loads:
                // mutating calls inside the loop can also change the memory the
                // readonly call observes, even when there are no explicit Store
                // instructions in the loop body.
                if (callHoist == CallHoistKind::ReadOnly && (loopHasMod || !loopStores.empty())) {
                    // Conservative: readonly calls may read any memory, and we
                    // cannot know the precise address set. Only hoist when the
                    // loop has no mutating memory operations at all.
                    ++idx;
                    continue;
                }

                Instr hoisted = std::move(instr);
                block->instructions.erase(block->instructions.begin() + idx);

                std::size_t insertIndex = preheader->instructions.size();
                if (zanna::il::isTerminated(*preheader) && insertIndex > 0)
                    --insertIndex;
                auto inserted = preheader->instructions.insert(
                    preheader->instructions.begin() + insertIndex, std::move(hoisted));

                // Copy result before any further vector operations could
                // invalidate the iterator returned by insert().
                auto hoistedResult = inserted->result;
                if (hoistedResult)
                    invariants.insert(*hoistedResult);

                changed = true;
            }
        }
    }

    if (!changed)
        return PreservedAnalyses::all();

    // Hoisting mutates instruction placement across loop/preheader boundaries.
    // Rebuilding analyses is safer than preserving stale CFG/dominance/loop data.
    return PreservedAnalyses::none();
}

} // namespace il::transform
