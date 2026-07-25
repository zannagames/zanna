//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/passes/PreRegAllocOptPass.cpp
// Purpose: Run AArch64 pre-register-allocation MIR cleanup in the modular pipeline.
// Key invariants:
//   - Delegates to runPreRegAllocOpt(); return value is intentionally discarded.
//   - ti must be non-null; returns false immediately if not set.
// Ownership/Lifetime:
//   - Stateless pass; mutates AArch64Module::mir in place.
// Links: src/codegen/aarch64/passes/PreRegAllocOptPass.hpp,
//        src/codegen/aarch64/PreRegAllocOpt.hpp
//
//===----------------------------------------------------------------------===//

#include "codegen/aarch64/passes/PreRegAllocOptPass.hpp"

#include "codegen/aarch64/PreRegAllocOpt.hpp"

/**
 * @file
 * @brief Implements the module-level adapter for pre-allocation cleanup.
 */

namespace zanna::codegen::aarch64::passes {

/**
 * @brief Applies `runPreRegAllocOpt()` independently to each function.
 * @param[in,out] module Module whose MIR is rewritten.
 * @param[in,out] diags Sink receiving a missing-target diagnostic.
 * @return `false` when target metadata is absent, otherwise `true`.
 */
bool PreRegAllocOptPass::run(AArch64Module &module, Diagnostics &diags) {
    if (!module.ti) {
        diags.error("PreRegAllocOptPass: ti must be non-null");
        return false;
    }
    for (auto &fn : module.mir)
        (void)runPreRegAllocOpt(fn);
    return true;
}

} // namespace zanna::codegen::aarch64::passes
