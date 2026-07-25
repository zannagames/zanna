//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/passes/BlockLayoutPass.hpp
// Purpose: Declare the block layout pass for the AArch64 codegen pipeline.
// Key invariants: Must run after RegAllocPass and before PeepholePass.
//                 Only reorders MBasicBlock entries in MFunction::blocks;
//                 never adds, removes, or modifies instructions.
//                 Entry block (index 0) always remains first.
// Ownership/Lifetime: Stateless pass; mutates AArch64Module::mir in place.
// Links: src/codegen/aarch64/passes/BlockLayoutPass.cpp,
//        docs/devdocs/specs/aarch64.md
//
//===----------------------------------------------------------------------===//

#pragma once

#include "codegen/aarch64/passes/PassManager.hpp"

/**
 * @file
 * @brief Declares greedy trace layout for AArch64 MIR basic blocks.
 */

namespace zanna::codegen::aarch64::passes {

/**
 * @brief Reorders MIR blocks to expose profitable layout fallthroughs.
 *
 * Greedy traces prefer unconditional successors, explicit false edges, then
 * conditional targets. The entry block remains first and disconnected traces
 * resume in original order.
 */
class BlockLayoutPass final : public Pass {
  public:
    /**
     * @brief Reorders blocks in every MIR function.
     * @param[in,out] module Module whose function block vectors may be reordered.
     * @param diags Unused diagnostic sink; layout has no failure path.
     * @return Always `true`.
     */
    bool run(AArch64Module &module, Diagnostics &diags) override;
};

} // namespace zanna::codegen::aarch64::passes
