//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/support/string_interner.hpp
// Purpose: Declares string interning and symbol types.
// Key invariants:
//   - Symbol id 0 is invalid; live symbols are one-based storage indices.
//   - Every map string_view points into the same object's owned deque.
//   - A successfully moved-from interner is empty and reusable.
// Ownership/Lifetime:
//   - The interner owns canonical string storage; returned views borrow it.
// Links: src/support/string_interner.cpp, src/support/symbol.hpp,
//        docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares the thread-safe string-to-symbol interning table.
/// @details The interner owns one canonical copy of each distinct string and
///          assigns compact, one-based @ref Symbol identifiers. Individual
///          operations are synchronized; borrowed lookup views remain stable
///          across further interning but not removal, assignment, or destruction.

#pragma once

#include "symbol.hpp"
#include <deque>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace il::support {

/// @brief Interns strings to provide stable Symbol identifiers.
/// @invariant Symbol 0 is reserved for invalid.
/// @ownership Stores copies of strings internally.
/// @details Unique strings are stored in a deque so appending new entries does
///          not invalidate views of earlier entries. Symbols are local to one
///          interner and may not be resolved meaningfully by another instance.
class StringInterner {
  public:
    /// @brief Construct an interner optionally bounded by @p maxSymbols.
    /// @param maxSymbols Maximum number of distinct strings that can be stored.
    /// @details The limit defaults to the full 32-bit Symbol address space.
    ///          Once reached, new unique strings produce the invalid symbol,
    ///          while already interned strings remain resolvable.
    explicit StringInterner(uint32_t maxSymbols = std::numeric_limits<uint32_t>::max()) noexcept;

    /// @brief Intern a string to produce a stable symbol for repeated use.
    /// @param str String to intern.
    /// @return Symbol uniquely identifying the interned string, or symbol zero
    ///         when the configured unique-string capacity is exhausted.
    /// @details Stores a copy of @p str if it has not been seen before. Repeated
    ///          calls with equal content return the existing symbol without
    ///          duplicating storage.
    Symbol intern(std::string_view str);

    /// @brief Retrieve the original string associated with a Symbol.
    /// @param sym Symbol previously returned by intern().
    /// @return View of the interned string, or an empty view when invalid.
    /// @details This convenience API cannot distinguish an invalid symbol from
    ///          a valid symbol whose interned text is empty; use
    ///          @ref lookupOptional when that distinction matters.
    std::string_view lookup(Symbol sym) const;

    /// @brief Check whether @p sym identifies a currently interned string.
    /// @param sym Symbol to validate against this interner.
    /// @return True when @p sym is nonzero and within the owned storage range.
    [[nodiscard]] bool contains(Symbol sym) const;

    /// @brief Retrieve an interned string while preserving invalid-vs-empty state.
    /// @param sym Symbol previously returned by intern().
    /// @return String view for a valid symbol, or std::nullopt for an invalid one.
    [[nodiscard]] std::optional<std::string_view> lookupOptional(Symbol sym) const;

    /// @brief Return the number of currently interned symbols.
    /// @return Count of canonical strings currently owned by the interner.
    [[nodiscard]] size_t size() const;

    /// @brief Remove symbols appended after a transaction checkpoint.
    /// @param symbolCount Number of earliest symbols to retain.
    /// @details Existing symbol ids remain stable because only a storage suffix
    ///          is removed. Values at or above the removed suffix may later be
    ///          assigned new text; views into removed strings are invalidated.
    ///          A count greater than the current size is a no-op.
    void truncate(size_t symbolCount);

    /// @brief Copy constructor.
    /// @details Deep-copies all interned strings and rebuilds the internal map
    ///          so that string_view keys point into the new storage.
    /// @param other Interner to copy from.
    StringInterner(const StringInterner &other);

    /// @brief Copy assignment operator.
    /// @details Replaces the current contents with a deep copy of @p other
    ///          and rebuilds the internal map.
    /// @param other Interner to copy from.
    /// @return Reference to this interner.
    StringInterner &operator=(const StringInterner &other);

    /// @brief Move constructor.
    /// @details Moves the owned string storage and rebuilds the string_view-keyed
    ///          lookup table so every key points into the destination object. On
    ///          success, @p other is empty and can intern new symbol 1.
    /// @param other Interner to move from; left empty and reusable on success.
    StringInterner(StringInterner &&other);

