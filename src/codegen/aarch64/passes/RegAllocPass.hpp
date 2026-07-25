//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/passes/RegAllocPass.hpp
// Purpose: Declare the register allocation pass for the AArch64 codegen pipeline.
// Key invariants: Requires AArch64Module::mir to be populated by LoweringPass.
//                 Assigns physical registers to all virtual registers in-place.
// Ownership/Lifetime: Stateless pass; mutates AArch64Module::mir in place.
// Links: docs/internals/codemap.md, src/codegen/aarch64/RegAllocLinear.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "codegen/aarch64/passes/PassManager.hpp"

/// @file
/// @brief Declares the AArch64 machine-register allocation pipeline pass.
///
/// The pass coalesces compatible virtual-register copies and then invokes the
/// target's linear-scan allocator independently for every lowered MIR function.

namespace zanna::codegen::aarch64::passes {

/// @brief Coalesce copies and allocate physical registers for all MIR functions.
///
/// `RegAllocPass` is the pipeline adapter around the AArch64 coalescer and
/// linear-scan allocator. Functions are independent allocation units and may be
/// processed concurrently. The pass must run after legalization and before any
/// stage, such as post-allocation scheduling, that relies on physical operands.
///
/// The object is stateless. All output, including spill code inserted by the
/// allocator, is written directly to `AArch64Module::mir`.
class RegAllocPass final : public Pass {
  public:
    /// @brief Allocate every lowered function in the module.
    ///
    /// Each function is first copy-coalesced and then passed to the linear-scan
    /// allocator. When parallel code generation is enabled, workers claim
    /// functions from a shared index; each worker still mutates only its claimed
    /// function. Allocation exceptions are captured and reported through
    /// @p diags after all workers have joined.
    ///
    /// @param[in,out] module Pipeline state whose MIR functions are allocated.
    ///                         `module.ti` must identify the active AArch64 target.
    /// @param[in,out] diags Diagnostic sink that receives a missing-target or
    ///                      allocation-failure diagnostic.
    /// @return `true` when target information is present and every function is
    ///         allocated successfully; `false` otherwise.
    bool run(AArch64Module &module, Diagnostics &diags) override;
};

} // namespace zanna::codegen::aarch64::passes
