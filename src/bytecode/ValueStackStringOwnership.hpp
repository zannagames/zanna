//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/bytecode/ValueStackStringOwnership.hpp
// Purpose: Encapsulate the bytecode VM value-stack string ownership bitmap.
// Key invariants:
//   - Each entry corresponds one-to-one with a BytecodeVM value-stack slot.
//   - Stored bytes are canonical Boolean values (zero or one).
// Ownership/Lifetime:
//   - The tracker owns its bitmap and never owns or releases runtime strings.
//   - Snapshots are returned by value and remain independent of later mutations.
// Links: BytecodeVM.hpp, BytecodeVM.cpp
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Defines the compact side table used to track bytecode stack-string
 *        ownership.
 *
 * Bytecode slots are intentionally untagged, so reference ownership cannot be
 * encoded in `BCSlot` itself. This helper centralizes the parallel bitmap used
 * during normal stack mutation and trap/resume snapshots. Bounds assumptions
 * are programmer invariants enforced with assertions in debug builds.
 */

#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace zanna {
namespace bytecode {

/// @brief Tracks which bytecode value-stack slots own runtime string handles.
/// @details `BCSlot` intentionally remains an 8-byte untagged union for bytecode
///          ABI stability, so ownership metadata must live beside the value
///          stack. This class gives that parallel metadata one narrow API:
///          callers ask about slots by value-stack index, clear ranges by
///          intent, and snapshot/restore only through named operations used by
///          exception handling. Keeping direct bitmap access out of dispatch
///          code reduces the chance of stale ownership flags causing leaks or
///          double releases.
/// @invariant Every stored byte is either zero or one.
/// @note This type tracks responsibility only; it never touches string handles.
class ValueStackStringOwnership {
  public:
    /// @brief Replace the ownership map with @p count slots initialized to @p owned.
    /// @param count Number of value-stack slots represented by the map.
    /// @param owned Initial ownership state stored in every slot.
    void assign(size_t count, bool owned) {
        owned_.assign(count, owned ? 1 : 0);
    }

    /// @brief Clear all tracked slots and release backing storage.
    /// @post The represented slot count is zero.
    void clear() {
        owned_.clear();
    }

    /// @brief Set every tracked slot to non-owning.
    /// @details Used when the VM resets or restores the value stack wholesale.
    /// @post The represented slot count is unchanged.
    void clearAll() {
        std::fill(owned_.begin(), owned_.end(), 0);
    }

    /// @brief Return whether a value-stack index owns a runtime string handle.
    /// @param index Index into the VM value stack.
    /// @return True when the slot owns a reference that must be released.
    [[nodiscard]] bool owns(size_t index) const {
        assert(index < owned_.size());
        return owned_[index] != 0;
    }

    /// @brief Update one value-stack index's string ownership state.
    /// @param index Index into the VM value stack.
    /// @param owned New ownership state for the slot.
    void set(size_t index, bool owned) {
        assert(index < owned_.size());
        owned_[index] = owned ? 1 : 0;
    }

    /// @brief Mark one value-stack index as non-owning.
    /// @param index Index into the VM value stack.
    void clearSlot(size_t index) {
        set(index, false);
    }

    /// @brief Capture an ownership prefix for trap/resume restoration.
    /// @param count Number of leading value-stack slots to copy.
    /// @return A compact copy of the first @p count ownership flags.
    [[nodiscard]] std::vector<uint8_t> snapshotPrefix(size_t count) const {
        assert(count <= owned_.size());
        return std::vector<uint8_t>(owned_.begin(), owned_.begin() + count);
    }

    /// @brief Restore a previously captured ownership prefix.
    /// @param flags Captured ownership flags.
    /// @param count Number of flags to restore from @p flags.
    /// @pre @p count is no larger than both @p flags and this map.
    /// @post Entries at indices greater than or equal to @p count are unchanged.
    void restorePrefix(const std::vector<uint8_t> &flags, size_t count) {
        assert(count <= flags.size());
        assert(count <= owned_.size());
        std::copy(flags.begin(), flags.begin() + count, owned_.begin());
    }

  private:
    std::vector<uint8_t> owned_; ///< Per-value-stack-slot ownership flags.
};

} // namespace bytecode
} // namespace zanna
