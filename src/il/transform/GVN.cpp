//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/transform/GVN.cpp
// Purpose: Eliminate redundant ownership-neutral expressions and loads along
//          dominator-tree paths.
// Key invariants:
//   - Reused values dominate every replacement and remain textually available.
//   - String loads are never merged because each load creates a distinct owned
//     reference even when the underlying bytes are unchanged.
// Ownership/Lifetime: Rewrites functions in place; available-value state owns
//                     copied IL values and borrows block identities.
// Links: il/transform/GVN.hpp, il/transform/LoadSafety.hpp,
//        docs/il/il-passes.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements Global Value Numbering with redundant load elimination.
/// @details Performs value numbering along dominator-tree paths to replace
///          redundant pure computations, and memoizes load results when alias
///          analysis proves they are still valid. The traversal is preorder so
///          only dominating information is visible in each block.

#include "il/transform/GVN.hpp"

#include "il/transform/AnalysisIDs.hpp"
#include "il/transform/AnalysisManager.hpp"
#include "il/transform/LoadSafety.hpp"
#include "il/transform/ValueKey.hpp"

#include "il/analysis/BasicAA.hpp"
#include "il/analysis/CFG.hpp"
#include "il/analysis/Dominators.hpp"
#include "il/transform/analysis/Liveness.hpp" // for CFGInfo

#include "il/core/Function.hpp"
#include "il/core/Instr.hpp"
#include "il/core/Opcode.hpp"
#include "il/core/OpcodeInfo.hpp"
#include "il/core/Type.hpp"
#include "il/core/Value.hpp"

#include "il/utils/Utils.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <optional>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace il::core;

namespace il::transform {

namespace {
using il::transform::ValueEq;
using il::transform::ValueHash;
using il::transform::ValueKey;
using il::transform::ValueKeyHash;

/// @brief Key describing a load by pointer, type, and (optional) byte size.
/// @details Used to memoize load results for redundant load elimination. The
///          size field is optional because some types may not map to a known
///          size; in that case the key still differentiates by pointer+type.
struct LoadKey {
    /// Pointer operand read by the load.
    Value ptr;
    /// Loaded value type.
    Type::Kind type{Type::Kind::Void};
    /// Access width in bytes when statically known.
    std::optional<unsigned> size;

    /// @brief Compare load identity structurally.
    /// @param o Candidate key.
    /// @return True when pointer, type, and optional width match.
    bool operator==(const LoadKey &o) const noexcept {
        ValueEq eq;
        return type == o.type && eq(ptr, o.ptr) && size == o.size;
    }
};

/// @brief Hash functor for @ref LoadKey.
/// @details Combines the pointer hash, type kind, and size (when present) to
///          produce a stable hash for unordered maps.
struct LoadKeyHash {
    /// @brief Hash a load key.
    /// @param k Key to hash.
    /// @return Combined pointer, type, and width hash.
    size_t operator()(const LoadKey &k) const noexcept {
        ValueHash hv;
        size_t h = hv(k.ptr) ^ (static_cast<size_t>(k.type) * 0x9e3779b97f4a7c15ULL);
        if (k.size)
            h ^= static_cast<size_t>(*k.size + 0x517cc1b727220a95ULL);
        return h;
    }
};

/// @brief A reusable SSA value and the block that defines it.
/// @details GVN walks the dominator tree, but verifier legality still depends
///          on textual def-before-use ordering across blocks. Tracking the
///          defining block lets the pass reject replacements that would
///          introduce a use of a temp before its textual definition.
struct AvailableValue {
    /// Dominating SSA value available for replacement.
    Value value;
    /// Block defining @ref value.
    BasicBlock *block{nullptr};
    /// Definition index within @ref block.
    std::size_t instrIndex{0};
};

/// @brief Per-path state threaded through the dominator-tree traversal.
/// @details Contains value-numbering expressions and memoized loads visible on
///          the current dominating path. State is copied when recursing into
///          children to preserve path sensitivity.
struct State {
    /// Available pure expressions keyed by structural value identity.
    std::unordered_map<ValueKey, std::vector<AvailableValue>, ValueKeyHash> exprs;
    /// Available non-string loads keyed by address and type.
    std::unordered_map<LoadKey, std::vector<AvailableValue>, LoadKeyHash> loads;
};

/// @brief Check verifier-compatible textual availability of a dominating value.
/// @param order Function block-to-textual-index map.
/// @param avail Candidate reusable definition.
/// @param useBlock Block containing the replacement site.
/// @param useInstrIndex Instruction index of the replacement site.
/// @return True when the definition precedes the use in the same block or its
///         defining block is not textually later.
static bool isTextuallyAvailable(const std::unordered_map<const BasicBlock *, std::size_t> &order,
                                 const AvailableValue &avail,
                                 const BasicBlock *useBlock,
                                 std::size_t useInstrIndex) {
    const BasicBlock *defBlock = avail.block;
    if (!defBlock || !useBlock)
        return false;
    if (defBlock == useBlock)
        return avail.instrIndex < useInstrIndex;
    auto defIt = order.find(defBlock);
    auto useIt = order.find(useBlock);
    if (defIt == order.end() || useIt == order.end())
        return false;
    return defIt->second <= useIt->second;
}

/// @brief Per-function summary answering "can a memory write sit on some path
///        from block D to block C?" for the dominator-tree recursion.
/// @details Dominance alone does not make a memoized load reusable: a store
///          in a block that does not dominate C can still lie on a path from
///          the load's block to C (a loop body writing a slot the header read,
///          or one arm of a diamond). ZB-32: GVN inherited the parent's load
///          facts into every dominated child, so `load %slot` after a loop was
///          replaced by the header's load although the body stored `%slot`.
///          `clobbers` marks blocks containing a store, an impure call, or any
///          instruction with write effects; `reach` is the transitive
///          successor closure (a block is in its own closure only when it lies
///          on a cycle). Both are computed once per GVN run.
struct PathClobberInfo {
    std::unordered_map<const BasicBlock *, std::size_t> index;
    std::vector<bool> clobbers;
    std::vector<std::vector<bool>> reach; ///< reach[from][to]

