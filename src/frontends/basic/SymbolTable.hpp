//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/SymbolTable.hpp
// Purpose: Declares case-insensitive BASIC symbol metadata, storage-slot
//          bookkeeping, string-label caching, and implicit class-field scopes.
// Key invariants:
//   - Main-table and generated field keys use the same lowercase byte spelling.
//   - Procedure-local symbols take precedence over active implicit fields.
//   - Field scopes resolve from innermost to outermost.
//   - Procedure reset retains only entries carrying cached string labels.
// Ownership/Lifetime:
//   - SymbolTable owns all SymbolInfo records and FieldScope containers.
//   - Field scopes borrow ClassLayout pointers; layouts must outlive their
//     active scopes.
//   - Returned pointers and references are invalidated by applicable map or
//     scope mutation.
// Links: src/frontends/basic/SymbolTable.cpp,
//        src/frontends/basic/LowererTypes.hpp,
//        src/frontends/basic/Lowerer.hpp,
//        src/frontends/basic/OopLoweringContext.hpp
//
//===----------------------------------------------------------------------===//
#pragma once

#include "frontends/basic/LowererTypes.hpp"
#include "frontends/basic/ast/NodeFwd.hpp"
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

/// @file
/// @brief Declares unified symbol and implicit-field tracking for BASIC lowering.

namespace il::frontends::basic {

/// @brief Computed class field layout borrowed by active field scopes.
struct ClassLayout;

/// @brief Unified symbol table for BASIC variable and type tracking.
/// @details Consolidates definition, lookup, type inference, storage slots,
///          classification flags, cached literal labels, and implicit instance
///          fields. Main procedure symbols shadow fields of the same canonical
///          name.
/// @invariant Every key created through the public name-based API is the
///            lowercase byte form of the supplied BASIC identifier.
class SymbolTable {
  public:
    /// @brief BASIC semantic type stored in each symbol record.
    using AstType = ::il::frontends::basic::Type;

    /// @brief Default constructor creates an empty symbol table.
    /// @post @ref empty returns @c true and no field scope is active.
    SymbolTable() = default;

    // =========================================================================
    // Core Symbol Operations
    // =========================================================================

    /// @brief Ensure a symbol exists, creating with defaults if absent.
    /// @param name Symbol identifier (case-insensitive).
    /// @return Reference to the (possibly new) symbol record.
    /// @note This always targets the main table; an active field of the same
    ///       name is not returned.
    SymbolInfo &define(std::string_view name);

    /// @brief Look up a symbol without creating it.
    /// @param name Symbol identifier to find.
    /// @return Mutable pointer to a main symbol or innermost matching field, or
    ///         @c nullptr if not found.
    /// @warning Subsequent table or field-scope mutation may invalidate the
    ///          returned pointer.
    [[nodiscard]] SymbolInfo *lookup(std::string_view name);

    /// @brief Const lookup for read-only access.
    /// @param name Symbol identifier to find case-insensitively.
    /// @return Pointer to a main symbol or innermost matching field, or
    ///         @c nullptr when undefined.
    [[nodiscard]] const SymbolInfo *lookup(std::string_view name) const;

    /// @brief Check if a symbol is defined.
    /// @param name Symbol identifier to find across the main table and fields.
    /// @return @c true when @ref lookup finds a record.
    [[nodiscard]] bool contains(std::string_view name) const;

    /// @brief Remove a symbol from the table.
    /// @param name Main-table symbol identifier to erase.
    /// @return @c true if the symbol existed and was removed.
    /// @note Active field-scope entries are never removed by this operation.
    bool remove(std::string_view name);

    /// @brief Clear all symbols except those with cached string labels.
    /// @details Preserves string literal deduplication across procedures.
    void resetForNewProcedure();

    /// @brief Clear all symbols unconditionally.
    /// @post The main table and all field scopes are empty.
    void clear();

    // =========================================================================
    // Type Operations
    // =========================================================================

    /// @brief Set the declared type for a symbol.
    /// @param name Symbol identifier.
    /// @param type AST type to assign.
    void setType(std::string_view name, AstType type);

    /// @brief Get the type for a symbol if known.
    /// @param name Symbol identifier resolved through normal lookup precedence.
    /// @return Stored type, or @c std::nullopt when no record exists.
    /// @note A value may be returned even when @ref hasExplicitType is false
    ///       because every record carries a default type.
    [[nodiscard]] std::optional<AstType> getType(std::string_view name) const;

