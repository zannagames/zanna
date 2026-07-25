//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/passes/EmitPass.hpp
// Purpose: Declare the final emission pass for the x86-64 codegen pipeline.
// Key invariants:
//   - Emission requires register allocation to have marked completion.
// Ownership/Lifetime:
//   - Pass stores backend configuration by value and mutates Module state.
// Links: src/codegen/x86_64/passes/EmitPass.cpp,
//        src/codegen/x86_64/passes/PassManager.hpp,
//        src/codegen/x86_64/Backend.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "codegen/x86_64/passes/PassManager.hpp"

/// @file
/// @brief Declares the terminal x86-64 textual-assembly emission pass.

namespace zanna::codegen::x64::passes {

/// @brief Emits assembly text for an allocated module through the backend facade.
/// @details This terminal alternative to @ref BinaryEmitPass validates shared
///          pipeline state, emits MIR and literal-pool assembly, appends native
///          writable scalar globals, and stores the owned result on the module.
class EmitPass final : public Pass {
  public:
    /// @brief Construct the emission pass with the given codegen configuration.
    /// @param options Backend options copied into the pass.
    explicit EmitPass(CodegenOptions options) noexcept;

    /// @brief Generates x86-64 assembly text from the module's MIR.
    /// @details Requires completed register allocation, a selected target, and
    ///          a frame record for every MIR function. On success replaces
    ///          @c module.codegenResult; expected failures are reported through
    ///          @p diags without installing a partial result.
    /// @param module Shared code-generation state mutated in place.
    /// @param diags Diagnostic sink for prerequisite and backend failures.
    /// @return @c true when assembly was installed, otherwise @c false.
    bool run(Module &module, Diagnostics &diags) override;

  private:
    /// @brief Backend and target-platform policy captured at construction.
    CodegenOptions options_;
};

} // namespace zanna::codegen::x64::passes
