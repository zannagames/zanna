//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
// File: src/vm/ops/common/Branching.hpp
// Purpose: Declare shared helpers for branching opcode handlers.
// Key invariants: Targets describe valid block transitions and are validated
//                 before being executed. Case tables capture value-to-target
//                 mappings used by switch-style dispatch.
// Ownership/Lifetime: Helpers operate on VM-owned state and never assume
//                     ownership of blocks, frames, or instructions.
// Links: docs/il/il-guide.md#reference
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares shared branch-target, case-selection, and jump helpers.
/// @details The abstractions carry non-owning VM control-flow references,
///          validate target completeness, propagate destination parameters,
///          and provide a uniform scalar switch representation.

#pragma once

#include "vm/VM.hpp"

#include "il/core/Instr.hpp"

#include <cstdint>
#include <span>

namespace il::vm::ops::common {

/// @brief Scalar value used when selecting switch cases.
struct Scalar {
    int32_t value = 0; ///< Signed scalar payload.
};

/// @brief Describes a concrete branch destination.
struct Target {
    VM *vm = nullptr;                       ///< Owning VM required for evaluation.
    const il::core::Instr *instr = nullptr; ///< Source instruction providing metadata.
    size_t labelIndex = 0;                  ///< Index into @ref il::core::Instr::labels.
    const VM::BlockMap *blocks = nullptr;   ///< Block lookup by label.
    const il::core::BasicBlock **currentBlock = nullptr; ///< Pointer to currently executing block.
    size_t *ip = nullptr; ///< Instruction pointer within current block.

    /// @brief Determine whether the target refers to a valid jump destination.
    /// @return @c true when every required pointer and label index is valid.
    bool valid() const noexcept {
        return vm != nullptr && instr != nullptr && blocks != nullptr && currentBlock != nullptr &&
               ip != nullptr && labelIndex < instr->labels.size();
    }
};

/// @brief Entry in a switch case table mapping ranges or exact values to targets.
struct Case {
    Scalar lower{};       ///< Lower bound of the case (inclusive).
    Scalar upper{};       ///< Upper bound when representing a range (inclusive).
    bool isRange = false; ///< Whether the case represents a range rather than a single value.
    Target target{};      ///< Branch destination associated with the case.

    /// @brief Construct an exact-match case entry.
    /// @param value Scalar matched by the entry.
    /// @param target Destination selected on equality.
    /// @return Exact case containing @p value as both inclusive bounds.
    static Case exact(Scalar value, Target target) noexcept {
        Case entry{};
        entry.lower = value;
        entry.upper = value;
        entry.isRange = false;
        entry.target = target;
        return entry;
    }

    /// @brief Construct an inclusive range case entry.
    /// @param lo Inclusive lower bound.
    /// @param hi Inclusive upper bound.
    /// @param target Destination selected for values within the range.
    /// @return Range case preserving the supplied bounds and target.
    static Case range(Scalar lo, Scalar hi, Target target) noexcept {
        Case entry{};
        entry.lower = lo;
        entry.upper = hi;
        entry.isRange = true;
        entry.target = target;
        return entry;
    }
};

/// @brief Select the branch target associated with a scrutinee value.
/// @param scrutinee Scalar value to match.
/// @param table Ordered exact/range cases; the first match wins.
/// @param default_tgt Fallback returned when no case matches.
/// @return Matching case target or @p default_tgt.
Target select_case(Scalar scrutinee, std::span<const Case> table, Target default_tgt);

/// @brief Transfer control to the provided target, propagating block parameters.
/// @param frame Active frame supplying operands and receiving staged parameters.
/// @param target Validated non-owning description of the destination and control state.
void jump(Frame &frame, Target target);

/// @brief Evaluate the scrutinee operand for a switch instruction.
/// @param frame Active frame supplying the operand.
/// @param instr Switch-like instruction containing the scrutinee.
/// @return Scrutinee narrowed to the shared signed 32-bit representation.
Scalar eval_scrutinee(Frame &frame, const il::core::Instr &instr);

} // namespace il::vm::ops::common
