//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: support/small_vector.hpp
// Purpose: Stack-optimized vector that avoids heap allocation for small sizes.
// Key invariants: heap_ is non-null exactly when storage has spilled to the heap;
//                 size_ <= capacity() always; capacity() reports N while inline;
//                 only elements in [0, size_) are constructed.
// Ownership/Lifetime: The container owns its constructed elements and heap buffer;
//                     inline storage is raw memory tied to the object's lifetime.
// Links: docs/internals/codemap.md#support-library
//
// SmallVector stores up to N elements inline (on the stack) and only allocates
// from the heap when the size exceeds N. This is particularly useful for
// function call arguments where most calls have few arguments.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Defines a vector-like container with fixed-capacity inline storage.
/// @details `SmallVector` constructs its first `N` elements inside the container
///          object and allocates only after that capacity is exceeded. It owns
///          every constructed element, provides contiguous storage throughout,
///          and preserves standard vector-style iterator invalidation rules when
///          storage changes.

#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <new>
#include <span>
#include <type_traits>
#include <utility>

namespace il::support {

/// @brief A vector-like container with inline storage for small element counts.
///
/// @details `SmallVector<T, N>` stores up to `N` elements in inline storage
///          without allocating. When more elements are needed, it permanently
///          switches to allocator-backed storage until cleared and explicitly
///          replaced by an assignment path that fits inline. Elements occupy a
///          contiguous range `[data(), data() + size())` in either mode.
///
/// @tparam T Copy-constructible element type, or nothrow move-constructible type.
/// @tparam N Positive number of elements stored inline; defaults to eight.
template <typename T, size_t N = 8> class SmallVector {
    static_assert(N > 0, "SmallVector inline capacity must be positive");
    static_assert(
        std::is_copy_constructible_v<T> || std::is_nothrow_move_constructible_v<T>,
        "SmallVector reallocation requires copy construction or noexcept move construction");

    using Allocator = std::allocator<T>;
    using AllocTraits = std::allocator_traits<Allocator>;

    alignas(T) std::byte inlineStorage_[sizeof(T) * N]{}; ///< Raw inline storage.
    T *heap_{nullptr};                                    ///< Heap storage when size > N.
    size_t capacity_{0};                                  ///< Heap capacity (0 when inline).
    size_t size_{0};                                      ///< Current constructed element count.
    [[no_unique_address]] Allocator allocator_{};         ///< Allocator used for heap storage.

    /// @brief Check whether the vector is currently using heap storage.
    /// @return true if elements reside on the heap, false if inline.
    [[nodiscard]] bool isHeap() const noexcept {
        return heap_ != nullptr;
    }

    /// @brief Return the typed pointer for inline raw storage.
    /// @return Pointer to the first inline slot; slots may be unconstructed.
    /// @warning Only positions below `size_` contain live `T` objects.
    [[nodiscard]] T *inlineData() noexcept {
        return std::launder(reinterpret_cast<T *>(inlineStorage_));
    }

    /// @brief Return the typed pointer for const inline raw storage.
    /// @return Pointer to the first inline slot; slots may be unconstructed.
    /// @warning Only positions below `size_` contain live `T` objects.
    [[nodiscard]] const T *inlineData() const noexcept {
        return std::launder(reinterpret_cast<const T *>(inlineStorage_));
    }

    /// @brief Destroy @p count constructed elements starting at @p ptr.
    /// @param ptr Pointer to the first constructed element to destroy.
    /// @param count Number of contiguous elements to destroy.
    static void destroyRange(T *ptr, size_t count) noexcept {
        for (size_t i = count; i > 0; --i)
            std::destroy_at(ptr + (i - 1));
    }

    /// @brief Release heap storage after all constructed elements are gone.
    /// @details Leaves the vector in inline-storage mode with zero heap capacity.
    /// @pre No live elements remain in the heap buffer.
    void releaseHeap() noexcept {
        if (heap_) {
            AllocTraits::deallocate(allocator_, heap_, capacity_);
            heap_ = nullptr;
            capacity_ = 0;
        }
    }

    /// @brief Replace this vector with a copied snapshot of @p other.
    /// @param other Source vector whose elements should be copied.
    /// @details Allocates and constructs replacement heap storage before mutating
    ///          this object when copies can throw, so copy assignment preserves the
    ///          old value if allocation or element construction fails.  For small,
    ///          nothrow-copyable vectors it keeps the replacement in inline storage.
    void replaceWithCopiedElements(const SmallVector &other) {
        if (other.size_ == 0) {
            clear();
            releaseHeap();
            return;
        }

        if constexpr (std::is_nothrow_copy_constructible_v<T>) {
            if (other.size_ <= N) {
                clear();
                releaseHeap();
                for (size_t index = 0; index < other.size_; ++index)
                    std::construct_at(inlineData() + index, other.data()[index]);
                size_ = other.size_;
                return;
            }
        }

        const size_t newCapacity = std::max(other.size_, N);
        T *newBuf = AllocTraits::allocate(allocator_, newCapacity);
        size_t constructed = 0;
        try {
            for (; constructed < other.size_; ++constructed)
                std::construct_at(newBuf + constructed, other.data()[constructed]);
        } catch (...) {
            destroyRange(newBuf, constructed);
            AllocTraits::deallocate(allocator_, newBuf, newCapacity);
            throw;
        }

        clear();
        releaseHeap();
        heap_ = newBuf;
        capacity_ = newCapacity;
        size_ = other.size_;
    }

    /// @brief Replace this vector by moving another vector's inline elements.
    /// @param other Source vector currently using inline storage.
    /// @details Constructs all moved elements in replacement heap storage before
    ///          touching this object when element moves can throw.  For small,
    ///          nothrow-movable vectors it keeps the replacement in inline storage.
    ///          If a throwing move fails, this vector is unchanged and the source
    ///          remains valid in its moved-from-prefix state, matching ordinary
    ///          move-assignment expectations.
    void replaceWithMovedInlineElements(SmallVector &other) {
        if (other.size_ == 0) {
            clear();
            releaseHeap();
            return;
        }

        if constexpr (std::is_nothrow_move_constructible_v<T>) {
            if (other.size_ <= N) {
                clear();
                releaseHeap();
                moveInlineFrom(other);
                return;
            }
        }

        const size_t newCapacity = std::max(other.size_, N);
        T *newBuf = AllocTraits::allocate(allocator_, newCapacity);
        size_t constructed = 0;
        try {
            for (; constructed < other.size_; ++constructed)
                std::construct_at(newBuf + constructed, std::move(other.inlineData()[constructed]));
        } catch (...) {
            destroyRange(newBuf, constructed);
            AllocTraits::deallocate(allocator_, newBuf, newCapacity);
            throw;
        }

        clear();
        releaseHeap();
        heap_ = newBuf;
        capacity_ = newCapacity;
        size_ = other.size_;
        other.clear();
    }

    /// @brief Compute a checked growth capacity for at least @p minCapacity elements.
    /// @param minCapacity Minimum required capacity.
    /// @return Capacity to reserve, growing geometrically where possible.
    /// @throws std::bad_array_new_length if @p minCapacity exceeds allocator limits.
    [[nodiscard]] size_t growthCapacity(size_t minCapacity) const {
        const size_t maxCapacity = AllocTraits::max_size(allocator_);
        if (minCapacity > maxCapacity)
            throw std::bad_array_new_length();

        size_t next = capacity();
        if (next == 0)
            next = N;
        while (next < minCapacity) {
            if (next > maxCapacity / 2) {
                next = maxCapacity;
                break;
            }
            next *= 2;
        }
        return next < minCapacity ? minCapacity : next;
    }

    /// @brief Return size() + 1 after checking allocator limits.
    /// @return The next element count after one append.
    /// @throws std::bad_array_new_length when appending would exceed max_size().
    /// @details Calculating size_ + 1 directly can wrap before growth checks run
    ///          on pathological inputs.  Centralising the check keeps append paths
    ///          consistent with reserve().
    [[nodiscard]] size_t checkedAppendSize() const {
        if (size_ >= AllocTraits::max_size(allocator_))
            throw std::bad_array_new_length();
        return size_ + 1;
    }

    /// @brief Grow storage and append a new element while old elements remain alive.
    /// @tparam Args Constructor argument types for the appended element.
    /// @param newCapacity Capacity of the replacement heap buffer.
    /// @param args Arguments forwarded to the appended element constructor.
    /// @return Reference to the newly appended element.
    /// @details The appended element is constructed before existing elements are
    ///          moved or copied.  That ordering preserves correctness for calls
    ///          such as `v.emplace_back(v[0])`, where constructor arguments may
    ///          reference elements in the old storage.
    template <typename... Args> T &growAndEmplaceBack(size_t newCapacity, Args &&...args) {
        T *newBuf = AllocTraits::allocate(allocator_, newCapacity);
        size_t constructedExisting = 0;
        bool tailConstructed = false;
        try {
            std::construct_at(newBuf + size_, std::forward<Args>(args)...);
            tailConstructed = true;
            for (; constructedExisting < size_; ++constructedExisting) {
                std::construct_at(newBuf + constructedExisting,
                                  std::move_if_noexcept(data()[constructedExisting]));
            }
        } catch (...) {
            destroyRange(newBuf, constructedExisting);
            if (tailConstructed)
                std::destroy_at(newBuf + size_);
            AllocTraits::deallocate(allocator_, newBuf, newCapacity);
            throw;
        }

        T *oldData = data();
        const size_t oldSize = size_;
        T *oldHeap = heap_;
        const size_t oldCapacity = capacity_;

        destroyRange(oldData, oldSize);
        if (oldHeap)
            AllocTraits::deallocate(allocator_, oldHeap, oldCapacity);

        heap_ = newBuf;
        capacity_ = newCapacity;
        size_ = oldSize + 1;
        return heap_[oldSize];
    }

    /// @brief Move elements from another vector's inline storage into this vector.
    /// @param other Inline-backed source whose elements should be transferred.
    /// @details Constructs elements sequentially in this object's unused inline
    ///          storage, then destroys the source elements and leaves @p other
    ///          empty. On a throwing move, constructed destination elements are
    ///          destroyed while the source remains valid but may be partly moved.
    void moveInlineFrom(SmallVector &other) noexcept(std::is_nothrow_move_constructible_v<T>) {
        size_t constructed = 0;
        if constexpr (std::is_nothrow_move_constructible_v<T>) {
            for (; constructed < other.size_; ++constructed)
                std::construct_at(inlineData() + constructed,
                                  std::move(other.inlineData()[constructed]));
        } else {
            try {
                for (; constructed < other.size_; ++constructed)
                    std::construct_at(inlineData() + constructed,
                                      std::move(other.inlineData()[constructed]));
            } catch (...) {
                destroyRange(inlineData(), constructed);
                throw;
            }
        }
        size_ = constructed;
        other.clear();
    }

  public:
    using value_type = T;
    using size_type = size_t;
    using difference_type = ptrdiff_t;
    using reference = T &;
    using const_reference = const T &;
    using pointer = T *;
    using const_pointer = const T *;
    using iterator = T *;
    using const_iterator = const T *;

    /// @brief Construct an empty SmallVector.
    /// @details Selects inline-storage mode with no live elements or allocation.
    SmallVector() noexcept = default;

    /// @brief Construct from initializer list.
    /// @param init List of elements to copy into the vector.
    /// @details Reserves enough contiguous capacity, then copy-constructs values
    ///          in list order. Partially constructed state is cleaned on failure.
    SmallVector(std::initializer_list<T> init) {
        reserve(init.size());
        size_t constructed = 0;
        try {
            for (const T &value : init) {
                std::construct_at(data() + constructed, value);
                ++constructed;
            }
        } catch (...) {
            destroyRange(data(), constructed);
            releaseHeap();
            throw;
        }
        size_ = constructed;
    }

    /// @brief Copy constructor.
    /// @param other Source vector to copy elements from.
    /// @details The new vector chooses storage based on @p other's size rather
    ///          than copying its spare capacity. Construction is rolled back if
    ///          an element copy throws.
    SmallVector(const SmallVector &other) {
        reserve(other.size_);
        size_t constructed = 0;
        try {
            for (; constructed < other.size_; ++constructed)
                std::construct_at(data() + constructed, other.data()[constructed]);
        } catch (...) {
            destroyRange(data(), constructed);
            releaseHeap();
            throw;
        }
        size_ = constructed;
    }

    /// @brief Move constructor.
    /// @details If @p other uses heap storage, the buffer is stolen in O(1).
    ///          If @p other uses inline storage, elements are moved element-wise.
    /// @param other Source vector to move from; left empty after the move.
    SmallVector(SmallVector &&other) noexcept(std::is_nothrow_move_constructible_v<T>) {
        if (other.isHeap()) {
            heap_ = other.heap_;
            capacity_ = other.capacity_;
            size_ = other.size_;
            other.heap_ = nullptr;
            other.capacity_ = 0;
            other.size_ = 0;
        } else {
            moveInlineFrom(other);
        }
    }

    /// @brief Destructor.
    /// @details Destroys live elements in reverse order and releases heap storage
    ///          when the vector has exceeded its inline capacity.
    ~SmallVector() {
        clear();
        releaseHeap();
    }

    /// @brief Copy assignment.
    /// @param other Source vector to copy elements from.
    /// @return Reference to this vector.
    /// @details Self-assignment is a no-op. Otherwise replacement construction
    ///          preserves the existing value until potentially throwing copies
    ///          have succeeded.
    SmallVector &operator=(const SmallVector &other) {
        if (this != &other)
            replaceWithCopiedElements(other);
        return *this;
    }

    /// @brief Move assignment.
    /// @details Builds replacement storage before mutating this vector when moving
    ///          inline elements; heap-backed sources are still stolen in O(1).
    /// @param other Source vector to move from; left empty after the move.
    /// @return Reference to this vector.
    SmallVector &operator=(SmallVector &&other) noexcept(std::is_nothrow_move_constructible_v<T>) {
        if (this != &other) {
            if (other.isHeap()) {
                clear();
                releaseHeap();
                heap_ = other.heap_;
                capacity_ = other.capacity_;
                size_ = other.size_;
                other.heap_ = nullptr;
                other.capacity_ = 0;
                other.size_ = 0;
            } else {
                replaceWithMovedInlineElements(other);
            }
        }
        return *this;
    }

    /// @brief Reserve capacity for at least @p n elements.
    /// @details If @p n exceeds current capacity, allocates a new heap buffer
    ///          and relocates existing elements by nothrow move or by copy.
    ///          No-op if capacity is already sufficient. Reallocation invalidates
    ///          every pointer, reference, iterator, and span into the vector.
    /// @param n Minimum number of elements the vector should be able to hold.
    /// @throws std::bad_array_new_length If @p n exceeds allocator limits.
    void reserve(size_t n) {
        if (n <= capacity())
            return;
        if (n > AllocTraits::max_size(allocator_))
            throw std::bad_array_new_length();

        T *newBuf = AllocTraits::allocate(allocator_, n);
        size_t constructed = 0;
        try {
            for (; constructed < size_; ++constructed) {
                std::construct_at(newBuf + constructed, std::move_if_noexcept(data()[constructed]));
            }
        } catch (...) {
            destroyRange(newBuf, constructed);
            AllocTraits::deallocate(allocator_, newBuf, n);
            throw;
        }

        T *oldData = data();
        const size_t oldSize = size_;
        T *oldHeap = heap_;
        const size_t oldCapacity = capacity_;

        destroyRange(oldData, oldSize);
        if (oldHeap)
            AllocTraits::deallocate(allocator_, oldHeap, oldCapacity);

        heap_ = newBuf;
        capacity_ = n;
        size_ = oldSize;
    }

    /// @brief Add an element to the end.
    /// @param value Element to copy-append.
    /// @details Delegates to @ref emplace_back, including its alias-safe growth
    ///          behavior when @p value refers to an existing element.
    void push_back(const T &value) {
        emplace_back(value);
    }

    /// @brief Add an element to the end (move version).
    /// @param value Element to move-append.
    /// @details Delegates construction and any required geometric growth to
    ///          @ref emplace_back.
    void push_back(T &&value) {
        emplace_back(std::move(value));
    }

    /// @brief Construct an element in place at the end.
    /// @tparam Args Constructor argument types.
    /// @param args Arguments forwarded to the element constructor.
    /// @return Reference to the newly constructed element.
    /// @details Grows geometrically when full. A growth operation invalidates
    ///          existing references and iterators; construction failure leaves
    ///          the element count unchanged.
    template <typename... Args> reference emplace_back(Args &&...args) {
        if (size_ >= capacity())
            return growAndEmplaceBack(growthCapacity(checkedAppendSize()),
                                      std::forward<Args>(args)...);
        T *ptr = data() + size_;
        std::construct_at(ptr, std::forward<Args>(args)...);
        ++size_;
        return *ptr;
    }

    /// @brief Remove the last element.
    /// @pre The vector is not empty.
    /// @details Destroys the final element without changing storage capacity.
    void pop_back() {
        assert(size_ > 0);
        --size_;
        std::destroy_at(data() + size_);
    }

    /// @brief Clear all elements.
    /// @details Destroys all live elements in reverse order while retaining the
    ///          current inline or heap allocation for later reuse.
    void clear() noexcept {
        destroyRange(data(), size_);
        size_ = 0;
    }

    /// @brief Resize to @p n elements.
    /// @details New elements beyond the current size are default-initialized.
    ///          If @p n is smaller than size(), excess elements are destroyed.
    ///          If construction of a new element fails, already-added elements
    ///          from this resize are removed and the original size is restored.
    /// @param n Desired element count.
    void resize(size_t n) {
        if (n < size_) {
            destroyRange(data() + n, size_ - n);
            size_ = n;
            return;
        }
        if (n > capacity())
            reserve(n);
        size_t constructed = size_;
        try {
            for (; constructed < n; ++constructed)
                std::construct_at(data() + constructed);
        } catch (...) {
            destroyRange(data() + size_, constructed - size_);
            throw;
        }
        size_ = n;
    }

    /// @brief Resize to @p n elements, filling new slots with @p value.
    /// @param n Desired element count.
    /// @param value Value to assign to newly created elements.
    /// @details Copies @p value before reallocation when it may alias an existing
    ///          element. Shrinking destroys the discarded suffix; growth failure
    ///          removes any elements constructed by this call.
    void resize(size_t n, const T &value) {
        if (n < size_) {
            destroyRange(data() + n, size_ - n);
            size_ = n;
            return;
        }
        if (n > capacity()) {
            T valueCopy(value);
            reserve(n);
            size_t constructed = size_;
            try {
                for (; constructed < n; ++constructed)
                    std::construct_at(data() + constructed, valueCopy);
            } catch (...) {
                destroyRange(data() + size_, constructed - size_);
                throw;
            }
            size_ = n;
            return;
        }
        size_t constructed = size_;
        try {
            for (; constructed < n; ++constructed)
                std::construct_at(data() + constructed, value);
        } catch (...) {
            destroyRange(data() + size_, constructed - size_);
            throw;
        }
        size_ = n;
    }

    // Accessors

    /// @brief Return the number of elements in the vector.
    /// @return Current element count.
    [[nodiscard]] size_t size() const noexcept {
        return size_;
    }

    /// @brief Return the total capacity (inline or heap).
    /// @return Maximum number of elements storable without reallocation.
    [[nodiscard]] size_t capacity() const noexcept {
        return isHeap() ? capacity_ : N;
    }

    /// @brief Return true if the vector contains no elements.
    /// @return true when size() == 0.
    [[nodiscard]] bool empty() const noexcept {
        return size_ == 0;
    }

    /// @brief Return a pointer to the underlying element storage.
    /// @return Pointer to the first element (inline or heap buffer).
    /// @note The pointer denotes contiguous storage but must not be dereferenced
    ///       when the vector is empty.
    [[nodiscard]] T *data() noexcept {
        return isHeap() ? heap_ : inlineData();
    }

    /// @brief Return a const pointer to the underlying element storage.
    /// @return Const pointer to the first element (inline or heap buffer).
    /// @note The pointer denotes contiguous storage but must not be dereferenced
    ///       when the vector is empty.
    [[nodiscard]] const T *data() const noexcept {
        return isHeap() ? heap_ : inlineData();
    }

    /// @brief Access element by index (unchecked in release builds).
    /// @param i Zero-based index; must be less than size().
    /// @return Reference to the element at position @p i.
    [[nodiscard]] reference operator[](size_t i) noexcept {
        assert(i < size_);
        return data()[i];
    }

    /// @brief Access element by index (const, unchecked in release builds).
    /// @param i Zero-based index; must be less than size().
    /// @return Const reference to the element at position @p i.
    [[nodiscard]] const_reference operator[](size_t i) const noexcept {
        assert(i < size_);
        return data()[i];
    }

    /// @brief Access the first element.
    /// @return Reference to the front element; undefined if empty.
    [[nodiscard]] reference front() noexcept {
        assert(size_ > 0);
        return data()[0];
    }

    /// @brief Access the first element (const).
    /// @return Const reference to the front element; undefined if empty.
    [[nodiscard]] const_reference front() const noexcept {
        assert(size_ > 0);
        return data()[0];
    }

    /// @brief Access the last element.
    /// @return Reference to the back element; undefined if empty.
    [[nodiscard]] reference back() noexcept {
        assert(size_ > 0);
        return data()[size_ - 1];
    }

    /// @brief Access the last element (const).
    /// @return Const reference to the back element; undefined if empty.
    [[nodiscard]] const_reference back() const noexcept {
        assert(size_ > 0);
        return data()[size_ - 1];
    }

    // Iterators

    /// @brief Return an iterator to the first element.
    /// @return Iterator pointing to the beginning of the element range.
    [[nodiscard]] iterator begin() noexcept {
        return data();
    }

    /// @brief Return a const iterator to the first element.
    /// @return Const iterator pointing to the beginning of the element range.
    [[nodiscard]] const_iterator begin() const noexcept {
        return data();
    }

    /// @brief Return a const iterator to the first element.
    /// @return Const iterator pointing to the beginning of the element range.
    [[nodiscard]] const_iterator cbegin() const noexcept {
        return data();
    }

    /// @brief Return an iterator past the last element.
    /// @return Iterator pointing one past the last element.
    [[nodiscard]] iterator end() noexcept {
        return data() + size_;
    }

    /// @brief Return a const iterator past the last element.
    /// @return Const iterator pointing one past the last element.
    [[nodiscard]] const_iterator end() const noexcept {
        return data() + size_;
    }

    /// @brief Return a const iterator past the last element.
    /// @return Const iterator pointing one past the last element.
    [[nodiscard]] const_iterator cend() const noexcept {
        return data() + size_;
    }

    /// @brief Implicit conversion to span for API compatibility.
    /// @return A read-only span covering all elements.
    [[nodiscard]] operator std::span<const T>() const & noexcept {
        return {data(), size_};
    }

    /// @brief Disallow implicit spans from temporaries.
    /// @details A span borrows the vector storage.  Deleting the rvalue overload
    ///          prevents accidental dangling spans from expressions such as
    ///          `std::span<const T>{makeVector()}` while preserving lvalue use.
    [[nodiscard]] operator std::span<const T>() const && = delete;

    /// @brief Explicit conversion to mutable span.
    /// @return A mutable span covering all elements.
    [[nodiscard]] std::span<T> span() & noexcept {
        return {data(), size_};
    }

    /// @brief Disallow mutable spans from temporaries.
    /// @details Prevents a borrowing mutable view from outliving a temporary
    ///          vector and its inline or heap storage.
    [[nodiscard]] std::span<T> span() && = delete;

    /// @brief Explicit conversion to const span.
    /// @return A read-only span covering all elements.
    [[nodiscard]] std::span<const T> span() const & noexcept {
        return {data(), size_};
    }

    /// @brief Disallow const spans from temporaries.
    /// @details Prevents a borrowing read-only view from outliving a temporary
    ///          vector and its inline or heap storage.
    [[nodiscard]] std::span<const T> span() const && = delete;
};

} // namespace il::support
