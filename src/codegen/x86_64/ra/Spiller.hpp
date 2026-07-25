//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/ra/Spiller.hpp
// Purpose: Declare helper utilities responsible for spill slot management and
//          load/store emission during linear-scan register allocation.
// Key invariants:
//   - New-slot counters grow monotonically per register class; reusable plans
//     may be assigned an earlier index.
//   - Helper routines return instructions for insertion; they do not mutate MIR directly.
//   - Slots with non-overlapping lifetimes can be reused to reduce stack frame size.
// Ownership/Lifetime:
//   - Spiller instances own their slot counters; callers manage produced instruction sequences.
// Links: src/codegen/x86_64/ra/Spiller.cpp,
//        src/codegen/x86_64/ra/Allocator.hpp,
//        src/codegen/x86_64/TargetX64.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file Spiller.hpp
 * @brief Declares x86-64 spill-slot bookkeeping and spill instruction creation.
 *
 * Spiller assigns independent GPR and XMM placeholder slots, optionally reuses
 * slots whose recorded lifetimes do not overlap, and creates the MIR loads and
 * stores needed when the allocator evicts a virtual register.
 */

#pragma once

#include "../MachineIR.hpp"
#include "../TargetX64.hpp"

#include <cstddef>
#include <deque>
#include <unordered_map>
#include <vector>

namespace zanna::codegen::x64 {
struct AllocationResult;
} // namespace zanna::codegen::x64

namespace zanna::codegen::x64::ra {

/// Per-virtual-register allocator state, defined in Allocator.hpp.
struct VirtualAllocation; // Forward declaration defined in Allocator.hpp.

/// @brief Describes the spill status of a virtual register.
struct SpillPlan {
    ///< Whether the value has a memory home that must preserve its contents.
    bool needsSpill{false}; ///< True when the value must reside in memory.
    ///< Zero-based per-register-class slot, or -1 before assignment.
    int slot{-1};           ///< Assigned spill slot index; -1 until materialised.
};

/// @brief Describes the lifetime of a spill slot for reuse analysis.
/// @details The half-open interval `[start, end)` records the most recent
///          assignment. A cleared @c inUse flag makes the slot immediately
///          reusable regardless of those endpoints.
struct SlotLifetime {
    ///< First linear instruction index covered by the current assignment.
    std::size_t start{0}; ///< First instruction index using this slot.
    ///< Exclusive end of the current assignment.
    std::size_t end{0};   ///< Last instruction index using this slot (exclusive).
    ///< Whether the slot is currently assigned to a value considered live.
    bool inUse{false};    ///< True if the slot is currently assigned to a live value.
};

/// @brief Helper component managing spill slot allocation and load/store codegen.
/// @details Implements spill slot reuse by tracking the lifetime of each slot
///          and assigning slots with non-overlapping lifetimes to different
///          values. The component owns only bookkeeping; instructions returned
///          or appended by its methods remain owned by caller-provided buffers.
class Spiller {
  public:
    /// @brief Construct a spiller with no allocated GPR or XMM slots.
    Spiller() = default;

    /// @brief Ensure the spill plan owns a stack slot for @p cls.
    /// @details Existing assignments are left unchanged. A new assignment
    ///          consumes the next class-specific slot and records an unbounded
    ///          lifetime so reuse-aware allocation cannot reclaim it.
    /// @param cls Register class selecting the GPR or XMM slot namespace.
    /// @param plan Plan to update when it has no slot.
    /// @post @p plan has a non-negative slot and needsSpill is true when a new
    ///       slot was assigned. An already assigned plan is not modified.
    void ensureSpillSlot(RegClass cls, SpillPlan &plan);

    /// @brief Assign a spill slot with lifetime-based reuse analysis.
    /// @details Reuses the first inactive slot or the first slot whose recorded
    ///          end is no later than @p start. If no such slot exists, appends a
    ///          new class-specific slot. An already assigned plan is unchanged.
    /// @param cls Register class for the spill slot.
    /// @param plan Spill plan to receive the slot assignment.
    /// @param start Start of the live interval (instruction index).
    /// @param end End of the live interval (instruction index, exclusive).
    /// @post A newly assigned plan has needsSpill set and its slot lifetime is
    ///       recorded as `[start, end)`.
    void ensureSpillSlotWithReuse(RegClass cls,
                                  SpillPlan &plan,
                                  std::size_t start,
                                  std::size_t end);

    /// @brief Emit a load from @p plan into @p dst.
    /// @param cls Register class selecting MOVmr for GPR or MOVSDmr for XMM.
    /// @param dst Physical register that receives the reloaded value.
    /// @param plan Assigned spill plan naming the source slot.
    /// @return A standalone MIR load instruction.
    /// @pre `plan.slot >= 0`.
    /// @throws std::out_of_range if the slot is negative.
    /// @throws std::overflow_error if its placeholder index or displacement
    ///         exceeds the reserved representable range.
    [[nodiscard]] MInstr makeLoad(RegClass cls, PhysReg dst, const SpillPlan &plan) const;

