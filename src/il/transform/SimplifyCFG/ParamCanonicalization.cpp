//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Implements the SimplifyCFG parameter canonicalisation routines.  The helpers
// tighten block parameter lists by removing unused entries and by eliminating
// parameters that receive the same value from every predecessor.  They also
// adjust predecessor branch arguments so control-flow edges remain arity
// compatible.  The transformations operate in place on a function and preserve
// the module's semantics.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Utilities that keep block parameters and branch arguments aligned.
/// @details Provides the core algorithms used by the SimplifyCFG pass to drop
///          redundant block parameters, update predecessor edges, and maintain
///          argument ordering without rebuilding surrounding data structures.

#include "il/transform/SimplifyCFG/ParamCanonicalization.hpp"

#include "il/transform/SimplifyCFG/Utils.hpp"

#include "il/core/BasicBlock.hpp"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace il::transform::simplify_cfg {
namespace {

/// @brief Synchronise predecessor branch arguments with an updated block signature.
///
/// @details Iterates over every predecessor terminator that targets @p block and
///          ensures its branch argument list mirrors the block's current
///          parameter layout.  Arguments are truncated when the block dropped
///          parameters, cleared when the block takes no parameters, and verified
///          to remain in lock-step to avoid mismatched arities after other
///          canonicalisation steps.
///
/// @param ctx   SimplifyCFG context providing access to the parent function.
/// @param block Block whose incoming arguments must be realigned.
/// @return `true` when every targeted edge has the required argument arity.
bool realignBranchArgs(SimplifyCFG::SimplifyCFGPassContext &ctx, il::core::BasicBlock &block) {
    bool ok = true;
    for (auto &pred : ctx.function.blocks) {
        il::core::Instr *term = findTerminator(pred);
        if (!term)
            continue;

        for (size_t edgeIdx = 0; edgeIdx < term->labels.size(); ++edgeIdx) {
            if (term->labels[edgeIdx] != block.label)
                continue;

            if (term->brArgs.empty())
                continue;

            if (term->brArgs.size() <= edgeIdx) {
                ok = false;
                continue;
            }

            auto &args = term->brArgs[edgeIdx];

            if (block.params.empty()) {
                args.clear();
                continue;
            }

            if (args.size() > block.params.size())
                args.resize(block.params.size());

            if (args.size() != block.params.size())
                ok = false;
        }
    }
    if (!ok && ctx.isDebugLoggingEnabled())
        ctx.logDebug("skipped full branch argument realignment for malformed block '" +
                     block.label + "'");
    return ok;
}

/// @brief Check that every incoming edge carries a full argument list for @p block.
/// @details Parameter canonicalization mutates the block signature and predecessor
///          arguments together.  If any predecessor is already malformed, the
///          helper skips the rewrite instead of making the arity mismatch worse.
/// @param ctx SimplifyCFG context providing the parent function.
/// @param block Block whose incoming edges should be inspected.
/// @return True when all incoming edges either do not target @p block or carry
///         exactly one argument per current block parameter.
bool incomingArgsAligned(SimplifyCFG::SimplifyCFGPassContext &ctx,
                         const il::core::BasicBlock &block) {
    for (auto &pred : ctx.function.blocks) {
        il::core::Instr *term = findTerminator(pred);
        if (!term)
            continue;

        for (size_t edgeIdx = 0; edgeIdx < term->labels.size(); ++edgeIdx) {
            if (term->labels[edgeIdx] != block.label)
                continue;
            if (term->brArgs.empty())
                continue;
            if (term->brArgs.size() <= edgeIdx)
                return false;
            if (term->brArgs[edgeIdx].size() != block.params.size())
                return false;
        }
    }
    return true;
}

/// @brief Remove parameters that receive the same value from every predecessor.
///
/// @details Walks all incoming edges to @p block and checks whether each block
///          parameter is always passed the same SSA value.  When a unanimous
///          value is found, the helper substitutes that value directly inside
///          the block and erases the parameter alongside the corresponding
///          branch arguments.  The scan repeats until no more parameters can be
///          eliminated, guaranteeing a fixed point even when substitutions
///          expose additional redundancies.
///
/// @param ctx   SimplifyCFG context exposing the current function and logging.
/// @param block Block under inspection.
/// @returns True if any parameters were removed.
bool shrinkParamsEqualAcrossPreds(SimplifyCFG::SimplifyCFGPassContext &ctx,
                                  il::core::BasicBlock &block) {
    bool removedAny = false;

    while (true) {
        bool removedThisIteration = false;

        for (size_t paramIdx = 0; paramIdx < block.params.size();) {
            const unsigned paramId = block.params[paramIdx].id;
            il::core::Value commonValue{};
            bool hasCommonValue = false;
            bool mismatch = false;

            for (auto &pred : ctx.function.blocks) {
                il::core::Instr *term = findTerminator(pred);
                if (!term)
                    continue;

                for (size_t edgeIdx = 0; edgeIdx < term->labels.size(); ++edgeIdx) {
                    if (term->labels[edgeIdx] != block.label)
                        continue;

                    if (term->brArgs.size() <= edgeIdx) {
                        mismatch = true;
                        break;
                    }

                    const auto &args = term->brArgs[edgeIdx];
                    if (args.size() != block.params.size()) {
                        mismatch = true;
                        break;
                    }

                    const il::core::Value &incoming = args[paramIdx];
                    if (!hasCommonValue) {
                        commonValue = incoming;
                        hasCommonValue = true;
                    } else if (!valuesEqual(incoming, commonValue)) {
                        mismatch = true;
                        break;
                    }
                }

                if (mismatch)
                    break;
            }

            if (!hasCommonValue || mismatch) {
                ++paramIdx;
                continue;
            }

            if (!incomingArgsAligned(ctx, block)) {
                ++paramIdx;
                continue;
            }

            // Block params may be used in dominated successor blocks after
            // mem2reg and frontend boolean lowering. This helper only rewrites
            // the current block; removing a param with cross-block uses would
            // leave those successor references dangling.
            if (isTempUsedOutsideBlock(ctx.function, block, paramId)) {
                ++paramIdx;
                continue;
            }

            // Do not substitute cross-block temporaries.  When the common value
            // is a Temp defined in a different block (e.g. a predecessor's
            // parameter), substituting it here creates an implicit cross-block
            // reference.  Later passes such as dropUnusedParams may then
            // incorrectly remove the defining parameter, leaving dangling uses.
            // Constants and non-temp values are always safe to substitute.
            if (commonValue.kind == il::core::Value::Kind::Temp) {
                // Check if the common value is defined within this block
                bool definedLocally = false;
                for (const auto &p : block.params) {
                    if (p.id == commonValue.id) {
                        definedLocally = true;
                        break;
                    }
                }
                if (!definedLocally) {
                    for (const auto &instr : block.instructions) {
                        if (instr.result && *instr.result == commonValue.id) {
                            definedLocally = true;
                            break;
                        }
                    }
                }
                if (!definedLocally) {
                    ++paramIdx;
                    continue;
                }
            }

            /// Substitute the unanimous incoming value for one block parameter use.
            auto replaceUses = [&](il::core::Value &value) {
                if (value.kind == il::core::Value::Kind::Temp && value.id == paramId)
                    value = commonValue;
            };

            for (auto &instr : block.instructions) {
                for (auto &operand : instr.operands)
                    replaceUses(operand);

                for (auto &argList : instr.brArgs) {
                    for (auto &val : argList)
                        replaceUses(val);
                }
            }

            for (auto &pred : ctx.function.blocks) {
                il::core::Instr *term = findTerminator(pred);
                if (!term)
                    continue;

                for (size_t edgeIdx = 0; edgeIdx < term->labels.size(); ++edgeIdx) {
                    if (term->labels[edgeIdx] != block.label)
                        continue;

                    if (term->brArgs.size() <= edgeIdx)
                        continue;

                    auto &args = term->brArgs[edgeIdx];
                    if (paramIdx < args.size()) {
                        args.erase(args.begin() + static_cast<std::ptrdiff_t>(paramIdx));
                    }
                }
            }

            block.params.erase(block.params.begin() + static_cast<std::ptrdiff_t>(paramIdx));
            removedThisIteration = true;
            removedAny = true;
        }

        if (!removedThisIteration)
            break;
    }

    if (removedAny && !realignBranchArgs(ctx, block))
        return false;

    return removedAny;
}

/// @brief Drop block parameters whose SSA value is never referenced.
///
/// @details Scans the block's instructions and branch arguments to determine
///          whether each parameter identifier is used.  When a parameter is
///          dead, the helper erases it and prunes the matching argument from
///          every predecessor edge before finally realigning the remaining
///          arguments.  The process repeats until all unused parameters are
///          removed so later passes operate on a minimal signature.
///
/// @param ctx   SimplifyCFG context with access to the function being mutated.
/// @param block Block whose parameters are assessed.
/// @param allUsedIds Function-wide set of temporary ids appearing in value uses.
/// @returns True if any parameters were eliminated.
bool dropUnusedParams(SimplifyCFG::SimplifyCFGPassContext &ctx,
                      il::core::BasicBlock &block,
                      const std::unordered_set<unsigned> &allUsedIds) {
    bool removedAny = false;

    for (size_t paramIdx = 0; paramIdx < block.params.size();) {
        const unsigned paramId = block.params[paramIdx].id;
        const bool used = allUsedIds.count(paramId) > 0;

        if (used) {
            ++paramIdx;
            continue;
        }

        if (!incomingArgsAligned(ctx, block)) {
            ++paramIdx;
            continue;
        }

        for (auto &pred : ctx.function.blocks) {
            il::core::Instr *term = findTerminator(pred);
            if (!term)
                continue;

            for (size_t edgeIdx = 0; edgeIdx < term->labels.size(); ++edgeIdx) {
                if (term->labels[edgeIdx] != block.label)
                    continue;

                if (term->brArgs.size() <= edgeIdx)
                    continue;

                auto &args = term->brArgs[edgeIdx];
                if (paramIdx < args.size()) {
                    args.erase(args.begin() + static_cast<std::ptrdiff_t>(paramIdx));
                }
            }
        }

        block.params.erase(block.params.begin() + static_cast<std::ptrdiff_t>(paramIdx));
        removedAny = true;
    }

    if (removedAny && !realignBranchArgs(ctx, block))
        return false;

    return removedAny;
}

} // namespace

