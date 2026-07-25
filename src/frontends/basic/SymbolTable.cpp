//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/SymbolTable.cpp
// Purpose: Implements canonical BASIC symbol lookup, classification, slot
//          metadata, literal-label retention, and implicit field scopes.
// Key invariants:
//   - Main procedure symbols are searched before field scopes.
//   - Field scopes are searched from innermost to outermost.
//   - All public name-based operations canonicalize ASCII/locale bytes to
//     lowercase while preserving BASIC type suffixes.
//   - String labels survive procedure resets for cross-procedure deduplication.
// Ownership/Lifetime:
//   - The table owns symbol records and field-scope maps but borrows each
//     active ClassLayout.
// Links: src/frontends/basic/SymbolTable.hpp,
//        src/frontends/basic/LowererTypes.hpp,
//        src/frontends/basic/TypeSuffix.hpp,
//        src/frontends/basic/Lowerer.hpp
//
//===----------------------------------------------------------------------===//

#include "frontends/basic/SymbolTable.hpp"

#include "frontends/basic/TypeSuffix.hpp"

#include <cctype>

/// @file
/// @brief Implements case-insensitive BASIC symbol and field-scope management.

namespace il::frontends::basic {
namespace {

/// @brief Produce the case-insensitive storage key for BASIC symbols.
/// @details Preserves BASIC type suffix characters while folding ASCII letters
///          to lowercase. This keeps `A$` distinct from `A` while making
///          `Counter` and `counter` resolve to the same symbol.
/// @param name Symbol spelling supplied by a caller.
/// @return Canonical map key.
/// @note Conversion uses the active C locale and preserves every byte position,
///       including BASIC type-suffix punctuation.
std::string canonicalSymbolKey(std::string_view name) {
    std::string key;
    key.reserve(name.size());
    for (unsigned char ch : name)
        key.push_back(static_cast<char>(std::tolower(ch)));
    return key;
}

} // namespace

// =============================================================================
// Core Symbol Operations
// =============================================================================

/// @brief Get or create the symbol entry for @p name, initialized with BASIC defaults.
/// @param name Case-insensitive symbol spelling canonicalized for main storage.
/// @return Reference to the (existing or newly inserted) symbol info.
/// @post A newly inserted record has I64 default type, no explicit type, and
///       all classification, reference, and storage flags cleared.
SymbolInfo &SymbolTable::define(std::string_view name) {
    std::string key = canonicalSymbolKey(name);
    auto [it, inserted] = symbols_.emplace(std::move(key), SymbolInfo{});
    if (inserted) {
        // Initialize with BASIC defaults
        it->second.type = AstType::I64;
        it->second.hasType = false;
        it->second.isArray = false;
        it->second.isBoolean = false;
        it->second.referenced = false;
        it->second.isObject = false;
        it->second.objectClass.clear();
        it->second.isStatic = false;
        it->second.isByRefParam = false;
    }
    return it->second;
}

/// @brief Look up a symbol (mutable), checking the main table then field scopes.
/// @param name Case-insensitive symbol spelling.
/// @return Pointer to the main-table record or innermost field record, or
///         @c nullptr if undefined.
SymbolInfo *SymbolTable::lookup(std::string_view name) {
    // Check main symbol table first (heterogeneous lookup, no allocation)
    const std::string key = canonicalSymbolKey(name);
    if (auto it = symbols_.find(key); it != symbols_.end())
        return &it->second;

    // Fall back to field scopes
    return lookupInFieldScopes(key);
}

/// @brief Look up a symbol (const), checking the main table then field scopes.
/// @param name Case-insensitive symbol spelling.
/// @return Const pointer to the main-table record or innermost field record, or
///         @c nullptr if undefined.
const SymbolInfo *SymbolTable::lookup(std::string_view name) const {
    // Heterogeneous lookup, no allocation
    const std::string key = canonicalSymbolKey(name);
    if (auto it = symbols_.find(key); it != symbols_.end())
        return &it->second;

    return lookupInFieldScopes(key);
}

/// @brief Test whether a symbol (in the main table or a field scope) exists.
/// @param name Case-insensitive symbol spelling.
/// @return @c true when normal lookup finds a main or field record.
bool SymbolTable::contains(std::string_view name) const {
    return lookup(name) != nullptr;
}

/// @brief Remove a symbol from the main table.
/// @param name Case-insensitive main-table symbol spelling.
/// @return @c true if a symbol was erased; @c false if it was not present.
/// @note Field-scope records are not affected.
bool SymbolTable::remove(std::string_view name) {
    // Heterogeneous lookup for erase
    const std::string key = canonicalSymbolKey(name);
    auto it = symbols_.find(key);
    if (it != symbols_.end()) {
        symbols_.erase(it);
        return true;
    }
    return false;
}

/// @brief Reset per-procedure symbol state while preserving interned string literals.
/// @details Symbols carrying a string label are kept (with their mutable state reset) for
///          cross-procedure literal deduplication; all others are erased. Field scopes are
///          cleared.
/// @post Retained records preserve only their string labels; type,
///       classification, reference, and slot metadata return to defaults.
void SymbolTable::resetForNewProcedure() {
    // Preserve symbols with string labels (for literal deduplication)
    for (auto it = symbols_.begin(); it != symbols_.end();) {
        SymbolInfo &info = it->second;
        if (!info.stringLabel.empty()) {
            // Reset mutable state but keep the string label
            info.type = AstType::I64;
            info.hasType = false;
            info.isArray = false;
            info.isBoolean = false;
            info.referenced = false;
            info.isObject = false;
            info.objectClass.clear();
            info.slotId.reset();
            info.arrayLengthSlot.reset();
            info.isStatic = false;
            info.isByRefParam = false;
            ++it;
        } else {
            it = symbols_.erase(it);
        }
    }

    // Clear field scopes
    fieldScopes_.clear();
}

/// @brief Remove all symbols and field scopes.
/// @post @ref symbols_ and @ref fieldScopes_ are empty.
void SymbolTable::clear() {
    symbols_.clear();
    fieldScopes_.clear();
}

// =============================================================================
// Type Operations
// =============================================================================

/// @brief Set a symbol's explicit type (marking it boolean when applicable).
/// @param name Main-table symbol to define or update.
/// @param type BASIC semantic type to store.
/// @post The record has explicit type metadata and is Boolean only when it is a
///       non-array Boolean symbol.
void SymbolTable::setType(std::string_view name, AstType type) {
    auto &info = define(name);
    info.type = type;
    info.hasType = true;
    info.isBoolean = !info.isArray && type == AstType::Bool;
}

/// @brief Get a symbol's recorded type.
/// @param name Symbol resolved through normal lookup precedence.
/// @return The stored type, or @c std::nullopt if the symbol is undefined.
std::optional<SymbolTable::AstType> SymbolTable::getType(std::string_view name) const {
    const auto *info = lookup(name);
    if (!info)
        return std::nullopt;
    return info->type;
}

/// @brief Test whether a symbol has an explicitly assigned (vs inferred) type.
/// @param name Symbol resolved through normal lookup precedence.
/// @return @c true when the symbol exists and its type has been recorded.
bool SymbolTable::hasExplicitType(std::string_view name) const {
    const auto *info = lookup(name);
    return info && info->hasType;
}

// =============================================================================
// Symbol Classification
// =============================================================================

/// @brief Mark a symbol as referenced, inferring its type if not already explicit.
/// @param name Symbol name.
/// @param inferredType Optional type to apply; falls back to BASIC suffix-based inference.
/// @post A non-empty name exists in the main table, has a recorded type, and
///       carries the referenced flag.
void SymbolTable::markReferenced(std::string_view name, std::optional<AstType> inferredType) {
    if (name.empty())
        return;

    auto &info = define(name);

    // Apply inferred type if symbol doesn't have an explicit type
    if (!info.hasType) {
        if (inferredType) {
            info.type = *inferredType;
        } else {
            // Fall back to suffix-based inference
            info.type = inferAstTypeFromName(name);
        }
        info.hasType = true;
        info.isBoolean = !info.isArray && info.type == AstType::Bool;
    }

    info.referenced = true;
}

/// @brief Mark a symbol as an array (pointer-typed; clears the boolean flag).
/// @param name Main-table symbol to define and classify; empty is ignored.
void SymbolTable::markArray(std::string_view name) {
    if (name.empty())
        return;

    auto &info = define(name);
    info.isArray = true;
    // Arrays are pointer-typed; clear boolean flag
    if (info.isBoolean)
        info.isBoolean = false;
}

/// @brief Mark a symbol as a STATIC local (persists across procedure calls).
/// @param name Main-table symbol to define and classify; empty is ignored.
void SymbolTable::markStatic(std::string_view name) {
    if (name.empty())
        return;

    auto &info = define(name);
    info.isStatic = true;
}

/// @brief Mark a symbol as an object reference of the given class.
/// @param name Symbol name.
/// @param className Owning class name (recorded as the symbol's object class).
/// @post A non-empty @p name is classified as an object with recorded type
///       metadata; primitive type storage is otherwise unchanged.
void SymbolTable::markObject(std::string_view name, std::string className) {
    if (name.empty())
        return;

    auto &info = define(name);
    info.isObject = true;
    info.objectClass = std::move(className);
    info.hasType = true;
}

/// @brief Mark a symbol as a by-reference parameter (borrowed; not released by the callee).
/// @param name Main-table symbol to define and classify; empty is ignored.
void SymbolTable::markByRef(std::string_view name) {
    if (name.empty())
        return;

    auto &info = define(name);
    info.isByRefParam = true;
}

// =============================================================================
// Symbol Query
// =============================================================================

/// @brief True if the named symbol is an array.
/// @param name Symbol resolved through normal lookup precedence.
/// @return @c true when the symbol exists and is classified as an array.
bool SymbolTable::isArray(std::string_view name) const {
    const auto *info = lookup(name);
    return info && info->isArray;
}

/// @brief True if the named symbol is an object reference.
/// @param name Symbol resolved through normal lookup precedence.
/// @return @c true when the symbol exists and is classified as an object.
bool SymbolTable::isObject(std::string_view name) const {
    const auto *info = lookup(name);
    return info && info->isObject;
}

/// @brief True if the named symbol is a STATIC local.
/// @param name Symbol resolved through normal lookup precedence.
/// @return @c true when the symbol exists and is classified as static.
bool SymbolTable::isStatic(std::string_view name) const {
    const auto *info = lookup(name);
    return info && info->isStatic;
}

/// @brief True if the named symbol is a by-reference parameter.
/// @param name Symbol resolved through normal lookup precedence.
/// @return @c true when the symbol exists and is classified as BYREF.
bool SymbolTable::isByRef(std::string_view name) const {
    const auto *info = lookup(name);
    return info && info->isByRefParam;
}

/// @brief True if the named symbol has been referenced.
/// @param name Symbol resolved through normal lookup precedence.
/// @return @c true when the symbol exists and carries the referenced flag.
bool SymbolTable::isReferenced(std::string_view name) const {
    const auto *info = lookup(name);
    return info && info->referenced;
}

/// @brief Get the object-class name of a symbol, or "" if it is not an object.
/// @param name Symbol resolved through normal lookup precedence.
/// @return Stored class name, or an empty string for absent/non-object symbols.
std::string SymbolTable::getObjectClass(std::string_view name) const {
    const auto *info = lookup(name);
    if (!info || !info->isObject)
        return {};
    return info->objectClass;
}

// =============================================================================
// Slot Management
// =============================================================================

/// @brief Associate a symbol with its IL storage-slot id.
/// @param name Main-table symbol to define or update.
/// @param slotId Lowerer-assigned storage slot identifier.
void SymbolTable::setSlotId(std::string_view name, unsigned slotId) {
    auto &info = define(name);
    info.slotId = slotId;
}

/// @brief Get a symbol's storage-slot id, or nullopt if unset/undefined.
/// @param name Symbol resolved through normal lookup precedence.
/// @return Stored slot identifier, or @c std::nullopt when absent or unset.
std::optional<unsigned> SymbolTable::getSlotId(std::string_view name) const {
    const auto *info = lookup(name);
    if (!info)
        return std::nullopt;
    return info->slotId;
}

/// @brief Associate an array symbol with the slot holding its length.
/// @param name Main-table symbol to define or update.
/// @param slotId Storage slot containing the materialized array length.
void SymbolTable::setArrayLengthSlot(std::string_view name, unsigned slotId) {
    auto &info = define(name);
    info.arrayLengthSlot = slotId;
}

/// @brief Get an array symbol's length-slot id, or nullopt if unset/undefined.
/// @param name Symbol resolved through normal lookup precedence.
/// @return Stored length-slot identifier, or @c std::nullopt when absent or
///         unset.
std::optional<unsigned> SymbolTable::getArrayLengthSlot(std::string_view name) const {
    const auto *info = lookup(name);
    if (!info)
        return std::nullopt;
    return info->arrayLengthSlot;
}

// =============================================================================
// String Literal Caching
// =============================================================================

/// @brief Record the IL global label of an interned string literal for a symbol.
/// @param name Main-table symbol to define or update.
/// @param label Global label transferred into the symbol record.
void SymbolTable::setStringLabel(std::string_view name, std::string label) {
    auto &info = define(name);
    info.stringLabel = std::move(label);
}

/// @brief Get a symbol's interned string-literal label, or "" if none.
/// @param name Symbol resolved through normal lookup precedence.
/// @return Cached global label, or an empty string when absent or unset.
std::string SymbolTable::getStringLabel(std::string_view name) const {
    const auto *info = lookup(name);
    if (!info)
        return {};
    return info->stringLabel;
}

/// @brief Test whether a symbol carries an interned string-literal label.
/// @param name Symbol resolved through normal lookup precedence.
/// @return @c true when the symbol exists and its label is non-empty.
bool SymbolTable::hasStringLabel(std::string_view name) const {
    const auto *info = lookup(name);
    return info && !info->stringLabel.empty();
}

// =============================================================================
// Field Scope Management
// =============================================================================

/// @brief Push a field scope for the given class layout (active during method lowering).
/// @param layout Class layout whose fields become implicitly resolvable (null pushes an empty
///        scope).
/// @details Pre-populates the scope with a symbol per layout field so unqualified field names
///          resolve to the implicit `ME` receiver. Field types, array flags,
///          Boolean classification, and object class names are copied; other
///          mutable symbol state starts unset.
/// @post One new innermost scope is active even when @p layout is null.
void SymbolTable::pushFieldScope(const ClassLayout *layout) {
    FieldScope scope;
    scope.layout = layout;

    if (layout) {
        // Populate field symbols from the class layout
        for (const auto &field : layout->fields) {
            SymbolInfo info;
            info.type = field.type;
            info.hasType = true;
            info.isArray = field.isArray;
            info.isBoolean = (field.type == AstType::Bool);
            info.referenced = false;
            info.isObject = !field.objectClassName.empty();
            info.objectClass = field.objectClassName;
            scope.symbols.emplace(canonicalSymbolKey(field.name), std::move(info));
        }
    }

    fieldScopes_.push_back(std::move(scope));
}

/// @brief Pop the innermost field scope (no-op if none is active).
/// @post A non-empty scope stack has lost exactly its former final scope.
void SymbolTable::popFieldScope() {
    if (!fieldScopes_.empty())
        fieldScopes_.pop_back();
}

/// @brief Test whether @p name resolves to a field in any active field scope.
/// @param name Case-insensitive field spelling; empty is rejected.
/// @return @c true when any active scope contains the canonical key.
bool SymbolTable::isFieldInScope(std::string_view name) const {
    if (name.empty())
        return false;

    // Heterogeneous lookup, no allocation
    for (auto it = fieldScopes_.rbegin(); it != fieldScopes_.rend(); ++it) {
        if (it->symbols.find(canonicalSymbolKey(name)) != it->symbols.end())
            return true;
    }
    return false;
}

/// @brief Return the innermost active field scope, or nullptr if none.
/// @return Pointer to the final scope in @ref fieldScopes_, or @c nullptr.
const FieldScope *SymbolTable::activeFieldScope() const {
    if (fieldScopes_.empty())
        return nullptr;
    return &fieldScopes_.back();
}

// =============================================================================
// Internal Helpers
// =============================================================================

/// @brief Search active field scopes (innermost first) for a field symbol (mutable).
/// @param name Already canonicalized field key.
/// @return Pointer to the field's symbol info, or @c nullptr if not found.
SymbolInfo *SymbolTable::lookupInFieldScopes(std::string_view name) {
    // Heterogeneous lookup, no allocation
    for (auto scopeIt = fieldScopes_.rbegin(); scopeIt != fieldScopes_.rend(); ++scopeIt) {
        auto symIt = scopeIt->symbols.find(name);
        if (symIt != scopeIt->symbols.end())
            return &symIt->second;
    }
    return nullptr;
}

/// @brief Search active field scopes (innermost first) for a field symbol (const).
/// @param name Already canonicalized field key.
/// @return Pointer to the field's symbol info, or @c nullptr if not found.
const SymbolInfo *SymbolTable::lookupInFieldScopes(std::string_view name) const {
    // Heterogeneous lookup, no allocation
    for (auto scopeIt = fieldScopes_.rbegin(); scopeIt != fieldScopes_.rend(); ++scopeIt) {
        auto symIt = scopeIt->symbols.find(name);
        if (symIt != scopeIt->symbols.end())
            return &symIt->second;
    }
    return nullptr;
}

} // namespace il::frontends::basic
