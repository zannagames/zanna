//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: support/arena.hpp
// Purpose: Declares bump allocator for temporary objects.
// Key invariants: None.
// Ownership/Lifetime: Arena owns all allocated memory.
// Links: docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares fixed-capacity and chunk-growing bump allocators.
/// @details Both arenas return aligned storage without individual deallocation.
///          `Arena` reuses one fixed byte buffer, while `GrowingArena` appends
///          chunks and tracks non-trivial object destructors for LIFO reset and
///          destruction. Construction rollback restores consumed capacity.

#pragma once

#include <cstddef>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

namespace il::support {
/// @brief Simple bump allocator for fast allocations.
///
/// Uses a contiguous internal buffer and a bump-pointer strategy to satisfy
/// allocation requests. Each call to allocate() advances the current position
/// in the buffer by the requested size and alignment. Individual allocations
/// cannot be freed; invoke reset() to make the entire buffer reusable.
/// @invariant Allocations are not individually freed; use reset() to reuse.
/// @ownership Owns its internal buffer.
class Arena {
  public:
    /// @brief Create arena with @p size bytes of storage.
    /// @param size Capacity in bytes.
    explicit Arena(size_t size);

    /// @brief Allocate @p size bytes with alignment @p align.
    /// @param size Number of bytes to allocate.
    /// @param align Alignment requirement.
    /// @return Pointer to allocated memory or nullptr on failure.
    /// @notes Fails if @p align is zero, not a power of two, or capacity exceeded.
    void *allocate(size_t size, size_t align);

    /// @brief Reset arena, making all allocations available again.
    void reset();

  private:
    /// Backing storage for all allocations; owned by the arena.
    std::vector<std::byte> buffer_;
    /// Current offset within buffer_ for the next allocation.
    size_t offset_ = 0;
};

/// @brief Growing arena allocator that allocates additional chunks as needed.
///
/// Unlike the basic Arena which has a fixed capacity, GrowingArena automatically
/// allocates new chunks when the current one is exhausted. This makes it suitable
/// for AST allocation where the total size is not known in advance.
///
/// Objects with non-trivial destructors are tracked and destroyed when the arena
/// is destroyed or reset. For trivially-destructible types, no destruction
/// overhead is incurred.
///
/// @invariant Individual allocations cannot be freed; reset() reclaims all memory.
/// @ownership Owns all allocated chunks and manages object destruction.
class GrowingArena {
  public:
    /// @brief Create a growing arena with specified initial and growth chunk sizes.
    /// @param initialChunkSize Size of the first chunk in bytes (default 4KB).
    /// @param growthChunkSize Size of subsequent chunks (default 8KB).
    explicit GrowingArena(size_t initialChunkSize = 4096, size_t growthChunkSize = 8192);

    /// @brief Destructor - destroys all tracked objects and frees memory.
    ~GrowingArena();

    /// @brief Copy construction is disabled because allocation ownership is unique.
    GrowingArena(const GrowingArena &) = delete;

    /// @brief Copy assignment is disabled because allocation ownership is unique.
    GrowingArena &operator=(const GrowingArena &) = delete;

    /// @brief Move-construct by transferring chunks and destructor records.
    /// @param other Arena whose state is transferred.
    GrowingArena(GrowingArena &&other) noexcept;

    /// @brief Move-assign after destroying objects currently owned by this arena.
    /// @param other Arena whose state is transferred.
    /// @return Reference to this arena.
    GrowingArena &operator=(GrowingArena &&other) noexcept;

    /// @brief Allocate raw memory with specified alignment.
    /// @param size Number of bytes to allocate.
    /// @param align Alignment requirement (must be power of two).
    /// @return Pointer to allocated memory, never nullptr (throws on failure).
    void *allocate(size_t size, size_t align);

    /// @brief Construct an object of type T in the arena.
    /// @tparam T Type of object to construct.
    /// @tparam Args Constructor argument types.
    /// @param args Constructor arguments forwarded to T's constructor.
    /// @return Pointer to the constructed object (never nullptr).
    /// @details For trivially-destructible types, no tracking overhead is incurred.
    ///          For types with non-trivial destructors, the destructor will be
    ///          called when the arena is destroyed or reset. Destructor tracking
    ///          storage is reserved before construction so a bookkeeping allocation
    ///          failure cannot leave a live object untracked.
    template <typename T, typename... Args> T *create(Args &&...args) {
        static_assert(std::is_nothrow_destructible_v<T>,
                      "GrowingArena requires nothrow destructible arena objects");
        if constexpr (!std::is_trivially_destructible_v<T>) {
            if (destructors_.size() == destructors_.max_size())
                throw std::bad_alloc();
            destructors_.reserve(destructors_.size() + 1);
        }

        const AllocationMark mark = markAllocationPoint();
        void *mem = allocate(sizeof(T), alignof(T));
        T *obj = nullptr;
        try {
            obj = new (mem) T(std::forward<Args>(args)...);
        } catch (...) {
            rewindTo(mark);
            throw;
        }

        if constexpr (!std::is_trivially_destructible_v<T>) {
            try {
                /// @brief Destroy one nontrivial arena object during rewind or reset.
                /// @param p Pointer to the constructed object.
                destructors_.push_back({obj, [](void *p) { static_cast<T *>(p)->~T(); }});
            } catch (...) {
                obj->~T();
                rewindTo(mark);
                throw;
            }
        }

        return obj;
    }

