//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tools/common/ArgvView.hpp
// Purpose: Lightweight non-owning view over argv-style argument arrays.
// Key invariants: Never modifies or owns the underlying argument storage.
// Ownership/Lifetime: Borrows pointers from the C runtime; callers must ensure
//                     validity through the view's lifetime.
// Links: docs/internals/architecture.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Defines a non-owning, sliceable view over C-style argument arrays.
/// @details `ArgvView` packages an argument count and borrowed pointer without
///          allocating or modifying command-line storage. Suffix views share
///          the same underlying null-terminated argument strings.

#pragma once

#include <string_view>

namespace zanna::tools {

/// @brief Lightweight non-owning view over argv-style argument arrays.
/// @details Encapsulates the argument count and pointer pair supplied by the C
///          runtime so helpers can inspect and slice the list without copying.
/// @invariant A positive @ref argc requires non-null @ref argv and non-null
///            entries throughout the referenced range.
/// @ownership Borrows the array and strings; the caller keeps them alive.
struct ArgvView {
    /// @brief Number of arguments referenced by the view (may be zero).
    int argc;
    /// @brief Borrowed pointer to the argument array; not owned by the view.
    char **argv;

    /// @brief Determine whether the view contains no arguments.
    /// @return True when `argc` is non-positive or the pointer is null.
    [[nodiscard]] bool empty() const {
        return argc <= 0 || argv == nullptr;
    }

    /// @brief Access the first argument in the sequence.
    /// @details Returns an empty view when the sequence is empty.
    /// @return String view referencing the first argument.
    /// @note The returned view is invalidated with the underlying argument text.
    [[nodiscard]] std::string_view front() const {
        return empty() ? std::string_view{} : std::string_view(argv[0]);
    }

    /// @brief Read the argument at @p index, returning an empty view on overflow.
    /// @param index Offset into the argv array.
    /// @return View over the requested argument or empty view when invalid.
    /// @note An empty result can also represent a valid empty argument.
    [[nodiscard]] std::string_view at(int index) const {
        if (index < 0 || index >= argc || argv == nullptr) {
            return std::string_view{};
        }
        return std::string_view(argv[index]);
    }

    /// @brief Produce a suffix view that skips the first @p count entries.
    /// @param count Number of arguments to drop.
    /// @return New view representing the remaining arguments.
    /// @pre @p count is nonnegative and this view satisfies its pointer invariant.
    /// @details Dropping the full range or more yields the canonical empty view.
    ///          The returned view borrows the same argument storage.
    [[nodiscard]] ArgvView drop_front(int count = 1) const {
        if (count >= argc) {
            return ArgvView{0, nullptr};
        }
        return ArgvView{argc - count, argv + count};
    }
};

} // namespace zanna::tools
