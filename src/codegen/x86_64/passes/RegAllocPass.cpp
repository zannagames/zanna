//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/passes/RegAllocPass.cpp
// Purpose: Implement the register allocation stage for the x86-64 pipeline.
// Key invariants:
//   - Register allocation is only complete when legalisation has run and the
//     module carries legalised MIR.
// Ownership/Lifetime:
//   - Stateless transformation; mutates Module::mir in place.
// Links: src/codegen/x86_64/passes/RegAllocPass.hpp,
//        src/codegen/x86_64/RegAllocLinear.hpp,
//        src/codegen/x86_64/Backend.hpp
//
//===----------------------------------------------------------------------===//

#include "codegen/x86_64/passes/RegAllocPass.hpp"

#include <string>

/// @file
/// @brief Implements register-allocation gating and module-level delegation.

namespace zanna::codegen::x64::passes {

/// @brief Run register allocation on legalized MIR.
/// @details The pass checks the @p module bookkeeping flags and concrete MIR
///          state before invoking @ref allocateModuleMIR, which runs each
///          function's allocation/frame pipeline and updates its parallel frame
///          summary. On success it flips @c registersAllocated, allowing
///          subsequent scheduling, optimization, and emission.
/// @param module Backend pipeline state to update.
/// @param diags Diagnostics sink for ordering, consistency, or allocation errors.
/// @return @c true when all functions are allocated; @c false after recording
///         a diagnostic.
bool RegAllocPass::run(Module &module, Diagnostics &diags) {
    if (!module.legalised) {
        diags.error("regalloc: legalisation must run before register allocation");
        return false;
    }

    if (module.target == nullptr) {
        diags.error("regalloc: target selection is missing");
        return false;
    }
    if (module.mir.size() != module.frames.size()) {
        diags.error("regalloc: MIR/frame state is inconsistent");
        return false;
    }

    std::string errors;
    if (!allocateModuleMIR(module.mir, module.frames, *module.target, module.options, errors)) {
        if (errors.empty())
            errors = "regalloc: register allocation failed";
        diags.error(errors);
        return false;
    }

    module.registersAllocated = true;
    return true;
}

} // namespace zanna::codegen::x64::passes
