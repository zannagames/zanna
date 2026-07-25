//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/core/StableList.hpp
// Purpose: Declares a small random-access owning list whose element addresses
//          survive container growth and unrelated insert/erase operations.
// Key invariants:
//   - Each live element occupies a non-null slot in stable pooled storage.
//   - References and pointers to an element remain valid until that element is
//     erased or the list is cleared/destroyed.
//   - Iterator invalidation follows the backing vector of ownership slots, but
//     element object addresses do not move when the slot vector reallocates.
// Ownership/Lifetime: StableList owns all elements. It intentionally mirrors the
//          subset of std::vector used by IL block and instruction containers.
// Links: il/core/BasicBlock.hpp, il/core/Function.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace il::core {

/// @brief Owning random-access sequence with stable element addresses.
/// @details `StableList` stores elements in reusable fixed-address slots
///          allocated in chunks while exposing an interface close to vector operations
///          used throughout the IL pipeline: indexing, range iteration,
///          insertion, erasure, assignment, and capacity reservation. The slot
///          vector may reallocate, so iterators have normal vector-like
///          invalidation rules, but the pointed-to `T` objects themselves are
///          not moved by unrelated container mutations.
/// @tparam T Element type owned by the list.
template <class T> class StableList {
    struct Slot {
        std::optional<T> value;
        Slot *nextFree{nullptr};

        /// @brief Access the live value stored in this slot.
        /// @return Pointer to the value, or nullptr while the slot is free.
        T *get() noexcept {
            return value ? &*value : nullptr;
        }

        /// @brief Access the live value stored in this slot without mutation.
        /// @return Const pointer to the value, or nullptr while the slot is free.
        const T *get() const noexcept {
            return value ? &*value : nullptr;
        }
    };

    struct Chunk {
        std::unique_ptr<Slot[]> slots;
        std::size_t size{0};
    };

    using Storage = std::vector<Slot *>;

    template <bool IsConst> class IteratorBase {
        using StorageIterator = std::conditional_t<IsConst,
                                                   typename Storage::const_iterator,
                                                   typename Storage::iterator>;

      public:
        using iterator_category = std::random_access_iterator_tag;
        using difference_type = typename StorageIterator::difference_type;
        using value_type = T;
        using reference = std::conditional_t<IsConst, const T &, T &>;
        using pointer = std::conditional_t<IsConst, const T *, T *>;

        /// @brief Construct a singular iterator.
        IteratorBase() = default;

        /// @brief Convert a mutable iterator to a const iterator.
        /// @param other Mutable iterator to wrap.
        template <bool OtherConst, typename = std::enable_if_t<IsConst && !OtherConst>>
        IteratorBase(const IteratorBase<OtherConst> &other) : it_(other.it_) {}

        /// @brief Dereference the iterator to the owned element.
        /// @return Reference to the element in the current slot.
        reference operator*() const {
            return *(*it_)->get();
        }

        /// @brief Access the current element as a pointer.
        /// @return Pointer to the element in the current slot.
        pointer operator->() const {
            return (*it_)->get();
        }

        /// @brief Advance to the next slot.
        /// @return Reference to this iterator.
        IteratorBase &operator++() {
            ++it_;
            return *this;
        }

        /// @brief Advance to the next slot, returning the old iterator.
        /// @return Iterator value before incrementing.
        IteratorBase operator++(int) {
            IteratorBase copy = *this;
            ++(*this);
            return copy;
        }

        /// @brief Move to the previous slot.
        /// @return Reference to this iterator.
        IteratorBase &operator--() {
            --it_;
            return *this;
        }

        /// @brief Move to the previous slot, returning the old iterator.
        /// @return Iterator value before decrementing.
        IteratorBase operator--(int) {
            IteratorBase copy = *this;
            --(*this);
            return copy;
        }

        /// @brief Advance by @p n slots.
        /// @param n Signed slot distance.
        /// @return Reference to this iterator.
        IteratorBase &operator+=(difference_type n) {
            it_ += n;
            return *this;
        }

        /// @brief Move backward by @p n slots.
        /// @param n Signed slot distance.
        /// @return Reference to this iterator.
        IteratorBase &operator-=(difference_type n) {
            it_ -= n;
            return *this;
        }

        /// @brief Return an iterator advanced by @p n slots.
        /// @param n Signed slot distance.
        /// @return Advanced iterator.
        IteratorBase operator+(difference_type n) const {
            IteratorBase copy = *this;
            copy += n;
            return copy;
        }

        /// @brief Return an iterator moved backward by @p n slots.
        /// @param n Signed slot distance.
        /// @return Rewound iterator.
        IteratorBase operator-(difference_type n) const {
            IteratorBase copy = *this;
            copy -= n;
            return copy;
        }

        /// @brief Compute distance between two iterators.
        /// @param rhs Iterator to subtract from this iterator.
        /// @return Number of slots between the iterators.
        difference_type operator-(const IteratorBase &rhs) const {
            return it_ - rhs.it_;
        }

        /// @brief Index relative to the current iterator.
        /// @param n Signed slot offset.
        /// @return Reference to the element at the offset.
        reference operator[](difference_type n) const {
            return *(*this + n);
        }

        /// @brief Compare iterator positions for equality.
        /// @param rhs Iterator over the same backing slot vector.
        /// @return True when both iterators address the same slot position.
        bool operator==(const IteratorBase &rhs) const {
            return it_ == rhs.it_;
        }

        /// @brief Compare iterator positions for inequality.
        /// @param rhs Iterator over the same backing slot vector.
        /// @return True when the positions differ.
        bool operator!=(const IteratorBase &rhs) const {
            return !(*this == rhs);
        }

        /// @brief Compare iterator positions.
        /// @param rhs Iterator over the same backing slot vector.
        /// @return True when this position precedes @p rhs.
        bool operator<(const IteratorBase &rhs) const {
            return it_ < rhs.it_;
        }

        /// @brief Compare iterator positions.
        /// @param rhs Iterator over the same backing slot vector.
        /// @return True when this position follows @p rhs.
        bool operator>(const IteratorBase &rhs) const {
            return rhs < *this;
        }

        /// @brief Compare iterator positions.
        /// @param rhs Iterator over the same backing slot vector.
        /// @return True when this position does not follow @p rhs.
        bool operator<=(const IteratorBase &rhs) const {
            return !(rhs < *this);
        }

        /// @brief Compare iterator positions.
        /// @param rhs Iterator over the same backing slot vector.
        /// @return True when this position does not precede @p rhs.
        bool operator>=(const IteratorBase &rhs) const {
            return !(*this < rhs);
        }

      private:
        friend class StableList<T>;
        template <bool> friend class IteratorBase;

        /// @brief Construct an iterator from a backing storage iterator.
        /// @param it Backing slot-vector position.
        explicit IteratorBase(StorageIterator it) : it_(it) {}

        StorageIterator it_{};
    };

  public:
    using value_type = T;
    using size_type = typename Storage::size_type;
    using difference_type = typename Storage::difference_type;
    using reference = T &;
    using const_reference = const T &;
    using pointer = T *;
    using const_pointer = const T *;
    using iterator = IteratorBase<false>;
    using const_iterator = IteratorBase<true>;
    using reverse_iterator = std::reverse_iterator<iterator>;
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;

    /// @brief Construct an empty list.
    StableList() = default;

    /// @brief Copy-construct a list by cloning every element.
    /// @param other List whose elements should be copied.
    StableList(const StableList &other) {
        assign(other.begin(), other.end());
    }

    /// @brief Move-construct a list, preserving owned element addresses.
    /// @param other Source list whose slots are transferred.
    StableList(StableList &&other) noexcept {
        moveFrom(std::move(other));
    }

    /// @brief Construct a list from an initializer list.
    /// @param values Values to copy into the list.
    StableList(std::initializer_list<T> values) {
        assign(values.begin(), values.end());
    }

    /// @brief Copy-assign by cloning every element from @p other.
    /// @param other List whose contents should replace this list.
    /// @return Reference to this list.
    StableList &operator=(const StableList &other) {
        if (this == &other)
            return *this;
        StableList copy(other);
        swap(copy);
        return *this;
    }

    /// @brief Move-assign by transferring ownership slots.
    /// @param other Source list.
    /// @return Reference to this list.
    StableList &operator=(StableList &&other) noexcept {
        if (this == &other)
            return *this;
        clear();
        chunks_.clear();
        free_ = nullptr;
        freeCount_ = 0;
        moveFrom(std::move(other));
        return *this;
    }

    /// @brief Destroy all live elements and release pooled storage.
    ~StableList() {
        clear();
    }

    /// @brief Replace contents with copies from an initializer list.
    /// @param values Values to copy.
    /// @return Reference to this list.
    StableList &operator=(std::initializer_list<T> values) {
        assign(values.begin(), values.end());
        return *this;
    }

    /// @brief Replace contents with copies from a vector.
    /// @param values Vector whose values are copied.
    /// @return Reference to this list.
    StableList &operator=(const std::vector<T> &values) {
        assign(values.begin(), values.end());
        return *this;
    }

    /// @brief Replace contents with moved values from a vector.
    /// @param values Vector whose values are moved into this list.
    /// @return Reference to this list.
    StableList &operator=(std::vector<T> &&values) {
        clear();
        reserve(values.size());
        for (auto &value : values)
            push_back(std::move(value));
        return *this;
    }

    /// @brief Return an iterator to the first element.
    iterator begin() noexcept {
        return iterator(slots_.begin());
    }

    /// @brief Return an iterator one past the last element.
    iterator end() noexcept {
        return iterator(slots_.end());
    }

    /// @brief Return a const iterator to the first element.
    const_iterator begin() const noexcept {
        return const_iterator(slots_.begin());
    }

    /// @brief Return a const iterator one past the last element.
    const_iterator end() const noexcept {
        return const_iterator(slots_.end());
    }

    /// @brief Return a const iterator to the first element.
    const_iterator cbegin() const noexcept {
        return begin();
    }

    /// @brief Return a const iterator one past the last element.
    const_iterator cend() const noexcept {
        return end();
    }

    /// @brief Return a reverse iterator to the last element.
    /// @return Mutable reverse iterator positioned at the final live element.
    reverse_iterator rbegin() noexcept {
        return reverse_iterator(end());
    }

    /// @brief Return a reverse iterator one before the first element.
    /// @return Mutable reverse end iterator.
    reverse_iterator rend() noexcept {
        return reverse_iterator(begin());
    }

    /// @brief Return a const reverse iterator to the last element.
    /// @return Const reverse iterator positioned at the final live element.
    const_reverse_iterator rbegin() const noexcept {
        return const_reverse_iterator(end());
    }

    /// @brief Return a const reverse iterator one before the first element.
    /// @return Const reverse end iterator.
    const_reverse_iterator rend() const noexcept {
        return const_reverse_iterator(begin());
    }

    /// @brief Return a const reverse iterator to the last element.
    /// @return Const reverse iterator positioned at the final live element.
    const_reverse_iterator crbegin() const noexcept {
        return rbegin();
    }

    /// @brief Return a const reverse iterator one before the first element.
    /// @return Const reverse end iterator.
    const_reverse_iterator crend() const noexcept {
        return rend();
    }

    /// @brief Check whether the list has no elements.
    /// @return True when empty.
    [[nodiscard]] bool empty() const noexcept {
        return slots_.empty();
    }

    /// @brief Return the number of elements.
    /// @return Number of live elements.
    [[nodiscard]] size_type size() const noexcept {
        return slots_.size();
    }

    /// @brief Return the reserved slot capacity.
    /// @return Backing vector capacity.
    [[nodiscard]] size_type capacity() const noexcept {
        return slots_.capacity();
    }

    /// @brief Reserve space for at least @p count slots.
    /// @param count Requested capacity.
    void reserve(size_type count) {
        slots_.reserve(count);
        if (count > slots_.size())
            ensureFree(count - slots_.size());
    }

    /// @brief Access an element by index.
    /// @param index Zero-based index.
    /// @return Mutable reference to the element.
    reference operator[](size_type index) {
        return *slots_[index]->get();
    }

    /// @brief Access an element by index.
    /// @param index Zero-based index.
    /// @return Const reference to the element.
    const_reference operator[](size_type index) const {
        return *slots_[index]->get();
    }

    /// @brief Bounds-checked element access.
    /// @param index Zero-based index.
    /// @return Mutable reference to the element.
    /// @throws std::out_of_range When @p index is not less than size().
    reference at(size_type index) {
        return *slots_.at(index)->get();
    }

    /// @brief Bounds-checked element access.
    /// @param index Zero-based index.
    /// @return Const reference to the element.
    /// @throws std::out_of_range When @p index is not less than size().
    const_reference at(size_type index) const {
        return *slots_.at(index)->get();
    }

    /// @brief Access the first element.
    /// @return Mutable reference to the first element.
    reference front() {
        return *slots_.front()->get();
    }

    /// @brief Access the first element.
    /// @return Const reference to the first element.
    const_reference front() const {
        return *slots_.front()->get();
    }

    /// @brief Access the last element.
    /// @return Mutable reference to the last element.
    reference back() {
        return *slots_.back()->get();
    }

    /// @brief Access the last element.
    /// @return Const reference to the last element.
    const_reference back() const {
        return *slots_.back()->get();
    }

    /// @brief Remove all elements.
    void clear() noexcept {
        for (Slot *slot : slots_)
            release(slot);
        slots_.clear();
    }

    /// @brief Append a copied element.
    /// @param value Value to copy.
    void push_back(const T &value) {
        appendSlot(create(value));
    }

    /// @brief Append a moved element.
    /// @param value Value to move.
    void push_back(T &&value) {
        appendSlot(create(std::move(value)));
    }

    /// @brief Construct an element at the end of the list.
    /// @tparam Args Constructor argument types.
    /// @param args Constructor arguments forwarded to `T`.
    /// @return Reference to the newly constructed element.
    template <class... Args> reference emplace_back(Args &&...args) {
        appendSlot(create(std::forward<Args>(args)...));
        return back();
    }

    /// @brief Remove the last element.
    void pop_back() {
        release(slots_.back());
        slots_.pop_back();
    }

    /// @brief Resize the list, default-constructing new elements when growing.
    /// @param count New element count.
    void resize(size_type count) {
        if (count < slots_.size()) {
            while (slots_.size() > count)
                pop_back();
            return;
        }
        slots_.reserve(count);
        while (slots_.size() < count)
            emplace_back();
    }

    /// @brief Replace contents with copies from an iterator range.
    /// @tparam InputIt Input iterator type.
    /// @param first First value to copy.
    /// @param last One-past-last value to copy.
    template <class InputIt> void assign(InputIt first, InputIt last) {
        StableList replacement;
        for (; first != last; ++first)
            replacement.pushForwarded(*first);
        swap(replacement);
    }

    /// @brief Insert a copied element before @p pos.
    /// @param pos Insertion position.
    /// @param value Value to copy.
    /// @return Iterator to the inserted element.
    iterator insert(const_iterator pos, const T &value) {
        Slot *slot = create(value);
        typename Storage::iterator inserted;
        try {
            inserted = slots_.insert(pos.it_, slot);
        } catch (...) {
            release(slot);
            throw;
        }
        return iterator(inserted);
    }

    /// @brief Insert a moved element before @p pos.
    /// @param pos Insertion position.
    /// @param value Value to move.
    /// @return Iterator to the inserted element.
    iterator insert(const_iterator pos, T &&value) {
        Slot *slot = create(std::move(value));
        typename Storage::iterator inserted;
        try {
            inserted = slots_.insert(pos.it_, slot);
        } catch (...) {
            release(slot);
            throw;
        }
        return iterator(inserted);
    }

    /// @brief Insert copies or moves from an iterator range before @p pos.
    /// @tparam InputIt Input iterator type.
    /// @param pos Insertion position.
    /// @param first First value to insert.
    /// @param last One-past-last value to insert.
    /// @return Iterator to the first inserted element, or @p pos if the range is empty.
    template <class InputIt> iterator insert(const_iterator pos, InputIt first, InputIt last) {
        const difference_type offset = pos - cbegin();
        std::vector<Slot *> incoming;
        try {
            for (; first != last; ++first)
                incoming.push_back(createForwarded(*first));
        } catch (...) {
            for (Slot *slot : incoming)
                release(slot);
            throw;
        }
        if (incoming.empty())
            return begin() + offset;

        auto slotIt = slots_.begin() + offset;
        try {
            slotIt = slots_.insert(slotIt, incoming.begin(), incoming.end());
        } catch (...) {
            for (Slot *slot : incoming)
                release(slot);
            throw;
        }
        return iterator(slotIt);
    }

    /// @brief Erase the element at @p pos.
    /// @param pos Element position to erase.
    /// @return Iterator to the element following the erased one.
    iterator erase(const_iterator pos) {
        release(*pos.it_);
        return iterator(slots_.erase(pos.it_));
    }

    /// @brief Erase the half-open range [`first`, `last`).
    /// @param first First element to erase.
    /// @param last One-past-last element to erase.
    /// @return Iterator to the element following the erased range.
    iterator erase(const_iterator first, const_iterator last) {
        for (auto it = first.it_; it != last.it_; ++it)
            release(*it);
        return iterator(slots_.erase(first.it_, last.it_));
    }

  private:
    static constexpr size_type kSlotsPerChunk = 64;

    /// @brief Grow pooled slot storage until at least @p requested slots are free.
    /// @param requested Minimum desired free-slot count.
    void ensureFree(size_type requested) {
        while (freeCount_ < requested) {
            const size_type chunkSize = std::max(kSlotsPerChunk, requested - freeCount_);
            Chunk chunk{std::make_unique<Slot[]>(chunkSize), chunkSize};
            chunks_.push_back(std::move(chunk));
            Chunk &stored = chunks_.back();
            for (size_type index = 0; index < chunkSize; ++index) {
                stored.slots[index].nextFree = free_;
                free_ = &stored.slots[index];
            }
            freeCount_ += chunkSize;
        }
    }

    /// @brief Acquire a pooled slot and construct an element in place.
    /// @tparam Args Element-constructor argument types.
    /// @param args Arguments forwarded to `T`.
    /// @return Live slot not yet inserted into the logical sequence.
    /// @details If construction throws, the slot is returned to the free list.
    template <class... Args> Slot *create(Args &&...args) {
        ensureFree(1);
        Slot *slot = free_;
        free_ = free_->nextFree;
        --freeCount_;
        slot->nextFree = nullptr;
        try {
            slot->value.emplace(std::forward<Args>(args)...);
        } catch (...) {
            slot->nextFree = free_;
            free_ = slot;
            ++freeCount_;
            throw;
        }
        return slot;
    }

    /// @brief Destroy a live element and return its slot to the free list.
    /// @param slot Non-null slot owned by this list.
    void release(Slot *slot) noexcept {
        slot->value.reset();
        slot->nextFree = free_;
        free_ = slot;
        ++freeCount_;
    }

    /// @brief Append an already constructed slot to the logical sequence.
    /// @param slot Live pooled slot.
    /// @details Returns @p slot to the free list if vector insertion throws.
    void appendSlot(Slot *slot) {
        try {
            slots_.push_back(slot);
        } catch (...) {
            release(slot);
            throw;
        }
    }

    /// @brief Preserve a range element's value category while acquiring a slot.
    /// @tparam U Forwarded source type.
    /// @param value Value copied or moved into the new element.
    /// @return Constructed pooled slot.
    template <class U> Slot *createForwarded(U &&value) {
        return create(std::forward<U>(value));
    }

    /// @brief Append a copied or moved element while preserving value category.
    /// @tparam U Value category and cv-qualified type accepted by `T`.
    /// @param value Value forwarded into a newly owned element.
    template <class U> void pushForwarded(U &&value) {
        appendSlot(createForwarded(std::forward<U>(value)));
    }

    /// @brief Exchange logical storage, pooled chunks, and free-list state.
    /// @param other List to exchange with.
    void swap(StableList &other) noexcept {
        slots_.swap(other.slots_);
        chunks_.swap(other.chunks_);
        std::swap(free_, other.free_);
        std::swap(freeCount_, other.freeCount_);
    }

    /// @brief Transfer all owned storage while leaving the source empty.
    /// @param other Source list.
    void moveFrom(StableList &&other) noexcept {
        slots_ = std::move(other.slots_);
        chunks_ = std::move(other.chunks_);
        free_ = other.free_;
        freeCount_ = other.freeCount_;
        other.slots_.clear();
        other.free_ = nullptr;
        other.freeCount_ = 0;
    }

    Storage slots_;
    std::vector<Chunk> chunks_;
    Slot *free_{nullptr};
    size_type freeCount_{0};
};

/// @brief Return an iterator advanced by @p n from @p it.
/// @tparam T StableList element type.
/// @param n Signed slot distance.
/// @param it Base iterator.
/// @return Advanced iterator.
template <class T>
typename StableList<T>::iterator operator+(typename StableList<T>::difference_type n,
                                           typename StableList<T>::iterator it) {
    return it + n;
}

/// @brief Return a const iterator advanced by @p n from @p it.
/// @tparam T StableList element type.
/// @param n Signed slot distance.
/// @param it Base iterator.
/// @return Advanced const iterator.
template <class T>
typename StableList<T>::const_iterator operator+(typename StableList<T>::difference_type n,
                                                 typename StableList<T>::const_iterator it) {
    return it + n;
}

} // namespace il::core
