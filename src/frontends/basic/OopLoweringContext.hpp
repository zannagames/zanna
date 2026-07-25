//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/OopLoweringContext.hpp
// Purpose: Context object for OOP lowering pipeline providing cached metadata
//          lookups and name-mangling helpers.
// Key invariants: Provides consistent access to OOP metadata and lowering state.
//                 Context must not outlive the Lowerer or OopIndex it borrows.
// Ownership/Lifetime: Non-owning references to Lowerer and OopIndex; short-lived.
// Links: docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//

/// @file OopLoweringContext.hpp
/// @brief Declares cached OOP metadata and naming services for BASIC lowering.
/// @details The context borrows the active Lowerer and OopIndex, while owning
///          lookup caches keyed by the spellings supplied by callers. Both
///          successful and failed metadata lookups are cached.

#pragma once

#include <string>
#include <unordered_map>

namespace il::frontends::basic {

// Forward declarations to minimize dependencies
class Lowerer;
class OopIndex;
struct ClassInfo;
struct ClassLayout;
struct Expr;

/// @brief Context object passed through OOP lowering pipeline.
/// @details Provides a consistent interface for OOP lowering functions to access
///          metadata and emit IL, without directly coupling to Lowerer internals.
///          This context is created at the start of OOP-related lowering operations
///          and passed through the pipeline.
///
///          The context provides:
///          - Cached class metadata lookups (via findClassInfo)
///          - Cached class layout lookups (via findClassLayout)
///          - Object class resolution (via resolveObjectClass)
///          - Destructor name mangling (via getDestructorName)
///
///          Using OopLoweringContext reduces code duplication across OOP lowering
///          files and ensures consistent caching behavior.
///
/// @invariant References are borrowed from the Lowerer; context must not outlive it.
/// @invariant Caches are populated lazily and cleared when the context is destroyed.
struct OopLoweringContext {
    /// @brief Reference to the main lowering state.
    /// @details Provides access to IL emission, symbol tables, and general lowering utilities.
    /// @invariant Non-owning; the Lowerer must outlive this context.
    Lowerer &lowerer;

    /// @brief Reference to the OOP metadata index.
    /// @details Contains class definitions, method signatures, and inheritance information.
    /// @invariant Non-owning; the OopIndex must outlive this context.
    OopIndex &oopIndex;

    /// @brief Cache for resolved class info.
    /// @details Maps qualified class names to their metadata, avoiding repeated lookups.
    std::unordered_map<std::string, const ClassInfo *> classCache;

    /// @brief Cache for resolved class layouts.
    /// @details Maps class names (as discovered) to their layout metadata.
    std::unordered_map<std::string, const ClassLayout *> layoutCache;

    /// @brief Create a context for OOP lowering.
    /// @param lowerer Reference to the active lowering state.
    /// @param oopIndex Reference to the OOP metadata index.
    /// @pre @p lowerer and @p oopIndex outlive the constructed context.
    OopLoweringContext(Lowerer &lowerer, OopIndex &oopIndex)
        : lowerer(lowerer), oopIndex(oopIndex) {}

    // =========================================================================
    // Class Metadata Lookups
    // =========================================================================

    /// @brief Look up class info with caching.
    /// @details Caches index-owned pointers, including @c nullptr, under the
    ///          exact @p className key.
    /// @param className Qualified class name.
    /// @return Pointer to class info or nullptr if not found.
    const ClassInfo *findClassInfo(const std::string &className);

    /// @brief Look up class layout with caching.
    /// @details Delegates to Lowerer::findClassLayout and caches its pointer,
    ///          including @c nullptr, under the exact @p className key.
    /// @param className Class name (may be qualified or different casing).
    /// @return Pointer to layout when found; nullptr otherwise.
    const ClassLayout *findClassLayout(const std::string &className);

    // =========================================================================
    // Object Class Resolution
    // =========================================================================

    /// @brief Resolve the class name associated with an expression.
    /// @details Delegates to Lowerer::resolveObjectClass.
    /// @param expr Expression to analyze.
    /// @return Class name or empty string if not resolvable.
    std::string resolveObjectClass(const Expr &expr) const;

    // =========================================================================
    // Name Mangling Helpers
    // =========================================================================

    /// @brief Get the mangled destructor name for a class.
    /// @param className Qualified class name.
    /// @return Mangled destructor symbol name.
    std::string getDestructorName(const std::string &className) const;

    /// @brief Get the mangled constructor name for a class.
    /// @param className Qualified class name.
    /// @return Mangled constructor symbol name.
    std::string getConstructorName(const std::string &className) const;

    /// @brief Get the mangled method name for a class.
    /// @param className Qualified class name.
    /// @param methodName Method name.
    /// @return Mangled method symbol name.
    std::string getMethodName(const std::string &className, const std::string &methodName) const;

    // =========================================================================
    // Namespace Utilities
    // =========================================================================

    /// @brief Qualify an unqualified class name with the current namespace.
    /// @details Delegates unchanged to Lowerer::qualify.
    /// @param className Class spelling to resolve relative to the current namespace.
    /// @return The lowerer's resolved qualified spelling.
    std::string qualify(const std::string &className) const;
};

} // namespace il::frontends::basic