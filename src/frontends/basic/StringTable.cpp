//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/StringTable.cpp
// Purpose: Implements the common string interning table through the legacy
//          BASIC frontend translation-unit path.
// Key invariants:
//   - Equal literal byte sequences reuse one cached label.
//   - Fresh labels follow monotonically increasing .L<number> spelling unless
//     the counter is explicitly reset.
//   - The emitter runs at most once for each newly interned table entry.
// Ownership/Lifetime:
//   - The table owns literal-to-label mappings and its callable emitter.
//   - Callback arguments are borrowed only for the duration of invocation.
// Links: src/frontends/basic/StringTable.hpp,
//        src/frontends/common/StringTable.hpp,
//        src/frontends/basic/Lowerer.hpp
//
//===----------------------------------------------------------------------===//

#include "frontends/basic/StringTable.hpp"

/// @file
/// @brief Implements deterministic string literal interning and emission.

namespace il::frontends::basic {

/// @brief Construct a string table with an emitter callback.
/// @details Stores the provided callback and leaves the table empty; no globals
///          are emitted until @ref intern is invoked.
/// @param emitter Callback moved into the table and invoked for new literals;
///                an empty callable disables emission.
StringTable::StringTable(GlobalEmitter emitter) : emitter_(std::move(emitter)) {}

/// @brief Replace the global-emission callback.
/// @details Updates the callback used for subsequent @ref intern calls. Existing
///          cached strings are not re-emitted when the emitter changes.
/// @param emitter New callback moved into the table; an empty callable disables
///                future emission.
void StringTable::setEmitter(GlobalEmitter emitter) {
    emitter_ = std::move(emitter);
}

// =============================================================================
// Core Operations
// =============================================================================

/// @brief Return the label for a string literal, creating it if necessary.
/// @details If the literal was already interned, returns the cached label. If
///          not, generates the next deterministic label (".L<id>"), invokes the
///          emitter callback if present, records the mapping, and returns it.
/// @param content Exact literal byte sequence to intern.
/// @return Deterministic label associated with @p content.
/// @post A previously absent literal is cached and has advanced @ref nextId_ by
///       one; a cache hit changes no table state and invokes no callback.
std::string StringTable::intern(const std::string &content) {
    // Check if already interned
    auto it = stringToLabel_.find(content);
    if (it != stringToLabel_.end())
        return it->second;

    // Generate new label
    std::string label = generateLabel();

    // Register the global via callback
    if (emitter_)
        emitter_(label, content);

    // Cache and return
    stringToLabel_.emplace(content, label);
    return label;
}

/// @brief Check whether a string literal has been interned.
/// @param content Exact literal byte sequence to query.
/// @return @c true if the table already contains a label for @p content.
bool StringTable::contains(const std::string &content) const {
    return stringToLabel_.find(content) != stringToLabel_.end();
}

/// @brief Look up a label without inserting a new entry.
/// @details This does not call the emitter or allocate new labels.
/// @param content Exact literal byte sequence to look up.
/// @return Cached label, or an empty string if not interned.
std::string StringTable::lookup(const std::string &content) const {
    auto it = stringToLabel_.find(content);
    if (it != stringToLabel_.end())
        return it->second;
    return {};
}

// =============================================================================
// Statistics and Debugging
// =============================================================================

/// @brief Return the number of unique strings in the table.
/// @return Number of cached content-to-label entries.
std::size_t StringTable::size() const noexcept {
    return stringToLabel_.size();
}

/// @brief Check whether the table is empty.
/// @return @c true when no literal mappings are cached.
bool StringTable::empty() const noexcept {
    return stringToLabel_.empty();
}

/// @brief Report the next label id that would be assigned.
/// @details Useful for debugging and deterministic output checks.
/// @return Numeric suffix used by the next call to @ref generateLabel.
std::size_t StringTable::nextId() const noexcept {
    return nextId_;
}

// =============================================================================
// Lifecycle Management
// =============================================================================

/// @brief Remove all cached literals and reset the label counter.
/// @details This does not emit any diagnostics or callbacks; it simply clears
///          the local cache and returns the table to its initial state.
/// @post @ref empty returns @c true and @ref nextId returns zero; the configured
///       emitter is unchanged.
void StringTable::clear() {
    stringToLabel_.clear();
    nextId_ = 0;
}

/// @brief Reset the label counter without clearing cached literals.
/// @details Intended for specialized tooling; if existing entries remain, the
///          next generated label may collide with earlier labels.
/// @post @ref nextId returns zero and all existing mappings remain intact.
/// @warning Callers must ensure any subsequent generated label cannot collide
///          with a still-live emitted global.
void StringTable::resetCounter() {
    nextId_ = 0;
}

// =============================================================================
// Internal Helpers
// =============================================================================

/// @brief Generate the next deterministic string label.
/// @details Labels are emitted as ".L<id>" where @c id is a monotonically
///          increasing counter.
/// @return Newly generated label string.
/// @post @ref nextId_ has increased by one.
std::string StringTable::generateLabel() {
    return ".L" + std::to_string(nextId_++);
}

} // namespace il::frontends::basic
