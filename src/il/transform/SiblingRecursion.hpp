//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/transform/SiblingRecursion.hpp
// Purpose: Declares the SiblingRecursion function pass — converts double
//          self-recursion with associative combination (e.g., fib(n-1)+fib(n-2))
//          into single recursion with an accumulator loop, halving total calls.
// Key invariants:
//   - Only fires on functions with exactly 2 self-recursive calls in the same
//     block whose results are combined with an associative+commutative add.
//   - The combined result must be immediately returned.
//   - CFG is restructured: the recurse block gains an accumulator parameter,
//     and a new "done" exit block is created.
// Ownership/Lifetime: Stateless FunctionPass; instantiated by the registry.
// Links: il/transform/PassRegistry.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Declares reduction of eligible sibling self-recursion to one recursive call.
 *
 * @details The pass recognizes a narrowly defined pair of self-calls whose
 *          results are immediately combined by associative integer addition.
 *          It retains one recursive call, carries the first result through an
 *          accumulator block parameter, and loops over the second subproblem
 *          until the original signed base case reaches a new exit block.
 */

#pragma once

#include "il/transform/PassRegistry.hpp"

namespace il::transform {

/// @brief Convert double self-recursion with associative combination into
///        single recursion with an accumulator loop.
///
/// Detects patterns like `fib(n) = fib(n-1) + fib(n-2)` and transforms the
/// second recursive call into a loop iteration, halving total function calls.
class SiblingRecursion : public FunctionPass {
  public:
    /// @brief Return the stable pipeline identifier.
    /// @return The pass name `"sibling-recursion"`.
    std::string_view id() const override;

    /// @brief Convert one eligible double-recursive function into an accumulator loop.
    /// @param function Function inspected and possibly restructured.
    /// @param analysis Analysis manager supplied by the pipeline; not queried.
    /// @return All analyses if no pattern matches, otherwise no preserved analyses.
    PreservedAnalyses run(core::Function &function, AnalysisManager &analysis) override;
};

/// @brief Register the SiblingRecursion pass with the provided registry.
/// @param registry Registry that receives the parallel-safe function-pass factory.
void registerSiblingRecursionPass(PassRegistry &registry);

} // namespace il::transform
