//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/PeepholeDCE.hpp
// Purpose: Template-based dead code elimination shared between AArch64 and
//          x86-64 peephole passes.
//
// Key invariants:
//   - Performs backward liveness analysis within a single basic block.
//   - Only removes instructions with no side effects whose defined register
//     is not live at that point.
//   - Iterates until a fixed point is reached (x86-64 mode) or runs a
//     single pass (AArch64 mode), controlled by the Traits parameter.
//
// Ownership/Lifetime:
//   - Operates on mutable instruction vectors owned by the caller.
//   - Temporary data structures (liveness sets, removal masks) are stack
//     allocated and do not outlive the call.
//
// Links: docs/internals/architecture.md, codegen/common/PeepholeUtil.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "codegen/common/ICE.hpp"
#include "codegen/common/PeepholeUtil.hpp"

#include <algorithm>
#include <string>
#include <unordered_set>
#include <vector>

/// @file
/// @brief Implements traits-based block-local machine dead-code elimination.

namespace zanna::codegen::common {

/// @brief Template-based dead code elimination for machine IR basic blocks.
///
/// @details Performs a backward liveness scan over a vector of machine
///          instructions, marking instructions as dead when they define a
///          register that is not in the current live set and have no side
///          effects. The algorithm is parameterised on a @p Traits type that
///          provides target-specific helpers.
///
/// ## Required Traits interface
///
/// @code
/// struct ExampleTraits {
///     using MInstr  = /* target MInstr type */;
///     using RegKey  = /* uint16_t or uint32_t */;
///
///     // If true, repeat the backward scan until no more dead instructions
///     // are found (fixed-point iteration). If false, run a single pass.
///     static constexpr bool kIterateToFixpoint = false;
///
///     // Return true when the instruction has observable side effects.
///     static bool hasSideEffects(const MInstr &instr) noexcept;
///
///     // Return the register key defined by the instruction, or nullopt.
///     static std::optional<RegKey> getDefRegKey(const MInstr &instr) noexcept;
///
///     // Insert all register keys used (read) by the instruction into the set.
///     static void collectUsedRegKeys(const MInstr &instr,
///                                     std::unordered_set<RegKey> &live) noexcept;
///
///     // Insert register keys that are conservatively live at block exit.
///     static void addBlockExitLiveKeys(std::unordered_set<RegKey> &live) noexcept;
///
///     // Return true when the instruction is a label or branch target that
///     // requires all allocatable registers to be treated as live. When true,
///     // addAllAllocatableKeys() is called to mark them live.
///     static bool isLabelOrBranchTarget(const MInstr &instr) noexcept;
///
///     // Insert all allocatable register keys (called at labels/targets).
///     static void addAllAllocatableKeys(std::unordered_set<RegKey> &live) noexcept;
/// };
/// @endcode
///
/// @tparam Traits Target-specific type providing the required interface.
/// @param instrs  Mutable instruction vector to compact in place.
/// @return Number of dead instructions removed.
template <typename Traits> std::size_t runBlockDCE(std::vector<typename Traits::MInstr> &instrs) {
    using MInstr = typename Traits::MInstr;
    using RegKey = typename Traits::RegKey;

    if (instrs.empty())
        return 0;

    std::size_t totalEliminated = 0;
    bool changed = true;
    constexpr std::size_t kMaxDCEIterations = 100;
    std::size_t iterCount = 0;

    // Outer loop: repeat until fixed point when requested by Traits.
    while (changed && iterCount++ < kMaxDCEIterations) {
        changed = false;

        std::unordered_set<RegKey> liveRegs;

        // Seed the live set with registers conservatively assumed live at
        // block exit (callee-saved, return registers, etc.).
        Traits::addBlockExitLiveKeys(liveRegs);

        std::vector<bool> toRemove(instrs.size(), false);

        // Backward scan: process instructions from last to first.
        for (std::size_t i = instrs.size(); i > 0; --i) {
            const std::size_t idx = i - 1;
            const auto &instr = instrs[idx];

            // Instructions with side effects are always kept.
            if (Traits::hasSideEffects(instr)) {
                // At label/branch-target boundaries, conservatively assume
                // all allocatable registers may be live.
                if (Traits::isLabelOrBranchTarget(instr))
                    Traits::addAllAllocatableKeys(liveRegs);

                Traits::collectUsedRegKeys(instr, liveRegs);
                continue;
            }

            // Try to obtain the register defined by this instruction.
            auto defKey = Traits::getDefRegKey(instr);
            if (!defKey.has_value()) {
                // No definition — keep and collect uses.
                Traits::collectUsedRegKeys(instr, liveRegs);
                continue;
            }

            // If the defined register is not live, the instruction is dead.
            if (liveRegs.find(*defKey) == liveRegs.end()) {
                toRemove[idx] = true;
                changed = true;
                continue;
            }

            // The result is live: remove it from the live set (we are
            // defining it here) and add the instruction's uses.
            liveRegs.erase(*defKey);
            Traits::collectUsedRegKeys(instr, liveRegs);
        }

        // Compact the instruction vector.
        if (changed) {
            std::size_t removed =
                static_cast<std::size_t>(std::count(toRemove.begin(), toRemove.end(), true));
            removeMarkedInstructions(instrs, toRemove);
            totalEliminated += removed;
        }

        // When the traits disable fixed-point iteration, exit after one pass.
        if constexpr (!Traits::kIterateToFixpoint)
            break;
    }
    if (changed) {
        ZANNA_ICE("peephole dead-code elimination did not converge after " +
                  std::to_string(kMaxDCEIterations) + " iterations");
    }

    return totalEliminated;
}

} // namespace zanna::codegen::common