/// @brief Entry point that canonicalises parameters and branch arguments.
///
/// @details Iterates the function's blocks, skipping exception-handling regions
///          where parameter manipulation is unsafe, and applies both redundancy
///          elimination helpers.  The routine aggregates statistics, emits
///          optional debug logs, and returns whether the function changed so the
///          surrounding pass manager can schedule follow-up work when needed.
///
/// @param ctx SimplifyCFG context bound to the function under transformation.
/// @returns True if any block parameters or arguments were simplified.
bool canonicalizeParamsAndArgs(SimplifyCFG::SimplifyCFGPassContext &ctx) {
    il::core::Function &F = ctx.function;

    bool changed = false;

    // Pre-compute the set of all temp IDs used across the entire function ONCE.
    // dropUnusedParams checks this set to determine if a block param is referenced
    // anywhere (not just in the defining block — cross-block domination uses exist
    // after mem2reg). Building the set here avoids O(blocks²) per-block scanning.
    std::unordered_set<unsigned> allUsedIds;
    for (const auto &scanBlock : F.blocks) {
        for (const auto &instr : scanBlock.instructions) {
            for (const auto &operand : instr.operands) {
                if (operand.kind == il::core::Value::Kind::Temp)
                    allUsedIds.insert(operand.id);
            }
            for (const auto &argList : instr.brArgs) {
                for (const auto &value : argList) {
                    if (value.kind == il::core::Value::Kind::Temp)
                        allUsedIds.insert(value.id);
                }
            }
        }
    }

    // Build a set of function-argument param IDs for entry block protection.
    std::unordered_set<unsigned> funcParamIds;
    for (const auto &fp : F.params)
        funcParamIds.insert(fp.id);

    for (auto &block : F.blocks) {
        if (ctx.isEHSensitive(block))
            continue;

        if (block.params.empty())
            continue;

        // For the entry block, only remove params that are NOT function arguments
        // and are genuinely unused. Function-argument params must be preserved
        // because external callers pass a fixed number of arguments.
        // Skip shrink/drop entirely for entry block to be safe — let DCE handle it
        // with its own funcParamId protection.
        if (&block == &F.blocks.front())
            continue;

        const size_t beforeShrink = block.params.size();
        if (shrinkParamsEqualAcrossPreds(ctx, block)) {
            const size_t removed = beforeShrink - block.params.size();
            if (removed > 0) {
                changed = true;
                ctx.stats.paramsShrunk += removed;
                if (ctx.isDebugLoggingEnabled()) {
                    std::string message = "replaced duplicated params in block '" + block.label +
                                          "', removed " + std::to_string(removed);
                    ctx.logDebug(message);
                }
            }
        }

        if (block.params.empty())
            continue;

        const size_t beforeDrop = block.params.size();
        if (dropUnusedParams(ctx, block, allUsedIds)) {
            const size_t removed = beforeDrop - block.params.size();
            if (removed > 0) {
                changed = true;
                ctx.stats.paramsShrunk += removed;
                if (ctx.isDebugLoggingEnabled()) {
                    std::string message = "dropped unused params in block '" + block.label +
                                          "', removed " + std::to_string(removed);
                    ctx.logDebug(message);
                }
            }
        }
    }

    return changed;
}

} // namespace il::transform::simplify_cfg
