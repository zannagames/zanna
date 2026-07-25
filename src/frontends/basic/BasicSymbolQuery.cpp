//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/BasicSymbolQuery.cpp
// Purpose: Implement read-only BASIC lowering symbol and type queries.
// Ownership/Lifetime: Each facade borrows a Lowerer for its full lifetime.
// Links: src/frontends/basic/BasicSymbolQuery.hpp,
//        src/frontends/basic/Lowerer.hpp,
//        src/frontends/basic/SemanticAnalyzer.hpp
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Implements the BASIC symbol query facade.
/// @details Provides the out-of-line definitions for @ref BasicSymbolQuery, a
///          lightweight adapter around the lowerer and semantic analyzer. The
///          methods are read-only and simply translate queries into existing
///          APIs without caching or mutating state.
//
//===----------------------------------------------------------------------===//

#include "frontends/basic/BasicSymbolQuery.hpp"

#include "frontends/basic/Lowerer.hpp"
#include "frontends/basic/SemanticAnalyzer.hpp"

namespace il::frontends::basic {

/// @brief Construct a query facade bound to a lowerer.
/// @details Stores a reference to the lowerer, which is used for all subsequent
///          symbol and type queries.
/// @param lowerer Lowerer providing access to symbol tables and semantics.
/// @warning @p lowerer must outlive the constructed facade.
BasicSymbolQuery::BasicSymbolQuery(const Lowerer &lowerer) noexcept : lowerer_(lowerer) {}

// =============================================================================
// Module-Level Queries
// =============================================================================

/// @brief Check whether a name refers to a module-level symbol.
/// @details Delegates to the semantic analyzer when available; returns false
///          if semantic analysis is not attached.
/// @param name Symbol name to query.
/// @return True if the symbol is module-level.
bool BasicSymbolQuery::isModuleLevelGlobal(std::string_view name) const {
    const auto *sema = lowerer_.semanticAnalyzer();
    if (!sema)
        return false;
    return sema->isModuleLevelSymbol(std::string(name));
}

/// @brief Check whether a symbol is tracked as a cross-procedure global.
/// @details Delegates to the lowerer's tracking state without consulting the
///          semantic analyzer.
/// @param name Symbol name to query.
/// @return True if the symbol is a cross-procedure global.
bool BasicSymbolQuery::isCrossProcGlobal(std::string_view name) const {
    return lowerer_.isCrossProcGlobal(std::string(name));
}

// =============================================================================
// Symbol Type Queries
// =============================================================================

/// @brief Determine whether a symbol represents an array.
/// @details Looks up the symbol in the lowerer's table and returns the array flag.
/// @param name Symbol name to query.
/// @return True when the symbol is an array.
bool BasicSymbolQuery::isSymbolArray(std::string_view name) const {
    const auto *info = lowerer_.findSymbol(name);
    return info && info->isArray;
}

/// @brief Determine whether a symbol represents an object instance.
/// @details Looks up the symbol in the lowerer's table and returns the object flag.
/// @param name Symbol name to query.
/// @return True when the symbol is an object.
bool BasicSymbolQuery::isSymbolObject(std::string_view name) const {
    const auto *info = lowerer_.findSymbol(name);
    return info && info->isObject;
}

/// @brief Check whether a symbol has an explicit type annotation.
/// @details Returns true only when the symbol exists and carries an explicit type.
/// @param name Symbol name to query.
/// @return True when an explicit type is recorded.
bool BasicSymbolQuery::hasExplicitType(std::string_view name) const {
    const auto *info = lowerer_.findSymbol(name);
    return info && info->hasType;
}

/// @brief Retrieve the declared or inferred type for a symbol.
/// @details Returns the type stored in the lowerer's symbol table, or
///          `std::nullopt` when the symbol is unknown.
/// @param name Symbol name to query.
/// @return Optional BASIC type for the symbol.
std::optional<Type> BasicSymbolQuery::getSymbolType(std::string_view name) const {
    const auto *info = lowerer_.findSymbol(name);
    if (!info)
        return std::nullopt;
    return info->type;
}

/// @brief Retrieve the element type for an array symbol.
/// @details Returns the symbol's element type when the symbol exists and is an
///          array; otherwise returns `std::nullopt`.
/// @param name Array symbol name to query.
/// @return Optional BASIC type for the array elements.
std::optional<Type> BasicSymbolQuery::getArrayElementType(std::string_view name) const {
    const auto *info = lowerer_.findSymbol(name);
    if (!info || !info->isArray)
        return std::nullopt;
    return info->type;
}

// =============================================================================
// Object/Class Queries
// =============================================================================

/// @brief Resolve the class name associated with an object symbol.
/// @details Checks the symbol table first, then consults the module-level object
///          class cache as a fallback, even when no direct symbol entry exists.
/// @param name Symbol name to query.
/// @return Class name or empty string if none is known.
std::string BasicSymbolQuery::getObjectClassForSymbol(std::string_view name) const {
    // Check symbol lookup first
    const auto *info = lowerer_.findSymbol(name);
    if (info && info->isObject && !info->objectClass.empty())
        return info->objectClass;

    // Check module-level scalar object cache
    std::string moduleClass = lowerer_.lookupModuleArrayElemClass(name);
    if (!moduleClass.empty())
        return moduleClass;

    return {};
}

/// @brief Resolve the element class for a module-level object array.
/// @details Delegates to the lowerer's module array element class lookup.
/// @param name Array symbol name to query.
/// @return Class name or empty string if none is known.
std::string BasicSymbolQuery::getObjectArrayElementClass(std::string_view name) const {
    return lowerer_.lookupModuleArrayElemClass(name);
}

// =============================================================================
// Field Scope Queries
// =============================================================================

/// @brief Check whether a field name is in the current object scope.
/// @details Delegates to the lowerer's active field-scope tracking.
/// @param name Field name to query.
/// @return True when a field of that name is in scope.
bool BasicSymbolQuery::isFieldInScope(std::string_view name) const {
    return lowerer_.isFieldInScope(name);
}

// =============================================================================
// Semantic Analyzer Delegation
// =============================================================================

/// @brief Look up a symbol's inferred type from the semantic analyzer.
/// @details Queries the semantic analyzer for the symbol's inferred type and
///          maps it to the BASIC AST type enum used by lowering.
/// @param name Symbol name to query.
/// @return Optional BASIC type based on semantic inference.
std::optional<Type> BasicSymbolQuery::lookupInferredType(std::string_view name) const {
    const auto *sema = lowerer_.semanticAnalyzer();
    if (!sema)
        return std::nullopt;

    auto semaType = sema->lookupVarType(std::string(name));
    if (!semaType)
        return std::nullopt;

    // Convert SemanticAnalyzer::Type to AST Type
    using SemaType = SemanticAnalyzer::Type;
    switch (*semaType) {
        case SemaType::Int:
            return Type::I64;
        case SemaType::Float:
            return Type::F64;
        case SemaType::String:
            return Type::Str;
        case SemaType::Bool:
            return Type::Bool;
        default:
            return std::nullopt;
    }
}

} // namespace il::frontends::basic
