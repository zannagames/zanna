//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/passes/CodegenStatsPass.cpp
// Purpose: Implements the AArch64 codegen statistics pass.
// Key invariants:
//   - Functions are reported in module order; the total comes last.
// Ownership/Lifetime:
//   - Stateless.
// Links: src/codegen/aarch64/passes/CodegenStatsPass.hpp
//
//===----------------------------------------------------------------------===//

#include "codegen/aarch64/passes/CodegenStatsPass.hpp"

#include "codegen/aarch64/CodegenStats.hpp"

/// @file
/// @brief Implements CodegenStatsPass::run().

namespace zanna::codegen::aarch64::passes {

/// @copydoc CodegenStatsPass::run
bool CodegenStatsPass::run(AArch64Module &module, Diagnostics &diags) {
    CodegenStats total;
    for (const auto &fn : module.mir) {
        const CodegenStats stats = computeCodegenStats(fn);
        diags.warning(formatCodegenStats(stats, fn.name));
        total.add(stats);
    }
    diags.warning(formatCodegenStats(total, "<module>"));
    return true;
}

} // namespace zanna::codegen::aarch64::passes