    /// @brief True when some clobbering block X (X != from) satisfies
    ///        from ->* X ->* to; `to` itself counts when it lies on a cycle.
    [[nodiscard]] bool clobberedBetween(const BasicBlock *from, const BasicBlock *to) const {
        auto fi = index.find(from);
        auto ti = index.find(to);
        if (fi == index.end() || ti == index.end())
            return true; // unknown block: be conservative
        const std::size_t f = fi->second;
        const std::size_t t = ti->second;
        const auto &fromReach = reach[f];
        for (std::size_t x = 0; x < clobbers.size(); ++x) {
            if (!clobbers[x] || x == f)
                continue;
            if (fromReach[x] && reach[x][t])
                return true;
        }
        return false;
    }
};

/// @brief Build the path-clobber summary for @p F.
/// @param F Function being optimized.
/// @param cfg Successor map for @p F.
/// @param AA Alias analysis used to classify calls.
/// @return Summary consulted when facts flow to a dominated child.
static PathClobberInfo buildPathClobberInfo(Function &F,
                                            const il::transform::CFGInfo &cfg,
                                            zanna::analysis::BasicAA &AA) {
    PathClobberInfo info;
    const std::size_t n = F.blocks.size();
    info.index.reserve(n);
    for (std::size_t i = 0; i < n; ++i)
        info.index.emplace(&F.blocks[i], i);
    info.clobbers.assign(n, false);
    for (std::size_t i = 0; i < n; ++i) {
        for (const Instr &I : F.blocks[i].instructions) {
            bool writes = false;
            if (I.op == Opcode::Store) {
                writes = true;
            } else if (I.op == Opcode::Call || I.op == Opcode::CallIndirect) {
                auto mr = AA.modRef(I);
                writes = mr != zanna::analysis::ModRefResult::NoModRef &&
                         mr != zanna::analysis::ModRefResult::Ref;
            } else {
                using il::core::MemoryEffects;
                auto me = memoryEffects(I.op);
                writes = me == MemoryEffects::Write || me == MemoryEffects::ReadWrite;
            }
            if (writes) {
                info.clobbers[i] = true;
                break;
            }
        }
    }
    // Transitive successor closure by DFS from every block (blocks are small
    // in number per function; this is O(n * (n + e)) once per run).
    info.reach.assign(n, std::vector<bool>(n, false));
    std::vector<std::size_t> stack;
    for (std::size_t start = 0; start < n; ++start) {
        auto &row = info.reach[start];
        stack.clear();
        auto push = [&](const BasicBlock *b) {
            auto it = info.index.find(b);
            if (it == info.index.end() || row[it->second])
                return;
            row[it->second] = true;
            stack.push_back(it->second);
        };
        auto succIt = cfg.successors.find(&F.blocks[start]);
        if (succIt != cfg.successors.end())
            for (const BasicBlock *s : succIt->second)
                push(s);
        while (!stack.empty()) {
            const std::size_t cur = stack.back();
            stack.pop_back();
            auto it = cfg.successors.find(&F.blocks[cur]);
            if (it == cfg.successors.end())
                continue;
            for (const BasicBlock *s : it->second)
                push(s);
        }
    }
    return info;
}

/// @brief Visit a basic block and apply GVN/RLE transformations.
/// @details Walks instructions in order, eliminating redundant loads and pure
///          expressions. Load elimination uses exact key matches first, then
///          falls back to MustAlias checks. Stores and impure calls invalidate
///          load memoization conservatively. After processing the block, the
///          function recurses into dominator children, passing a copy of the
///          current state so only dominating facts are visible.
/// @param F Function being optimized.
/// @param B Current basic block in dominator traversal.
/// @param DT Dominator tree for the function.
/// @param AA Alias analysis used for load/store reasoning.
/// @param blockOrder Textual block order used to avoid cross-block
///                   use-before-def substitutions.
/// @param state Current value/load memoization state (copied per child).
/// @param changed Output flag set true if any instruction is removed.
void visitBlock(Function &F,
                BasicBlock *B,
                const zanna::analysis::DomTree &DT,
                zanna::analysis::BasicAA &AA,
                const std::unordered_map<const BasicBlock *, std::size_t> &blockOrder,
                const PathClobberInfo &paths,
                State state,
                bool &changed) {
    for (std::size_t idx = 0; idx < B->instructions.size();) {
        Instr &I = B->instructions[idx];

        // Redundant Load Elimination
        if (I.op == Opcode::Load && I.type.kind != Type::Kind::Str && I.result &&
            !I.operands.empty() && isLoadKnownNonTrapping(F, I)) {
            const Value &ptr = I.operands[0];
            auto loadSize = zanna::analysis::BasicAA::typeSizeBytes(I.type);
            LoadKey key{ptr, I.type.kind, loadSize};

            // Try exact match first
            auto it = state.loads.find(key);
            if (it != state.loads.end()) {
                for (auto avail = it->second.rbegin(); avail != it->second.rend(); ++avail) {
                    if (!isTextuallyAvailable(blockOrder, *avail, B, idx))
                        continue;
                    zanna::il::replaceUsesDominatedBy(F, *I.result, avail->value, *B, idx, DT);
                    B->instructions.erase(B->instructions.begin() + static_cast<long>(idx));
                    changed = true;
                    goto next_instruction;
                }
            }

            // Otherwise, scan for alias-equivalent entries (MustAlias)
            bool replaced = false;
            for (const auto &kv : state.loads) {
                if (kv.first.type != key.type)
                    continue;
                if (AA.alias(kv.first.ptr, key.ptr, kv.first.size, key.size) !=
                    zanna::analysis::AliasResult::MustAlias) {
                    continue;
                }
                for (auto avail = kv.second.rbegin(); avail != kv.second.rend(); ++avail) {
                    if (!isTextuallyAvailable(blockOrder, *avail, B, idx))
                        continue;
                    zanna::il::replaceUsesDominatedBy(F, *I.result, avail->value, *B, idx, DT);
                    B->instructions.erase(B->instructions.begin() + static_cast<long>(idx));
                    changed = true;
                    replaced = true;
                    break;
                }
                if (replaced)
                    break;
            }
            if (replaced)
                goto next_instruction;

            // Record available load
            state.loads[key].push_back(AvailableValue{Value::temp(*I.result), B, idx});
            ++idx;
            continue;
        }

        // Memory clobber: stores or other writes invalidate relevant loads
        if (I.op == Opcode::Store && I.operands.size() >= 2) {
            const Value &stPtr = I.operands[0];
            auto storeSize = zanna::analysis::BasicAA::typeSizeBytes(I.type);
            for (auto it = state.loads.begin(); it != state.loads.end();) {
                if (AA.alias(it->first.ptr, stPtr, it->first.size, storeSize) !=
                    zanna::analysis::AliasResult::NoAlias)
                    it = state.loads.erase(it);
                else
                    ++it;
            }
            ++idx;
            continue;
        }

        if (I.op == Opcode::Call || I.op == Opcode::CallIndirect) {
            auto mr = AA.modRef(I);
            if (mr != zanna::analysis::ModRefResult::NoModRef &&
                mr != zanna::analysis::ModRefResult::Ref) {
                state.loads.clear();
            }
            ++idx;
            continue;
        }

        // Other known writes invalidate all memoised loads. Be careful to not
        // treat Unknown (e.g. branch/ret) as a write.
        {
            using il::core::MemoryEffects;
            auto me = memoryEffects(I.op);
            if (me == MemoryEffects::Write || me == MemoryEffects::ReadWrite) {
                state.loads.clear();
                ++idx;
                continue;
            }
        }

        // Pure expression GVN
        if (auto key = makeValueKey(I)) {
            auto found = state.exprs.find(*key);
            if (found != state.exprs.end()) {
                for (auto avail = found->second.rbegin(); avail != found->second.rend(); ++avail) {
                    if (!isTextuallyAvailable(blockOrder, *avail, B, idx))
                        continue;
                    zanna::il::replaceUsesDominatedBy(F, *I.result, avail->value, *B, idx, DT);
                    B->instructions.erase(B->instructions.begin() + static_cast<long>(idx));
                    changed = true;
                    goto next_instruction;
                }
            }
            state.exprs[*key].push_back(AvailableValue{Value::temp(*I.result), B, idx});
            ++idx;
            continue;
        }

        // Default: advance
        ++idx;
    next_instruction:;
    }

    // Recurse to children in dominator-tree preorder
    auto it = DT.children.find(B);
    if (it != DT.children.end()) {
        for (auto *Child : it->second) {
            // ZB-32: a dominated child may still be reached through a block
            // that writes memory (loop body, diamond arm). Load facts do not
            // survive such a path; pure expressions do.
            if (!state.loads.empty() && paths.clobberedBetween(B, Child)) {
                State childState = state;
                childState.loads.clear();
                visitBlock(F, Child, DT, AA, blockOrder, paths, childState, changed);
            } else {
                visitBlock(F, Child, DT, AA, blockOrder, paths, state, changed);
            }
        }
    }
}

} // namespace

/// @brief Return the unique identifier for the GVN pass.
/// @details Used by the pass registry and pipeline definitions.
/// @return The canonical pass id string "gvn".
std::string_view GVN::id() const {
    return "gvn";
}

/// @brief Execute GVN over a function.
/// @details Initializes analysis dependencies (CFG, dominators, alias analysis),
///          then walks the dominator tree from the entry block. If no changes
///          are made, all analyses are preserved; otherwise a conservative
///          invalidation is returned.
/// @param function Function to optimize.
/// @param analysis Analysis manager used to query required analyses.
/// @return Preserved analysis set after the transformation.
PreservedAnalyses GVN::run(Function &function, AnalysisManager &analysis) {
    // Query required analyses
    auto &cfg = analysis.getFunctionResult<il::transform::CFGInfo>(kAnalysisCFG, function);
    auto &dom = analysis.getFunctionResult<zanna::analysis::DomTree>(kAnalysisDominators, function);
    auto &aa = analysis.getFunctionResult<zanna::analysis::BasicAA>(kAnalysisBasicAA, function);

    bool changed = false;

    if (function.blocks.empty())
        return PreservedAnalyses::all();

    State state;
    std::unordered_map<const BasicBlock *, std::size_t> blockOrder;
    blockOrder.reserve(function.blocks.size());
    for (std::size_t i = 0; i < function.blocks.size(); ++i)
        blockOrder.emplace(&function.blocks[i], i);

    // Start at entry block
    const PathClobberInfo paths = buildPathClobberInfo(function, cfg, aa);
    visitBlock(function, &function.blocks.front(), dom, aa, blockOrder, paths, state, changed);

    if (!changed)
        return PreservedAnalyses::all();

    PreservedAnalyses p;
    p.preserveAllModules();
    p.preserveCFG();
    p.preserveDominators();
    p.preserveLoopInfo();
    return p;
}

/// @brief Register the GVN pass in the pass registry.
/// @details Associates the "gvn" identifier with a factory that constructs
///          a new @ref GVN instance.
/// @param registry Pass registry to update.
void registerGVNPass(PassRegistry &registry) {
    // Sequential: depends on whole-module CFG-backed dominator analysis while deleting
    // instructions.
    /// Construct a fresh GVN pass for each pipeline request.
    registry.registerFunctionPass("gvn", []() { return std::make_unique<GVN>(); }, false);
}

} // namespace il::transform
