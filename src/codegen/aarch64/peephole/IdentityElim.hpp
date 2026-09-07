//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/peephole/IdentityElim.hpp
// Purpose: Declarations for identity move elimination and consecutive move
//          folding peephole sub-passes.
//
// Key invariants:
//   - Identity move removal only removes provably redundant mov r,r / fmov d,d.
//   - Consecutive move folding checks liveness before transforming.
//
// Ownership/Lifetime:
//   - Operates on mutable instruction vectors owned by the caller.
//
// Links: codegen/aarch64/Peephole.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "../InstrEffects.hpp"
#include "../MachineIR.hpp"
#include "../Peephole.hpp"

#include <cstddef>
#include <vector>

/// @file
/// @brief Declares identity-move recognition and adjacent-move folding.

namespace zanna::codegen::aarch64::peephole {

/// @brief Test whether an instruction is a no-op GPR register move.
/// @param instr Machine instruction to inspect.
/// @return `true` for a well-formed `MovRR` whose physical operands match.
[[nodiscard]] bool isIdentityMovRR(const MInstr &instr) noexcept;

/// @brief Test whether an instruction is a no-op floating-point register move.
/// @param instr Machine instruction to inspect.
/// @return `true` for a well-formed `FMovRR` whose physical operands match.
[[nodiscard]] bool isIdentityFMovRR(const MInstr &instr) noexcept;

/// @brief Forward the source through an adjacent pair of register moves.
///
/// Rewrites `mov r1, r0; mov r2, r1` to source `r2` directly from `r0` and
/// turns the first move into an identity instruction for later removal. The
/// intermediate register must be dead after the pair and must not be an
/// argument register awaiting an implicit call use.
///
/// @param[in,out] instrs Block-local instruction sequence to rewrite.
/// @param idx Index of the first `MovRR`.
/// @param[in,out] stats Statistics updated when the fold succeeds.
/// @param exitLive Optional physical registers live at the enclosing block's
///        exit (blockExitLive()); the intermediate register is live when it
///        is in this set.
/// @return `true` when the adjacent pair was folded.
[[nodiscard]] bool tryFoldConsecutiveMoves(std::vector<MInstr> &instrs,
                                           std::size_t idx,
                                           PeepholeStats &stats,
                                           const PhysRegSet *exitLive = nullptr);

/// @brief Forward an immediate through an adjacent register move.
///
/// Rewrites `mov r1, #imm; mov r2, r1` so `r2` receives the immediate directly
/// and turns the original materialization into an identity move. The same
/// intermediate-register liveness and implicit ABI-use restrictions as
/// @ref tryFoldConsecutiveMoves apply.
///
/// @param[in,out] instrs Block-local instruction sequence to rewrite.
/// @param idx Index of the `MovRI` candidate.
/// @param[in,out] stats Statistics updated when the fold succeeds.
/// @param exitLive Optional physical registers live at the enclosing block's
///        exit (blockExitLive()).
/// @return `true` when the immediate and move pair was folded.
[[nodiscard]] bool tryFoldImmThenMove(std::vector<MInstr> &instrs,
                                      std::size_t idx,
                                      PeepholeStats &stats,
                                      const PhysRegSet *exitLive = nullptr);

} // namespace zanna::codegen::aarch64::peephole
