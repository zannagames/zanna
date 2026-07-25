//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/passes/RegAllocPass.hpp
// Purpose: Declare the register allocation stage for the x86-64 pipeline.
// Key invariants:
//   - Register allocation requires legalisation to have succeeded first.
// Ownership/Lifetime:
//   - Stateless pass; marks Module::registersAllocated on success.
// Links: src/codegen/x86_64/passes/RegAllocPass.cpp,
//        src/codegen/x86_64/passes/PassManager.hpp,
//        src/codegen/x86_64/RegAllocLinear.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "codegen/x86_64/passes/PassManager.hpp"

/// @file
/// @brief Declares module-wide x86-64 register allocation.

namespace zanna::codegen::x64::passes {

/// @brief Allocates physical registers and finalizes frames for legalized MIR.
/// @details Validates the target and parallel MIR/frame state, delegates the
///          per-function pipeline to @ref allocateModuleMIR, and exposes a
///          completion flag consumed by post-allocation and emission passes.
class RegAllocPass final : public Pass {
  public:
    /// @brief Run the register allocation pass: assign physical registers to virtual registers.
    /// @param module Shared state containing legalized MIR, frames, and target.
    /// @param diags Sink receiving ordering, consistency, and allocation failures.
    /// @return @c true after every function is allocated and the completion
    ///         flag is set; otherwise @c false.
    bool run(Module &module, Diagnostics &diags) override;
};

} // namespace zanna::codegen::x64::passes
