//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: codegen/aarch64/FrameBuilder.cpp
// Purpose: AArch64 AAPCS64 stack-frame layout construction.
//          Implements FrameBuilder: assigns FP-relative negative offsets to
//          local variables (allocas), register spill slots, and outgoing
//          argument areas. finalize() rounds the total up to 16-byte alignment
//          and writes MFunction::localFrameSize.
//
// AAPCS64 frame layout (high → low):
//   [FP+8]  saved LR        (paired STP x29,x30)
//   [FP+0]  saved FP
//   [FP-8]  first local/spill slot
//   ...     more locals / spill slots
//   [SP]    base of outgoing arg area
//
// Key invariants:
//   - All slot offsets are negative (below the frame pointer).
//   - Slots are 8-byte aligned minimum; total frame is 16-byte aligned.
//   - addLocal()/ensureSpill() must precede finalize().
//   - finalize() is idempotent but should be called exactly once.
// Ownership/Lifetime:
//   - Borrows MFunction*; the MFunction must outlive the FrameBuilder.
// Links: src/codegen/aarch64/FrameBuilder.hpp,
//        src/codegen/aarch64/RegAllocLinear.cpp (spill usage),
//        src/codegen/aarch64/AsmEmitter.cpp (prologue/epilogue)
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Implements validated AAPCS64 local, spill, and outgoing-area layout.
 *
 * Slot allocation rejects invalid size/alignment inputs and arithmetic
 * overflow. Spill reuse is confined to one basic-block epoch because liveness
 * instruction indices reset at block boundaries. Finalization derives a
 * 16-byte-aligned frame size from both pre-existing and newly allocated slots.
 */

#include "FrameBuilder.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>

