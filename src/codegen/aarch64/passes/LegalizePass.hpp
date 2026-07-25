//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/passes/LegalizePass.hpp
// Purpose: Declare the AArch64 MIR legalization pass.
// Key invariants: Runs after LoweringPass and before RegAllocPass. All
//                 overflow pseudos and backend-required entry sequences must
//                 be expanded before register allocation and emission.
// Ownership/Lifetime: Stateless pass; mutates AArch64Module::mir in place.
// Links: docs/internals/codemap.md, src/codegen/aarch64/LowerOvf.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "codegen/aarch64/passes/PassManager.hpp"

/**
 * @file
 * @brief Declares pre-register-allocation AArch64 MIR legalization.
 */

namespace zanna::codegen::aarch64::passes {

/**
 * @brief Expands required pseudos/entry setup and recomputes call metadata.
 */
class LegalizePass final : public Pass {
  public:
    /**
     * @brief Legalizes every MIR function before register allocation.
     * @param[in,out] module Module whose MIR is expanded in place.
     * @param[in,out] diags Sink receiving missing-target or expansion errors.
     * @return `true` when all functions legalize successfully.
     */
    bool run(AArch64Module &module, Diagnostics &diags) override;
};

} // namespace zanna::codegen::aarch64::passes
