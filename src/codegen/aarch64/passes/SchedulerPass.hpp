//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/passes/SchedulerPass.hpp
// Purpose: Declare the post-RA instruction scheduling pass for AArch64.
// Key invariants: Runs after RegAllocPass (physical registers must be assigned).
//                 Reorders instructions within each basic block to reduce
//                 pipeline stalls by increasing the distance between a producer
//                 instruction and the consumer that depends on its result.
//                 Terminators are always kept at the end of the block.
//                 Does not add, remove, or modify instructions — only reorders.
// Ownership/Lifetime: Stateless pass; mutates AArch64Module::mir in place.
// Links: docs/internals/codemap.md, src/codegen/aarch64/Scheduler.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "codegen/aarch64/passes/PassManager.hpp"

/// @file
/// @brief Declares dependency-aware post-allocation scheduling for AArch64 MIR.

namespace zanna::codegen::aarch64::passes {

/// @brief Post-RA instruction scheduler using list scheduling with AArch64 latencies.
///
/// For each manageable basic-block segment, the pass constructs a dependency
/// DAG from post-allocation physical-register operands, memory accesses, NZCV,
/// stack-pointer effects, and calls. A deterministic priority queue then selects
/// ready instructions by descending critical-path length and original position.
///
/// Calls and mid-block control transfers divide a block into independently
/// scheduled segments, while the final terminator group remains in its original
/// order. Oversized functions or segments are deliberately left unchanged to
/// bound compile time. The scheduler neither creates nor removes instructions.
class SchedulerPass final : public Pass {
  public:
    /// @brief Schedule every eligible basic block in every allocated function.
    ///
    /// @param[in,out] module Pipeline state containing post-allocation MIR.
    ///                         `module.ti` supplies caller-saved register sets
    ///                         used to model call clobbers.
    /// @param[in,out] diags Diagnostic sink used when target information is
    ///                      missing.
    /// @return `true` after scheduling, or `false` if `module.ti` is null.
    bool run(AArch64Module &module, Diagnostics &diags) override;
};

} // namespace zanna::codegen::aarch64::passes
