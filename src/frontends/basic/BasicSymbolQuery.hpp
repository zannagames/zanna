//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/BasicSymbolQuery.hpp
// Purpose: Lightweight facade for common symbol/type queries during lowering.
// Key invariants: All methods are const and perform no mutation; zero-copy
//                 where possible via string_view returns.
// Ownership/Lifetime: Holds one Lowerer reference and reaches SemanticAnalyzer
//                     through it; the Lowerer must outlive this object.
// Links: src/frontends/basic/BasicSymbolQuery.cpp,
//        src/frontends/basic/Lowerer.hpp,
//        src/frontends/basic/SemanticAnalyzer.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file BasicSymbolQuery.hpp
 * @brief Declares read-only symbol, type, scope, and class queries for lowering.
 *
 * The facade stores one borrowed Lowerer reference and performs each lookup
 * against live lowering or semantic-analysis state without caching results.
 */

#pragma once

#include "frontends/basic/ast/NodeFwd.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace il::frontends::basic {

class Lowerer;
class SemanticAnalyzer;
class OopIndex;

/// @brief Lightweight facade for symbol/type queries during BASIC lowering.
/// @details Consolidates common lookup patterns that appear across lowering,
///          scanning, and OOP code. This facade is cheap to construct (one
///          reference) and all methods are const, making it suitable for
///          passing by value to helper functions.
///
/// Common query patterns supported:
/// - isModuleLevelGlobal: Check if a symbol is defined at module scope
/// - getArrayElementType: Get the element type for array symbols
/// - getObjectClassForSymbol: Get the class name for object-typed symbols
/// - lookupClassLayout: Find compiled class layout information
/// - isSymbolArray/isSymbolObject: Quick type classification
///
/// Usage:
/// @code
///   BasicSymbolQuery query(lowerer);
///   if (query.isModuleLevelGlobal(name)) { ... }
///   if (auto cls = query.getObjectClassForSymbol(name)) { ... }
/// @endcode
class BasicSymbolQuery {
  public:
    /// @brief Construct a query facade from a lowerer.
    /// @param lowerer Lowering context providing symbol tables and OOP index.
    /// @warning @p lowerer must outlive this facade.
    explicit BasicSymbolQuery(const Lowerer &lowerer) noexcept;

    // =========================================================================
    // Module-Level Queries
    // =========================================================================

    /// @brief Check if a symbol is defined at module level.
    /// @param name Symbol identifier to check.
    /// @return True if the symbol was declared at module scope (not local).
    [[nodiscard]] bool isModuleLevelGlobal(std::string_view name) const;

    /// @brief Check if a module-level symbol is used across procedures.
    /// @details Cross-procedure globals require runtime-backed storage.
    /// @param name Symbol identifier to check.
    /// @return True if the symbol is shared between @main and other procedures.
    [[nodiscard]] bool isCrossProcGlobal(std::string_view name) const;

    // =========================================================================
    // Symbol Type Queries
    // =========================================================================

    /// @brief Check if a symbol represents an array.
    /// @param name Symbol identifier to check.
    /// @return True if the symbol is marked as array-typed.
    [[nodiscard]] bool isSymbolArray(std::string_view name) const;

    /// @brief Check if a symbol represents an object reference.
    /// @param name Symbol identifier to check.
    /// @return True if the symbol holds an object pointer.
    [[nodiscard]] bool isSymbolObject(std::string_view name) const;

    /// @brief Check if a symbol has an explicitly declared type.
    /// @param name Symbol identifier to check.
    /// @return True if type was set via DIM/CONST/param declaration.
    [[nodiscard]] bool hasExplicitType(std::string_view name) const;

    /// @brief Get the AST type for a symbol.
    /// @param name Symbol identifier to query.
    /// @return The declared or inferred type, or nullopt if unknown.
    [[nodiscard]] std::optional<Type> getSymbolType(std::string_view name) const;

    /// @brief Get the array element type for an array symbol.
    /// @param name Array symbol identifier.
    /// @return Element type if symbol is a known array, nullopt otherwise.
    [[nodiscard]] std::optional<Type> getArrayElementType(std::string_view name) const;

    // =========================================================================
    // Object/Class Queries
    // =========================================================================

    /// @brief Get the class name for an object-typed symbol.
    /// @details Checks direct symbol metadata first, then the lowerer's module
    ///          class cache used for array-element and object class tracking.
    /// @param name Symbol identifier to query.
    /// @return Qualified class name when either source knows one, empty otherwise.
    [[nodiscard]] std::string getObjectClassForSymbol(std::string_view name) const;

    /// @brief Get the element class for an object array.
    /// @details Directly queries the lowerer's module array-element class cache
    ///          without independently confirming the symbol's array type.
    /// @param name Array symbol identifier.
    /// @return Element class name if symbol is an object array, empty otherwise.
    [[nodiscard]] std::string getObjectArrayElementClass(std::string_view name) const;

    // =========================================================================
    // Field Scope Queries
    // =========================================================================

    /// @brief Check if a name refers to a field in the current class scope.
    /// @param name Identifier to check.
    /// @return True if name matches a field in the active field scope.
    [[nodiscard]] bool isFieldInScope(std::string_view name) const;

    // =========================================================================
    // Semantic Analyzer Delegation
    // =========================================================================

    /// @brief Look up the inferred type from semantic analysis.
    /// @details Converts scalar integer, float, string, and Boolean categories
    ///          to AST Type. Arrays, objects, unknown types, missing symbols,
    ///          and an absent analyzer yield `std::nullopt`.
    /// @param name Variable name to query.
    /// @return Inferred type if semantic analysis recorded one, nullopt otherwise.
    [[nodiscard]] std::optional<Type> lookupInferredType(std::string_view name) const;

  private:
    ///< Borrowed lowering context supplying every query's live state.
    const Lowerer &lowerer_;
};

} // namespace il::frontends::basic
