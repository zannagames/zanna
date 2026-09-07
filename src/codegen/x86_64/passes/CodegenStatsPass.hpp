//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/passes/CodegenStatsPass.hpp
// Purpose: Reports codegen statistics for every function and the module as
//          diagnostics when `ZANNA_CODEGEN_STATS` is set; registered after the
//          peephole pass at every optimization level.
// Key invariants:
//   - Never fails and never mutates MIR or frames.
// Ownership/Lifetime:
//   - Stateless pass.
// Links: src/codegen/x86_64/CodegenStats.hpp, src/codegen/x86_64/CodegenPipeline.cpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "codegen/x86_64/passes/PassManager.hpp"

/// @file
/// @brief Declares the x86-64 codegen statistics pass.

namespace zanna::codegen::x64::passes {

/// @brief Emits `[codegen-stats]` diagnostics for the module's final MIR.
class CodegenStatsPass final : public Pass {
  public:
    /// @brief Report every function and the module total.
    /// @return Always `true`.
    bool run(Module &module, Diagnostics &diags) override;
};

} // namespace zanna::codegen::x64::passes
