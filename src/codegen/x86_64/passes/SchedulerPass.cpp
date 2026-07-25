//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/passes/SchedulerPass.cpp
// Purpose: Implement the x86-64 post-RA instruction scheduling pass.
// Key invariants:
//   - Runs after register allocation on physical-register MIR.
//   - Skipped when optimizeLevel < 1.
// Ownership/Lifetime:
//   - Stateless pass; mutates Module::mir in place via Scheduler utilities.
// Links: src/codegen/x86_64/passes/SchedulerPass.hpp,
//        src/codegen/x86_64/Scheduler.hpp
//
//===----------------------------------------------------------------------===//

#include "codegen/x86_64/passes/SchedulerPass.hpp"

#include "codegen/x86_64/Scheduler.hpp"

/// @file
/// @brief Implements pass-manager gating for post-allocation scheduling.

namespace zanna::codegen::x64::passes {

/// @brief Run the post-RA instruction scheduler over the module's MIR.
/// @details Requires that register allocation has already completed so the
///          scheduler can reason about physical-register dependencies. At -O0
///          the pass returns success after the ordering check. At higher levels,
///          @ref scheduleModule processes all functions and its aggregate
///          reorder count is intentionally discarded.
/// @param module Pipeline state holding the legalised, register-allocated MIR.
/// @param diags Diagnostic sink for pipeline-ordering issues.
/// @return @c true on success or skip; @c false after an ordering diagnostic.
bool SchedulerPass::run(Module &module, Diagnostics &diags) {
    if (!module.registersAllocated) {
        diags.error("scheduler: register allocation must run before scheduling");
        return false;
    }

    if (module.options.optimizeLevel < 1)
        return true;

    (void)scheduleModule(module.mir);
    return true;
}

} // namespace zanna::codegen::x64::passes
