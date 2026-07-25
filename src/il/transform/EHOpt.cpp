//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/transform/EHOpt.cpp
// Purpose: Implements exception handling optimization.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Removes redundant eh.push/eh.pop pairs in IL functions.
/// @details Scans each basic block for eh.push/eh.pop pairs that bracket
///          regions with no potentially-throwing instructions (calls, traps,
///          checked operations). When no throw is possible, the EH overhead
///          is dead and both instructions are erased. Dead handler blocks are
///          cleaned up by subsequent DCE/SimplifyCFG passes.

#include "il/transform/EHOpt.hpp"

#include "il/core/BasicBlock.hpp"
#include "il/core/Function.hpp"
#include "il/core/Instr.hpp"
#include "il/core/Module.hpp"
#include "il/core/Opcode.hpp"
#include "il/verify/VerifierTable.hpp"

#include <algorithm>
#include <vector>

namespace il::transform {

using namespace il::core;

namespace {

/// @brief Check whether an opcode could potentially throw or trap.
/// @details Conservative: any call, trap, or checked arithmetic instruction
///          is considered potentially throwing. All other instructions are
///          considered safe.
/// @param op Opcode to classify.
/// @return True when verifier metadata or the conservative fallback marks the
///         operation as potentially trapping.
bool canThrow(Opcode op) {
    if (const auto props = il::verify::lookup(op); props && props->canTrap)
        return true;

    switch (op) {
        case Opcode::Call:
        case Opcode::CallIndirect:
        case Opcode::Trap:
        case Opcode::TrapFromErr:
        case Opcode::IdxChk:
        case Opcode::CastFpToSiRteChk:
        case Opcode::CastFpToUiRteChk:
        case Opcode::CastSiNarrowChk:
        case Opcode::CastUiNarrowChk:
            return true;
        default:
            return false;
    }
}

/// @brief Optimize EH pairs in a single function.
/// @param fn Function whose basic blocks are scanned and updated.
/// @return True if any EH instructions were removed.
bool ehOptFunction(Function &fn) {
    bool changed = false;

    for (auto &bb : fn.blocks) {
        // Look for eh.push/eh.pop pairs within the same block where the
        // region between them has no potentially-throwing instructions.
        // This handles the common case of trivial try blocks.
        auto &instrs = bb.instructions;

        // Scan for eh.push instructions
        for (size_t i = 0; i < instrs.size(); ++i) {
            if (instrs[i].op != Opcode::EhPush)
                continue;

            // Find the matching eh.pop in the same block
            size_t popIdx = SIZE_MAX;
            bool hasThrowingInstr = false;

            for (size_t j = i + 1; j < instrs.size(); ++j) {
                if (instrs[j].op == Opcode::EhPop) {
                    popIdx = j;
                    break;
                }
                // Another eh.push means nested EH; stop scanning
                if (instrs[j].op == Opcode::EhPush)
                    break;
                if (canThrow(instrs[j].op)) {
                    hasThrowingInstr = true;
                    break;
                }
            }

            if (popIdx == SIZE_MAX || hasThrowingInstr)
                continue;

            // Safe to remove: no throwing instructions between push and pop.
            // Mark both for removal (erase in reverse order to preserve indices).
            instrs.erase(instrs.begin() + static_cast<ptrdiff_t>(popIdx));
            instrs.erase(instrs.begin() + static_cast<ptrdiff_t>(i));
            changed = true;
            // Adjust index since we removed two instructions before/at current pos
            --i;
        }
    }

    return changed;
}

} // namespace

/// @brief Remove redundant same-block EH regions throughout a module.
/// @param module Module updated in place.
/// @return True when at least one `eh.push`/`eh.pop` pair was removed.
bool ehOpt(Module &module) {
    bool changed = false;
    for (auto &fn : module.functions)
        changed |= ehOptFunction(fn);
    if (changed)
        module.internOwnedIdentifiers();
    return changed;
}

} // namespace il::transform
