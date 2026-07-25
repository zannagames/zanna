//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/passes/LoweringPass.hpp
// Purpose: Declare the IL-to-adapter lowering pass for the x86-64 codegen pipeline.
// Key invariants:
//   - Lowering preserves SSA identifiers and records value kinds for temporaries.
// Ownership/Lifetime:
//   - Pass is stateless and mutates the provided Module in place.
// Links: src/codegen/x86_64/passes/LoweringPass.cpp,
//        src/codegen/x86_64/passes/PassManager.hpp,
//        src/codegen/x86_64/LowerILToMIR.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "codegen/x86_64/passes/PassManager.hpp"

/// @file
/// @brief Declares canonical-IL to x86-64 adapter-IL conversion.

namespace zanna::codegen::x64::passes {

/// @brief Converts canonical IL modules into the x86-64 backend adapter form.
/// @details The pass preserves SSA identities and source locations, records
///          backend value classes and integer widths, converts control-flow
///          edges, and invalidates downstream stage flags and primary outputs.
class LoweringPass final : public Pass {
  public:
    /// @brief Adapts every function in the module's canonical IL.
    /// @param module Shared pipeline state whose adapter representation is replaced.
    /// @param diags Sink receiving unsupported-form or adaptation failures.
    /// @return @c true after installing the complete adapter module; otherwise
    ///         @c false after reporting the caught exception.
    bool run(Module &module, Diagnostics &diags) override;
};

} // namespace zanna::codegen::x64::passes
