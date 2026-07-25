//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
/// @file OopLoweringContext.cpp
/// @brief Implements OOP lowering context helpers.
/// @details Provides the out-of-line definitions for
///          @ref OopLoweringContext. The context caches class metadata lookups
///          and forwards naming and qualification requests to the lowerer or
///          OOP index to keep object-oriented lowering consistent.
//===----------------------------------------------------------------------===//

#include "frontends/basic/OopLoweringContext.hpp"
#include "frontends/basic/Lowerer.hpp"
#include "frontends/basic/NameMangler_OOP.hpp"
#include "frontends/basic/OopIndex.hpp"

namespace il::frontends::basic {

// =============================================================================
// Class Metadata Lookups
// =============================================================================

/// @brief Look up class metadata for a given class name.
/// @details Checks the local cache first, then queries the OOP index and caches
///          the result, including @c nullptr, for subsequent lookups under the
///          exact input key.
/// @param className Name of the class to resolve.
/// @return Pointer to class metadata, or nullptr if not found.
const ClassInfo *OopLoweringContext::findClassInfo(const std::string &className) {
    // Check cache first
    auto it = classCache.find(className);
    if (it != classCache.end())
        return it->second;

    // Look up in OOP index and cache result
    const ClassInfo *info = oopIndex.findClass(className);
    classCache[className] = info;
    return info;
}

/// @brief Look up the field layout for a class.
/// @details Checks the local layout cache and falls back to the lowerer's class
///          layout query, caching the result, including @c nullptr, under the
///          exact input key.
/// @param className Name of the class to resolve.
/// @return Pointer to class layout, or nullptr if not found.
const ClassLayout *OopLoweringContext::findClassLayout(const std::string &className) {
    // Check cache first
    auto it = layoutCache.find(className);
    if (it != layoutCache.end())
        return it->second;

    // Delegate to Lowerer and cache result
    const ClassLayout *layout = lowerer.findClassLayout(className);
    layoutCache[className] = layout;
    return layout;
}

// =============================================================================
// Object Class Resolution
// =============================================================================

/// @brief Resolve the class name of an object expression.
/// @details Delegates to the lowerer's object-class resolution routine, which
///          uses semantic information to determine the runtime class.
/// @param expr Expression representing an object instance.
/// @return Resolved class name, or empty string if unknown.
std::string OopLoweringContext::resolveObjectClass(const Expr &expr) const {
    return lowerer.resolveObjectClass(expr);
}

// =============================================================================
// Name Mangling Helpers
// =============================================================================

/// @brief Compute the mangled destructor name for a class.
/// @details Uses the shared OOP name mangler to produce the runtime symbol.
/// @param className Class name to mangle.
/// @return Mangled destructor symbol.
std::string OopLoweringContext::getDestructorName(const std::string &className) const {
    return mangleClassDtor(className);
}

/// @brief Compute the mangled constructor name for a class.
/// @details Uses the shared OOP name mangler to produce the runtime symbol.
/// @param className Class name to mangle.
/// @return Mangled constructor symbol.
std::string OopLoweringContext::getConstructorName(const std::string &className) const {
    return mangleClassCtor(className);
}

/// @brief Compute the mangled method name for a class member.
/// @details Combines the class and method names using the OOP name mangler to
///          match the runtime symbol naming scheme.
/// @param className Owning class name.
/// @param methodName Method name to mangle.
/// @return Mangled method symbol.
std::string OopLoweringContext::getMethodName(const std::string &className,
                                              const std::string &methodName) const {
    return mangleMethod(className, methodName);
}

// =============================================================================
// Namespace Utilities
// =============================================================================

/// @brief Qualify a class name with the current namespace.
/// @details Delegates unchanged to the lowerer's namespace qualification helper.
/// @param className Class name to qualify relative to the current namespace.
/// @return The lowerer's resolved qualified spelling.
std::string OopLoweringContext::qualify(const std::string &className) const {
    return lowerer.qualify(className);
}

} // namespace il::frontends::basic
