//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/passes/CodegenStatsPass.cpp
// Purpose: Implements the x86-64 codegen statistics pass.
// Key invariants:
//   - Functions are reported in module order; the total comes last.
// Ownership/Lifetime:
//   - Stateless.
// Links: src/codegen/x86_64/passes/CodegenStatsPass.hpp
//
//===----------------------------------------------------------------------===//

#include "codegen/x86_64/passes/CodegenStatsPass.hpp"

#include "codegen/x86_64/CodegenStats.hpp"

/// @file
/// @brief Implements CodegenStatsPass::run() for x86-64.

namespace zanna::codegen::x64::passes {

/// @copydoc CodegenStatsPass::run
bool CodegenStatsPass::run(Module &module, Diagnostics &diags) {
    CodegenStats total;
    const FrameInfo emptyFrame{};
    for (std::size_t i = 0; i < module.mir.size(); ++i) {
        const FrameInfo &frame = i < module.frames.size() ? module.frames[i] : emptyFrame;
        const CodegenStats stats = computeCodegenStats(module.mir[i], frame);
        diags.warning(formatCodegenStats(stats, module.mir[i].name));
        total.add(stats);
    }
    diags.warning(formatCodegenStats(total, "<module>"));
    return true;
}

} // namespace zanna::codegen::x64::passes
