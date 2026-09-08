//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/PhysLiveness.hpp
// Purpose: Physical-register liveness of an allocated x86-64 MIR function:
//          per-block live-in and live-out masks solved over MirCfg from the
//          effects model, for the verifier, the post-RA peepholes, and the
//          function-wide allocator's fixed ranges.
// Key invariants:
//   - Every register fact comes from effectsOf(): explicit operands, memory
//     address registers, implicit RAX/RDX/RCX effects, call argument reads
//     and caller-saved clobbers, return reads.
//   - Edges come from MirCfg, so the solution agrees with every other
//     consumer of the CFG.
// Ownership/Lifetime:
//   - PhysLiveness is a snapshot; it is invalidated by any change to the
//     function.
// Links: src/codegen/x86_64/PhysLiveness.cpp, src/codegen/x86_64/MirCfg.hpp,
//        src/codegen/x86_64/OperandRoles.hpp, src/codegen/x86_64/MirVerify.cpp,
//        docs/internals/backend-codegen-review-2026-09.md (Phase 3 C8)
//
//===----------------------------------------------------------------------===//

#pragma once

#include "codegen/x86_64/MachineIR.hpp"
#include "codegen/x86_64/OperandRoles.hpp"
#include "codegen/x86_64/TargetX64.hpp"

#include <cstddef>
#include <vector>

/// @file
/// @brief Declares the x86-64 post-allocation physical liveness solver.

namespace zanna::codegen::x64 {

/// @brief Per-block physical-register liveness masks (bit = PhysReg ordinal).
struct PhysLiveness {
    std::vector<PhysRegMask> liveIn;  ///< Registers read before written, per block.
    std::vector<PhysRegMask> liveOut; ///< Union of the successors' live-in, per block.
};

/// @brief Solve physical liveness for @p fn under @p target.
/// @param fn Allocated (or partially allocated) function; virtual registers
///        are ignored.
/// @param target ABI description for call/return effects.
/// @return One live-in and one live-out mask per block.
[[nodiscard]] PhysLiveness computePhysLiveness(const MFunction &fn, const TargetInfo &target);

/// @brief Physical registers that must be treated as live at the exit of
///        block @p bi after register allocation.
/// @details The solved live-out of the block plus RSP/RBP, plus the integer
///          and floating-point return registers when the block leaves the
///          function (through `RET` or by falling off the end of the last
///          block). A callee-saved register is live only when some successor
///          actually reads it; the epilogue's restores are explicit
///          instructions, so a value left in one before them is dead.
/// @param fn Function owning the block.
/// @param bi Block index in `[0, fn.blocks.size())`.
/// @param target ABI description supplying the return registers.
/// @param liveness Physical liveness of @p fn in its current shape.
/// @return The live-at-exit mask.
[[nodiscard]] PhysRegMask blockExitLive(const MFunction &fn,
                                        std::size_t bi,
                                        const TargetInfo &target,
                                        const PhysLiveness &liveness);

} // namespace zanna::codegen::x64
