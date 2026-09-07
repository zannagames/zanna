//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/peephole/LoopOpt.hpp
// Purpose: Declarations for loop-invariant constant hoisting and loop phi-slot
//          spill elimination.
//
// Key invariants:
//   - Only hoists MovRI to callee-saved registers (x19-x28).
//   - The register must be defined only by MovRI with the same immediate value
//     throughout the loop body.
//   - Phi spill elimination rewrites only call-free, dominance-proven loops.
//
// Ownership/Lifetime:
//   - Operates on mutable MFunction owned by the caller.
//
// Links: codegen/aarch64/Peephole.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "../MachineIR.hpp"
#include "../TargetAArch64.hpp"

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

/// @brief Eliminate redundant phi-slot spill/reload cycles in loop back-edges.
///
/// When a loop header starts with loads from phi spill slots and the latch
/// ends with stores to the same slots before branching back, the store+load
/// round-trip through the stack is redundant on the back-edge path. This pass
/// splits the loop header so the back-edge branches directly to the loop body
/// (after the phi loads), using register movs instead of memory round-trips.
///
/// Candidate loops must be dominance-proven and call-free, begin with at least
/// two consecutive FP-relative phi loads, and have a complete one-to-one set of
/// matching back-edge stores. Slots observable by any other load are rejected.
/// Parallel edge moves must be acyclic because this pass does not reserve a
/// scratch register. At most one header is split per invocation because block
/// insertion invalidates the collected indices.
///
/// @param[in,out] fn Post-allocation function whose loop header may be split.
/// @return Number of spill/reload pairs eliminated.
std::size_t eliminateLoopPhiSpills(MFunction &fn, const TargetInfo &targetInfo);

} // namespace zanna::codegen::aarch64::peephole
