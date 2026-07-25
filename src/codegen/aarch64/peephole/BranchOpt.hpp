//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/peephole/BranchOpt.hpp
// Purpose: Declarations for branch optimizations: CBZ/CBNZ fusion, cset
//          branch fusion, branch inversion, block reordering, and
//          branch-to-next removal.
//
// Key invariants:
//   - Branch rewrites preserve control-flow semantics.
//   - Block reordering only moves cold blocks (trap/error handlers) to the end.
//
// Ownership/Lifetime:
//   - Operates on mutable MFunction/instruction vectors owned by the caller.
//
// Links: codegen/aarch64/Peephole.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "../MachineIR.hpp"
#include "../Peephole.hpp"

#include <cstddef>
#include <string>
#include <vector>

/// @file
/// @brief Declares local branch fusion and safe cold-block layout transforms.

namespace zanna::codegen::aarch64::peephole {

/// @brief Obtain the logical inverse of a supported AArch64 condition code.
/// @param cond Null-terminated lowercase condition mnemonic, or null.
/// @return A pointer to a static inverse mnemonic, or null when @p cond is null
///         or is not one of the supported condition codes.
[[nodiscard]] const char *invertCondition(const char *cond) noexcept;

/// @brief Test whether an instruction is an unconditional branch to a label.
/// @param instr Machine instruction to inspect.
/// @param label Exact target label expected in the branch operand.
/// @return `true` only for a well-formed `Br` instruction targeting @p label.
[[nodiscard]] bool isBranchTo(const MInstr &instr, const std::string &label) noexcept;

/// @brief Fuse a zero comparison and conditional branch into `CBZ` or `CBNZ`.
///
/// Recognizes adjacent `CmpRI reg, 0` or `TstRR reg, reg` followed by `B.eq`
/// or `B.ne`. On success the compare is replaced by the fused branch and the
/// original conditional branch is erased.
///
/// @param[in,out] instrs Instruction sequence being rewritten.
/// @param idx Index of the compare candidate.
/// @param[in,out] stats Statistics updated when a fusion succeeds.
/// @return `true` when the sequence at @p idx was fused.
[[nodiscard]] bool tryCbzCbnzFusion(std::vector<MInstr> &instrs,
                                    std::size_t idx,
                                    PeepholeStats &stats);

/// @brief Fuse a single-bit mask and zero test into `TBZ` or `TBNZ`.
///
/// The scan accepts either an existing `CBZ`/`CBNZ` consumer or the
/// `TstRR` plus `B.eq`/`B.ne` form emitted for boolean branches. It stops when
/// control flow, flag writes, or definitions of involved registers make the
/// rewrite unsafe. The mask result must be dead after its branch.
///
/// @param[in,out] instrs Instruction sequence being rewritten.
/// @param idx Index of the `AndRI` candidate.
/// @param[in,out] stats Statistics updated when a fusion succeeds.
/// @param carriedExitRegs Optional sorted physical-register identifiers carried
///        live across the enclosing block's exit. A carried mask destination
///        prevents fusion because its successor use is invisible locally.
/// @return `true` when the mask and branch were replaced by a bit-test branch.
[[nodiscard]] bool tryTbzTbnzFusion(std::vector<MInstr> &instrs,
                                    std::size_t idx,
                                    PeepholeStats &stats,
                                    const std::vector<uint16_t> *carriedExitRegs = nullptr);

/// @brief Fold a materialized condition and zero branch into `B.cond`.
///
/// Scans forward from `Cset` to a matching `CBZ` or `CBNZ` while requiring
/// NZCV and the materialized register to remain valid. `CBZ` uses the inverse
/// of the `Cset` condition; `CBNZ` reuses it directly. The `Cset` destination
/// must have no later observable use.
///
/// @param[in,out] instrs Instruction sequence being rewritten.
/// @param idx Index of the `Cset` candidate.
/// @param[in,out] stats Statistics updated when a fusion succeeds.
/// @param carriedExitRegs Optional sorted list of physical registers carried
///        live across the enclosing block's exit without any in-block use
///        (MBasicBlock::carriedExitRegs); a CSET into such a register is
///        never fused away.
/// @return `true` when the materialization and zero branch were folded.
[[nodiscard]] bool tryCsetBranchFusion(std::vector<MInstr> &instrs,
                                       std::size_t idx,
                                       PeepholeStats &stats,
                                       const std::vector<uint16_t> *carriedExitRegs = nullptr);

/// @brief Move safely relocatable trap and error blocks to the function tail.
///
/// Coldness is inferred from bounded label tokens and known no-return helpers.
/// A block is moved only when neither its incoming predecessor nor its own exit
/// relies on layout fallthrough. Relative order is preserved within the hot and
/// moved-cold partitions.
///
/// @param[in,out] fn Function whose owned block vector may be reordered.
/// @return Number of cold blocks moved.
std::size_t reorderBlocks(MFunction &fn);

} // namespace zanna::codegen::aarch64::peephole