namespace zanna::codegen::aarch64 {

namespace {

/// @brief Check whether @p value is a positive power-of-two alignment.
/// @param value Candidate alignment.
/// @return `true` when @p value is positive with exactly one bit set.
[[nodiscard]] bool isPositivePowerOfTwo(int value) noexcept {
    return value > 0 && (value & (value - 1)) == 0;
}

/// @brief Validate a stack object size/alignment pair before frame allocation.
/// @details AArch64 frame slots are addressed through immediate encodings that
///          assume concrete positive sizes and power-of-two alignment. Rejecting
///          bad inputs here keeps frontend or lowering mistakes from becoming
///          silently clamped frame offsets.
/// @param what Object kind included in an exception message.
/// @param sizeBytes Required positive object size.
/// @param alignBytes Required positive power-of-two alignment.
/// @throws std::invalid_argument if the size or alignment is invalid.
void validateStackObjectSpec(const char *what, int sizeBytes, int alignBytes) {
    if (sizeBytes <= 0)
        throw std::invalid_argument(std::string("AArch64 frame ") + what +
                                    " size must be positive");
    if (!isPositivePowerOfTwo(alignBytes))
        throw std::invalid_argument(std::string("AArch64 frame ") + what +
                                    " alignment must be a positive power of two");
}

/// @brief Add two frame byte counts with signed-int overflow checking.
/// @param lhs Existing nonnegative byte count.
/// @param rhs Nonnegative increment.
/// @param what Quantity name included in an exception message.
/// @return Representable sum.
/// @throws std::overflow_error if the sum exceeds int range.
[[nodiscard]] int checkedAddFrameBytes(int lhs, int rhs, const char *what) {
    if (rhs < 0)
        throw std::invalid_argument(std::string("AArch64 frame ") + what +
                                    " byte count must be non-negative");
    if (lhs > std::numeric_limits<int>::max() - rhs)
        throw std::overflow_error(std::string("AArch64 frame ") + what + " exceeds int range");
    return lhs + rhs;
}

/// @brief Return the positive extent represented by a negative FP-relative offset.
/// @param offset Nonpositive FP-relative offset.
/// @param what Object kind included in an exception message.
/// @return Nonnegative distance below the frame pointer.
/// @throws std::overflow_error if negating @p offset would overflow.
[[nodiscard]] int positiveExtentFromOffset(int offset, const char *what) {
    if (offset > 0)
        throw std::invalid_argument(std::string("AArch64 frame ") + what +
                                    " offset must be FP-relative and non-positive");
    if (offset == std::numeric_limits<int>::min())
        throw std::overflow_error(std::string("AArch64 frame ") + what +
                                  " offset cannot be represented as a positive extent");
    return -offset;
}

} // namespace

void FrameBuilder::addLocal(unsigned tempId, int sizeBytes, int alignBytes) {
    validateStackObjectSpec("local", sizeBytes, alignBytes);
    // If already present, ignore.
    for (const auto &L : fn_->frame.locals)
        if (L.tempId == tempId)
            return;
    // Assign an aligned slot and record offset.
    const int off = assignAlignedSlot(sizeBytes, alignBytes);
    fn_->frame.locals.push_back(MFunction::StackLocal{tempId, sizeBytes, alignBytes, off});
}

int FrameBuilder::localOffset(unsigned tempId) const {
    return fn_->frame.getLocalOffset(tempId);
}

std::optional<int> FrameBuilder::tryLocalOffset(unsigned tempId) const {
    for (const auto &local : fn_->frame.locals) {
        if (local.tempId == tempId)
            return local.offset;
    }
    return std::nullopt;
}

int FrameBuilder::ensureSpill(uint32_t vreg, int sizeBytes, int alignBytes) {
    validateStackObjectSpec("spill", sizeBytes, alignBytes);
    if (const auto *slot = findLatestSpillSlot(vreg))
        return slot->offset;
    const int off = assignAlignedSlot(sizeBytes, alignBytes);
    fn_->frame.spills.push_back(MFunction::SpillSlot{vreg, sizeBytes, alignBytes, off});
    return off;
}

int FrameBuilder::ensureSpillWithReuse(uint32_t vreg,
                                       unsigned lastUseInstrIdx,
                                       unsigned currentInstrIdx,
                                       int sizeBytes,
                                       int alignBytes) {
    validateStackObjectSpec("spill", sizeBytes, alignBytes);
    // Fast path: reuse the most-recent slot assignment for this vreg only while its
    // tracked lifetime is still active. Once that lifetime is dead and the slot has
    // potentially been recycled for another vreg in the same block, the caller must
    // re-run slot selection instead of blindly reusing the cached offset.
    if (const auto *slot = findLatestSpillSlot(vreg)) {
        if (auto *lifetime = findSlotLifetime(slot->offset)) {
            const bool sameEpoch = lifetime->epoch == blockEpoch_;
            const bool stillLive = lifetime->lastUseIdx >= currentInstrIdx;
            if (lifetime->vreg == vreg && (!sameEpoch || stillLive)) {
                lifetime->lastUseIdx = std::max(lifetime->lastUseIdx, lastUseInstrIdx);
                return slot->offset;
            }
        } else {
            return slot->offset;
        }
    }

    // Try to reuse a dead slot.  A slot is dead when:
    //   (a) it was recorded in the SAME block epoch (same basic block), AND
    //   (b) its previous occupant's last use index is before the current instruction.
    //
    // Cross-epoch (cross-block) reuse is prohibited because currentInstrIdx
    // is a per-block counter that resets to 0 at each block boundary.
    for (auto &L : slotLifetimes_) {
        if (L.sizeBytes == sizeBytes && L.alignBytes >= alignBytes && L.epoch == blockEpoch_ &&
            L.lastUseIdx < currentInstrIdx) {
            // Recycle: update the vreg→offset mapping and refresh the lifetime.
            fn_->frame.spills.push_back(
                MFunction::SpillSlot{vreg, sizeBytes, alignBytes, L.offset});
            L.vreg = vreg;
            L.lastUseIdx = lastUseInstrIdx;
            // epoch stays the same (still the current block)
            return L.offset;
        }
    }

    // No dead slot available: allocate a fresh one and track its lifetime.
    const int off = assignAlignedSlot(sizeBytes, alignBytes);
    fn_->frame.spills.push_back(MFunction::SpillSlot{vreg, sizeBytes, alignBytes, off});
    slotLifetimes_.push_back(
        SlotLifetime{vreg, off, sizeBytes, alignBytes, lastUseInstrIdx, blockEpoch_});
    return off;
}

void FrameBuilder::setMaxOutgoingBytes(int bytes) {
    if (bytes < 0)
        throw std::invalid_argument("AArch64 frame outgoing argument area must be non-negative");
    fn_->frame.maxOutgoingBytes = std::max(fn_->frame.maxOutgoingBytes, bytes);
}

void FrameBuilder::finalize() {
    // Account for any previously assigned locals/spills on the function as well as
    // slots assigned via this builder instance.
    int usedBytes = slotCursor_.usedBytes();
    for (const auto &L : fn_->frame.locals)
        usedBytes = std::max(usedBytes, positiveExtentFromOffset(L.offset, "local"));
    for (const auto &S : fn_->frame.spills)
        usedBytes = std::max(usedBytes, positiveExtentFromOffset(S.offset, "spill"));
    // Account for FP-LR area implicitly; our offsets start at -8, so usedBytes already counts
    // slots. Add any reserved outgoing-arg area.
    usedBytes = checkedAddFrameBytes(usedBytes, fn_->frame.maxOutgoingBytes, "total size");
    // Round up to 16-byte alignment.
    usedBytes = common::roundUpBytes(usedBytes, kStackAlignment);
    fn_->frame.totalBytes = usedBytes;
    fn_->localFrameSize = usedBytes; // bridge for current emitter plan field
}

int FrameBuilder::assignAlignedSlot(int sizeBytes, int alignBytes) {
    validateStackObjectSpec("slot", sizeBytes, alignBytes);
    return slotCursor_.allocate(sizeBytes, alignBytes).offset;
}

MFunction::SpillSlot *FrameBuilder::findLatestSpillSlot(uint32_t vreg) noexcept {
    for (auto it = fn_->frame.spills.rbegin(); it != fn_->frame.spills.rend(); ++it) {
        if (it->vreg == vreg)
            return &*it;
    }
    return nullptr;
}

const MFunction::SpillSlot *FrameBuilder::findLatestSpillSlot(uint32_t vreg) const noexcept {
    for (auto it = fn_->frame.spills.rbegin(); it != fn_->frame.spills.rend(); ++it) {
        if (it->vreg == vreg)
            return &*it;
    }
    return nullptr;
}

FrameBuilder::SlotLifetime *FrameBuilder::findSlotLifetime(int offset) noexcept {
    for (auto &lifetime : slotLifetimes_) {
        if (lifetime.offset == offset)
            return &lifetime;
    }
    return nullptr;
}

const FrameBuilder::SlotLifetime *FrameBuilder::findSlotLifetime(int offset) const noexcept {
    for (const auto &lifetime : slotLifetimes_) {
        if (lifetime.offset == offset)
            return &lifetime;
    }
    return nullptr;
}

} // namespace zanna::codegen::aarch64
