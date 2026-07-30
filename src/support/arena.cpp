//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/support/arena.cpp
// Purpose: Implements fixed and chunk-growing bump allocators for compiler
//          support objects.
// Key invariants:
//   - Alignment/capacity arithmetic is checked before advancing a cursor.
//   - Nontrivial GrowingArena objects are destroyed once in strict LIFO order.
//   - Reentrant destruction cannot reset or move storage still in use.
// Ownership/Lifetime:
//   - Arena instances uniquely own their backing buffers and destructor records.
//   - Returned pointers remain valid only until reset, move-from, or destruction.
// Links: src/support/arena.hpp, src/support/alignment.hpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Defines the `il::support::Arena` bump allocator implementation.
/// @details The arena backs many short-lived allocations inside the compiler's
///          support layer.  It owns a contiguous byte buffer, hands out aligned
///          slices on demand, and exposes a single `reset()` entry point that
///          rewinds the allocation cursor.  The design deliberately avoids
///          deallocation of individual blocks so call sites can trade off
///          lifetime tracking for speed when building transient data structures.

#include "arena.hpp"

#include "alignment.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace il::support {
/// @brief Construct an arena that manages a fixed-capacity backing buffer.
///
/// @details The constructor initializes the internal byte vector with @p size
///          elements and places the bump pointer at the start of the buffer.
///          This guarantees that the first allocation returns the first byte in
///          the buffer while subsequent allocations advance the pointer.  No
///          dynamic allocation occurs beyond reserving the storage owned by the
///          vector, keeping construction cheap enough to use on the stack.
///
/// @param size Number of bytes reserved for subsequent allocation requests.
Arena::Arena(size_t size) : buffer_(size) {}

/// @brief Allocate memory from the arena honoring the requested alignment.
///
/// @details Allocation proceeds in a handful of steps:
///          1. Validate that @p align is a non-zero power of two to keep
///             bit-mask alignment logic well defined.
///          2. Compute the aligned pointer relative to the arena's base while
///             guarding every arithmetic operation against overflow.
///          3. Ensure the new allocation fits inside the backing buffer.
///          4. Advance the bump pointer and return the aligned slice.
///          Failure at any stage returns @c nullptr without mutating state so
///          callers can attempt fallbacks.  This is sufficient for the compiler
///          where allocation failure typically signals an out-of-memory
///          condition.
///
/// @param size Number of bytes to allocate from the arena.
/// @param align Alignment requirement in bytes; must be a non-zero power of two.
/// @return Pointer to aligned memory on success, or nullptr on failure.
void *Arena::allocate(size_t size, size_t align) {
    // Reject zero or non power-of-two alignments.
    if (!isPowerOfTwo(align))
        return nullptr;
    if (buffer_.empty())
        return nullptr;

    const size_t current = offset_;
    const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(buffer_.data());
    if (current > std::numeric_limits<std::uintptr_t>::max() - base)
        return nullptr;

    const std::uintptr_t current_ptr = base + current;
    auto aligned = checkedAlignUp<std::uintptr_t>(current_ptr, static_cast<std::uintptr_t>(align));
    if (!aligned)
        return nullptr;
    const std::uintptr_t aligned_ptr = *aligned;
    const size_t adjustment = static_cast<size_t>(aligned_ptr - current_ptr);

    if (adjustment > std::numeric_limits<size_t>::max() - current)
        return nullptr;

    const size_t aligned_offset = current + adjustment;

    if (size > std::numeric_limits<size_t>::max() - aligned_offset)
        return nullptr;

    const size_t new_offset = aligned_offset + size;

    if (aligned_offset > buffer_.size() || size > buffer_.size() - aligned_offset)
        return nullptr;

    if (new_offset > buffer_.size())
        return nullptr;

    offset_ = new_offset;
    return buffer_.data() + aligned_offset;
}

/// @brief Reset the arena to reuse the entire buffer for future allocations.
///
/// @details Clearing the bump pointer invalidates all outstanding allocations
///          because subsequent requests begin writing from the start of the
///          buffer.  Callers typically pair this with stack allocation of the
///          arena so reclamation happens deterministically at scope exit after a
///          full phase of compilation completes.
void Arena::reset() {
    offset_ = 0;
}

// =============================================================================
// GrowingArena implementation
// =============================================================================

/// @brief Attempt an aligned bump allocation within one existing chunk.
/// @details Invalid alignment, missing storage, arithmetic overflow, or
///          insufficient remaining capacity returns nullptr without advancing
///          the chunk cursor.
/// @param sz Number of bytes requested.
/// @param align Required non-zero power-of-two alignment.
/// @return Pointer into this chunk, or nullptr when the request cannot fit.
void *GrowingArena::Chunk::tryAllocate(size_t sz, size_t align) {
    // Validate alignment (power of two, non-zero)
    if (!isPowerOfTwo(align))
        return nullptr;
    if (!data || offset > size)
        return nullptr;

    const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(data.get());
    if (offset > std::numeric_limits<std::uintptr_t>::max() - base)
        return nullptr;

    const std::uintptr_t current = base + offset;
    auto alignedPtr = checkedAlignUp<std::uintptr_t>(current, static_cast<std::uintptr_t>(align));
    if (!alignedPtr)
        return nullptr;

    const std::uintptr_t aligned = *alignedPtr;
    const size_t adjustment = static_cast<size_t>(aligned - current);
    if (adjustment > std::numeric_limits<size_t>::max() - offset)
        return nullptr;

    const size_t alignedOffset = offset + adjustment;
    if (sz > std::numeric_limits<size_t>::max() - alignedOffset)
        return nullptr;

    const size_t newOffset = alignedOffset + sz;
    if (alignedOffset > size || sz > size - alignedOffset)
        return nullptr;

    offset = newOffset;
    return data.get() + alignedOffset;
}

