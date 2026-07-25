//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/ra/LiveIntervals.cpp
// Purpose: Implement the lightweight live interval analysis that feeds the
//          linear-scan allocator. The analysis walks each machine instruction
//          in program order and records first/last touch positions for virtual
//          registers.
// Key invariants:
//   - Instruction indices are monotonically increasing per function.
//   - Repeated invocations rebuild the analysis state deterministically.
// Ownership/Lifetime:
//   - Operates on a const MIR reference; results stored in value-owned containers.
// Links: src/codegen/x86_64/ra/LiveIntervals.hpp
//
//===----------------------------------------------------------------------===//

#include "LiveIntervals.hpp"

#include <limits>

/// @file
/// @brief Implements deterministic function-wide virtual-register touch ranges.

namespace zanna::codegen::x64::ra {

namespace {

/// @brief Update an interval with a new observation position.
/// @details The helper expands the closed-open [start, end) range tracked by the
///          interval so that it covers @p pos. When the interval has not been
///          observed before the function seeds both bounds from the provided
///          index. Subsequent updates take the minimum/maximum with existing
///          bounds to avoid shrinking the live range when operands reappear
///          later in the instruction stream.
/// @param interval Interval structure that records the first and last touches
///                  of a virtual register.
/// @param pos Instruction index associated with the latest observation.
void updateInterval(LiveInterval &interval, std::size_t pos) {
    if (interval.start == 0U && interval.end == 0U) {
        interval.start = pos;
        interval.end = pos + 1U;
        return;
    }
    interval.start = std::min(interval.start, pos);
    interval.end = std::max(interval.end, pos + 1U);
}

} // namespace

/// @brief Compute live intervals for every virtual register in a function.
/// @details Walks the machine function in program order, assigns a monotonically
///          increasing instruction index to each opcode, and feeds every
///          encountered virtual register into @ref updateInterval. Memory
///          operands are also inspected so base registers extending live ranges
///          through loads and stores are recorded. The analysis resets any
///          previous results before executing so repeated invocations stay
///          deterministic.
/// @param func Machine function whose instructions should be analysed.
void LiveIntervals::run(const MFunction &func) {
    intervals_.clear();

    std::size_t index = 0U;
    for (const auto &block : func.blocks) {
        for (const auto &instr : block.instructions) {
            for (const auto &operand : instr.operands) {
                if (const auto *reg = std::get_if<OpReg>(&operand); reg && !reg->isPhys) {
                    auto &interval = intervals_[reg->idOrPhys];
                    if (interval.vreg == kInvalidVReg) {
                        interval.vreg = reg->idOrPhys;
                        interval.cls = reg->cls;
                        interval.start = index;
                        interval.end = index + 1U;
                    } else {
                        updateInterval(interval, index);
                    }
                } else if (const auto *mem = std::get_if<OpMem>(&operand)) {
                    if (!mem->base.isPhys) {
                        auto &interval = intervals_[mem->base.idOrPhys];
                        if (interval.vreg == kInvalidVReg) {
                            interval.vreg = mem->base.idOrPhys;
                            interval.cls = mem->base.cls;
                            interval.start = index;
                            interval.end = index + 1U;
                        } else {
                            updateInterval(interval, index);
                        }
                    }
                    // Also track the index register if present
                    if (mem->hasIndex && !mem->index.isPhys) {
                        auto &interval = intervals_[mem->index.idOrPhys];
                        if (interval.vreg == kInvalidVReg) {
                            interval.vreg = mem->index.idOrPhys;
                            interval.cls = mem->index.cls;
                            interval.start = index;
                            interval.end = index + 1U;
                        } else {
                            updateInterval(interval, index);
                        }
                    }
                }
            }
            ++index;
        }
    }
}

/// @brief Retrieve the computed interval for a virtual register.
/// @details Performs a dictionary lookup against the cached analysis state and
///          returns @c nullptr when the register was never observed. The method
///          avoids inserting new entries so callers can cheaply probe for
///          optional intervals during allocation. Returned storage remains
///          owned by this analysis and is invalidated when @ref run clears it.
/// @param vreg Identifier of the virtual register to query.
/// @return Pointer to the interval owned by the analysis, or @c nullptr when no
///         interval exists.
const LiveInterval *LiveIntervals::lookup(uint16_t vreg) const noexcept {
    auto it = intervals_.find(vreg);
    if (it == intervals_.end()) {
        return nullptr;
    }
    return &it->second;
}

} // namespace zanna::codegen::x64::ra
