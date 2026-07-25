//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/utils/UseDefInfo.hpp
// Purpose: Provide use-def chain tracking for efficient SSA value replacement.
// Key invariants: Use lists are invalidated when instructions are added/removed.
// Ownership/Lifetime: UseDefInfo does not own the function; caller must ensure
//                     the function outlives the UseDefInfo instance.
// Links: docs/dev/analysis.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares temporary use tracking and whole-function value rewriting.
/// @details `UseDefInfo` indexes temporary-valued instruction operands and
///          branch arguments. It offers a rescan-based replacement operation
///          for generally mutable IL and a cached-site operation for callers
///          that can guarantee instruction storage remains stable.

#pragma once

#include "il/core/Value.hpp"

#include <unordered_map>
#include <vector>

namespace il::core {
struct Function;
struct BasicBlock;
struct Instr;
} // namespace il::core

namespace zanna::il {

/// @brief Tracks temporary use counts and provides safe value replacement.
/// @details The original implementation cached raw operand pointers so
///          replacements could touch only direct use sites. That strategy is
///          not safe for Zanna IL because most optimization passes mutate
///          `BasicBlock::instructions` vectors in place, invalidating cached
///          addresses after insert/erase operations. The current implementation
///          keeps a pointer to the owning function, maintains best-effort use
///          counts, and performs replacement via a fresh function scan so the
///          API remains safe under normal optimizer mutation patterns.
///
/// Usage:
/// @code
///   UseDefInfo info(F);  // Build initial use counts for function F
///   info.replaceAllUses(tempId, newValue);  // safe on mutable IL
/// @endcode
///
/// @warning The use-def info becomes stale if instructions are added, removed,
///          or have their operands modified through other means. The
///          replacement API rescans the function and rebuilds counts, but
///          `hasUses()` / `useCount()` only reflect the latest state observed
///          by this object.
class UseDefInfo {
  public:
    /// @brief Construct initial use counts for all temporaries in function @p F.
    /// @param F Function to analyze.
    explicit UseDefInfo(::il::core::Function &F);

    /// @brief Replace all uses of temporary @p tempId with @p replacement.
    /// @details Performs a safe full-function rewrite and then rebuilds the
    ///          cached use counts from the mutated function.
    /// @param tempId Temporary identifier to replace.
    /// @param replacement New value to substitute.
    /// @return Number of uses replaced.
    std::size_t replaceAllUses(unsigned tempId, const ::il::core::Value &replacement);

    /// @brief Replace uses through cached operand pointers while instruction
    ///        storage is guaranteed not to move.
    /// @details This is the linear-time bulk-rewrite path for transforms that
    ///          defer every instruction insertion and erasure until all value
    ///          replacements are complete. Calling it after storage mutation is
    ///          invalid; general mutable passes must use replaceAllUses().
    /// @param tempId Temporary identifier whose recorded uses are rewritten.
    /// @param replacement Value assigned to each cached operand location.
    /// @return Number of cached use sites rewritten, or zero when none exist.
    /// @warning Every pointer cached by the most recent build or replacement
    ///          must still designate the same live operand.
    std::size_t replaceAllUsesStableStorage(unsigned tempId,
                                            const ::il::core::Value &replacement);

    /// @brief Check if a temporary has any uses.
    /// @param tempId Temporary identifier to check.
    /// @return True if the temporary has at least one use.
    [[nodiscard]] bool hasUses(unsigned tempId) const;

    /// @brief Count the number of uses of a temporary.
    /// @param tempId Temporary identifier to count.
    /// @return Number of uses (0 if not found).
    [[nodiscard]] std::size_t useCount(unsigned tempId) const;

  private:
    /// @brief Owning function whose operands are queried or rewritten.
    ::il::core::Function *function_{nullptr};

    /// @brief Map from temporary ID to observed use count.
    std::unordered_map<unsigned, std::size_t> useCounts_;

    /// @brief Direct operand locations, valid until instruction storage moves.
    std::unordered_map<unsigned, std::vector<::il::core::Value *>> useSites_;

    /// @brief Replace the indexed function and rebuild all cached use data.
    /// @details Clears both caches before scanning ordinary operands and branch
    ///          arguments in every instruction.
    /// @param F Function whose use information becomes authoritative.
    void build(::il::core::Function &F);

    /// @brief Record a use if the value is a temporary.
    /// @details Non-temporary values are ignored. Temporary values increment
    ///          their count and contribute their address to the stable-storage
    ///          cache.
    /// @param v Mutable operand or branch argument encountered during a scan.
    void recordUse(::il::core::Value &v);
};

} // namespace zanna::il
