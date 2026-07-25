//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/passes/LegalizePass.cpp
// Purpose: Implement the MIR legalisation stage in the x86-64 codegen pipeline.
//          Lowers the adapter IL into machine IR, captures the shared rodata
//          literal pool, and records one frame summary per function.
// Key invariants:
//   - Legalisation is only successful when all functions lower cleanly to MIR.
//   - Failures are surfaced through the Diagnostics sink so later passes do not
//     infer partial state.
// Ownership/Lifetime:
//   - Stateless pass; mutates Module state in place.
// Links: src/codegen/x86_64/passes/LegalizePass.hpp,
//        src/codegen/x86_64/Backend.hpp
//
//===----------------------------------------------------------------------===//

#include "codegen/x86_64/passes/LegalizePass.hpp"

#include <string>

/// @file
/// @brief Implements x86-64 MIR legalization and downstream-state invalidation.

namespace zanna::codegen::x64::passes {

/// @brief Lower the adapter module to legalized MIR.
/// @details Emits a descriptive diagnostic when lowering has not populated the
///          module's adapter artifact or when MIR legalization fails. If the
///          target is absent, resolves it from the stored ABI option. On
///          success, populates @c mir, @c frames, and @c roData, marks MIR
///          legal, marks registers unallocated, and clears prior textual,
///          text-section, read-only-data, and debug-line emission results.
/// @param module Backend pipeline state being mutated.
/// @param diags Diagnostics sink used to report ordering or lowering failures.
/// @return @c true when complete legalized MIR is installed; @c false when a
///         diagnostic was recorded.
bool LegalizePass::run(Module &module, Diagnostics &diags) {
    if (!module.lowered) {
        diags.error("legalize: lowering has not produced an adapter module");
        return false;
    }

    if (module.target == nullptr)
        module.target = &selectTarget(module.options.targetABI);

    std::string errors;
    if (!legalizeModuleToMIR(*module.lowered,
                             *module.target,
                             module.options,
                             module.roData,
                             module.mir,
                             module.frames,
                             errors)) {
        if (errors.empty())
            errors = "legalize: MIR legalisation failed";
        diags.error(errors);
        return false;
    }

    module.legalised = true;
    module.registersAllocated = false;
    module.codegenResult.reset();
    module.binaryText.reset();
    module.binaryRodata.reset();
    module.binaryTextSections.clear();
    module.debugLineData.clear();
    return true;
}

} // namespace zanna::codegen::x64::passes
