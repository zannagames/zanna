//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/passes/LoweringPass.hpp
// Purpose: Declare the IL-to-MIR lowering pass for the AArch64 codegen pipeline.
// Key invariants: Populates AArch64Module::mir from AArch64Module::ilMod.
//                 ilMod and ti must be non-null before this pass runs.
// Ownership/Lifetime: Stateless pass; mutates the provided AArch64Module in place.
// Links: docs/internals/codemap.md, src/codegen/aarch64/LowerILToMIR.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "codegen/aarch64/passes/PassManager.hpp"

/**
 * @file
 * @brief Declares module-wide AArch64 IL-to-MIR lowering.
 */

namespace zanna::codegen::aarch64::passes {

/**
 * @brief Lowers an IL module into independently generated AArch64 MIR functions.
 */
class LoweringPass final : public Pass {
  public:
    /**
     * @brief Rebuilds module MIR, literal metadata, and sanitized local labels.
     * @param[in,out] module Module whose prior codegen products are cleared and
     *        whose IL functions are lowered.
     * @param[in,out] diags Sink receiving missing-input or lowering failures.
     * @return `true` when every function lowers successfully.
     */
    bool run(AArch64Module &module, Diagnostics &diags) override;
};

} // namespace zanna::codegen::aarch64::passes