    /// @brief Check if a symbol has an explicitly declared type.
    /// @param name Symbol identifier resolved through normal lookup precedence.
    /// @return @c true when the record exists and @ref SymbolInfo::hasType is set.
    [[nodiscard]] bool hasExplicitType(std::string_view name) const;

    // =========================================================================
    // Symbol Classification
    // =========================================================================

    /// @brief Mark a symbol as referenced in the current procedure.
    /// @details Also infers type from name suffix if not already set.
    /// @param name Symbol identifier.
    /// @param inferredType Optional type from semantic analysis.
    void markReferenced(std::string_view name, std::optional<AstType> inferredType = std::nullopt);

    /// @brief Mark a symbol as an array.
    /// @param name Main-table symbol to define and classify; empty is ignored.
    void markArray(std::string_view name);

    /// @brief Mark a symbol as a STATIC procedure-local variable.
    /// @param name Main-table symbol to define and classify; empty is ignored.
    void markStatic(std::string_view name);

    /// @brief Mark a symbol as an object reference.
    /// @param name Symbol identifier.
    /// @param className Fully-qualified class name for the object type.
    void markObject(std::string_view name, std::string className);

    /// @brief Mark a symbol as a BYREF parameter.
    /// @param name Main-table symbol to define and classify; empty is ignored.
    void markByRef(std::string_view name);

    // =========================================================================
    // Symbol Query
    // =========================================================================

    /// @brief Check if symbol is an array.
    /// @param name Symbol resolved through normal lookup precedence.
    /// @return @c true when the symbol exists and is classified as an array.
    [[nodiscard]] bool isArray(std::string_view name) const;

    /// @brief Check if symbol is an object reference.
    /// @param name Symbol resolved through normal lookup precedence.
    /// @return @c true when the symbol exists and is an object reference.
    [[nodiscard]] bool isObject(std::string_view name) const;

    /// @brief Check if symbol is a STATIC variable.
    /// @param name Symbol resolved through normal lookup precedence.
    /// @return @c true when the symbol exists and is static.
    [[nodiscard]] bool isStatic(std::string_view name) const;

    /// @brief Check if symbol is a BYREF parameter.
    /// @param name Symbol resolved through normal lookup precedence.
    /// @return @c true when the symbol exists and is a by-reference parameter.
    [[nodiscard]] bool isByRef(std::string_view name) const;

    /// @brief Check if symbol has been referenced.
    /// @param name Symbol resolved through normal lookup precedence.
    /// @return @c true when the symbol exists and its referenced flag is set.
    [[nodiscard]] bool isReferenced(std::string_view name) const;

    /// @brief Get the object class name for an object symbol.
    /// @param name Symbol resolved through normal lookup precedence.
    /// @return Stored object class, or an empty string for absent/non-object
    ///         symbols.
    [[nodiscard]] std::string getObjectClass(std::string_view name) const;

    // =========================================================================
    // Slot Management
    // =========================================================================

    /// @brief Assign a slot ID to a symbol.
    /// @param name Main-table symbol to define or update.
    /// @param slotId Lowerer-assigned storage slot identifier.
    void setSlotId(std::string_view name, unsigned slotId);

    /// @brief Get the slot ID for a symbol.
    /// @param name Symbol resolved through normal lookup precedence.
    /// @return Stored slot ID, or @c std::nullopt when absent or unset.
    [[nodiscard]] std::optional<unsigned> getSlotId(std::string_view name) const;

    /// @brief Set the array length slot for a symbol.
    /// @param name Main-table symbol to define or update.
    /// @param slotId Slot holding the array's materialized length.
    void setArrayLengthSlot(std::string_view name, unsigned slotId);

    /// @brief Get the array length slot for a symbol.
    /// @param name Symbol resolved through normal lookup precedence.
    /// @return Stored length-slot ID, or @c std::nullopt when absent or unset.
    [[nodiscard]] std::optional<unsigned> getArrayLengthSlot(std::string_view name) const;

    // =========================================================================
    // String Literal Caching
    // =========================================================================

    /// @brief Cache a string literal label for deduplication.
    /// @param name Main-table symbol key associated with the literal.
    /// @param label IL global label moved into the record.
    void setStringLabel(std::string_view name, std::string label);

