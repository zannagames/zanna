//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/passes/LegalizePass.hpp
// Purpose: Declare the legalisation pass for the x86-64 codegen pipeline.
// Key invariants:
//   - Legalisation requires lowering to have populated the adapter IL module.
// Ownership/Lifetime:
//   - Stateless pass; toggles flags on the shared Module instance.
// Links: src/codegen/x86_64/passes/LegalizePass.cpp,
//        src/codegen/x86_64/passes/PassManager.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "codegen/x86_64/passes/PassManager.hpp"

/// @file
/// @brief Declares adapter-IL to legalized-MIR conversion for x86-64.

namespace zanna::codegen::x64::passes {

/// @brief Lowers the adapter IL module into x86-64 legal machine IR.
/// @details The pass establishes a target descriptor if necessary, constructs
///          MIR, frame summaries, and the shared literal pool, then invalidates
///          downstream allocation and emission artifacts.
class LegalizePass final : public Pass {
  public:
    /// @brief Converts the module's lowered adapter representation to legal MIR.
    /// @param module Shared pipeline state containing @c lowered and options.
    /// @param diags Sink receiving missing-prerequisite and legalization errors.
    /// @return @c true after installing complete MIR state; otherwise @c false.
    bool run(Module &module, Diagnostics &diags) override;
};

} // namespace zanna::codegen::x64::passes
