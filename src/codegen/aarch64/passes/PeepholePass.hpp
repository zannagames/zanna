//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/passes/PeepholePass.hpp
// Purpose: Declare the peephole optimisation pass for the AArch64 codegen pipeline.
// Key invariants: Must run after RegAllocPass (operates on physical-register MIR).
// Ownership/Lifetime: Stateless pass; mutates AArch64Module::mir in place.
// Links: docs/internals/codemap.md, src/codegen/aarch64/Peephole.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "codegen/aarch64/passes/PassManager.hpp"

/**
 * @file
 * @brief Declares validated post-allocation AArch64 peephole pipeline modes.
 */

namespace zanna::codegen::aarch64::passes {

/**
 * @brief Applies full or post-scheduling cleanup and validates resulting MIR.
 */
class PeepholePass final : public Pass {
  public:
    /// @brief Controls which subset of peephole patterns to apply.
    enum class Mode {
        Full,               ///< Apply all peephole patterns (default).
        PostScheduleCleanup ///< Apply only lightweight cleanup after scheduling.
    };

    /**
     * @brief Selects the optimizer subset for subsequent runs.
     * @param mode Pattern set to use; defaults to `Mode::Full`.
     */
    explicit PeepholePass(Mode mode = Mode::Full) noexcept : mode_(mode) {}

    /**
     * @brief Optimizes and validates every allocated MIR function.
     * @param[in,out] module Module whose MIR and callee-saved metadata are rewritten.
     * @param[in,out] diags Sink receiving missing-target, validation, or optional
     *        statistics messages.
     * @return `false` if target metadata is missing or post-pass MIR validation fails.
     */
    bool run(AArch64Module &module, Diagnostics &diags) override;

  private:
    Mode mode_{Mode::Full};
};

} // namespace zanna::codegen::aarch64::passes