    /// @brief Get the cached string label for a symbol.
    /// @param name Symbol resolved through normal lookup precedence.
    /// @return Cached label, or an empty string when absent or unset.
    [[nodiscard]] std::string getStringLabel(std::string_view name) const;

    /// @brief Check if a symbol has a cached string label.
    /// @param name Symbol resolved through normal lookup precedence.
    /// @return @c true when the symbol exists and its label is non-empty.
    [[nodiscard]] bool hasStringLabel(std::string_view name) const;

    // =========================================================================
    // Field Scope Management (OOP)
    // =========================================================================

    /// @brief Push a field scope for class method lowering.
    /// @param layout Class layout providing field definitions.
    void pushFieldScope(const ClassLayout *layout);

    /// @brief Pop the current field scope.
    /// @post Removes the innermost scope when present; an empty stack is
    ///       unchanged.
    void popFieldScope();

    /// @brief Check if a name refers to a field in the current scope.
    /// @param name Field identifier searched from innermost to outermost scope.
    /// @return @c true when any active scope contains the canonical name.
    [[nodiscard]] bool isFieldInScope(std::string_view name) const;

    /// @brief Get the active field scope, if any.
    /// @return Pointer to the innermost active scope, or @c nullptr.
    /// @warning Pushing or popping a scope may invalidate the returned pointer.
    [[nodiscard]] const FieldScope *activeFieldScope() const;

    // =========================================================================
    // Iteration
    // =========================================================================

    /// @brief Iterate over all symbols (non-const).
    /// @details Visits main-table entries in unspecified hash iteration order;
    ///          active field scopes are excluded.
    /// @tparam Func Callable accepting canonical-name and mutable SymbolInfo
    ///              references.
    /// @param fn Callable invoked once per main-table entry.
    template <typename Func> void forEach(Func &&fn) {
        for (auto &[name, info] : symbols_)
            fn(name, info);
    }

    /// @brief Iterate over all symbols (const).
    /// @details Visits main-table entries in unspecified hash iteration order;
    ///          active field scopes are excluded.
    /// @tparam Func Callable accepting canonical-name and const SymbolInfo
    ///              references.
    /// @param fn Callable invoked once per main-table entry.
    template <typename Func> void forEach(Func &&fn) const {
        for (const auto &[name, info] : symbols_)
            fn(name, info);
    }

    /// @brief Get the number of symbols in the table.
    /// @return Number of main-table entries; field entries are excluded.
    [[nodiscard]] std::size_t size() const noexcept {
        return symbols_.size();
    }

    /// @brief Check if the table is empty.
    /// @return @c true when the main table has no entries, regardless of field
    ///         scope contents.
    [[nodiscard]] bool empty() const noexcept {
        return symbols_.empty();
    }

    // =========================================================================
    // Direct Access (for migration compatibility)
    // =========================================================================

    /// @brief Map type with heterogeneous lookup support.
    using SymbolMap = std::unordered_map<std::string, SymbolInfo, StringHash, std::equal_to<>>;

    /// @brief Get direct access to the underlying map.
    /// @note Prefer using define/lookup methods for new code.
    /// @return Mutable main-table map; field scopes are excluded.
    /// @warning Direct insertion must preserve canonical-key invariants.
    [[nodiscard]] SymbolMap &raw() noexcept {
        return symbols_;
    }

    /// @brief Get read-only direct access to the underlying main table.
    /// @return Const map reference; field scopes are excluded.
    [[nodiscard]] const SymbolMap &raw() const noexcept {
        return symbols_;
    }

  private:
    /// @brief Main symbol storage indexed by canonicalized name.
    SymbolMap symbols_;

    /// @brief Stack of field scopes for class method lowering.
    std::vector<FieldScope> fieldScopes_;

    /// @brief Internal helper to find symbol in field scopes.
    /// @param name Already canonicalized symbol key.
    /// @return Mutable pointer to the innermost match, or @c nullptr.
    [[nodiscard]] SymbolInfo *lookupInFieldScopes(std::string_view name);

    /// @brief Const field-scope lookup from innermost to outermost.
    /// @param name Already canonicalized symbol key.
    /// @return Const pointer to the innermost match, or @c nullptr.
    [[nodiscard]] const SymbolInfo *lookupInFieldScopes(std::string_view name) const;
};

} // namespace il::frontends::basic
