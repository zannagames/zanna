//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Implements the string interner that assigns stable Symbol handles to unique
// strings.  The interner owns the canonical copies of the strings and provides
// constant-time lookup from handles back to their original text.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Realises the compact string interning facility used across IL.
/// @details Symbols provide fast, comparable handles for compiler identifiers.
///          This translation unit implements the storage and lookup routines so
///          headers remain lightweight while providing a single source of truth
///          for handle semantics.

#include "string_interner.hpp"

#include <utility>

namespace il::support {

/// @brief Construct an interner that caps the number of distinct symbols.
/// @param maxSymbols Maximum number of unique strings that may be interned.
/// @note The limit prevents unbounded growth in long-running tooling contexts.
StringInterner::StringInterner(uint32_t maxSymbols) noexcept : maxSymbols_(maxSymbols) {}

/// @brief Copy-construct an interner, rebuilding the lookup table for the clone.
/// @param other Source interner whose stored strings should be duplicated.
/// @details Copies the owned string storage and limit before invoking
///          @ref rebuildMap so the new instance immediately reconstructs the handle
///          lookup table.  The map is rebuilt rather than copied directly to
///          keep string_view keys pointing at the newly owned storage.
StringInterner::StringInterner(const StringInterner &other) : maxSymbols_(0) {
    std::lock_guard lock(other.mutex_);
    storage_ = other.storage_;
    maxSymbols_ = other.maxSymbols_;
    rebuildMap();
}

/// @brief Assign from another interner, copying storage while preserving limits.
/// @param other Source interner whose strings and configuration are cloned.
/// @return Reference to the receiving interner after assignment.
/// @details Self-assignment is handled explicitly; otherwise the string storage
///          and cap are copied before @ref rebuildMap refreshes the lookup map to
///          reference the new owned storage.
StringInterner &StringInterner::operator=(const StringInterner &other) {
    if (this == &other)
        return *this;
    StringInterner copy(other);
    std::lock_guard lock(mutex_);
    swapWith(copy);
    return *this;
}

/// @brief Move-construct an interner and rebuild view-backed lookup keys.
/// @param other Source interner whose storage is transferred.
/// @details The storage deque is moved first, then @ref rebuildMap recreates every
///          string_view key so it points into this object's storage. The source
///          remains valid but otherwise has the standard unspecified moved-from
///          state; its stale lookup map is explicitly cleared.
StringInterner::StringInterner(StringInterner &&other) : maxSymbols_(0) {
    std::lock_guard lock(other.mutex_);
    storage_ = std::move(other.storage_);
    maxSymbols_ = other.maxSymbols_;
    rebuildMap();
    other.map_.clear();
}

/// @brief Move-assign an interner and rebuild view-backed lookup keys.
/// @param other Source interner whose storage is transferred.
/// @return Reference to this interner.
/// @details Builds the new map against moved storage before replacing this
///          object's map, avoiding stale string_view keys after assignment.
StringInterner &StringInterner::operator=(StringInterner &&other) {
    if (this == &other)
        return *this;
    StringInterner moved(std::move(other));
    std::lock_guard lock(mutex_);
    swapWith(moved);
    return *this;
}

/// @brief Intern the given string and return its Symbol handle.
///
/// @details The interner stores owned strings in `storage_` and maps them to
///          their corresponding `Symbol` via `map_`.  When a string is interned
///          for the first time, a copy is appended to `storage_` and a new
///          symbol is created using the 1-based index of that storage slot.
///          Reinterning the same view simply returns the existing handle,
///          guaranteeing pointer stability.  Symbol id zero is reserved and
///          never produced so clients can use it as an "invalid" sentinel.
///
/// @param str String view to intern; copied when not already stored.
/// @return Stable Symbol handle identifying the string.
Symbol StringInterner::intern(std::string_view str) {
    std::lock_guard lock(mutex_);
    auto it = map_.find(str);
    if (it != map_.end())
        return it->second;
    if (storage_.size() >= maxSymbols_)
        return {};
    map_.reserve(map_.size() + 1);
    storage_.emplace_back(str);
    Symbol sym{static_cast<uint32_t>(storage_.size())};
    const std::string &stored = storage_.back();
    try {
        auto [inserted, didInsert] = map_.emplace(std::string_view{stored}, sym);
        if (!didInsert) {
            storage_.pop_back();
            return inserted->second;
        }
    } catch (...) {
        storage_.pop_back();
        throw;
    }
    return sym;
}

/// @brief Retrieve the interned string associated with a Symbol handle.
///
/// @details Valid symbols have identifiers in the range [1, storage_.size()].
///          Requests outside this range, including the reserved id zero, yield
///          an empty view to signal an invalid lookup.  The returned view refers
///          directly to owned storage and therefore inherits its lifetime from
///          the interner.
///
/// @param sym Symbol handle to resolve.
/// @return View of the stored string, or empty view for invalid handles.
std::string_view StringInterner::lookup(Symbol sym) const {
    auto value = lookupOptional(sym);
    if (!value)
        return {};
    return *value;
}

/// @brief Check whether a symbol belongs to this interner.
/// @param sym Candidate symbol handle.
/// @return True when @p sym indexes an owned interned string.
bool StringInterner::contains(Symbol sym) const {
    std::lock_guard lock(mutex_);
    return sym.id != 0 && sym.id <= storage_.size();
}

/// @brief Retrieve an interned string without conflating invalid and empty values.
/// @param sym Symbol handle to resolve.
/// @return Optional view of the stored string, or std::nullopt for invalid symbols.
std::optional<std::string_view> StringInterner::lookupOptional(Symbol sym) const {
    std::lock_guard lock(mutex_);
    if (sym.id == 0 || sym.id > storage_.size())
        return std::nullopt;
    return std::string_view{storage_[sym.id - 1]};
}

/// @brief Return the number of unique strings currently interned.
/// @return Size of the owned canonical string storage.
/// @details Acquires the interner mutex so the snapshot is safe relative to
///          concurrent interning and rollback operations.
size_t StringInterner::size() const {
    std::lock_guard lock(mutex_);
    return storage_.size();
}

/// @brief Roll the interner back to an earlier symbol count.
/// @param symbolCount Number of lowest-numbered symbols to retain.
/// @details Removes the storage and lookup entries for the newest symbols until
///          the requested count is reached. Existing lower identifiers preserve
///          their text; a count above the current size leaves the interner intact.
/// @warning String views and symbols in the removed suffix no longer identify
///          their former strings and must be discarded by the caller.
void StringInterner::truncate(size_t symbolCount) {
    std::lock_guard lock(mutex_);
    while (storage_.size() > symbolCount) {
        map_.erase(std::string_view{storage_.back()});
        storage_.pop_back();
    }
}

/// @brief Rebuild the string-to-symbol lookup table from owned storage.
/// @details Clears the existing map, reserves enough buckets for the stored
///          strings, and re-inserts each value with a @ref Symbol whose id is the
///          1-based index into @ref storage_.  The method is used after copying
///          to ensure string_view keys reference the correct storage buffer.
void StringInterner::rebuildMap() {
    map_ = buildMapForStorage(storage_);
}

/// @brief Build a lookup map whose string_view keys point into supplied storage.
/// @param storage String storage that owns every key's bytes.
/// @return Fully populated intern map for the supplied storage.
StringInterner::InternMap StringInterner::buildMapForStorage(
    const std::deque<std::string> &storage) {
    InternMap map;
    map.reserve(storage.size());
    uint32_t id = 1;
    for (const std::string &value : storage) {
        map.emplace(std::string_view{value}, Symbol{id++});
    }
    return map;
}

/// @brief Swap every storage component with another interner.
/// @param other Interner whose state should be exchanged with this object.
/// @details The lookup map and string storage must move together because map keys
///          are string_view references into the storage deque.  Assignment
///          operators use this helper after preparing a fully rebuilt temporary
///          interner so this object is replaced atomically from the caller's view.
void StringInterner::swapWith(StringInterner &other) noexcept {
    using std::swap;
    swap(map_, other.map_);
    swap(storage_, other.storage_);
    swap(maxSymbols_, other.maxSymbols_);
}
} // namespace il::support
