//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/PeepholeCopyProp.hpp
// Purpose: Template-based copy propagation shared between AArch64 and x86-64
//          peephole passes.
//
// Key invariants:
//   - Propagates register copy origins forward through a basic block.
//   - Does not propagate through ABI registers (argument/return regs) to
//     avoid creating dead code that the DCE cannot eliminate.
//   - Invalidates copy relationships when a register is redefined.
//
// Ownership/Lifetime:
//   - Operates on mutable instruction vectors owned by the caller.
//   - Temporary maps are stack-allocated and do not outlive the call.
//
// Links: docs/internals/architecture.md, codegen/common/PeepholeUtil.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

/// @file
/// @brief Implements traits-based block-local physical copy propagation.

namespace zanna::codegen::common {

/// @brief Template-based copy propagation for machine IR basic blocks.
///
/// @details Tracks register-to-register copy chains within a basic block and
///          replaces uses of copied registers with their original source. This
///          enables subsequent passes (identity move removal, DCE) to clean up
///          redundant copies.
///
/// ## Required Traits interface
///
/// @code
/// struct ExampleTraits {
///     using MInstr   = /* target MInstr type */;
///     using Operand  = /* operand type (MOperand or std::variant) */;
///     using RegKey   = uint32_t;
///
///     // Return the mutable operand vector from an instruction.
///     static std::vector<Operand> &getOps(MInstr &instr);
///     static const std::vector<Operand> &getOps(const MInstr &instr);
///
///     // Return true when the operand is a physical register.
///     static bool isPhysReg(const Operand &op) noexcept;
///
///     // Return true when two operands refer to the same physical register.
///     static bool samePhysReg(const Operand &a, const Operand &b) noexcept;
///
///     // Return a unique key for the physical register in the operand.
///     static RegKey regKey(const Operand &op) noexcept;
///
///     // Return true when the operand is an ABI register (argument/return)
///     // that should not be propagated through.
///     static bool isABIReg(const Operand &op) noexcept;
///
///     // Classify the operand at index idx as (isUse, isDef).
///     static std::pair<bool, bool> classifyOperand(const MInstr &instr,
///                                                   std::size_t idx) noexcept;
///
///     // Identify GPR move instructions: return true and set src/dst indices
///     // for register-to-register GPR copies (e.g., MOVrr / MovRR).
///     // The caller checks isPhysReg() on both operands.
///     static bool isGPRMove(const MInstr &instr) noexcept;
///
///     // Identify FPR move instructions: return true for same-class FPR
///     // register-to-register copies (e.g., MOVSDrr / FMovRR).
///     static bool isFPRMove(const MInstr &instr) noexcept;
///
///     // Return true for instructions that should be skipped entirely
///     // (branches, returns) and optionally clear the copy map on calls.
///     // Returns a pair: (shouldSkip, shouldClearMap).
///     static std::pair<bool, bool> shouldSkipOrClear(const MInstr &instr) noexcept;
/// };
/// @endcode
///
/// @tparam Traits Target-specific type providing the required interface.
/// @param instrs  Mutable instruction vector to propagate copies through.
/// @return Number of operand replacements performed.
template <typename Traits>
std::size_t propagateCopies(std::vector<typename Traits::MInstr> &instrs) {
    using MInstr = typename Traits::MInstr;
    using Operand = typename Traits::Operand;
    using RegKey = typename Traits::RegKey;

    std::unordered_map<RegKey, Operand> copyOrigin;
    std::size_t propagated = 0;

    /// Invalidate all tracked copies whose recorded origin matches @p originKey.
    auto invalidateDependents = [&copyOrigin](RegKey originKey) {
        std::vector<RegKey> toErase;
        for (const auto &[key, origin] : copyOrigin) {
            if (Traits::regKey(origin) == originKey)
                toErase.push_back(key);
        }
        for (RegKey key : toErase)
            copyOrigin.erase(key);
    };

    /// Process a well-formed register copy, collapse its origin chain, and
    /// record the destination mapping.
    /// @return `true` when @p instr has two physical-register operands.
    auto handleMove = [&](MInstr &instr) -> bool {
        auto &ops = Traits::getOps(instr);
        if (ops.size() != 2)
            return false;
        if (!Traits::isPhysReg(ops[0]) || !Traits::isPhysReg(ops[1]))
            return false;

        const Operand &dst = ops[0];
        const Operand &src = ops[1];
        RegKey dstKey = Traits::regKey(dst);

        // Invalidate copies depending on the redefined destination.
        invalidateDependents(dstKey);
        copyOrigin.erase(dstKey);

        // Follow the copy chain for the source, unless it is an ABI register.
        RegKey srcKey = Traits::regKey(src);
        Operand origin = src;
        if (!Traits::isABIReg(src)) {
            auto it = copyOrigin.find(srcKey);
            if (it != copyOrigin.end())
                origin = it->second;
        }

        // Record the copy relationship if dst differs from origin.
        if (!Traits::samePhysReg(dst, origin)) {
            copyOrigin[dstKey] = origin;

            // Shorten the move to point directly at the origin.
            if (!Traits::samePhysReg(src, origin)) {
                ops[1] = origin;
                ++propagated;
            }
        }
        return true;
    };

    for (auto &instr : instrs) {
        // Skip or clear on branches/calls.
        auto [shouldSkip, shouldClear] = Traits::shouldSkipOrClear(instr);
        if (shouldClear)
            copyOrigin.clear();
        if (shouldSkip)
            continue;

        // Handle GPR moves.
        if (Traits::isGPRMove(instr)) {
            if (handleMove(instr))
                continue;
        }

        // Handle FPR moves.
        if (Traits::isFPRMove(instr)) {
            if (handleMove(instr))
                continue;
        }

        // For all other instructions: propagate copies in uses first,
        // then invalidate definitions.
        auto &ops = Traits::getOps(instr);

        // First pass: replace uses with their origin.
        for (std::size_t i = 0; i < ops.size(); ++i) {
            auto &op = ops[i];
            if (!Traits::isPhysReg(op))
                continue;

            auto [isUse, isDef] = Traits::classifyOperand(instr, i);
            if (isUse && !isDef && !Traits::isABIReg(op)) {
                RegKey key = Traits::regKey(op);
                auto it = copyOrigin.find(key);
                if (it != copyOrigin.end() && !Traits::samePhysReg(op, it->second)) {
                    op = it->second;
                    ++propagated;
                }
            }
        }

        // Second pass: invalidate definitions.
        for (std::size_t i = 0; i < ops.size(); ++i) {
            const auto &op = ops[i];
            if (!Traits::isPhysReg(op))
                continue;

            auto [isUse, isDef] = Traits::classifyOperand(instr, i);
            if (isDef) {
                RegKey key = Traits::regKey(op);
                invalidateDependents(key);
                copyOrigin.erase(key);
            }
        }
    }

    return propagated;
}

} // namespace zanna::codegen::common
