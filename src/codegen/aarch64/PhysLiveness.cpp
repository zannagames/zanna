//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/PhysLiveness.cpp
// Purpose: Implements the backward physical-register liveness solver shared
//          by the post-RA peephole stages and the MIR verifier.
// Key invariants:
//   - One fixed-point iteration in reverse block order per round; the
//     solution is the least fixed point (sets only grow).
//   - No opcode table of its own: every use/def comes from effectsOf().
// Ownership/Lifetime:
//   - Stateless; the result is returned by value.
// Links: src/codegen/aarch64/PhysLiveness.hpp, src/codegen/aarch64/MirCfg.hpp
//
//===----------------------------------------------------------------------===//

#include "codegen/aarch64/PhysLiveness.hpp"

#include "codegen/aarch64/MirCfg.hpp"

#include <cstddef>

/// @file
/// @brief Implements computePhysLiveness().

namespace zanna::codegen::aarch64 {

/// @copydoc computePhysLiveness
PhysLiveness computePhysLiveness(const MFunction &fn, const TargetInfo &target) {
    const std::size_t n = fn.blocks.size();
    const MirCfg cfg(fn);
    const auto &succs = cfg.successors();

    std::vector<PhysRegSet> gen(n);
    std::vector<PhysRegSet> kill(n);
    for (std::size_t bi = 0; bi < n; ++bi) {
        for (const auto &mi : fn.blocks[bi].instrs) {
            const InstrEffects fx = effectsOf(mi, target);
            gen[bi].bits |= fx.uses.bits & ~kill[bi].bits;
            kill[bi].bits |= fx.defs.bits;
        }
    }

    PhysLiveness result;
    result.liveIn.assign(n, {});
    result.liveOut.assign(n, {});
    bool changed = true;
    while (changed) {
        changed = false;
        for (std::size_t bi = n; bi-- > 0;) {
            PhysRegSet out;
            for (std::size_t s : succs[bi])
                out.bits |= result.liveIn[s].bits;
            PhysRegSet in;
            in.bits = gen[bi].bits | (out.bits & ~kill[bi].bits);
            if (out.bits != result.liveOut[bi].bits || in.bits != result.liveIn[bi].bits) {
                result.liveOut[bi] = out;
                result.liveIn[bi] = in;
                changed = true;
            }
        }
    }
    return result;
}

} // namespace zanna::codegen::aarch64
