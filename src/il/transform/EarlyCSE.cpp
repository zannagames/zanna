//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Implements common subexpression elimination over the dominator tree. The
// pass maintains a stack of expression tables (one per domtree level) during
// a pre-order DFS over the dominator tree.  An expression computed in a block
// B is visible in every block that B dominates, because B's table stays on the
// stack while those blocks are processed.  When the DFS leaves a block its
// table is popped, so expressions don't "leak" to sibling subtrees.
//
// Only pure, non-trapping, non-memory opcodes (as classified by makeValueKey)
// are eligible. Commutative operands are normalised so that "a+b" and "b+a"
// share the same key. Replacements are applied through the shared IL utility
// helper so the pass stays correct even when instruction vectors are mutated
// during elimination.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements dominator-tree-scoped common subexpression elimination.
/// @details Walks the dominator tree in pre-order with an explicit worklist,
///          maintaining a stack of expression maps one per domtree level.
///          Redundant pure expressions are replaced and removed.

#include "il/transform/EarlyCSE.hpp"

#include "il/transform/ValueKey.hpp"

#include "il/analysis/CFG.hpp"
#include "il/analysis/Dominators.hpp"
#include "il/core/BasicBlock.hpp"
#include "il/core/Instr.hpp"
#include "il/core/Module.hpp"
#include "il/utils/Utils.hpp"

#include <unordered_map>
#include <vector>

using namespace il::core;

namespace il::transform {

namespace {

/// @brief Previously computed value available for an expression key.
struct AvailableExpr {
    /// SSA value that may replace a dominated redundant computation.
    Value value;
};

/// @brief Expression table scoped to one dominator-tree level.
using CSETable = std::unordered_map<ValueKey, AvailableExpr, ValueKeyHash>;

/// @brief Process one basic block during the dominator-tree CSE walk.
/// @details For each pure instruction in @p B, looks up its expression key in
///          the scope stack (innermost first). On a hit the redundant result is
///          replaced and the instruction erased; on a miss the key is entered
///          into the current scope.
/// @param B         Block being processed.
/// @param F         Function being optimized; used for safe whole-function
///                  operand rewrites after an instruction is removed.
/// @param domTree   Dominator tree used to scope temp-producing replacements.
/// @param scopes    Stack of expression tables; the back element is the
///                  current scope. May be modified (entries appended to back).
/// @return True if any instruction was removed from @p B.
bool processBlock(BasicBlock &B,
                  Function &F,
                  const zanna::analysis::DomTree &domTree,
                  std::vector<CSETable> &scopes) {
    bool changed = false;
    for (std::size_t idx = 0; idx < B.instructions.size();) {
        Instr &I = B.instructions[idx];
        auto key = makeValueKey(I);
        if (!key) {
            ++idx;
            continue;
        }

        // Search scopes from innermost (back) to outermost (front).
        bool found = false;
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
            auto hit = it->find(*key);
            if (hit == it->end())
                continue;
            zanna::il::replaceUsesDominatedBy(F, *I.result, hit->second.value, B, idx, domTree);
            B.instructions.erase(B.instructions.begin() + static_cast<long>(idx));
            changed = true;
            found = true;
            break;
        }
        if (!found) {
            scopes.back().emplace(std::move(*key), AvailableExpr{Value::temp(*I.result)});
            ++idx;
        }
    }
    return changed;
}

} // namespace

/// @brief Run dominator-tree-scoped CSE on a function.
/// @details Builds the dominator tree once and walks it in pre-order using an
///          explicit stack. Each
///          domtree node pushes a new expression scope before processing its
///          basic block and pops it after processing all dominated children.
/// @param M Module containing \p F (needed to construct a CFGContext).
/// @param F Function to optimize in place.
/// @return True if any redundant instruction was removed; false otherwise.
bool runEarlyCSE(Module &M, Function &F) {
    if (F.blocks.empty())
        return false;

    zanna::analysis::CFGContext cfg = zanna::analysis::CFGContext::forInternedFunction(M, F);
    zanna::analysis::DomTree domTree = zanna::analysis::computeDominatorTree(cfg, F);

        // Iterative pre-order DFS over the dominator tree.
        // Each worklist entry is either "enter B" (push scope, process B, then
        // schedule children) or "leave" (pop scope).  We encode "leave" as
        // nullptr in the block slot.
        /// @brief Enter/leave marker for iterative dominator-tree traversal.
        struct WorkItem {
            BasicBlock *block; // nullptr → pop scope
        };

        std::vector<CSETable> scopes;
        scopes.reserve(F.blocks.size());
    bool changed = false;

        std::vector<WorkItem> worklist;
        worklist.reserve(F.blocks.size() * 2);
        worklist.push_back({&F.blocks.front()});

        while (!worklist.empty()) {
            WorkItem item = worklist.back();
            worklist.pop_back();

            if (!item.block) {
                // Pop the scope we pushed when we entered the corresponding block.
                scopes.pop_back();
                continue;
            }

            // Push a new expression scope for this block.
            scopes.emplace_back();
            // Schedule scope pop after all children are processed.
            worklist.push_back({nullptr});

            changed |= processBlock(*item.block, F, domTree, scopes);

            // Schedule dominated children (order doesn't matter for correctness).
            auto childIt = domTree.children.find(item.block);
            if (childIt != domTree.children.end()) {
                for (BasicBlock *child : childIt->second)
                    worklist.push_back({child});
            }
        }

    return changed;
}

} // namespace il::transform
