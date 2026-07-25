//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/passes/EmitPass.hpp
// Purpose: Declare the assembly emission pass for the AArch64 codegen pipeline.
// Key invariants: Requires AArch64Module::mir to have physical registers assigned.
//                 Populates AArch64Module::assembly with the final asm text.
// Ownership/Lifetime: Stateless pass; mutates AArch64Module::assembly.
// Links: docs/internals/codemap.md, src/codegen/aarch64/AsmEmitter.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "codegen/aarch64/passes/PassManager.hpp"

/**
 * @file
 * @brief Declares final AArch64 assembly-text emission.
 */

namespace zanna::codegen::aarch64::passes {

/**
 * @brief Serializes globals and allocated MIR as target-specific assembly text.
 *
 * The pass is stateless and replaces `AArch64Module::assembly`.
 */
class EmitPass final : public Pass {
  public:
    /**
     * @brief Emits the complete assembly module.
     * @param[in,out] module Module providing globals, target metadata, and
     *        allocated MIR; receives final text in `assembly`.
     * @param[in,out] diags Diagnostic sink used when target metadata is absent.
     * @return `true` on complete emission, otherwise `false`.
     */
    bool run(AArch64Module &module, Diagnostics &diags) override;
};

} // namespace zanna::codegen::aarch64::passes