    /// @brief Reset the arena, destroying all objects and reclaiming memory.
    /// @details After reset(), the arena can be reused. All previously allocated
    ///          pointers become invalid.
    void reset();

    /// @brief Get total bytes allocated across all chunks.
    /// @return Consumed-byte count, saturated at `SIZE_MAX` on overflow.
    [[nodiscard]] size_t totalAllocated() const noexcept;

    /// @brief Get number of chunks allocated.
    /// @return Number of currently owned allocation chunks.
    [[nodiscard]] size_t chunkCount() const noexcept {
        return chunks_.size();
    }

  private:
    /// @brief A memory chunk with bump-pointer allocation.
    struct Chunk {
        std::unique_ptr<std::byte[]> data; ///< Owned backing storage for this chunk.
        size_t size = 0;                   ///< Total capacity of the chunk in bytes.
        size_t offset = 0;                 ///< Bytes already handed out from the chunk.

        /// @brief Construct an empty chunk without backing storage.
        Chunk() = default;

        /// @brief Allocate a chunk with exactly @p sz bytes of backing storage.
        /// @param sz Chunk capacity in bytes.
        explicit Chunk(size_t sz) : data(std::make_unique<std::byte[]>(sz)), size(sz), offset(0) {}

        /// @brief Copy construction is disabled because chunk storage is uniquely owned.
        Chunk(const Chunk &) = delete;

        /// @brief Copy assignment is disabled because chunk storage is uniquely owned.
        Chunk &operator=(const Chunk &) = delete;

        /// @brief Move-construct by transferring unique backing storage.
        Chunk(Chunk &&) noexcept = default;

        /// @brief Move-assign by transferring unique backing storage.
        /// @return Reference to this chunk.
        Chunk &operator=(Chunk &&) noexcept = default;

        /// @brief Attempt to allocate memory from this chunk.
        /// @param sz The size in bytes to allocate.
        /// @param align The alignment requirement (must be a power of 2).
        /// @return Pointer to the allocated memory, or nullptr if insufficient space.
        /// @details Advances the internal offset by the aligned allocation size.
        void *tryAllocate(size_t sz, size_t align);
    };

    /// @brief Destructor record for non-trivially-destructible objects.
    struct DestructorRecord {
        void *object;            ///< Object whose destructor must run on reset/destroy.
        void (*destroy)(void *); ///< Type-erased destructor thunk for @c object.
    };

    /// @brief Saved bump-allocation position for exception rollback.
    struct AllocationMark {
        size_t chunkCount = 0; ///< Number of chunks present when the mark was taken.
        size_t lastOffset = 0; ///< Offset of the last chunk when the mark was taken.
    };

    /// @brief Allocate a new chunk of at least the given size.
    /// @param minSize Minimum requested capacity; zero is promoted to one.
    void allocateChunk(size_t minSize);

    /// @brief Destroy all tracked objects in reverse order.
    /// @details Clears the destructor registry after invoking every nothrow thunk.
    void destroyObjects();

    /// @brief Capture the current bump-allocation position.
    /// @return Mark that can be passed to @ref rewindTo.
    /// @details Used by create<T>() so constructor failure can restore arena
    ///          capacity to the state observed before raw memory was allocated.
    [[nodiscard]] AllocationMark markAllocationPoint() const noexcept;

    /// @brief Restore the bump-allocation position to @p mark.
    /// @param mark Previously captured allocation mark.
    /// @details Drops chunks allocated after the mark and restores the final
    ///          surviving chunk's offset.  Callers must only use this for memory
    ///          that has not been published as a live tracked object.
    void rewindTo(AllocationMark mark) noexcept;

    std::vector<Chunk> chunks_;                 ///< Allocated chunks in creation order.
    std::vector<DestructorRecord> destructors_; ///< Pending destructors, run LIFO on reset.
    size_t growthChunkSize_;                    ///< Size of each chunk allocated after the first.
};
} // namespace il::support