    /// @brief Emit a store from @p src into @p plan.
    /// @param cls Register class selecting MOVrm for GPR or MOVSDrm for XMM.
    /// @param plan Assigned spill plan naming the destination slot.
    /// @param src Physical register whose value is stored.
    /// @return A standalone MIR store instruction.
    /// @pre `plan.slot >= 0`.
    /// @throws std::out_of_range if the slot is negative.
    /// @throws std::overflow_error if its placeholder index or displacement
    ///         exceeds the reserved representable range.
    [[nodiscard]] MInstr makeStore(RegClass cls, const SpillPlan &plan, PhysReg src) const;

    /// @brief Spill the active value associated with @p vreg.
    /// @details Ensures a permanent slot, appends a store to @p prefix, returns
    ///          the occupied physical register to @p pool, and clears the
    ///          allocation result's current virtual-to-physical mapping.
    /// @param cls Register class of the value and register pool.
    /// @param vreg Virtual-register identifier being evicted.
    /// @param alloc Allocation state whose physical residency is cleared.
    /// @param pool Free-register pool receiving `alloc.phys`.
    /// @param prefix Instruction sequence receiving the spill store.
    /// @param result Result whose mapping for @p vreg is erased.
    /// @pre @p alloc denotes a value currently resident in `alloc.phys`.
    /// @post `alloc.hasPhys` and `alloc.cachedInBlock` are false,
    ///       `alloc.spill.needsSpill` is true, and the physical register has
    ///       been appended to @p pool.
    void spillValue(RegClass cls,
                    uint16_t vreg,
                    VirtualAllocation &alloc,
                    std::deque<PhysReg> &pool,
                    std::vector<MInstr> &prefix,
                    AllocationResult &result);

    /// @brief Spill the active value with lifetime-based slot reuse.
    /// @details Behaves like spillValue(), but assigns a reusable slot whose
    ///          lifetime is recorded as `[intervalStart, intervalEnd)`.
    /// @param cls Register class of the spilled value.
    /// @param vreg Virtual register identifier being spilled.
    /// @param alloc Allocation record tracking the virtual register state.
    /// @param pool Register pool receiving the freed physical register.
    /// @param prefix Instruction buffer that receives the spill store.
    /// @param result Global allocation result map updated to forget @p vreg.
    /// @param intervalStart Start of the value's live interval.
    /// @param intervalEnd End of the value's live interval.
    /// @pre @p alloc denotes a value currently resident in `alloc.phys`.
    /// @post The value is stored, its physical register is returned to @p pool,
    ///       and the current mapping for @p vreg is erased from @p result.
    void spillValueWithReuse(RegClass cls,
                             uint16_t vreg,
                             VirtualAllocation &alloc,
                             std::deque<PhysReg> &pool,
                             std::vector<MInstr> &prefix,
                             AllocationResult &result,
                             std::size_t intervalStart,
                             std::size_t intervalEnd);

    /// @brief Return the number of distinct GPR spill slots allocated so far.
    /// @return High-water count used to size the GPR spill area.
    [[nodiscard]] int gprSlots() const noexcept {
        return nextSpillSlotGPR_;
    }

    /// @brief Return the number of distinct XMM spill slots allocated so far.
    /// @return High-water count used to size the XMM spill area.
    [[nodiscard]] int xmmSlots() const noexcept {
        return nextSpillSlotXMM_;
    }

    /// @brief Mark a spill slot as no longer in use (for lifetime tracking).
    /// @details A negative or out-of-range slot is ignored. Releasing a valid
    ///          slot preserves its recorded interval but clears its in-use flag.
    /// @param cls Register class of the slot.
    /// @param slot Slot index to release.
    /// @post A valid selected lifetime entry has `inUse == false`.
    void releaseSlot(RegClass cls, int slot);

  private:
    ///< Next unused GPR slot index and GPR-area high-water count.
    int nextSpillSlotGPR_{0};
    ///< Next unused XMM slot index and XMM-area high-water count.
    int nextSpillSlotXMM_{0};

    /// Lifetime metadata indexed by GPR spill-slot number.
    std::vector<SlotLifetime> gprSlotLifetimes_{};

    /// Lifetime metadata indexed by XMM spill-slot number.
    std::vector<SlotLifetime> xmmSlotLifetimes_{};

    /// @brief Find a reusable slot with non-overlapping lifetime.
    /// @details Selects the first entry that is inactive or whose end is no
    ///          later than @p start. This query does not modify the entry.
    /// @param lifetimes Vector of slot lifetimes to search.
    /// @param start Start of the new value's live interval.
    /// @param end End of the new value's live interval; accepted for the full
    ///            interval-query interface but not needed by the current test.
    /// @return Index of a reusable slot, or -1 if none found.
    [[nodiscard]] int findReusableSlot(std::vector<SlotLifetime> &lifetimes,
                                       std::size_t start,
                                       std::size_t end) const;

    /// @brief Encode a class-specific spill slot as an RBP-relative placeholder.
    /// @param cls Register class selecting the reserved placeholder range.
    /// @param slot Zero-based slot index within that class.
    /// @return Memory operand whose negative displacement is later rewritten
    ///         by frame lowering to the final stack location.
    /// @throws std::out_of_range if @p slot is negative.
    /// @throws std::overflow_error if the slot exceeds its class's placeholder
    ///         range or the encoded displacement cannot fit in int32.
    [[nodiscard]] Operand makeFrameOperand(RegClass cls, int slot) const;
};

} // namespace zanna::codegen::x64::ra
