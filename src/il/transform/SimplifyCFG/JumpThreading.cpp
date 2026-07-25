//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Implements jump threading transformation for SimplifyCFG.
// Jump threading optimizes control flow by redirecting predecessors that
// pass known values for branch conditions directly to the target block,
// bypassing the intermediate conditional branch.
//
//===----------------------------------------------------------------------===//

#include "il/transform/SimplifyCFG/JumpThreading.hpp"

#include "il/transform/SimplifyCFG/Utils.hpp"

#include "il/core/BasicBlock.hpp"
#include "il/core/Function.hpp"
#include "il/core/Instr.hpp"
#include "il/core/Opcode.hpp"
#include "il/core/Value.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace il::transform::simplify_cfg {

namespace {

/// @brief Find a basic block by label.
/// @param F Function whose blocks are searched.
/// @param label Exact block label.
/// @return Borrowed mutable block, or null when absent.
il::core::BasicBlock *findBlock(il::core::Function &F, const std::string &label) {
    for (auto &block : F.blocks) {
        if (block.label == label)
            return &block;
    }
    return nullptr;
}

/// @brief Build a map of all predecessors for each block.
struct PredEdge {
    /// Block owning the incoming terminator edge.
    il::core::BasicBlock *block = nullptr;
    /// Successor slot in that terminator.
    size_t edgeIndex = 0;
};

/// @brief Index every concrete predecessor edge by target label.
/// @param F Function whose terminators are scanned.
/// @return Edge lists preserving duplicate successor slots.
std::unordered_map<std::string, std::vector<PredEdge>> buildPredecessorMap(il::core::Function &F) {
    std::unordered_map<std::string, std::vector<PredEdge>> preds;

    for (auto &block : F.blocks) {
        const il::core::Instr *term = findTerminator(block);
        if (!term)
            continue;

        for (size_t edge = 0; edge < term->labels.size(); ++edge) {
            preds[term->labels[edge]].push_back(PredEdge{&block, edge});
        }
    }

    return preds;
}

/// @brief Determine what constant value (if any) flows to a block parameter.
/// @param pred Predecessor block supplying the argument.
/// @param target Expected target block.
/// @param edgeIndex Successor slot from @p pred to @p target.
/// @param paramIndex Target block-parameter position.
/// @return Integer, float, or null constant at that slot, otherwise `std::nullopt`.
std::optional<il::core::Value> getConstantArgForParam(const il::core::BasicBlock &pred,
                                                      const il::core::BasicBlock &target,
                                                      size_t edgeIndex,
                                                      size_t paramIndex) {
    const il::core::Instr *term = findTerminator(pred);
    if (!term)
        return std::nullopt;

    if (edgeIndex >= term->labels.size() || term->labels[edgeIndex] != target.label)
        return std::nullopt;

    if (edgeIndex >= term->brArgs.size())
        return std::nullopt;

    const auto &args = term->brArgs[edgeIndex];
    if (paramIndex >= args.size())
        return std::nullopt;

    const il::core::Value &arg = args[paramIndex];
    if (arg.kind == il::core::Value::Kind::ConstInt ||
        arg.kind == il::core::Value::Kind::ConstFloat ||
        arg.kind == il::core::Value::Kind::NullPtr) {
        return arg;
    }

    return std::nullopt;
}

/// @brief Check if a block is a simple conditional branch with condition from params.
/// @param block Candidate intermediate block.
/// @return Parameter index used as the branch condition, or `std::nullopt`.
std::optional<size_t> findConditionParamIndex(const il::core::BasicBlock &block) {
    if (block.instructions.empty())
        return std::nullopt;

    const il::core::Instr &term = block.instructions.back();
    if (term.op != il::core::Opcode::CBr)
        return std::nullopt;

    if (term.operands.empty())
        return std::nullopt;

    const il::core::Value &cond = term.operands[0];
    if (cond.kind != il::core::Value::Kind::Temp)
        return std::nullopt;

    // Check if the condition is a block parameter
    for (size_t i = 0; i < block.params.size(); ++i) {
        if (block.params[i].id == cond.id)
            return i;
    }

    return std::nullopt;
}

/// @brief Check if a block has only a conditional branch (no other instructions).
/// @param block Candidate intermediate block.
/// @return `true` when its sole instruction is a conditional branch.
bool isSimpleCbrBlock(const il::core::BasicBlock &block) {
    // Allow blocks with only a cbr terminator, or with simple non-side-effect
    // instructions that can be duplicated
    if (block.instructions.empty())
        return false;

    const il::core::Instr &term = block.instructions.back();
    if (term.op != il::core::Opcode::CBr)
        return false;

    // For now, only thread if the block has just the cbr
    // More aggressive threading could duplicate small instruction sequences
    return block.instructions.size() == 1;
}

/// @brief Compute the arguments to pass to the threaded target.
/// @param pred Predecessor whose edge is redirected.
/// @param intermediate Conditional block being bypassed.
/// @param target Selected successor of @p intermediate.
/// @param predToIntermediateEdge Successor slot on @p pred.
/// @param targetBranchIdx Successor slot on the intermediate terminator.
/// @return Target arguments after substituting intermediate parameters, or
///         `std::nullopt` for inconsistent edge arity.
std::optional<std::vector<il::core::Value>> computeThreadedArgs(
    const il::core::BasicBlock &pred,
    const il::core::BasicBlock &intermediate,
    const il::core::BasicBlock &target,
    size_t predToIntermediateEdge,
    size_t targetBranchIdx) {
    const il::core::Instr *predTerm = findTerminator(pred);
    const il::core::Instr *intTerm = findTerminator(intermediate);
    if (!predTerm || !intTerm)
        return std::nullopt;

    // Get args that pred passes to intermediate
    if (predToIntermediateEdge >= predTerm->labels.size() ||
        predTerm->labels[predToIntermediateEdge] != intermediate.label)
        return std::nullopt;

    static const std::vector<il::core::Value> kNoArgs;
    const std::vector<il::core::Value> *predToIntArgs = &kNoArgs;
    if (predToIntermediateEdge < predTerm->brArgs.size()) {
        predToIntArgs = &predTerm->brArgs[predToIntermediateEdge];
    } else if (!intermediate.params.empty()) {
        return std::nullopt;
    }

    if (predToIntArgs->size() != intermediate.params.size())
        return std::nullopt;

    // Build mapping: intermediate param id -> value from pred
    std::unordered_map<unsigned, il::core::Value> mapping;
    for (size_t i = 0; i < intermediate.params.size(); ++i) {
        mapping[intermediate.params[i].id] = (*predToIntArgs)[i];
    }

    // Get args that intermediate would pass to target
    const std::vector<il::core::Value> *intToTargetArgs = &kNoArgs;
    if (targetBranchIdx < intTerm->brArgs.size()) {
        intToTargetArgs = &intTerm->brArgs[targetBranchIdx];
    } else if (!target.params.empty()) {
        return std::nullopt;
    }

    if (intToTargetArgs->size() != target.params.size())
        return std::nullopt;

    // Substitute values through the mapping
    std::vector<il::core::Value> result;
    result.reserve(intToTargetArgs->size());
    for (const auto &arg : *intToTargetArgs) {
        result.push_back(substituteValue(arg, mapping));
    }

    return result;
}

} // namespace

