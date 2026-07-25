//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/FrameLayoutUtils.hpp
// Purpose: Shared helpers for stack-slot allocation and frame-size alignment.
//
//===----------------------------------------------------------------------===//

#pragma once

#include <algorithm>
#include <cstddef>
#include <limits>
#include <stdexcept>

/// @file
/// @brief Provides validated alignment and downward-frame cursor utilities.

namespace zanna::codegen::common {

/// @brief Round @p value up to the next multiple of @p align.
/// @details Performs always-on validation so release builds do not divide by
///          zero or overflow when frame sizes become unexpectedly large.
/// @param value Nonnegative byte count to align.
/// @param align Positive byte alignment; need not be a power of two.
/// @return Smallest multiple of @p align not less than @p value.
/// @throws std::invalid_argument if @p value is negative or @p align is not positive.
/// @throws std::overflow_error if the rounded value exceeds int range.
[[nodiscard]] inline int roundUpBytes(int value, int align) {
    if (value < 0)
        throw std::invalid_argument("frame byte count must be non-negative");
    if (align <= 0)
        throw std::invalid_argument("frame alignment must be positive");
    const int remainder = value % align;
    if (remainder == 0)
        return value;
    const int delta = align - remainder;
    if (value > std::numeric_limits<int>::max() - delta)
        throw std::overflow_error("frame byte count overflows int range after alignment");
    return value + delta;
}

/// @brief Round @p value up to the next multiple of @p align.
/// @details Performs always-on validation so release builds do not divide by
///          zero or overflow when frame sizes become unexpectedly large.
/// @param value Byte count to align.
/// @param align Positive byte alignment; need not be a power of two.
/// @return Smallest multiple of @p align not less than @p value.
/// @throws std::invalid_argument if @p align is zero.
/// @throws std::overflow_error if the rounded value exceeds size_t range.
[[nodiscard]] inline std::size_t roundUpBytes(std::size_t value, std::size_t align) {
    if (align == 0)
        throw std::invalid_argument("frame alignment must be positive");
    const std::size_t remainder = value % align;
    if (remainder == 0)
        return value;
    const std::size_t delta = align - remainder;
    if (value > std::numeric_limits<std::size_t>::max() - delta)
        throw std::overflow_error("frame byte count overflows size_t range after alignment");
    return value + delta;
}

/// @brief Convert a byte count into fixed-size stack slots.
/// @details A zero byte count still reserves one slot, matching the previous
///          alloca behavior. Negative sizes and invalid slot widths are rejected.
/// @param bytes Requested object size in bytes.
/// @param slotBytes Positive fixed slot width.
/// @return Number of slots needed after rounding; at least one.
/// @throws std::invalid_argument if @p bytes is negative or @p slotBytes is not positive.
[[nodiscard]] inline int bytesToSlots(int bytes, int slotBytes) {
    if (bytes < 0)
        throw std::invalid_argument("stack object byte count must be non-negative");
    if (slotBytes <= 0)
        throw std::invalid_argument("stack slot size must be positive");
    const int clampedBytes = std::max(1, bytes);
    return roundUpBytes(clampedBytes, slotBytes) / slotBytes;
}

/// @brief Result of reserving one stack object in a downward-growing frame.
struct DownwardFrameSlot {
    int offset{0};        ///< Negative frame-pointer-relative base offset.
    int reservedBytes{0}; ///< Total bytes reserved for the slot, including padding.
};

/// @brief Tracks reserved bytes for a downward-growing frame.
///
/// Offsets returned by allocate() point at the lowest address of the reserved
/// slot, which is the correct base pointer for stack-allocated objects.
class DownwardFrameCursor {
  public:
    /// @brief Construct an empty downward-growing cursor.
    /// @param minSlotBytes Minimum reservation made for any allocation; values
    ///                     below one are clamped to one.
    explicit DownwardFrameCursor(int minSlotBytes = 1) noexcept
        : minSlotBytes_(std::max(1, minSlotBytes)) {}

    /// @brief Seed the cursor from an already-assigned negative slot offset.
    /// @param offset Frame-pointer-relative offset. Nonnegative values are ignored.
    /// @post The reserved byte count never decreases.
    void seedFromOffset(int offset) noexcept {
        if (offset < 0)
            usedBytes_ = std::max(usedBytes_, -offset);
    }

    /// @brief Reserve a new slot and return its frame-relative base offset.
    /// @details Rounds the current cursor up to @p alignBytes and advances by
    ///          max(minSlotBytes, sizeBytes), checking for invalid sizes and
    ///          signed overflow before publishing the new offset.
    /// @param sizeBytes Requested payload size.
    /// @param alignBytes Positive start alignment.
    /// @return Negative base offset and actual reserved payload bytes.
    /// @throws std::invalid_argument if @p sizeBytes is negative or @p alignBytes is not positive.
    /// @throws std::overflow_error if the frame offset exceeds int range.
    [[nodiscard]] DownwardFrameSlot allocate(int sizeBytes, int alignBytes) {
        if (sizeBytes < 0)
            throw std::invalid_argument("stack slot size must be non-negative");
        if (alignBytes <= 0)
            throw std::invalid_argument("stack slot alignment must be positive");
        const int effectiveSize = std::max(minSlotBytes_, sizeBytes);
        const int alignedStart = roundUpBytes(usedBytes_, alignBytes);
        if (alignedStart > std::numeric_limits<int>::max() - effectiveSize)
            throw std::overflow_error("downward frame cursor exceeds int range");
        usedBytes_ = alignedStart + effectiveSize;
        return DownwardFrameSlot{.offset = -usedBytes_, .reservedBytes = effectiveSize};
    }

    /// @brief Return the total bytes reserved so far.
    /// @return Monotonically nondecreasing cursor distance from the frame pointer.
    [[nodiscard]] int usedBytes() const noexcept {
        return usedBytes_;
    }

  private:
    /// Current distance below the frame pointer.
    int usedBytes_{0};

    /// Minimum payload reservation per allocation.
    int minSlotBytes_{1};
};

} // namespace zanna::codegen::common
