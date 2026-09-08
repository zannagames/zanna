//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/PhysLiveness.cpp
// Purpose: Backward dataflow over MirCfg for physical registers on x86-64
//          (gen = read before written, kill = written), and the exit-live
//          seed the post-RA rewrites read.
// Key invariants:
//   - The solver iterates to a fixed point over the sorted successor lists
//     of MirCfg; the result is deterministic.
//   - blockExitLive adds only what no in-block read can express: RSP/RBP and
//     the return registers of a block that leaves the function.
// Ownership/Lifetime:
//   - Pure functions; the result owns its vectors.
// Links: src/codegen/x86_64/PhysLiveness.hpp
//
//===----------------------------------------------------------------------===//

#include "codegen/x86_64/PhysLiveness.hpp"

#include "codegen/x86_64/MirCfg.hpp"
#include "codegen/x86_64/ra/Liveness.hpp"

/// @file
/// @brief Implements the x86-64 physical liveness solver and exit-live seed.

namespace zanna::codegen::x64 {

/// @copydoc computePhysLiveness
PhysLiveness computePhysLiveness(const MFunction &fn, const TargetInfo &target) {
    const std::size_t n = fn.blocks.size();
    const MirCfg cfg(fn);
    const auto &succs = cfg.successors();

    std::vector<PhysRegMask> gen(n, 0);
    std::vector<PhysRegMask> kill(n, 0);
    for (std::size_t bi = 0; bi < n; ++bi) {
        for (const auto &mi : fn.blocks[bi].instructions) {
            const InstrEffects fx = effectsOf(mi, target);
            gen[bi] |= fx.uses & ~kill[bi];
            kill[bi] |= fx.defs;
        }
    }

    PhysLiveness result;
    result.liveIn.assign(n, 0);
    result.liveOut.assign(n, 0);
    bool changed = true;
    while (changed) {
        changed = false;
        for (std::size_t bi = n; bi-- > 0;) {
            PhysRegMask out = 0;
            for (std::size_t s : succs[bi])
                out |= result.liveIn[s];
            const PhysRegMask in = gen[bi] | (out & ~kill[bi]);
            if (out != result.liveOut[bi] || in != result.liveIn[bi]) {
                result.liveOut[bi] = out;
                result.liveIn[bi] = in;
                changed = true;
            }
        }
    }
    return result;
}

/// @copydoc blockExitLive
PhysRegMask blockExitLive(const MFunction &fn,
                          std::size_t bi,
                          const TargetInfo &target,
                          const PhysLiveness &liveness) {
    using Desc = zanna::codegen::ra::BranchDesc;

    const MBasicBlock &block = fn.blocks[bi];
    PhysRegMask live = liveness.liveOut[bi];
    live |= physRegBit(PhysReg::RSP) | physRegBit(PhysReg::RBP);

    // Does control leave the function here: through RET, or by falling off
    // the end of the last block (which the verifier reports, but must not be
    // mis-optimised on the way there)?
    bool returns = bi + 1 >= fn.blocks.size();
    for (const MInstr &mi : block.instructions) {
        const Desc desc = ra::classifyControlFlow(mi);
        if (desc.kind == Desc::Kind::Return) {
            returns = true;
            break;
        }
        if (desc.kind == Desc::Kind::Uncond || desc.kind == Desc::Kind::Multi ||
            desc.kind == Desc::Kind::NoReturn) {
            returns = false;
            break;
        }
    }
    if (returns)
        live |= physRegBit(target.intReturnReg) | physRegBit(target.f64ReturnReg);
    return live;
}

} // namespace zanna::codegen::x64
