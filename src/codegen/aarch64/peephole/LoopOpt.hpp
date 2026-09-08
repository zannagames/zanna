//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/peephole/LoopOpt.hpp
// Purpose: Declarations for loop-invariant constant hoisting.
//
// Key invariants:
//   - Only hoists MovRI to callee-saved registers (x19-x28).
//   - The register must be defined only by MovRI with the same immediate value
//     throughout the loop body.
//
// Ownership/Lifetime:
//   - Operates on mutable MFunction owned by the caller.
//
// Links: codegen/aarch64/Peephole.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "../MachineIR.hpp"

#include <cstddef>

/// @file
/// @brief Declares post-allocation loop rewrites for AArch64 MIR.

namespace zanna::codegen::aarch64::peephole {

/// @brief Hoist repeated loop-invariant integer constants into loop preheaders.
///
/// Discovers natural loops from dominance-proven backward edges, then considers
/// only `MovRI` definitions of callee-saved GPRs X19--X28. Every definition in
/// a loop must materialize the same immediate, every use must be reached by an
/// appropriate definition, and the layout predecessor must be a valid
/// preheader. Successful hoists update allocator live-through metadata.
///
/// @param[in,out] fn Post-allocation function whose constants may be relocated.
/// @return Number of distinct register constants hoisted.
std::size_t hoistLoopConstants(MFunction &fn);

} // namespace zanna::codegen::aarch64::peephole
