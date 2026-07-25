//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/common/ScopeTracker.hpp
// Purpose: Stack-based lexical scope tracker mapping source identifiers to
//          unique mangled IL names. Supports push/pop, RAII guards, and
//          innermost-to-outermost name resolution.
// Key invariants:
//   * Resolution searches from innermost to outermost scope.
//   * ScopedScope balances exactly one push/pop pair.
//   * Generated local IDs increase monotonically until reset().
// Ownership: Owns the scope stack and copied binding strings; ScopedScope
//            borrows its tracker for the guard lifetime.
// References: docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Declares lexical scope tracking and local-name mangling support.
//
//===----------------------------------------------------------------------===//
#pragma once

#include <cstdint>
#include <optional>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace il::frontends::common {

/// @brief Lexical scope tracker with name mangling support.
/// @details Manages a stack of scopes for symbol resolution. Each scope is a
///          map from source names to mangled IL names. Resolution searches
///          from innermost to outermost scope.
class ScopeTracker {
  public:
    /// @brief RAII guard for automatic scope management.
    /// @details Pushes a scope on construction and pops it on destruction,
    ///          ensuring proper scope balancing even with exceptions.
    class ScopedScope {
      public:
        /// @brief Push a new scope.
        /// @param st The scope tracker to manage.
        explicit ScopedScope(ScopeTracker &st) : st_(st) {
            st_.pushScope();
        }

        /// @brief Pop the scope when the guard is destroyed.
        ~ScopedScope() {
            st_.popScope();
        }

        // Non-copyable, non-movable
        ScopedScope(const ScopedScope &) = delete;
        ScopedScope &operator=(const ScopedScope &) = delete;
        ScopedScope(ScopedScope &&) = delete;
        ScopedScope &operator=(ScopedScope &&) = delete;

      private:
        /// @brief Tracker whose innermost scope is owned by this guard.
        ScopeTracker &st_;
    };

    /// @brief Reset the tracker to an empty state.
    /// @details Clears all scopes and resets the ID counter.
    void reset() {
        stack_.clear();
        nextId_ = 0;
    }

    /// @brief Push a new empty scope onto the stack.
    void pushScope() {
        stack_.emplace_back();
    }

    /// @brief Pop the innermost scope if one exists.
    void popScope() {
        if (!stack_.empty())
            stack_.pop_back();
    }

    /// @brief Bind a name to a mangled identifier in the current scope.
    /// @param name Source identifier.
    /// @param mapped Mangled IL identifier.
    /// @return True when inserted; false when @p name already exists in the
    ///         current scope.
    /// @throws std::logic_error If no scope is active.
    bool bind(const std::string &name, const std::string &mapped) {
        requireScope();
        return stack_.back().emplace(name, mapped).second;
    }

    /// @brief Check if a name is declared in the current (innermost) scope.
    /// @param name Source identifier to check.
    /// @return True if the name exists in the current scope.
    [[nodiscard]] bool isDeclaredInCurrentScope(const std::string &name) const {
        return !stack_.empty() && stack_.back().contains(name);
    }

    /// @brief Declare a new local and generate a unique mangled name.
    /// @param name Source identifier.
    /// @return The generated unique mangled name.
    /// @throws std::logic_error If no scope is active or @p name is duplicated.
    /// @throws std::overflow_error If the local ID counter is exhausted.
    std::string declareLocal(const std::string &name) {
        requireScope();
        if (stack_.back().contains(name))
            throw std::logic_error("duplicate local declaration");
        if (nextId_ == std::numeric_limits<uint64_t>::max())
            throw std::overflow_error("local name counter exhausted");
        std::string unique = name + "_" + std::to_string(nextId_++);
        stack_.back().emplace(name, unique);
        return unique;
    }

    /// @brief Declare a local with a specific mangled name.
    /// @param name Source identifier.
    /// @param mangledName The mangled name to use.
    /// @return True when inserted; false for a duplicate name in this scope.
    /// @throws std::logic_error If no scope is active.
    bool declareLocalAs(const std::string &name, const std::string &mangledName) {
        requireScope();
        return stack_.back().emplace(name, mangledName).second;
    }

    /// @brief Resolve a name by searching from innermost to outermost scope.
    /// @param name Source identifier to resolve.
    /// @return The mangled name if found, or std::nullopt.
    [[nodiscard]] std::optional<std::string> resolve(const std::string &name) const {
        for (auto it = stack_.rbegin(); it != stack_.rend(); ++it) {
            auto found = it->find(name);
            if (found != it->end())
                return found->second;
        }
        return std::nullopt;
    }

    /// @brief Check if any scope is currently active.
    /// @return True if the scope stack is non-empty.
    [[nodiscard]] bool hasScope() const {
        return !stack_.empty();
    }

    /// @brief Get the current scope depth.
    /// @return Number of scopes on the stack.
    [[nodiscard]] std::size_t depth() const {
        return stack_.size();
    }

    /// @brief Get the next unique ID without consuming it.
    /// @return Current value of the ID counter.
    [[nodiscard]] uint64_t peekNextId() const {
        return nextId_;
    }

    /// @brief Consume and return the next unique ID.
    /// @return A unique ID that can be used for mangling.
    /// @throws std::overflow_error If the 64-bit ID counter is exhausted.
    uint64_t nextId() {
        if (nextId_ == std::numeric_limits<uint64_t>::max())
            throw std::overflow_error("local name counter exhausted");
        return nextId_++;
    }

  private:
    /// @brief Scope maps ordered from outermost to innermost.
    std::vector<std::unordered_map<std::string, std::string>> stack_;

    /// @brief Require at least one active lexical scope.
    /// @throws std::logic_error If stack_ is empty.
    void requireScope() const {
        if (stack_.empty())
            throw std::logic_error("scope operation requires an active scope");
    }

    /// @brief ID that will be consumed by the next generated local name.
    uint64_t nextId_{0};
};

} // namespace il::frontends::common
