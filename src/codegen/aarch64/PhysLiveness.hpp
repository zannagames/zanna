//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/PhysLiveness.hpp
// Purpose: Function-wide physical-register liveness over the shared MirCfg:
//          the one fact every post-RA rewrite reads before it redirects or
//          drops a definition that reaches a block end, and the dataflow
//          input of the verifier's post-RA rules.
// Key invariants:
//   - Register facts come from effectsOf(), so a call's argument reads and
//     caller-saved clobbers, a return's result reads, and the reserved
//     scratch clobbers are all visible to the solver.
//   - Edges come from MirCfg (ra::classifyControlFlow), so the solver sees
//     the same graph the allocator and the verifier see.
//   - liveOut[b] = union of liveIn[s] over successors s;
//     liveIn[b] = gen[b] | (liveOut[b] & ~kill[b]). A block without
//     successors has an empty live-out; blockExitLive() (MirCfg.hpp) adds
//     the function-exit registers on top.
// Ownership/Lifetime:
//   - PhysLiveness is a snapshot: it holds no reference to the function and
//     is invalidated by any change to its instructions or block order.
// Links: src/codegen/aarch64/PhysLiveness.cpp, src/codegen/aarch64/MirCfg.hpp,
//        src/codegen/aarch64/InstrEffects.hpp, src/codegen/aarch64/MirVerify.cpp,
//        docs/internals/backend-codegen-review-2026-09.md (Phase 3 C4)
//
//===----------------------------------------------------------------------===//

#pragma once

#include "codegen/aarch64/InstrEffects.hpp"
#include "codegen/aarch64/MachineIR.hpp"
#include "codegen/aarch64/TargetAArch64.hpp"

#include <vector>

/// @file
/// @brief Declares physical-register liveness for allocated AArch64 MIR.

namespace zanna::codegen::aarch64 {

/// @brief Per-block physical-register liveness of one allocated MIR function.
/// @invariant Both vectors are indexed by block position in `MFunction::blocks`
///            and have exactly one entry per block.
struct PhysLiveness {
    std::vector<PhysRegSet> liveIn;  ///< Registers live before each block's first instruction.
    std::vector<PhysRegSet> liveOut; ///< Registers live after each block's last instruction.
};

/// @brief Solve backward physical-register liveness for @p fn.
/// @details Gen/kill sets come from `effectsOf(mi, target)` for every
///          instruction, the CFG from `MirCfg`. Blocks that leave the function
///          (a `Ret`, a no-return call, or falling off the end) have an empty
///          live-out: the `Ret` itself reads the return registers, and a
///          no-return call reads its arguments. Post-RA rewrites that need the
///          conservative function-exit seed on top of this use
///          `blockExitLive()`.
/// @param fn     Allocated function (virtual registers are ignored).
/// @param target ABI description supplying call and return register sets.
/// @return Live-in and live-out sets, one per block.
[[nodiscard]] PhysLiveness computePhysLiveness(const MFunction &fn, const TargetInfo &target);

} // namespace zanna::codegen::aarch64
