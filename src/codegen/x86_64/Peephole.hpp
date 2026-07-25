//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/Peephole.hpp
// Purpose: Declare peephole optimisations over the provisional Machine IR.
// Key invariants:
//   - Rewrites preserve instruction ordering and semantics.
//   - Only pattern substitutions are applied; control flow is not changed.
//   - Block-level rewrites iterate to a fixed point (bounded by kMaxIterations).
// Ownership/Lifetime:
//   - Operates on mutable MIR owned by the caller.
//   - No dynamic resources allocated beyond temporary vectors.
// Links: src/codegen/x86_64/Peephole.cpp,
//        src/codegen/x86_64/MachineIR.hpp,
//        src/codegen/x86_64/TargetX64.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "MachineIR.hpp"
#include "TargetX64.hpp"

/**
 * @file
 * @brief Declares the composed post-allocation x86-64 peephole pipeline.
 *
 * The driver combines arithmetic simplification, move folding, frame-memory
 * forwarding, dead-code elimination, branch cleanup, trace layout, and cold
 * block placement. Repeated local and branch phases are bounded to guarantee
 * termination even if future rewrites interact unexpectedly.
 */

namespace zanna::codegen::x64 {

/// \brief Run conservative Machine IR peepholes for the x86-64 backend.
/// \details Applies pattern-based local rewrites to the Machine IR, including
///          redundant move elimination, strength reduction (e.g., multiply-by-power-of-2
///          to shift), and dead instruction removal. Block-level rewrites iterate
///          to a fixed point (bounded by kMaxIterations=100); branch cleanup
///          iterates to a fixed point and layout runs once.
/// \param fn The MIR function to optimize in-place.
/// \param target Target ABI metadata used by ABI-sensitive peepholes.
/// \return Total number of transformations applied across all passes.
std::size_t runPeepholes(MFunction &fn, const TargetInfo &target = sysvTarget());

} // namespace zanna::codegen::x64
