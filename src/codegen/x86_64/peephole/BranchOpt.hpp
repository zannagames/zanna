//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/peephole/BranchOpt.hpp
// Purpose: Declarations for branch optimizations: greedy trace block layout,
//          cold block reordering, branch chain elimination, conditional branch
//          inversion, and fallthrough jump removal.
// Key invariants:
//   - Branch rewrites preserve control-flow semantics.
//   - Entry block always stays first in the layout.
//   - Cold-block partitioning never breaks an implicit fall-through pair.
// Ownership/Lifetime:
//   - Operates on mutable MFunction/instruction vectors owned by the caller.
// Links: src/codegen/x86_64/peephole/BranchOpt.cpp,
//        src/codegen/x86_64/peephole/PeepholeCommon.hpp,
//        src/codegen/x86_64/Peephole.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "../MachineIR.hpp"
#include "PeepholeCommon.hpp"

#include <cstddef>

/// @file
/// @brief Declares function-level x86-64 block-layout and branch rewrites.

namespace zanna::codegen::x64::peephole {

/// @brief Greedy trace block layout — follow JMP targets to maximize fall-through.
/// @details Preserves the entry block and declines to reorder functions with
///          implicit fall-through edges.
/// @param fn Machine function whose block vector may be reordered.
/// @param stats Statistics incremented by the number of blocks in a changed layout.
void traceBlockLayout(MFunction &fn, PeepholeStats &stats);

/// @brief Move explicit @c UD2 blocks to the end of the function.
/// @details Protects blocks participating in implicit fall-through pairs and
///          preserves relative order within the hot and cold partitions.
/// @param fn Machine function whose block vector may be reordered.
/// @param stats Statistics incremented once per moved cold block.
void moveColdBlocks(MFunction &fn, PeepholeStats &stats);

/// @brief Eliminate branch chains: retarget JMP/JCC through single-JMP blocks.
/// @details Resolves forwarding transitively and leaves cyclic chains unchanged.
/// @param fn Machine function whose branch labels may be rewritten.
/// @param stats Statistics incremented once per retargeted instruction.
void eliminateBranchChains(MFunction &fn, PeepholeStats &stats);

/// @brief Invert conditional branches to eliminate trailing unconditional jumps.
/// @param fn Machine function rewritten in place.
/// @param stats Statistics incremented once per removed jump.
void invertConditionalBranches(MFunction &fn, PeepholeStats &stats);

/// @brief Remove jumps to the immediately following block.
/// @details Retains a trailing jump when it follows a conditional branch,
///          preserving the explicit false edge expected by later layout passes.
/// @param fn Machine function rewritten in place.
/// @param stats Statistics incremented once per removed jump.
void removeFallthroughJumps(MFunction &fn, PeepholeStats &stats);

} // namespace zanna::codegen::x64::peephole