    /// @brief Move assignment operator.
    /// @details Replaces this interner with @p other's storage and rebuilds the
    ///          lookup table from the moved strings. On success, @p other is
    ///          empty and reusable.
    /// @param other Interner to move from; left empty on success.
    /// @return Reference to this interner.
    StringInterner &operator=(StringInterner &&other);

  private:
    /// @brief Heterogeneous hash enabling map lookups keyed by string_view.
    /// @details The @c is_transparent tag lets the unordered_map hash both
    ///          std::string and std::string_view identically, so intern() can
    ///          probe the map with a borrowed view instead of allocating a
    ///          temporary std::string for every query.
    struct TransparentHash {
        using is_transparent = void;

        /// @brief Hash a borrowed string view by content.
        /// @param sv String view to hash.
        /// @return Hash value compatible with every overload in this functor.
        size_t operator()(std::string_view sv) const noexcept {
            return std::hash<std::string_view>{}(sv);
        }

        /// @brief Hash an owned string using the borrowed-view implementation.
        /// @param s String to hash.
        /// @return Content hash compatible with the string-view overload.
        size_t operator()(const std::string &s) const noexcept {
            return (*this)(std::string_view{s});
        }
    };

    /// @brief Heterogeneous equality matching the TransparentHash key family.
    /// @details Provides every string/string_view comparison pairing so the map
    ///          can compare a borrowed lookup key against a stored key without an
    ///          intermediate allocation. Must agree with TransparentHash on which
    ///          keys are equal.
    struct TransparentEqual {
        using is_transparent = void;

        /// @brief Compare two borrowed views by string content.
        /// @param lhs Left-hand string view.
        /// @param rhs Right-hand string view.
        /// @return True when the byte sequences are equal.
        bool operator()(std::string_view lhs, std::string_view rhs) const noexcept {
            return lhs == rhs;
        }

        /// @brief Compare two owned strings by content.
        /// @param lhs Left-hand string.
        /// @param rhs Right-hand string.
        /// @return True when the byte sequences are equal.
        bool operator()(const std::string &lhs, const std::string &rhs) const noexcept {
            return lhs == rhs;
        }

        /// @brief Compare an owned string with a borrowed view.
        /// @param lhs Left-hand owned string.
        /// @param rhs Right-hand string view.
        /// @return True when the byte sequences are equal.
        bool operator()(const std::string &lhs, std::string_view rhs) const noexcept {
            return lhs == rhs;
        }

        /// @brief Compare a borrowed view with an owned string.
        /// @param lhs Left-hand string view.
        /// @param rhs Right-hand owned string.
        /// @return True when the byte sequences are equal.
        bool operator()(std::string_view lhs, const std::string &rhs) const noexcept {
            return lhs == rhs;
        }
    };

    using InternMap =
        std::unordered_map<std::string_view, Symbol, TransparentHash, TransparentEqual>;

    /// Serializes access to storage_ and map_ for shared tooling/server use.
    mutable std::mutex mutex_;
    /// Maps string content to assigned symbols for O(1) lookup during interning.
    InternMap map_;
    /// Retains copies of interned strings so lookups return stable views.
    std::deque<std::string> storage_;
    /// Maximum number of unique symbols representable by this interner.
    uint32_t maxSymbols_;

    /// @brief Rebuild the lookup map from storage after move operations.
    /// @details After a move assignment or copy, the map's string_view keys
    /// may become invalidated. This method reconstructs the map by iterating
    /// through the storage_ deque and creating fresh entries.
    void rebuildMap();

    /// @brief Build a lookup map whose keys point into @p storage.
    /// @param storage Owned string storage that will back every string_view key.
    /// @return A complete string-to-symbol map for @p storage.
    /// @details The helper builds into a temporary map so assignment operators can
    ///          prepare all throwing state before replacing the receiving object.
    static InternMap buildMapForStorage(const std::deque<std::string> &storage);

    /// @brief Swap all owned interner state with @p other.
    /// @param other Interner whose state should be exchanged with this object.
    /// @details The intern map stores string_view keys into the companion storage
    ///          deque.  Swapping both containers together preserves that pairing
    ///          and lets assignment operators provide strong replacement semantics.
    void swapWith(StringInterner &other) noexcept;
};
} // namespace il::support
