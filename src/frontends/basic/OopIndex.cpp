//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE in the project root for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/OopIndex.cpp
// Purpose: Implementation of the pure OOP data model without AST dependencies.
//
//===----------------------------------------------------------------------===//

/// @file OopIndex.cpp
/// @brief Implements case-insensitive BASIC OOP metadata queries.
/// @details Direct queries inspect one indexed class, while hierarchy queries
///          repeatedly follow `baseQualified`. Returned pointers refer to
///          records owned by the index and remain valid only while the
///          corresponding containers are not invalidated.

#include "frontends/basic/OopIndex.hpp"

#include <algorithm>
#include <cctype>

namespace il::frontends::basic {

namespace {

/// @brief Case-insensitive ASCII string comparison for BASIC identifiers.
/// @details BASIC is a case-insensitive language, so class/field/method lookups
///          must compare identifiers without regard to case. Uses std::toupper
///          for ASCII comparison, which is sufficient for BASIC identifiers
///          (no Unicode support required).
/// @param a First string to compare.
/// @param b Second string to compare.
/// @return True if strings match case-insensitively; false otherwise.
bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::toupper(static_cast<unsigned char>(a[i])) !=
            std::toupper(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

} // namespace

/// @brief Look up a mutable class record by name (case-insensitive).
/// @details Searches the internal @c std::unordered_map for the requested class
///          name using case-insensitive comparison (BASIC is case-insensitive).
///          Returns a pointer to the stored @ref ClassInfo instance when found.
///          Returning @c nullptr keeps callers explicit about the missing-class
///          case without performing map insertions.
/// @param name Class identifier to locate.
/// @return Pointer to the associated @ref ClassInfo or @c nullptr when absent.
ClassInfo *OopIndex::findClass(const std::string &name) {
    // Case-insensitive lookup - BASIC identifiers are case-insensitive
    for (auto &kv : classes_) {
        if (iequals(kv.first, name))
            return &kv.second;
    }
    return nullptr;
}

/// @brief Look up an immutable class record by name (case-insensitive).
/// @details Const-qualified overload used by read-only consumers.  The method
///          performs case-insensitive lookup (BASIC is case-insensitive) but
///          preserves const-correctness so callers cannot mutate the stored
///          metadata.
/// @param name Class identifier to locate.
/// @return Pointer to the stored @ref ClassInfo or @c nullptr when absent.
const ClassInfo *OopIndex::findClass(const std::string &name) const {
    // Case-insensitive lookup - BASIC identifiers are case-insensitive
    for (const auto &kv : classes_) {
        if (iequals(kv.first, name))
            return &kv.second;
    }
    return nullptr;
}

// =============================================================================
// Field Query API Implementation
// =============================================================================

/// @brief Find an instance or static field declared directly by a class.
/// @details Resolves @p className case-insensitively, searches instance fields
///          before static fields, and compares field identifiers
///          case-insensitively.
/// @param className Qualified or unqualified class key to resolve.
/// @param fieldName Field identifier to compare.
/// @return Pointer to index-owned field metadata, or @c nullptr if absent.
const ClassInfo::FieldInfo *OopIndex::findField(const std::string &className,
                                                std::string_view fieldName) const {
    const ClassInfo *info = findClass(className);
    if (!info)
        return nullptr;

    // Search instance fields (case-insensitive)
    for (const auto &field : info->fields) {
        if (iequals(field.name, fieldName))
            return &field;
    }

    // Search static fields (case-insensitive)
    for (const auto &field : info->staticFields) {
        if (iequals(field.name, fieldName))
            return &field;
    }

    return nullptr;
}

/// @brief Find a field in a class or the first base class that declares it.
/// @details At each level, instance fields precede static fields. The search
///          follows `baseQualified` until it is empty or cannot be resolved.
/// @param className Class at which to begin the case-insensitive search.
/// @param fieldName Field identifier compared case-insensitively.
/// @return Pointer to index-owned field metadata, or @c nullptr if the hierarchy
///         contains no matching field.
const ClassInfo::FieldInfo *OopIndex::findFieldInHierarchy(const std::string &className,
                                                           std::string_view fieldName) const {
    const ClassInfo *cur = findClass(className);
    while (cur) {
        // Search instance fields
        for (const auto &field : cur->fields) {
            if (iequals(field.name, fieldName))
                return &field;
        }

        // Search static fields
        for (const auto &field : cur->staticFields) {
            if (iequals(field.name, fieldName))
                return &field;
        }

        // Move to base class
        if (cur->baseQualified.empty())
            break;
        cur = findClass(cur->baseQualified);
    }
    return nullptr;
}

// =============================================================================
// Method Query API Implementation
// =============================================================================

/// @brief Find a method declared directly by a class.
/// @details Attempts heterogeneous map lookup first, then performs a
///          case-insensitive scan to preserve BASIC identifier semantics.
/// @param className Qualified or unqualified class key to resolve.
/// @param methodName Method identifier to compare.
/// @return Pointer to index-owned method metadata, or @c nullptr if absent.
const ClassInfo::MethodInfo *OopIndex::findMethod(const std::string &className,
                                                  std::string_view methodName) const {
    const ClassInfo *info = findClass(className);
    if (!info)
        return nullptr;

    // Heterogeneous lookup - no temporary std::string allocation
    auto it = info->methods.find(methodName);
    if (it != info->methods.end())
        return &it->second;

    for (const auto &entry : info->methods) {
        if (iequals(entry.first, methodName))
            return &entry.second;
    }

    return nullptr;
}

/// @brief Find a method in a class or the first base class that declares it.
/// @details Each class uses exact heterogeneous lookup followed by a
///          case-insensitive scan before the search follows `baseQualified`.
/// @param className Class at which to begin the search.
/// @param methodName Method identifier to resolve using BASIC casing rules.
/// @return Pointer to index-owned method metadata, or @c nullptr if the
///         hierarchy contains no matching method.
const ClassInfo::MethodInfo *OopIndex::findMethodInHierarchy(const std::string &className,
                                                             std::string_view methodName) const {
    const ClassInfo *cur = findClass(className);
    while (cur) {
        // Heterogeneous lookup - no temporary std::string allocation
        auto it = cur->methods.find(methodName);
        if (it != cur->methods.end())
            return &it->second;

        for (const auto &entry : cur->methods) {
            if (iequals(entry.first, methodName))
                return &entry.second;
        }

        // Move to base class
        if (cur->baseQualified.empty())
            break;
        cur = findClass(cur->baseQualified);
    }
    return nullptr;
}

// =============================================================================
// Virtual Slot Query
// =============================================================================

/// @brief Return the stored virtual slot for a method resolved through inheritance.
/// @param index OOP index containing the class hierarchy.
/// @param qualifiedClass Class at which method lookup begins.
/// @param methodName Method identifier resolved by hierarchy lookup.
/// @return The stored slot index, or `-1` for a missing or non-virtual method.
int getVirtualSlot(const OopIndex &index,
                   const std::string &qualifiedClass,
                   const std::string &methodName) {
    // BUG-OOP-002/003 fix: Walk the inheritance hierarchy to find virtual methods
    const ClassInfo::MethodInfo *mi = index.findMethodInHierarchy(qualifiedClass, methodName);
    if (!mi)
        return -1;
    return mi->slot;
}

} // namespace il::frontends::basic
