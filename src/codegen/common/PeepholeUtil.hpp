//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
///
/// @file src/codegen/common/PeepholeUtil.hpp
/// @brief Shared utility helpers used by both the AArch64 and x86-64 peephole
///        optimization passes.
///
/// @details Houses small, target-independent helpers that both backends require
///          identically. Keeping the implementations here avoids duplication and
///          ensures any future correctness fix propagates to all targets at once.
///
/// @note All functions are `inline` so this header can be included directly
///       without introducing a separate translation unit.
///
//===----------------------------------------------------------------------===//

#pragma once

#include <vector>

namespace zanna::codegen::common {

/// @brief Remove all instructions marked for deletion from an instruction list.
///
/// @details Performs a stable compaction: surviving instructions are shifted
///          left in-place using move semantics so that their relative order is
///          preserved.  The vector is resized to the number of surviving entries
///          after the sweep.
///
/// @tparam MInstr Machine instruction type (AArch64 or x86-64 specific).
/// @param instrs  Mutable instruction list to compact in place.
/// @param toRemove Per-instruction deletion flags; must be the same size as
///                 @p instrs.  An entry of @c true at index @p i causes
///                 @c instrs[i] to be dropped from the output.
/// @pre `toRemove.size() == instrs.size()`.
/// @post `instrs` contains exactly the unmarked input elements in original order.
template <typename MInstr>
inline void removeMarkedInstructions(std::vector<MInstr> &instrs,
                                     const std::vector<bool> &toRemove) {
    std::size_t writeIdx = 0;
    for (std::size_t readIdx = 0; readIdx < instrs.size(); ++readIdx) {
        if (!toRemove[readIdx]) {
            if (writeIdx != readIdx)
                instrs[writeIdx] = std::move(instrs[readIdx]);
            ++writeIdx;
        }
    }
    instrs.resize(writeIdx);
}

} // namespace zanna::codegen::common