/// @copydoc threadJumps()
bool threadJumps(SimplifyCFG::SimplifyCFGPassContext &ctx) {
    il::core::Function &F = ctx.function;
    bool changed = false;

    // Build predecessor map
    auto predecessors = buildPredecessorMap(F);

    // Collect blocks to thread (don't modify while iterating)
    /// @brief Fully validated edge rewrite applied after the discovery scan.
    struct ThreadingCandidate {
        /// Predecessor whose terminator is changed.
        il::core::BasicBlock *pred{nullptr};
        /// Predictable conditional block being bypassed.
        il::core::BasicBlock *intermediate{nullptr};
        /// Selected final target label.
        std::string newTarget;
        /// Arguments after substituting intermediate block parameters.
        std::vector<il::core::Value> newArgs;
        /// Successor slot in the predecessor terminator.
        size_t predBranchIdx{0};
    };

    std::vector<ThreadingCandidate> candidates;

    for (auto &block : F.blocks) {
        // Skip EH-sensitive blocks
        if (ctx.isEHSensitive(block))
            continue;

        // Check if this is a simple cbr block
        if (!isSimpleCbrBlock(block))
            continue;

        // Block parameters may be used by dominated successor blocks. Bypassing
        // this block would remove the dominating definition without rewriting
        // those successor uses, so leave that shape to later, dominance-aware
        // cleanups.
        if (blockParamsUsedOutside(F, block))
            continue;

        // Find the condition parameter index
        auto condParamIdx = findConditionParamIndex(block);
        if (!condParamIdx)
            continue;

        const il::core::Instr &term = block.instructions.back();
        if (term.labels.size() != 2)
            continue;

        // Check each predecessor
        auto predIt = predecessors.find(block.label);
        if (predIt == predecessors.end())
            continue;

        for (PredEdge predEdge : predIt->second) {
            il::core::BasicBlock *pred = predEdge.block;
            // Skip self-loops
            if (pred == &block)
                continue;

            // Skip EH-sensitive predecessors
            if (ctx.isEHSensitive(*pred))
                continue;

            // Check if pred passes a constant for the condition
            auto constArg = getConstantArgForParam(*pred, block, predEdge.edgeIndex, *condParamIdx);
            if (!constArg)
                continue;

            // Determine which branch to take based on the constant
            bool condValue = false;
            if (constArg->kind == il::core::Value::Kind::ConstInt) {
                condValue = (constArg->i64 != 0);
            } else {
                continue; // Only handle integer constants for now
            }

            // CBr: true branch is index 0, false branch is index 1
            size_t targetBranchIdx = condValue ? 0 : 1;
            const std::string &newTarget = term.labels[targetBranchIdx];

            // Compute the arguments for the threaded jump
            auto *targetBlock = findBlock(F, newTarget);
            if (!targetBlock)
                continue;
            auto newArgs = computeThreadedArgs(
                *pred, block, *targetBlock, predEdge.edgeIndex, targetBranchIdx);
            if (!newArgs)
                continue;

            // Find which branch index in pred goes to this block
            il::core::Instr *predTerm = findTerminator(*pred);
            if (!predTerm)
                continue;

            if (predEdge.edgeIndex >= predTerm->labels.size() ||
                predTerm->labels[predEdge.edgeIndex] != block.label)
                continue;

            candidates.push_back({pred, &block, newTarget, *newArgs, predEdge.edgeIndex});
        }
    }

    // Apply threading transformations
    for (const auto &candidate : candidates) {
        il::core::Instr *predTerm = findTerminator(*candidate.pred);
        if (!predTerm)
            continue;

        // Update the predecessor's terminator
        if (candidate.predBranchIdx < predTerm->labels.size()) {
            predTerm->labels[candidate.predBranchIdx] = candidate.newTarget;
        }

        if (!candidate.newArgs.empty() || !predTerm->brArgs.empty()) {
            if (predTerm->brArgs.size() < predTerm->labels.size())
                predTerm->brArgs.resize(predTerm->labels.size());
            predTerm->brArgs[candidate.predBranchIdx] = candidate.newArgs;
        }

        changed = true;

        if (ctx.isDebugLoggingEnabled()) {
            std::string message = "threaded jump from '" + candidate.pred->label + "' through '" +
                                  candidate.intermediate->label + "' to '" + candidate.newTarget +
                                  "'";
            ctx.logDebug(message);
        }
    }

    return changed;
}

} // namespace il::transform::simplify_cfg