/// @brief Construct a growing arena and allocate its initial chunk.
/// @param initialChunkSize Requested capacity of the first chunk.
/// @param growthChunkSize Minimum capacity of later growth chunks.
GrowingArena::GrowingArena(size_t initialChunkSize, size_t growthChunkSize)
    : growthChunkSize_(growthChunkSize) {
    chunks_.reserve(4); // Reserve space for a few chunks
    allocateChunk(initialChunkSize);
}

/// @brief Destroy tracked objects in LIFO order before owned chunks are released.
GrowingArena::~GrowingArena() {
    destroyObjects();
}

/// @brief Move all chunks, destructor records, and growth policy from another arena.
/// @param other Arena whose owned storage is transferred.
GrowingArena::GrowingArena(GrowingArena &&other) noexcept
    : growthChunkSize_(other.growthChunkSize_) {
    if (other.destroyingObjects_)
        return;
    chunks_ = std::move(other.chunks_);
    destructors_ = std::move(other.destructors_);
}

/// @brief Replace this arena with another arena's owned state.
/// @details Existing tracked objects are destroyed before the move. Self-move
///          assignment is a no-op.
/// @param other Arena whose owned storage is transferred.
/// @return Reference to this arena.
GrowingArena &GrowingArena::operator=(GrowingArena &&other) noexcept {
    if (this == &other || destroyingObjects_ || other.destroyingObjects_)
        return *this;
    destroyObjects();
    chunks_ = std::move(other.chunks_);
    destructors_ = std::move(other.destructors_);
    growthChunkSize_ = other.growthChunkSize_;
    return *this;
}

/// @brief Allocate aligned raw storage, growing by a chunk when necessary.
/// @details The current last chunk is tried first. A new chunk is sized to at
///          least the larger of the growth policy and the request plus alignment
///          slack. Invalid alignment, overflow, and allocation failure throw.
/// @param size Number of bytes requested.
/// @param align Required non-zero power-of-two alignment.
/// @return Non-null aligned storage owned by the arena.
/// @throws std::bad_alloc When the request cannot be represented or satisfied.
void *GrowingArena::allocate(size_t size, size_t align) {
    if (!isPowerOfTwo(align))
        throw std::bad_alloc();

    // Try to allocate from the current chunk
    if (!chunks_.empty()) {
        if (void *ptr = chunks_.back().tryAllocate(size, align))
            return ptr;
    }

    // Need a new chunk - allocate at least enough for this request
    if (size > std::numeric_limits<size_t>::max() - align)
        throw std::bad_alloc();
    const size_t chunkSize = std::max(growthChunkSize_, size + align);
    allocateChunk(chunkSize);

    void *ptr = chunks_.back().tryAllocate(size, align);
    if (!ptr)
        throw std::bad_alloc();

    return ptr;
}

/// @brief Destroy tracked objects and reclaim all allocation cursors.
/// @details The first chunk is retained for reuse and every later chunk is
///          released. All pointers previously returned by the arena become invalid.
void GrowingArena::reset() {
    if (destroyingObjects_)
        return;
    destroyObjects();
    // Keep the first chunk, clear the rest
    if (!chunks_.empty()) {
        chunks_[0].offset = 0;
        chunks_.resize(1);
    }
}

/// @brief Sum bytes consumed across all current chunks.
/// @return Exact consumed-byte count, or `SIZE_MAX` if the sum would overflow.
size_t GrowingArena::totalAllocated() const noexcept {
    size_t total = 0;
    for (const auto &chunk : chunks_) {
        if (chunk.offset > std::numeric_limits<size_t>::max() - total)
            return std::numeric_limits<size_t>::max();
        total += chunk.offset;
    }
    return total;
}

/// @brief Capture the current allocation cursor for later rollback.
/// @return Mark describing the current chunk count and final chunk offset.
/// @details The mark is intentionally small and value-like so create<T>() can
///          take it before raw allocation and restore the arena if placement
///          construction fails.
GrowingArena::AllocationMark GrowingArena::markAllocationPoint() const noexcept {
    if (chunks_.empty())
        return {};
    return AllocationMark{chunks_.size(), chunks_.back().offset};
}

/// @brief Rewind the arena allocation cursor to a previous mark.
/// @param mark Allocation mark produced by @ref markAllocationPoint.
/// @details Any chunks allocated after the mark are released and the surviving
///          final chunk's offset is restored.  This is only used for failed
///          construction paths where no live object has been registered.
void GrowingArena::rewindTo(AllocationMark mark) noexcept {
    if (mark.chunkCount == 0) {
        chunks_.clear();
        return;
    }

    if (chunks_.size() > mark.chunkCount)
        chunks_.resize(mark.chunkCount);
    if (!chunks_.empty())
        chunks_.back().offset = mark.lastOffset;
}

/// @brief Append a fresh chunk with at least one byte of capacity.
/// @param minSize Requested minimum capacity; zero is promoted to one.
void GrowingArena::allocateChunk(size_t minSize) {
    chunks_.emplace_back(std::max<size_t>(1, minSize));
}

/// @brief Invoke every registered object destructor in strict reverse construction order.
/// @details Each record is removed before its callback. If that callback creates
///          another tracked object, the new record is appended and therefore
///          destroyed before any older pending object. Reentrant reset/move
///          requests observe @c destroyingObjects_ and cannot invalidate storage.
void GrowingArena::destroyObjects() noexcept {
    if (destroyingObjects_)
        return;
    destroyingObjects_ = true;
    while (!destructors_.empty()) {
        DestructorRecord record = destructors_.back();
        destructors_.pop_back();
        record.destroy(record.object);
    }
    destroyingObjects_ = false;
}

} // namespace il::support
