//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/passes/PreRegAllocOptPass.cpp
// Purpose: Run x86-64 pre-register-allocation MIR cleanup in the modular pipeline.
// Key invariants:
//   - Skipped entirely when optimizeLevel < 1.
// Ownership/Lifetime:
//   - Stateless; mutates Module::mir in place via PreRegAllocOpt utilities.
// Links: src/codegen/x86_64/passes/PreRegAllocOptPass.hpp,
//        src/codegen/x86_64/PreRegAllocOpt.hpp
//
//===----------------------------------------------------------------------===//

#include "codegen/x86_64/passes/PreRegAllocOptPass.hpp"

#include "codegen/x86_64/PreRegAllocOpt.hpp"

/// @file
/// @brief Implements pass-manager gating for pre-allocation MIR cleanup.

namespace zanna::codegen::x64::passes {

/// @brief Run pre-RA MIR cleanup if optimisation is enabled.
/// @details Returns immediately below optimization level 1. When enabled,
///          verifies legalization completed and register allocation did not,
///          then invokes @ref runPreRegAllocOpt for every function. Individual
///          transformation counts are intentionally discarded.
/// @param module Pipeline state whose @c mir is mutated in place.
/// @param diags Sink receiving stage-order failures.
/// @return @c true on success or skip; @c false after an ordering diagnostic.
bool PreRegAllocOptPass::run(Module &module, Diagnostics &diags) {
    if (module.options.optimizeLevel < 1)
        return true;
    if (!module.legalised) {
        diags.error("pre-ra-opt: legalisation must run before pre-register-allocation cleanup");
        return false;
    }
    if (module.registersAllocated) {
        diags.error("pre-ra-opt: cannot run after register allocation");
        return false;
    }

    for (auto &fn : module.mir)
        (void)runPreRegAllocOpt(fn);
    return true;
}

} // namespace zanna::codegen::x64::passes
