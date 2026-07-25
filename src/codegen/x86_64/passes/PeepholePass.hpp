//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/passes/PeepholePass.hpp
// Purpose: Declare the explicit post-RA peephole pass for the x86-64 pipeline.
// Key invariants:
//   - Runs after register allocation; operates on physical-register MIR.
// Ownership/Lifetime:
//   - Stateless pass; mutates Module::mir in place.
// Links: src/codegen/x86_64/passes/PeepholePass.cpp,
//        src/codegen/x86_64/passes/PassManager.hpp,
//        src/codegen/x86_64/Peephole.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "codegen/x86_64/passes/PassManager.hpp"

/// @file
/// @brief Declares the post-allocation x86-64 peephole pipeline pass.

namespace zanna::codegen::x64::passes {

/// @brief Post-RA peephole optimization pass for the x86-64 codegen pipeline.
/// @details Applies local physical-register rewrites independently to each MIR
///          function. Modules may be processed through a bounded worker pool;
///          optional aggregate statistics are reported through diagnostics.
class PeepholePass final : public Pass {
  public:
    /// @brief Run peephole rewrites over physical-register MIR.
    /// @details Rejects execution before register allocation, skips rewriting
    ///          below optimization level 1, and selects the configured target
    ///          descriptor if the module does not already carry one.
    /// @param module Pipeline state whose @c mir vector is mutated in place.
    /// @param diags Sink for ordering failures and optional statistics.
    /// @return @c true on success or optimization-level skip; @c false when
    ///         register allocation has not completed.
    bool run(Module &module, Diagnostics &diags) override;
};

} // namespace zanna::codegen::x64::passes
