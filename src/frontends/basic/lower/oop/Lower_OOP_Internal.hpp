//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
// File: src/frontends/basic/lower/oop/Lower_OOP_Internal.hpp
// Purpose: Internal shared declarations for OOP lowering implementation.
// Key invariants: For use only by OOP lowering translation units.
// Ownership/Lifetime: Non-owning references to Lowerer and OOP metadata.
// Links: docs/internals/codemap.md
//===----------------------------------------------------------------------===//

#pragma once

#include "frontends/basic/Lowerer.hpp"
#include "frontends/basic/OopLoweringContext.hpp"
#include "frontends/basic/lower/oop/Lower_OOP_RuntimeHelpers.hpp"

#include <functional>
#include <string>
#include <string_view>

/// @file
/// @brief Declares implementation-only class-field type resolvers shared by
///        BASIC OOP lowering translation units.

namespace il::frontends::basic {

// Internal helper functions shared between OOP lowering translation units.
// These are implementation details and should not be exposed in the public Lowerer interface.

// -------------------------------------------------------------------------
// Centralized OOP Resolution Helpers
// -------------------------------------------------------------------------
// These helpers consolidate patterns for resolving object class names from
// fields, arrays, and method return types. (BUG-061, BUG-082, BUG-089, etc.)

/// @brief Resolve the object class of a non-array field.
/// @param layout Class layout to search.
/// @param fieldName Name of the field to look up.
/// @param qualify Optional function to qualify the class name.
/// @return Qualified class name if field is an object type, empty otherwise.
std::string resolveFieldObjectClass(const ClassLayout *layout,
                                    std::string_view fieldName,
                                    const std::function<std::string(const std::string &)> &qualify);

/// @brief Resolve the element class of an array field.
/// @param layout Class layout to search.
/// @param fieldName Name of the array field to look up.
/// @param qualify Optional function to qualify the class name.
/// @return Qualified class name if field is an object array, empty otherwise.
std::string resolveFieldArrayElementClass(
    const ClassLayout *layout,
    std::string_view fieldName,
    const std::function<std::string(const std::string &)> &qualify);

} // namespace il::frontends::basic
