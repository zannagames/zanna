//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/passes/PreRegAllocOptPass.hpp
// Purpose: Pass-manager wrapper for x86-64 pre-register-allocation MIR cleanup.
// Key invariants:
//   - Runs before register allocation; operates on virtual-register MIR.
// Ownership/Lifetime:
//   - Stateless pass; mutates Module::mir in place.
// Links: src/codegen/x86_64/passes/PreRegAllocOptPass.cpp,
//        src/codegen/x86_64/passes/PassManager.hpp,
//        src/codegen/x86_64/PreRegAllocOpt.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "codegen/x86_64/passes/PassManager.hpp"

/// @file
/// @brief Declares the virtual-register MIR cleanup pipeline pass.

namespace zanna::codegen::x64::passes {

/// @brief Pre-register-allocation MIR optimization pass for the x86-64 pipeline.
/// @details Wraps @ref runPreRegAllocOpt for pass-manager use and enforces that
///          enabled cleanup runs only on legalized, unallocated MIR.
class PreRegAllocOptPass final : public Pass {
  public:
    /// @brief Run the cleanup pass over the legalised MIR.
    /// @details Optimization levels below 1 skip both rewriting and ordering
    ///          validation. Otherwise each function is cleaned in module order.
    /// @param module Pipeline state whose @c mir vector is mutated in place.
    /// @param diags Sink receiving invalid-stage-order diagnostics.
    /// @return @c true after cleanup or an optimization-level skip; @c false
    ///         when legalization is missing or allocation already ran.
    bool run(Module &module, Diagnostics &diags) override;
};

} // namespace zanna::codegen::x64::passes
