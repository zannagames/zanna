//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/FrameBuilder.hpp
// Purpose: AArch64 stack frame layout builder for codegen.
//          Manages incremental allocation of local variable slots, register
//          spill slots, and outgoing argument areas per AAPCS64 convention.
//          All slots are addressed as negative offsets from x29 (frame pointer).
// Key invariants:
//   - All offsets are negative (below the frame pointer).
//   - Stack slots are 8-byte aligned minimum; 16-byte overall frame alignment.
//   - addLocal() and ensureSpill() must be called before finalize().
//   - finalize() is idempotent and must run after all frame requirements are known.
// Ownership/Lifetime:
//   - FrameBuilder borrows the MFunction reference; MFunction must outlive it.
// Links: src/codegen/aarch64/FrameBuilder.cpp,
//        src/codegen/aarch64/MachineIR.hpp,
//        src/codegen/common/FrameLayout.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Declares incremental AAPCS64 frame-slot allocation and spill reuse.
 *
 * The builder resumes from any layout already recorded in its borrowed machine
 * function. Locals and spills grow downward from `x29`; outgoing arguments and
 * final stack alignment are folded into the frame size during @ref finalize.
 */

#pragma once

#include "MachineIR.hpp"
#include "TargetAArch64.hpp"
#include "codegen/common/FrameLayout.hpp"
#include "codegen/common/FrameLayoutUtils.hpp"

#include <optional>

namespace zanna::codegen::aarch64 {

/// @brief Centralizes AArch64 stack frame layout construction for codegen.
/// @details Manages incremental allocation of local variable slots, register spill
///          slots, and outgoing argument areas following AAPCS64 conventions. All
///          slots use negative offsets from the frame pointer (x29). The builder
///          tracks the next available offset and ensures proper alignment.
///
///          Usage flow:
///          1. During MIR lowering: addLocal() for each alloca instruction
///          2. During register allocation: ensureSpill() for each spilled vreg
///          3. After call lowering: setMaxOutgoingBytes() for stack arguments
///          4. Before prologue emission: finalize() to compute total frame size
///
/// @invariant All offsets are negative (below the frame pointer).
/// @invariant Stack slots are 8-byte aligned minimum; 16-byte alignment is
///            maintained for the overall frame.
class FrameBuilder : public common::FrameLayout {
  public:
    /// @brief Construct a frame builder bound to @p fn and seed its cursor.
    /// @details Replays any locals or spills already present in @p fn.frame
    ///          into the slot cursor so a second builder created after
    ///          register allocation continues allocation past existing slots
    ///          instead of overwriting them.
    /// @param fn Borrowed machine function that receives all layout records.
    explicit FrameBuilder(MFunction &fn) noexcept : fn_(&fn), slotCursor_(kSlotSizeBytes) {
        // Resume allocation after any locals/spills assigned by an earlier
        // builder instance (for example, when regalloc creates new spill slots).
        for (const auto &L : fn.frame.locals)
            slotCursor_.seedFromOffset(L.offset);
        for (const auto &S : fn.frame.spills)
            slotCursor_.seedFromOffset(S.offset);
    }

    /// @brief Declare a local stack slot by IL temp id.
    /// @param tempId The IL temporary identifier.
    /// @param sizeBytes Size of the slot in bytes.
    /// @param alignBytes Alignment requirement (default: 8 bytes for 64-bit values).
    void addLocal(unsigned tempId, int sizeBytes, int alignBytes = kSlotSizeBytes) override;

    /// @brief Returns the assigned FP-relative offset for a local variable.
    /// @param tempId The IL temporary identifier.
    /// @return Negative offset from the frame pointer.
    int localOffset(unsigned tempId) const override;

    /// @brief Look up the assigned FP-relative offset for a local variable.
    /// @details Unlike localOffset(), this method does not rely on 0 as a
    ///          missing-value sentinel. It lets lowering code distinguish a
    ///          present stack slot from an absent local even if frame layout rules
    ///          change in the future.
    /// @param tempId The IL temporary identifier to search for.
    /// @return The local's signed FP-relative offset, or std::nullopt when no
    ///         slot has been allocated for @p tempId.
    [[nodiscard]] std::optional<int> tryLocalOffset(unsigned tempId) const;

    /// @brief Ensure a spill slot exists for a virtual register.
    /// @param vreg Virtual register identifier.
    /// @param sizeBytes Size of the spill slot (default: 8 bytes).
    /// @param alignBytes Alignment requirement (default: 8 bytes).
    /// @return FP-relative offset of the spill slot.
    int ensureSpill(uint32_t vreg,
                    int sizeBytes = kSlotSizeBytes,
                    int alignBytes = kSlotSizeBytes) override;

