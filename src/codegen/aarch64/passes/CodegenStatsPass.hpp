//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/passes/CodegenStatsPass.hpp
// Purpose: Reports codegen statistics for every function and the module as
//          diagnostics when `ZANNA_CODEGEN_STATS` is set. Registered as the
//          last MIR pass at every optimization level, after ExpandPseudos, so
//          the numbers describe the instructions the emitters print.
// Key invariants:
//   - Never fails and never mutates MIR.
//   - One `[codegen-stats]` line per function plus one labelled `<module>`.
// Ownership/Lifetime:
//   - Stateless pass.
// Links: src/codegen/aarch64/CodegenStats.hpp, src/codegen/aarch64/CodegenPipeline.cpp,
//        scripts/codegen_stats.sh
//
//===----------------------------------------------------------------------===//

#pragma once

#include "codegen/aarch64/passes/PassManager.hpp"

/// @file
/// @brief Declares the AArch64 codegen statistics pass.

namespace zanna::codegen::aarch64::passes {

/// @brief Emits `[codegen-stats]` diagnostics for the module's final MIR.
class CodegenStatsPass final : public Pass {
  public:
    /// @brief Report every function and the module total.
    /// @param module Module whose MIR is counted (not modified).
    /// @param diags Sink receiving one warning-severity line per record.
    /// @return Always `true`.
    bool run(AArch64Module &module, Diagnostics &diags) override;
};

} // namespace zanna::codegen::aarch64::passes
