//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/il/transform/RuntimeFastPathOpt.hpp
// Purpose: Select specialized runtime helpers after simple provenance proofs.
// Key invariants:
//   - Only values returned by runtime signatures marked as known objects qualify.
//   - The factory definition must dominate the rewritten ownership call.
// Ownership/Lifetime: Stateless FunctionPass instantiated by the registry.
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Declares provenance-guided specialization of runtime ownership calls.
 *
 * @details The function pass uses live runtime signature metadata and
 *          dominance to prove when a value is a heap object rather than a
 *          string-like tagged handle. Generic retain/release calls can then be
 *          replaced with known-object helpers that skip dynamic discrimination.
 */

#pragma once

#include "il/transform/PassRegistry.hpp"

namespace il::transform {

/// @brief Specialize generic object ownership helpers using proven provenance.
/// @details Tracks direct calls whose runtime signatures guarantee an object
///          result, then rewrites dominated retain/release calls to helpers that
///          can omit dynamic object-versus-string discrimination.
class RuntimeFastPathOpt : public FunctionPass {
  public:
    /// @brief Return the stable pipeline identifier.
    /// @return The pass name `"runtime-fastpath"`.
    std::string_view id() const override;

    /// @brief Select known-object helper variants within one function.
    /// @param function Function whose direct-call callee names may be rewritten.
    /// @param analysis Manager supplying dominance information.
    /// @return All analyses when unchanged; otherwise CFG-related analyses are preserved.
    PreservedAnalyses run(core::Function &function, AnalysisManager &analysis) override;
};

/// @brief Register the runtime fast-path function-pass factory.
/// @param registry Registry that receives the parallel-safe pass entry.
void registerRuntimeFastPathOptPass(PassRegistry &registry);

} // namespace il::transform
