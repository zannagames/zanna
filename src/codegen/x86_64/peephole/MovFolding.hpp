//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/peephole/MovFolding.hpp
// Purpose: Declarations for move folding peephole sub-passes: consecutive
//          register-to-register move coalescing.
// Key invariants:
//   - Only folds when the intermediate register is provably dead, including at
//     the enclosing block's exit.
//   - Argument registers near calls are not folded to avoid ABI violations.
// Ownership/Lifetime:
//   - Operates on mutable instructions owned by the caller.
// Links: src/codegen/x86_64/peephole/MovFolding.cpp,
//        src/codegen/x86_64/peephole/PeepholeCommon.hpp,
//        src/codegen/x86_64/Peephole.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "../MachineIR.hpp"
#include "PeepholeCommon.hpp"

#include <cstddef>
#include <vector>

/// @file
/// @brief Declares adjacent physical-register move-chain folding.

namespace zanna::codegen::x64::peephole {

/// @brief Try to fold `mov r2, r1; mov r3, r2` into `mov r3, r1` at @p idx.
/// @details The fold is safe only when the intermediate register `r2` is dead
///          after the second move (no use before redefinition and no value
///          carried to a successor) and when neither move is an
///          argument-register write near an upcoming call (those would violate
///          the ABI handoff). On success the first move becomes an identity
///          kill that the next identity-elimination pass removes.
/// @param instrs Instruction list being scanned (mutated in place).
/// @param idx    Index of the first move in the candidate pair.
/// @param stats  Peephole statistics counter (incremented on success).
/// @return @c true if the fold was applied at @p idx; @c false for an
///         out-of-range or unsafe candidate.
[[nodiscard]] bool tryFoldConsecutiveMoves(std::vector<MInstr> &instrs,
                                           std::size_t idx,
                                           PeepholeStats &stats);

/// @brief Fold all adjacent move pairs in @p instrs using one suffix liveness scan.
/// @details Equivalent to applying @ref tryFoldConsecutiveMoves across the
///          block, but avoids rescanning the tail of long blocks for each
///          candidate pair by precomputing suffix use-before-definition and
///          call-before-definition masks. Branching and fallthrough blocks
///          conservatively seed every physical register as live at exit because
///          the post-RA block-local pass does not own a successor liveness map.
/// @param instrs Instruction list being scanned (mutated in place).
/// @param stats  Peephole statistics counter (incremented on each fold).
/// @return Number of folds applied.
std::size_t foldConsecutiveMoves(std::vector<MInstr> &instrs, PeepholeStats &stats);

} // namespace zanna::codegen::x64::peephole
