//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Implements the SimplifyCFG logic that recognises and removes empty forwarding
// blocks.  These blocks contain only a trivial branch to a successor and exist
// solely to forward arguments.  The routines below redirect predecessor edges
// to bypass the forwarding block, substitute argument values directly, and
// finally erase the redundant block once all edges have been retargeted.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Forwarding-block elimination helpers for SimplifyCFG.
/// @details Provides predicates for identifying empty forwarders, routines for
///          updating predecessor terminators, and the pass entry point that
///          collects statistics and debug output.

#include "il/transform/SimplifyCFG/ForwardingElimination.hpp"

#include "il/transform/SimplifyCFG/Utils.hpp"

#include "il/core/BasicBlock.hpp"
#include "il/core/Function.hpp"
#include "il/core/Instr.hpp"
#include "il/core/Opcode.hpp"
#include "il/transform/LoadSafety.hpp"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

namespace il::transform::simplify_cfg {
namespace {

/// @brief Classify opcodes whose unused execution is pure and non-trapping.
/// @param op Opcode to inspect.
/// @return `true` for the explicitly enumerated arithmetic, comparison, cast,
///         and constant/address operations.
bool isAlwaysNonTrappingSideEffectFree(il::core::Opcode op) {
    using il::core::Opcode;
    switch (op) {
        case Opcode::And:
        case Opcode::Or:
        case Opcode::Xor:
        case Opcode::Shl:
        case Opcode::LShr:
        case Opcode::AShr:
        case Opcode::ICmpEq:
        case Opcode::ICmpNe:
        case Opcode::SCmpLT:
        case Opcode::SCmpLE:
        case Opcode::SCmpGT:
        case Opcode::SCmpGE:
        case Opcode::UCmpLT:
        case Opcode::UCmpLE:
        case Opcode::UCmpGT:
        case Opcode::UCmpGE:
        case Opcode::FAdd:
        case Opcode::FSub:
        case Opcode::FMul:
        case Opcode::FDiv:
        case Opcode::FCmpEQ:
        case Opcode::FCmpNE:
        case Opcode::FCmpLT:
        case Opcode::FCmpLE:
        case Opcode::FCmpGT:
        case Opcode::FCmpGE:
        case Opcode::Zext1:
        case Opcode::Trunc1:
        case Opcode::ConstF64:
        case Opcode::ConstNull:
        case Opcode::GAddr:
        case Opcode::AddrOf:
            return true;
        default:
            return false;
    }
}

/// @brief Determine whether an unused instruction may disappear with a forwarder.
/// @param function Function providing allocation provenance for load safety.
/// @param instr Non-terminator instruction to classify.
/// @return `true` for a known pure/non-trapping operation or a proven-safe load.
bool canEraseForwardingInstruction(const il::core::Function &function,
                                   const il::core::Instr &instr) {
    if (hasSideEffects(instr))
        return false;
    if (instr.op == il::core::Opcode::Load)
        return isLoadKnownNonTrapping(function, instr);
    return isAlwaysNonTrappingSideEffectFree(instr.op);
}

/// @brief Canonicalize a non-switch terminator whose argument bundles are all empty.
/// @param instr Terminator whose redundant empty bundle vector may be cleared.
void clearIfAllBranchArgsEmpty(il::core::Instr &instr) {
    if (instr.op == il::core::Opcode::SwitchI32)
        return;
    if (instr.brArgs.empty())
        return;
    for (const auto &args : instr.brArgs)
        if (!args.empty())
            return;
    instr.brArgs.clear();
}

/// @brief Check whether a block is an EH-safe forwarding trampoline.
///
/// @details Valid forwarding blocks start with optional non-side-effecting
///          instructions, end with a single @c br terminator, and merely pass
///          through branch arguments to a unique successor.  The predicate
///          verifies exception-handling constraints, ensures no side effects
///          occur before the terminator, and rejects blocks whose terminator
///          reuses locally defined temporaries in its arguments.
///
/// @param ctx   SimplifyCFG context providing EH sensitivity checks.
/// @param block Candidate block to inspect.
/// @returns True when the block can be safely removed.
bool isEmptyForwardingBlock(SimplifyCFG::SimplifyCFGPassContext &ctx,
                            const il::core::BasicBlock &block) {
    if (isEntryLabel(block.label))
        return false;

    if (ctx.isEHSensitive(block))
        return false;

    if (block.instructions.empty())
        return false;

    const il::core::Instr *terminator = findTerminator(block);
    if (!terminator)
        return false;

    if (terminator->op != il::core::Opcode::Br)
        return false;

    if (terminator->labels.size() != 1)
        return false;

    if (&block.instructions.back() != terminator)
        return false;

    std::unordered_set<unsigned> definedTemps;
    for (const auto &instr : block.instructions) {
        if (instr.result)
            definedTemps.insert(*instr.result);
    }

    for (const auto &instr : block.instructions) {
        if (&instr == terminator)
            break;

        if (!canEraseForwardingInstruction(ctx.function, instr))
            return false;
    }

    if (!terminator->brArgs.empty()) {
        if (terminator->brArgs.size() != 1)
            return false;

        for (const auto &value : terminator->brArgs.front()) {
            if (value.kind == il::core::Value::Kind::Temp && definedTemps.contains(value.id))
                return false;
        }
    }

    // Reject blocks whose definitions are referenced from other blocks.  Loop
    // preheaders often contain pure setup instructions whose results are used
    // inside the loop; bypassing the preheader would erase those definitions.
    if (blockParamsUsedOutside(ctx.function, block) || blockResultsUsedOutside(ctx.function, block))
        return false;

    return true;
}

/// @brief Retarget a predecessor's terminator to bypass a forwarding block.
///
/// @details Locates occurrences of @p dead in the predecessor's terminator,
///          substitutes the forwarding block's parameters with their incoming
///          arguments, and rebuilds the argument list to match the successor's
///          expectations.  When the forwarding block forwarded arguments, the
///          helper materialises the substituted values so that the successor sees
///          the same inputs it would have received prior to removal.
///
/// @param pred Predecessor block whose terminator will be rewritten.
/// @param dead Forwarding block slated for removal.
/// @param succ Successor originally targeted by @p dead.
/// @return Number of predecessor edges actually rewritten.
size_t redirectPredecessor(il::core::BasicBlock &pred,
                           il::core::BasicBlock &dead,
                           il::core::BasicBlock &succ) {
    il::core::Instr *predTerm = findTerminator(pred);
    if (!predTerm)
        return 0;

    bool referencesDead = false;
    for (const auto &label : predTerm->labels) {
        if (label == dead.label) {
            referencesDead = true;
            break;
        }
    }

    if (!referencesDead)
        return 0;

    il::core::Instr *deadTerm = findTerminator(dead);
    if (!deadTerm || deadTerm->op != il::core::Opcode::Br || deadTerm->labels.size() != 1)
        return 0;

    const std::vector<il::core::Value> *deadArgs = nullptr;
    if (!deadTerm->brArgs.empty()) {
        if (deadTerm->brArgs.size() != 1)
            return 0;
        deadArgs = &deadTerm->brArgs.front();
    }

    std::unordered_map<unsigned, il::core::Value> substitution;
    substitution.reserve(dead.params.size());
    size_t rewritten = 0;

    for (size_t idx = 0; idx < predTerm->labels.size(); ++idx) {
        if (predTerm->labels[idx] != dead.label)
            continue;

        std::vector<il::core::Value> incomingArgs;
        if (idx < predTerm->brArgs.size())
            incomingArgs = predTerm->brArgs[idx];

        if (incomingArgs.size() != dead.params.size())
            continue;

        substitution.clear();
        for (size_t paramIdx = 0; paramIdx < dead.params.size(); ++paramIdx)
            substitution.emplace(dead.params[paramIdx].id, incomingArgs[paramIdx]);

        std::vector<il::core::Value> newArgs;
        if (deadArgs) {
            newArgs.reserve(deadArgs->size());
            for (const auto &value : *deadArgs)
                newArgs.push_back(substituteValue(value, substitution));
        }

        predTerm->labels[idx] = succ.label;
        ++rewritten;
        if (deadArgs || !newArgs.empty() || !predTerm->brArgs.empty()) {
            if (predTerm->brArgs.size() < predTerm->labels.size())
                predTerm->brArgs.resize(predTerm->labels.size());
            predTerm->brArgs[idx] = std::move(newArgs);
            clearIfAllBranchArgsEmpty(*predTerm);
        }
    }
    return rewritten;
}

} // namespace

/// @brief Remove every empty forwarding block within a function.
///
/// @details Collects the labels of eligible forwarders, redirects all
///          predecessors around each block, and erases the block once no edges
///          refer to it.  Statistics and optional debug logs track how many
///          predecessors were retargeted and how many blocks were deleted.  The
///          function returns whether the control-flow graph changed so callers
///          can trigger follow-up cleanups.
///
/// @param ctx SimplifyCFG context owning the function under transformation.
/// @returns True if any blocks were rewritten or removed.
bool removeEmptyForwarders(SimplifyCFG::SimplifyCFGPassContext &ctx) {
    il::core::Function &F = ctx.function;
    bool changed = false;

    std::vector<std::string> forwardingBlocks;
    forwardingBlocks.reserve(F.blocks.size());
    for (const auto &block : F.blocks) {
        if (isEmptyForwardingBlock(ctx, block))
            forwardingBlocks.push_back(block.label);
    }

    size_t removedBlocks = 0;

    for (const auto &deadLabel : forwardingBlocks) {
        /// Relocate the recorded forwarder label in current block storage.
        auto deadIt =
            std::find_if(F.blocks.begin(), F.blocks.end(), [&](const il::core::BasicBlock &block) {
                return block.label == deadLabel;
            });
        if (deadIt == F.blocks.end())
            continue;

        il::core::BasicBlock &dead = *deadIt;
        il::core::Instr *deadTerm = findTerminator(dead);
        if (!deadTerm || deadTerm->labels.size() != 1)
            continue;

        const std::string &succLabel = deadTerm->labels.front();
        if (succLabel == dead.label)
            continue;

        /// Resolve the forwarder's successor label in current block storage.
        auto succIt =
            std::find_if(F.blocks.begin(), F.blocks.end(), [&](const il::core::BasicBlock &block) {
                return block.label == succLabel;
            });
        if (succIt == F.blocks.end())
            continue;

        il::core::BasicBlock &succ = *succIt;

        size_t redirected = 0;
        for (auto &pred : F.blocks) {
            il::core::Instr *predTerm = findTerminator(pred);
            if (!predTerm)
                continue;

            bool touchesDead = false;
            for (const auto &label : predTerm->labels) {
                if (label == dead.label) {
                    touchesDead = true;
                    break;
                }
            }

            if (!touchesDead)
                continue;

            redirected += redirectPredecessor(pred, dead, succ);
        }

        if (redirected > 0) {
            changed = true;
            ctx.stats.predsMerged += redirected;
            if (ctx.isDebugLoggingEnabled()) {
                std::string message = "redirected " + std::to_string(redirected) +
                                      " predecessor edges around block '" + dead.label + "'";
                ctx.logDebug(message);
            }
        }

        bool hasPreds = false;
        for (const auto &pred : F.blocks) {
            const il::core::Instr *predTerm = findTerminator(pred);
            if (!predTerm)
                continue;

            for (const auto &label : predTerm->labels) {
                if (label == dead.label) {
                    hasPreds = true;
                    break;
                }
            }

            if (hasPreds)
                break;
        }

        if (hasPreds)
            continue;

        F.blocks.erase(F.blocks.begin() + std::distance(F.blocks.begin(), deadIt));
        ++removedBlocks;
    }

    if (removedBlocks > 0) {
        changed = true;
        ctx.stats.emptyBlocksRemoved += removedBlocks;
        if (ctx.isDebugLoggingEnabled()) {
            std::string message = "removed " + std::to_string(removedBlocks) +
                                  " empty forwarding block" + (removedBlocks == 1 ? "" : "s");
            ctx.logDebug(message);
        }
    }

    return changed;
}

} // namespace il::transform::simplify_cfg
