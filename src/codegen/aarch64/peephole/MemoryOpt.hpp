//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/peephole/MemoryOpt.hpp
// Purpose: Declarations for memory optimizations: LDP/STP merging,
//          store-load forwarding, and MADD fusion.
//
// Key invariants:
//   - LDP/STP merge only pairs adjacent FP-relative accesses with matching
//     offset alignment.
//   - Store-load forwarding only applies within a basic block scope.
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
/// @brief Declares local load/store and multiply-add peephole rewrites.

namespace zanna::codegen::aarch64::peephole {

/// @brief Merge adjacent scalar FP-relative accesses into a load/store pair.
///
/// Supports GPR and FPR eight-byte loads or stores whose offsets are adjacent,
/// pair-aligned, and encodable by the AArch64 pair instruction. Load
/// destinations must be distinct. The pair is normalized to ascending address
/// order regardless of the original instruction order.
///
/// @param[in,out] instrs Instruction sequence containing the candidate pair.
/// @param idx Index of the first scalar access.
/// @param[in,out] stats Statistics updated when a merge succeeds.
/// @return `true` when two instructions were replaced by one `LDP` or `STP`.
[[nodiscard]] bool tryLdpStpMerge(std::vector<MInstr> &instrs,
                                  std::size_t idx,
                                  PeepholeStats &stats);

/// @brief Forward FP-relative scalar stores to later same-slot loads.
///
/// Replaces a load with a same-class register move while the stored register
/// and memory value remain available. Overlapping stores, calls, control flow,
/// source-register definitions, and arbitrary-base memory stop the scan.
///
/// @param[in,out] instrs Block-local instruction sequence to rewrite.
/// @param[in,out] stats Statistics updated for each forwarded load.
/// @return Number of loads replaced by register moves.
std::size_t forwardStoreLoads(std::vector<MInstr> &instrs, PeepholeStats &stats);

/// @brief Fuse an adjacent integer multiply and addition into `MAddRRRR`.
///
/// The multiply result must be exactly one add input and must be dead after the
/// add. Either commutative add-input position is accepted. Deadness follows
/// the shared effects model (call argument/clobber sets, return registers)
/// and, when the scan reaches the block exit without a redefinition, the
/// block's exit-live set (see blockExitLive()).
///
/// @param[in,out] instrs Instruction sequence containing the candidate pair.
/// @param idx Index of the `MulRRR` candidate.
/// @param[in,out] stats Statistics updated when a fusion succeeds.
/// @param target ABI description for call and return effects.
/// @param exitLive Physical registers live at the enclosing block's exit.
/// @return `true` when the multiply and add were replaced by one instruction.
[[nodiscard]] bool tryMaddFusion(std::vector<MInstr> &instrs,
                                 std::size_t idx,
                                 PeepholeStats &stats,
                                 const TargetInfo &target,
                                 const PhysRegSet &exitLive);

} // namespace zanna::codegen::aarch64::peephole
