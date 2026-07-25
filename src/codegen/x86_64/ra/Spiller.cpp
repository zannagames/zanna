//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/ra/Spiller.cpp
// Purpose: Implement spill slot orchestration for the linear-scan allocator.
//          Provides helpers for reserving stack slots and emitting loads/stores
//          around Machine IR instructions when register pressure overflows.
// Key invariants:
//   - Spill slots are allocated in 8-byte increments relative to %rbp.
//   - Spill stores always precede instruction execution; loads precede uses.
// Ownership/Lifetime:
//   - Mutates AllocationResult to reflect spilled registers; does not own MIR.
// Links: src/codegen/x86_64/ra/Spiller.hpp,
//        src/codegen/x86_64/ra/Allocator.hpp,
//        src/codegen/x86_64/RegAllocLinear.hpp
//
//===----------------------------------------------------------------------===//

#include "Spiller.hpp"

#include "../RegAllocLinear.hpp"
#include "Allocator.hpp"

#include <deque>
#include <limits>
#include <stdexcept>

/// @file
/// @brief Provides stack spill management utilities for linear-scan allocation.
/// @details The spiller assigns stack slots lazily, emits loads and stores as
///          register pressure demands, and exposes helpers the coalescer and
///          allocator reuse when materialising PX_COPY bundles or evicting live
///          ranges.

