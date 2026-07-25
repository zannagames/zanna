//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/passes/BinaryEmitPass.hpp
// Purpose: Declare the binary emission pass for the AArch64 codegen pipeline.
//          Encodes MIR into machine code bytes via A64BinaryEncoder.
// Key invariants:
//   - Requires register allocation to have completed
//   - Populates AArch64Module::binaryText and AArch64Module::binaryRodata
// Ownership/Lifetime:
//   - Stateless pass; mutates AArch64Module binary fields
// Links: src/codegen/aarch64/binenc/A64BinaryEncoder.hpp,
//        src/codegen/aarch64/passes/PassManager.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "codegen/aarch64/passes/PassManager.hpp"

/**
 * @file
 * @brief Declares the AArch64 direct binary-emission pipeline pass.
 */

namespace zanna::codegen::aarch64::passes {

/**
 * @brief Encodes allocated MIR plus module data into object-writer sections.
 *
 * The pass owns no persistent state; all products are stored in the supplied
 * `AArch64Module`.
 */
class BinaryEmitPass final : public Pass {
  public:
    /**
     * @brief Runs direct binary emission for every MIR function.
     * @param[in,out] module Module whose text, rodata, data, and optional debug
     *        products are replaced.
     * @param[in,out] diags Diagnostic sink for validation/encoding failures.
     * @return `true` when every function and section was emitted.
     * @pre `module.ti` is non-null and MIR register allocation is complete.
     */
    bool run(AArch64Module &module, Diagnostics &diags) override;
};

} // namespace zanna::codegen::aarch64::passes
