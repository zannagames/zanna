//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/passes/SchedulerPass.hpp
// Purpose: Declare the x86-64 post-RA instruction scheduling pass.
// Key invariants:
//   - Runs after register allocation on physical-register MIR.
//   - Skipped when optimizeLevel < 1.
// Ownership/Lifetime:
//   - Stateless pass; mutates Module::mir in place.
// Links: src/codegen/x86_64/passes/SchedulerPass.cpp,
//        src/codegen/x86_64/passes/PassManager.hpp,
//        src/codegen/x86_64/Scheduler.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "codegen/x86_64/passes/PassManager.hpp"

/// @file
/// @brief Declares the post-allocation x86-64 instruction-scheduling pass.

namespace zanna::codegen::x64::passes {

/// @brief Post-RA instruction scheduling pass for the x86-64 codegen pipeline.
/// @details Reorders independent instructions within scheduling regions after
///          physical-register assignment, while preserving dependency and
///          barrier constraints enforced by the scheduler.
class SchedulerPass final : public Pass {
  public:
    /// @brief Run the scheduling pass on post-allocation MIR.
    /// @details Validates register allocation even when optimization level 0
    ///          subsequently skips scheduling.
    /// @param module Backend pipeline state containing physical-register MIR.
    /// @param diags Diagnostic sink for ordering errors.
    /// @return @c true after scheduling or an optimization-level skip;
    ///         @c false when allocation has not completed.
    bool run(Module &module, Diagnostics &diags) override;
};

} // namespace zanna::codegen::x64::passes