    /// @brief Ensure a spill slot for @p vreg, reusing a dead slot if available.
    ///
    /// @details A slot is dead when its previous occupant's last use occurred
    ///          before @p currentInstrIdx.  If such a slot exists and has a
    ///          compatible size, it is recycled for @p vreg without growing the
    ///          frame.  Otherwise a fresh slot is allocated as normal.
    ///
    /// @param vreg           Virtual register to assign a slot to.
    /// @param lastUseInstrIdx  Last instruction index that reads @p vreg
    ///                         (used to record this slot's new lifetime end).
    /// @param currentInstrIdx  Instruction index at the point of spill
    ///                         (slots with lastUse < this value are dead).
    /// @param sizeBytes      Slot size in bytes (default: 8).
    /// @param alignBytes     Alignment in bytes (default: 8).
    /// @return FP-relative offset of the (possibly reused) spill slot.
    int ensureSpillWithReuse(uint32_t vreg,
                             unsigned lastUseInstrIdx,
                             unsigned currentInstrIdx,
                             int sizeBytes = kSlotSizeBytes,
                             int alignBytes = kSlotSizeBytes);

    /// @brief Reserve space for outgoing arguments passed on the stack.
    /// @param bytes Maximum bytes needed for outgoing arguments.
    void setMaxOutgoingBytes(int bytes);

    /// @brief FrameLayout interface: reserve outgoing argument space.
    /// @param bytes Maximum required outgoing byte count.
    void setMaxOutgoing(int bytes) override {
        setMaxOutgoingBytes(bytes);
    }

    /// @brief Finalize frame layout and compute total frame size.
    ///
    /// @details May be called repeatedly after all locals and spills are
    ///          declared; recomputes the same aligned total from recorded state.
    void finalize() override;

    /// @brief FrameLayout interface: get the total frame size after finalize().
    /// @return Aligned local frame size, or zero without a bound function.
    int totalBytes() const override {
        return fn_ ? fn_->localFrameSize : 0;
    }

    /// @brief Notify the frame builder that a new basic block is starting.
    ///
    /// Increments the block epoch so that spill slots from previous blocks are
    /// never reused in the current block.  Must be called before processing
    /// each basic block during register allocation.
    /// @post Spill lifetimes from earlier epochs cannot be reused in the new block.
    void beginNewBlock() noexcept {
        ++blockEpoch_;
    }

  private:
    /// @brief Lifetime record for a single spill slot.
    ///
    /// Tracks the FP-relative offset, size/alignment compatibility, the
    /// instruction index of the last use of the most-recent vreg assigned to
    /// this slot, and the block epoch in which that last use occurred. Slots
    /// are only eligible for reuse within the SAME block epoch because
    /// @p currentInstrIdx is a per-block counter that resets to 0 at each block
    /// boundary, making cross-epoch comparisons meaningless.
    struct SlotLifetime {
        uint32_t vreg;       ///< Most-recent vreg assigned to this slot.
        int offset;          ///< FP-relative offset (always negative).
        int sizeBytes;       ///< Slot size (for size-compatible reuse check).
        int alignBytes;      ///< Guaranteed alignment of the slot.
        unsigned lastUseIdx; ///< Last instruction index reading the current vreg.
        uint32_t epoch;      ///< Block epoch when lastUseIdx was recorded.
    };

    MFunction *fn_{};
    common::DownwardFrameCursor slotCursor_{kSlotSizeBytes};
    uint32_t blockEpoch_{0}; ///< Monotonically-increasing block counter.

    /// Lifetime records for every slot allocated via ensureSpillWithReuse().
    std::vector<SlotLifetime> slotLifetimes_;

    /// @brief Find the most-recently-allocated spill slot assigned to @p vreg, or nullptr.
    /// @param vreg Virtual-register identifier.
    /// @return Mutable borrowed slot record, or null.
    [[nodiscard]] MFunction::SpillSlot *findLatestSpillSlot(uint32_t vreg) noexcept;
    /// @brief Const overload of findLatestSpillSlot().
    /// @param vreg Virtual-register identifier.
    /// @return Borrowed slot record, or null.
    [[nodiscard]] const MFunction::SpillSlot *findLatestSpillSlot(uint32_t vreg) const noexcept;
    /// @brief Find the SlotLifetime record for the slot at FP-relative @p offset, or nullptr.
    /// @param offset Negative FP-relative spill-slot offset.
    /// @return Mutable lifetime record, or null.
    [[nodiscard]] SlotLifetime *findSlotLifetime(int offset) noexcept;
    /// @brief Const overload of findSlotLifetime().
    /// @param offset Negative FP-relative spill-slot offset.
    /// @return Borrowed lifetime record, or null.
    [[nodiscard]] const SlotLifetime *findSlotLifetime(int offset) const noexcept;
    /// @brief Advance slotCursor_ by @p sizeBytes with @p alignBytes alignment; return the offset.
    /// @param sizeBytes Positive allocation size.
    /// @param alignBytes Positive power-of-two alignment.
    /// @return Newly assigned negative FP-relative offset.
    int assignAlignedSlot(int sizeBytes, int alignBytes);
};

} // namespace zanna::codegen::aarch64
