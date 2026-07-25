//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/passes/BinaryEmitPass.hpp
// Purpose: Declare the binary emission pass for the x86-64 codegen pipeline.
//          Encodes MIR into machine code bytes via X64BinaryEncoder instead of
//          emitting text assembly.
// Key invariants:
//   - Requires register allocation to have completed before running
//   - Populates Module::binaryTextSections and Module::binaryRodata CodeSections
//   - Does NOT produce assembly text (Module::codegenResult remains empty)
// Ownership/Lifetime:
//   - Pass stores backend options by value; borrows Module during run()
// Links: src/codegen/x86_64/passes/BinaryEmitPass.cpp,
//        src/codegen/x86_64/binenc/X64BinaryEncoder.hpp,
//        src/codegen/x86_64/passes/PassManager.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "codegen/x86_64/Backend.hpp"
#include "codegen/x86_64/passes/PassManager.hpp"

/// @file
/// @brief Declares the terminal x86-64 pass that emits object-ready binary sections.

namespace zanna::codegen::x64::passes {

/// @brief Encodes allocated MIR into object-writer-ready x86-64 sections.
/// @details This terminal alternative to @ref EmitPass stores per-function text,
///          module read-only data, initialized scalar data, and optional debug
///          line bytes in the shared pipeline module. The pass owns a copy of
///          its backend configuration and borrows the module only during
///          @ref run.
class BinaryEmitPass final : public Pass {
  public:
    /// @brief Construct the pass with the supplied codegen options.
    /// @details Options control output format (ELF/Mach-O/COFF) and debug
    ///          info presence. Stored by value so the pass can outlive the
    ///          caller's @ref CodegenOptions instance.
    /// @param options Backend configuration consumed during @ref run.
    explicit BinaryEmitPass(CodegenOptions options) noexcept;

    /// @brief Encode the module's MIR into machine code sections.
    /// @details Requires completed register allocation, a selected target, and
    ///          one frame record per MIR function. On success it replaces the
    ///          module's binary text, read-only-data, writable-data, and debug
    ///          artifacts while leaving text-assembly output untouched.
    ///          @c binaryText normally holds merged code only for debug-line
    ///          output; an empty module retains an empty merged section.
    /// @param module Pipeline state mutated in place.
    /// @param diags Sink receiving prerequisite or backend-emission failures.
    /// @return @c true after all artifacts are installed; @c false after
    ///         reporting a prerequisite or encoder failure.
    bool run(Module &module, Diagnostics &diags) override;

  private:
    /// @brief Backend and object-format policy captured at construction.
    CodegenOptions options_{}; ///< Backend configuration captured at construction.
};

} // namespace zanna::codegen::x64::passes
