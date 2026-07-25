//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/analysis/IntRangeAnalysis.hpp
// Purpose: Whole-function signed integer value-range analysis over SSA form.
// Key invariants:
//   - Ranges are conservative: a recorded range always contains every value
//     the temp can hold on entry to the block; absence of a range means
//     "unknown", never "impossible".
//   - The fixpoint uses bounded widening, so computation always terminates.
// Ownership/Lifetime:
//   - IntRangeInfo is a value type cached by the AnalysisManager; it holds no
//     pointers into the function it describes and is invalidated by any
//     structural mutation.
// Links: il/utils/CheckedIntRange.hpp, il/transform/CheckOpt.cpp, il/verify/InstructionChecker.cpp
//
//===----------------------------------------------------------------------===//

#pragma once

/// @brief Forward value-range dataflow for IL functions.
/// @invariant Block-entry range maps are sound over-approximations.
/// @ownership Value-type result owned by the AnalysisManager cache.

#include "il/utils/CheckedIntRange.hpp"

#include <optional>
#include <string>
#include <unordered_map>

namespace il::core {
struct BasicBlock;
struct Function;
struct Instr;
struct Value;
} // namespace il::core

namespace zanna::analysis {

/// @brief Map from SSA temp id to its known signed integer range.
using RangeMap = std::unordered_map<unsigned, ::il::utils::IntRange>;

/// @brief Whole-function value-range facts keyed by block label.
/// @details For each reachable basic block, records the ranges known to hold
///          for SSA temps (block params and dominating definitions) on entry
///          to the block, after merging every incoming CFG edge with its
///          branch-condition refinements.
struct IntRangeInfo {
    /// @brief Block label -> entry range facts for that block.
    std::unordered_map<std::string, RangeMap> blockEntry;

    /// @brief Look up the entry facts for @p label.
    /// @param label Basic-block label.
    /// @return Pointer to the entry map, or nullptr for unknown/unreachable
    ///         blocks (treat as "no facts").
    const RangeMap *entryFor(const std::string &label) const {
        auto it = blockEntry.find(label);
        return it == blockEntry.end() ? nullptr : &it->second;
    }
};

/// @brief Resolve the range of an operand value against known temp ranges.
/// @param value Operand to resolve; constants yield exact ranges.
/// @param ranges Known ranges for SSA temps.
/// @return Range when the operand is a constant or a mapped temp.
std::optional<::il::utils::IntRange> rangeForValue(const ::il::core::Value &value, const RangeMap &ranges);

/// @brief Derive the range implied for one side of a compare-driven branch.
/// @details Matches `cbr (cmp %v, C)`-shaped conditions (either operand order)
///          and reports the constraint that holds on the requested edge.
/// @param cmp Compare instruction feeding the conditional branch.
/// @param branchIndex Edge index (0 = taken/true, 1 = fallthrough/false).
/// @param constrainedValue Out: the SSA value the constraint applies to.
/// @param range Out: the implied inclusive range.
/// @return True when a constraint could be derived.
bool deriveCompareBranchRange(const ::il::core::Instr &cmp,
                              size_t branchIndex,
                              ::il::core::Value &constrainedValue,
                              ::il::utils::IntRange &range);

/// @brief Apply one instruction's range transfer function to @p ranges.
/// @details Computes the result range where the opcode supports it (checked
///          and plain arithmetic, masks, shifts, compares, checked casts and
///          bounds checks) and records check post-conditions: after `idx.chk`
///          with constant bounds the checked index is known in-bounds for all
///          later uses. Unknown results erase any stale mapping for the temp.
/// @param instr Instruction to interpret.
/// @param ranges In/out range state; updated in place.
/// @return The result range recorded for @p instr's temp, if any.
std::optional<::il::utils::IntRange> applyRangeTransfer(const ::il::core::Instr &instr, RangeMap &ranges);

/// @brief Bound the signed modulo-by-power-of-two bit-twiddle idiom.
/// @details The peephole pass strength-reduces `srem %z, 2^k` into
///          `%z - ((%z + (ashr(%z,63) & (2^k-1))) & -2^k)`, erasing the clean
///          `srem` that @ref applyRangeTransfer knows how to bound. This matcher
///          re-derives the identical bound directly from the lowered shape, so a
///          checked op CheckOpt demoted on the pre-lowered `srem` stays provable
///          after lowering. Requires @p block to contain @p subInstr and its
///          operand-defining instructions (straight-line, same block).
/// @param block Block containing @p subInstr (searched for operand defs).
/// @param subInstr Candidate `sub`/`isub.ovf` forming the idiom's outer subtract.
/// @param ranges Ranges known before @p subInstr (supplies the bias bound).
/// @return `[-(2^k - 1), 2^k - 1]` on an exact structural match, else nullopt.
std::optional<::il::utils::IntRange> matchPow2ModuloRange(const ::il::core::BasicBlock &block,
                                                          const ::il::core::Instr &subInstr,
                                                          const RangeMap &ranges);

/// @brief Compute block-entry integer ranges for every block in @p fn.
/// @details Forward worklist dataflow over the CFG. Edge facts start from the
///          predecessor's exit state, are refined by the branch condition on
///          that edge, and bind branch arguments to the target's block params.
///          Facts merge at joins via interval union; entries that keep
///          changing are widened away after a bounded number of updates.
/// @param fn Function to analyse.
/// @return Entry range facts for each reachable block.
IntRangeInfo computeIntRanges(const ::il::core::Function &fn);

} // namespace zanna::analysis
