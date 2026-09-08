//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/peephole/DCE.hpp
// Purpose: Declare conservative x86-64 physical-register and EFLAGS dead code
//          elimination for a single machine basic block.
// Key invariants:
//   - RSP modifications are never eliminated.
//   - Liveness is computed conservatively within one basic block.
// Ownership/Lifetime:
//   - Operates on mutable instructions owned by the caller.
// Links: src/codegen/x86_64/peephole/DCE.cpp,
//        src/codegen/x86_64/peephole/PeepholeCommon.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "../MachineIR.hpp"
#include "PeepholeCommon.hpp"

#include <cstddef>
#include <vector>

/// @file
/// @brief Declares conservative block-local x86-64 dead-code elimination.

namespace zanna::codegen::x64::peephole {

/// @brief Run dead-code elimination on a single basic block.
/// @details Backward walk over @p instrs that tracks each operand's live set
///          and removes instructions whose tracked register and flag outputs
///          are unused. Side-effecting instructions and RSP modifications are
///          retained. Labels conservatively make every allocatable register
///          live. The backward sweep starts from @p exitLive, the block's
///          solved exit-live mask (`blockExitLive`), or, when null, from the
///          conservative seed (every allocatable register plus RSP).
/// @param instrs   Instruction list being scanned (mutated in place).
/// @param stats    Peephole statistics counter (incremented per removal).
/// @param target   Target ABI metadata for implicit call/return uses.
/// @param exitLive Optional exit-live mask of the enclosing block.
/// @return Number of instructions removed.
std::size_t runBlockDCE(std::vector<MInstr> &instrs,
                        PeepholeStats &stats,
                        const TargetInfo &target,
                        const PhysRegMask *exitLive = nullptr);

} // namespace zanna::codegen::x64::peephole
