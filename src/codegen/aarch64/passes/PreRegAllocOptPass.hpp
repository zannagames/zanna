//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/passes/PreRegAllocOptPass.hpp
// Purpose: Pass-manager wrapper for AArch64 pre-register-allocation MIR cleanup.
// Key invariants:
//   - Must run after LegalizePass and before RegAllocPass.
//   - Delegates to PreRegAllocOpt free functions after validating target state.
// Ownership/Lifetime: Stateless pass; mutates AArch64Module::mir in place.
// Links: src/codegen/aarch64/passes/PreRegAllocOptPass.cpp,
//        src/codegen/aarch64/PreRegAllocOpt.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "codegen/aarch64/passes/PassManager.hpp"

/**
 * @file
 * @brief Declares the pass-manager adapter for pre-allocation MIR cleanup.
 */

namespace zanna::codegen::aarch64::passes {

/// @brief Runs virtual-copy forwarding and shifted-addressing folds before allocation.
class PreRegAllocOptPass final : public Pass {
  public:
    /**
     * @brief Applies pre-allocation cleanup to every MIR function.
     * @param[in,out] module Lowered/legalized module to rewrite.
     * @param[in,out] diags Sink receiving a missing-target diagnostic.
     * @return `false` only when `module.ti` is null.
     */
    bool run(AArch64Module &module, Diagnostics &diags) override;
};

} // namespace zanna::codegen::aarch64::passes
