//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/common/StringTable.hpp
// Purpose: String literal interning and deduplication for all frontends.
//
// This module provides efficient string literal management during lowering:
//   - Interning: Each unique string content gets exactly one IL global
//   - Label generation: Deterministic ".L<id>" labels for reproducible output
//   - Caching: Fast lookup for previously-seen strings
//
// Key invariants:
//   * Each distinct content value produces exactly one global label.
//   * Labels increase monotonically in first-intern order.
//   * Failed or recursively reentrant emitter calls do not leave partial
//     entries in the table.
// Ownership: Owns content, labels, insertion order, and the emitter callback;
//            normally lives for one module-lowering operation.
// References: docs/internals/architecture.md, docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Declares deterministic string-literal interning for frontends.
//
//===----------------------------------------------------------------------===//
#pragma once

#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace il::frontends::common {

/// @brief String literal interning table for IL lowering.
///
/// Manages string literal deduplication and global label generation.
/// Each unique string content is assigned a deterministic label and
/// registered as an IL global exactly once.
class StringTable {
  public:
    /// @brief Callback type for registering string globals in IL.
    /// @param label The generated label (e.g., ".L0")
    /// @param content The string literal content
    using GlobalEmitter = std::function<void(const std::string &label, const std::string &content)>;

    /// @brief Default constructor creates an empty table.
    StringTable() = default;

    /// @brief Construct with a global emitter callback.
    /// @param emitter Callback invoked when a new string global is needed.
    explicit StringTable(GlobalEmitter emitter) : emitter_(std::move(emitter)) {}

    /// @brief Set the global emitter callback.
    /// @param emitter Callback invoked for each subsequently interned new string.
    void setEmitter(GlobalEmitter emitter) {
        emitter_ = std::move(emitter);
    }

    // =========================================================================
    // Core Operations
    // =========================================================================

    /// @brief Get or create a label for a string literal.
    /// @param content The string literal content.
    /// @return The IL global label for this string (e.g., ".L0").
    /// @details If this is the first time seeing this content, a new
    ///          global is registered via the emitter callback.
    /// @throws std::logic_error If the emitter recursively interns a new string.
    /// @throws std::overflow_error If the label counter is exhausted.
    /// @throws Any exception raised by allocation or the emitter; insertion is
    ///         rolled back before the exception propagates.
    [[nodiscard]] std::string intern(const std::string &content) {
        // Check if already interned
        auto it = stringToLabel_.find(content);
        if (it != stringToLabel_.end())
            return it->second;

        if (emitting_)
            throw std::logic_error("StringTable emitter must not recursively intern new strings");
        if (nextId_ == std::numeric_limits<std::size_t>::max())
            throw std::overflow_error("string label counter exhausted");

        std::string label = ".L" + std::to_string(nextId_);
        auto [inserted, didInsert] = stringToLabel_.emplace(content, label);
        if (!didInsert)
            return inserted->second;
        try {
            insertionOrder_.push_back(content);
        } catch (...) {
            stringToLabel_.erase(content);
            throw;
        }
        ++nextId_;
        try {
            emitting_ = true;
            if (emitter_)
                emitter_(label, content);
            emitting_ = false;
        } catch (...) {
            emitting_ = false;
            --nextId_;
            insertionOrder_.pop_back();
            stringToLabel_.erase(content);
            throw;
        }
        return label;
    }

    /// @brief Check if a string has already been interned.
    /// @param content The string literal content.
    /// @return True if the string is already in the table.
    [[nodiscard]] bool contains(const std::string &content) const {
        return stringToLabel_.find(content) != stringToLabel_.end();
    }

    /// @brief Look up a label without interning.
    /// @param content The string literal content.
    /// @return The label if found, empty string otherwise.
    [[nodiscard]] std::string lookup(const std::string &content) const {
        auto it = stringToLabel_.find(content);
        if (it != stringToLabel_.end())
            return it->second;
        return {};
    }

    // =========================================================================
    // Statistics and Debugging
    // =========================================================================

    /// @brief Get the number of unique strings interned.
    /// @return Number of content-to-label entries.
    [[nodiscard]] std::size_t size() const noexcept {
        return stringToLabel_.size();
    }

    /// @brief Check if the table is empty.
    /// @return True when no content is interned.
    [[nodiscard]] bool empty() const noexcept {
        return stringToLabel_.empty();
    }

    /// @brief Get the next label ID that would be assigned.
    /// @return Numeric suffix for the next new label.
    [[nodiscard]] std::size_t nextId() const noexcept {
        return nextId_;
    }

    // =========================================================================
    // Lifecycle Management
    // =========================================================================

    /// @brief Clear all interned strings and reset the label counter.
    /// @details Call this between modules (not between procedures).
    void clear() {
        stringToLabel_.clear();
        insertionOrder_.clear();
        nextId_ = 0;
    }

    // =========================================================================
    // Iteration
    // =========================================================================

    /// @brief Iterate over all interned strings.
    /// @tparam Func Callable accepting `(const std::string&, const std::string&)`.
    /// @param fn Callback receiving (label, content) pairs.
    template <typename Func> void forEach(Func &&fn) const {
        for (const auto &content : insertionOrder_)
            fn(stringToLabel_.at(content), content);
    }

  private:
    /// @brief Map from string content to assigned label.
    std::unordered_map<std::string, std::string> stringToLabel_;
    /// @brief Contents in first-intern order for deterministic iteration.
    std::vector<std::string> insertionOrder_;

    /// @brief Counter for deterministic label generation.
    std::size_t nextId_{0};

    /// @brief Callback for emitting global string definitions.
    GlobalEmitter emitter_;
    /// @brief Reentrancy guard set while invoking emitter_.
    bool emitting_{false};
};

} // namespace il::frontends::common