namespace zanna::codegen::x64::ra {

namespace {

/// @brief Wrap a physical register in a Machine IR operand.
/// @details The helper mirrors the allocator's operand construction routine so
///          spiller-emitted loads and stores use the same operand encoding as
///          other MIR builders.  The register is cast to the underlying
///          `uint16_t` identifier required by the Machine IR type system.
/// @param cls Register class describing the operand.
/// @param reg Physical register referenced by the operand.
/// @return Machine operand representing @p reg.
[[nodiscard]] Operand makePhysOperand(RegClass cls, PhysReg reg) {
    return makePhysRegOperand(cls, static_cast<uint16_t>(reg));
}

} // namespace

/// @brief Find a reusable slot with non-overlapping lifetime.
/// @details Scans the slot lifetime vector for a slot that is either not in use
///          or has a lifetime that ends before the new value's lifetime starts.
///          This enables aggressive slot reuse, reducing stack frame size.
/// @param lifetimes Vector of slot lifetimes to search.
/// @param start Start of the new value's live interval.
/// @param end Exclusive end of the new interval; currently unnecessary for the
///            one-sided non-overlap test.
/// @return Index of a reusable slot, or -1 if none found.
/// @note The search does not reserve or update the returned slot.
int Spiller::findReusableSlot(std::vector<SlotLifetime> &lifetimes,
                              std::size_t start,
                              std::size_t end) const {
    for (std::size_t i = 0; i < lifetimes.size(); ++i) {
        auto &slot = lifetimes[i];
        // Slot can be reused if:
        // 1. It's not currently in use, OR
        // 2. Its lifetime ended before our lifetime starts (non-overlapping)
        if (!slot.inUse || slot.end <= start) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

/// @brief Lazily assign a stack slot to a spill plan.
/// @details Spill plans capture whether a value must live in memory.  When a
///          plan is first encountered, this function allocates the next free
///          slot for the register class by bumping a class-specific counter.
///          Subsequent calls notice that @ref SpillPlan::slot is non-negative and
///          return early, ensuring each value reuses the same slot.
/// @param cls Register class whose spill storage is being provisioned.
/// @param plan Spill descriptor that records slot ownership.
/// @post A previously unassigned plan owns a permanent class-specific slot and
///       has needsSpill set. An assigned plan is unchanged.
void Spiller::ensureSpillSlot(RegClass cls, SpillPlan &plan) {
    if (plan.slot >= 0) {
        return;
    }
    plan.needsSpill = true;
    if (cls == RegClass::GPR) {
        plan.slot = nextSpillSlotGPR_++;
        // Add a lifetime entry with infinite duration to prevent reuse.
        // This ensures slots allocated without reuse analysis (e.g., for
        // cross-block vregs) are never reclaimed by ensureSpillSlotWithReuse.
        gprSlotLifetimes_.push_back(SlotLifetime{0, std::numeric_limits<std::size_t>::max(), true});
        return;
    }
    plan.slot = nextSpillSlotXMM_++;
    xmmSlotLifetimes_.push_back(SlotLifetime{0, std::numeric_limits<std::size_t>::max(), true});
}

/// @brief Assign a spill slot with lifetime-based reuse analysis.
/// @details Reuses the first inactive slot or one whose previous assignment
///          ends no later than @p start. Otherwise a new slot is appended.
///          Existing assignments in @p plan return without changing the plan
///          or the recorded lifetime.
/// @param cls Register class for the spill slot.
/// @param plan Spill plan to receive the slot assignment.
/// @param start Start of the live interval (instruction index).
/// @param end End of the live interval (instruction index, exclusive).
/// @post A newly assigned slot is marked in use with interval `[start, end)`.
void Spiller::ensureSpillSlotWithReuse(RegClass cls,
                                       SpillPlan &plan,
                                       std::size_t start,
                                       std::size_t end) {
    if (plan.slot >= 0) {
        return;
    }
    plan.needsSpill = true;

    auto &lifetimes = (cls == RegClass::GPR) ? gprSlotLifetimes_ : xmmSlotLifetimes_;
    int &nextSlot = (cls == RegClass::GPR) ? nextSpillSlotGPR_ : nextSpillSlotXMM_;

    // Try to find a reusable slot
    int reusableSlot = findReusableSlot(lifetimes, start, end);
    if (reusableSlot >= 0) {
        // Reuse existing slot, update its lifetime
        plan.slot = reusableSlot;
        lifetimes[static_cast<std::size_t>(reusableSlot)].start = start;
        lifetimes[static_cast<std::size_t>(reusableSlot)].end = end;
        lifetimes[static_cast<std::size_t>(reusableSlot)].inUse = true;
        return;
    }

    // No reusable slot found, allocate a new one
    plan.slot = nextSlot++;
    lifetimes.push_back(SlotLifetime{start, end, true});
}

/// @brief Mark a spill slot as no longer in use.
/// @details Called when a spilled value's live interval ends, allowing the
///          slot to be reused by future spills with non-overlapping lifetimes.
///          Negative and out-of-range indices are harmless no-ops.
/// @param cls Register class of the slot.
/// @param slot Slot index to release.
/// @post A valid selected slot is marked not in use.
void Spiller::releaseSlot(RegClass cls, int slot) {
    if (slot < 0) {
        return;
    }

    auto &lifetimes = (cls == RegClass::GPR) ? gprSlotLifetimes_ : xmmSlotLifetimes_;
    auto idx = static_cast<std::size_t>(slot);
    if (idx < lifetimes.size()) {
        lifetimes[idx].inUse = false;
    }
}

/// @brief Emit a load instruction from a spill slot into a register.
/// @details The opcode chosen depends on the register class: general-purpose
///          registers use @c MOVmr while floating-point registers rely on
///          @c MOVSDmr.  The resulting instruction is ready to be inserted into
///          the MIR stream without additional operands.
/// @param cls Register class to load.
/// @param dst Physical register receiving the value.
/// @param plan Spill plan describing the source slot.
/// @return Machine instruction that reloads the spilled value.
/// @throws std::out_of_range If the plan has a negative slot.
/// @throws std::overflow_error If placeholder encoding exceeds its reserved
///         range or the int32 displacement range.
MInstr Spiller::makeLoad(RegClass cls, PhysReg dst, const SpillPlan &plan) const {
    if (cls == RegClass::GPR) {
        return MInstr::make(MOpcode::MOVmr,
                            {makePhysOperand(cls, dst), makeFrameOperand(cls, plan.slot)});
    }
    return MInstr::make(MOpcode::MOVSDmr,
                        {makePhysOperand(cls, dst), makeFrameOperand(cls, plan.slot)});
}

/// @brief Emit a store instruction from a register into a spill slot.
/// @details Mirroring @ref makeLoad, the helper selects the appropriate opcode
///          for the register class and packages the operands so callers can
///          append the instruction directly to a prefix or suffix list.
/// @param cls Register class owning the value.
/// @param plan Spill plan containing the destination slot.
/// @param src Physical register providing the value to spill.
/// @return Machine instruction that writes the register to the stack slot.
/// @throws std::out_of_range If the plan has a negative slot.
/// @throws std::overflow_error If placeholder encoding exceeds its reserved
///         range or the int32 displacement range.
MInstr Spiller::makeStore(RegClass cls, const SpillPlan &plan, PhysReg src) const {
    if (cls == RegClass::GPR) {
        return MInstr::make(MOpcode::MOVrm,
                            {makeFrameOperand(cls, plan.slot), makePhysOperand(cls, src)});
    }
    return MInstr::make(MOpcode::MOVSDrm,
                        {makeFrameOperand(cls, plan.slot), makePhysOperand(cls, src)});
}

/// @brief Materialise a spill for a live virtual register.
/// @details When the allocator runs out of free registers it calls into the
///          spiller to evict one active allocation.  The routine ensures a spill
///          slot exists, emits a store so the current value is preserved, returns
///          the physical register to the free pool, and updates bookkeeping so
///          future reloads know that the value resides in memory.
/// @param cls Register class of the spilled value.
/// @param vreg Virtual register identifier being spilled.
/// @param alloc Allocation record tracking the virtual register state.
/// @param pool Register pool receiving the freed physical register.
/// @param prefix Instruction buffer that receives the spill store.
/// @param result Global allocation result map updated to forget @p vreg.
/// @pre @p alloc is physically resident and its @c phys field belongs to @p cls.
/// @post The spill store is appended, the physical register is returned to
///       @p pool, and @p alloc is marked nonresident but spill-backed.
void Spiller::spillValue(RegClass cls,
                         uint16_t vreg,
                         VirtualAllocation &alloc,
                         std::deque<PhysReg> &pool,
                         std::vector<MInstr> &prefix,
                         AllocationResult &result) {
    ensureSpillSlot(cls, alloc.spill);
    prefix.push_back(makeStore(cls, alloc.spill, alloc.phys));
    pool.push_back(alloc.phys);
    alloc.hasPhys = false;
    alloc.cachedInBlock = false;
    alloc.spill.needsSpill = true;
    result.vregToPhys.erase(vreg);
}

/// @brief Materialise a spill with lifetime-based slot reuse.
/// @details This is the optimized version that enables spill slot reuse by
///          tracking the live interval of the spilled value. When two values
///          have non-overlapping lifetimes, they can share the same stack slot,
///          significantly reducing stack frame size for functions with high
///          register pressure.
/// @param cls Register class of the spilled value.
/// @param vreg Virtual register identifier being spilled.
/// @param alloc Allocation record tracking the virtual register state.
/// @param pool Register pool receiving the freed physical register.
/// @param prefix Instruction buffer that receives the spill store.
/// @param result Global allocation result map updated to forget @p vreg.
/// @param intervalStart Start of the value's live interval.
/// @param intervalEnd End of the value's live interval.
/// @pre @p alloc is physically resident and its @c phys field belongs to @p cls.
/// @post The spill store is appended, the physical register is returned to
///       @p pool, and @p alloc is marked nonresident but spill-backed.
void Spiller::spillValueWithReuse(RegClass cls,
                                  uint16_t vreg,
                                  VirtualAllocation &alloc,
                                  std::deque<PhysReg> &pool,
                                  std::vector<MInstr> &prefix,
                                  AllocationResult &result,
                                  std::size_t intervalStart,
                                  std::size_t intervalEnd) {
    ensureSpillSlotWithReuse(cls, alloc.spill, intervalStart, intervalEnd);
    prefix.push_back(makeStore(cls, alloc.spill, alloc.phys));
    pool.push_back(alloc.phys);
    alloc.hasPhys = false;
    alloc.cachedInBlock = false;
    alloc.spill.needsSpill = true;
    result.vregToPhys.erase(vreg);
}

/// @brief Create a memory operand referencing a spill slot.
/// @details Spill slots live at negative offsets from @c %rbp in units of eight
///          bytes. Each register class owns a disjoint placeholder index range
///          (kSpillSlotOffsetGPR / kSpillSlotOffsetXMM) so frame lowering can
///          classify slots purely from the displacement — no guessing from
///          neighbouring operands — and alloca placeholders (small indices)
///          can never collide with spill placeholders.
/// @param cls  Register class owning the slot.
/// @param slot Zero-based per-class slot index to reference.
/// @return Memory operand pointing to the spill slot.
/// @throws std::out_of_range If @p slot is negative.
/// @throws std::overflow_error If @p slot exceeds its reserved class range or
///         its byte displacement cannot be represented by int32.
Operand Spiller::makeFrameOperand(RegClass cls, int slot) const {
    if (slot < 0) {
        throw std::out_of_range("x86 spiller: negative spill slot index");
    }
    const int base = cls == RegClass::GPR ? kSpillSlotOffsetGPR : kSpillSlotOffsetXMM;
    const int rangeEnd = cls == RegClass::GPR ? kSpillSlotOffsetXMM : kMaxFramePlaceholderIndex;
    if (slot >= rangeEnd - base - 1) {
        throw std::overflow_error("x86 spiller: spill slot index exceeds its placeholder range");
    }
    const int64_t placeholderSlot = static_cast<int64_t>(slot) + static_cast<int64_t>(base) + 1;
    const int64_t offset64 = -placeholderSlot * static_cast<int64_t>(kSlotSizeBytes);
    if (offset64 < std::numeric_limits<int32_t>::min() ||
        offset64 > std::numeric_limits<int32_t>::max()) {
        throw std::overflow_error("x86 spiller: spill slot displacement overflows int32");
    }
    const auto baseReg = makePhysReg(RegClass::GPR, static_cast<uint16_t>(PhysReg::RBP));
    return makeMemOperand(baseReg, static_cast<int32_t>(offset64));
}

} // namespace zanna::codegen::x64::ra
