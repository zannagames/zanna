//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/PreRegAllocOpt.hpp
// Purpose: Conservative AArch64 MIR cleanup before register allocation:
//          identity-copy removal, single-use copy forwarding, and folding
//          shifted virtual-register computations into addressing/ALU forms.
//
// Key invariants:
//   - Must run after LegalizePass and before RegAllocPass.
//   - Forwarding operates on virtual-register copies; precolored ABI physical
//     operands are retained and treated conservatively.
//
// Ownership/Lifetime:
//   - Borrows MFunction for the duration of the call; no persistent state.
//
// Links: src/codegen/aarch64/PreRegAllocOpt.cpp,
//        src/codegen/aarch64/passes/PreRegAllocOptPass.cpp,
//        src/codegen/common/PreRAForwardCopy.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "codegen/aarch64/MachineIR.hpp"

#include <cstddef>

/**
 * @file
 * @brief Declares conservative AArch64 MIR cleanup before register allocation.
 *
 * The pass removes or forwards provably safe virtual-register copies and folds
 * single-def/single-use shifted values into AArch64 shifted-register ALU and
 * scaled register-offset memory instructions.
 */

namespace zanna::codegen::aarch64 {

/**
 * @brief Runs pre-register-allocation copy and addressing cleanup.
 *
 * Copy forwarding uses the shared backend-neutral traversal with AArch64
 * operand-role traits. Addressing folds may be disabled for diagnostics by
 * setting `ZANNA_NO_ADDR_FOLDS` in the process environment.
 *
 * @param[in,out] fn Legalized MIR function to rewrite in place.
 * @return Aggregate number of forwarded/removed copies and addressing patterns
 *         folded.
 * @pre Legalization is complete and general register allocation has not run.
 */
std::size_t runPreRegAllocOpt(MFunction &fn);

} // namespace zanna::codegen::aarch64
