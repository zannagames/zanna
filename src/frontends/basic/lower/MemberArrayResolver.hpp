//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/lower/MemberArrayResolver.hpp
// Purpose: Consolidates member array field resolution for the BASIC lowerer.
//          Previously, the pattern "is this a field? is it an array? is it
//          object-typed?" was duplicated across RuntimeStatementLowerer_Assign,
//          Emit_Expr, Lowerer_Expr, and Lower_OOP_Helpers (BUG-056, BUG-058,
//          BUG-089, BUG-108). This helper unifies those checks.
// Key invariants:
//   - Returns default (non-field) info when no class layout or field is found
//   - Local variables/parameters shadow implicit field arrays (BUG-108)
//   - Handles both dotted (obj.field) and implicit (field inside method) cases
// Ownership/Lifetime:
//   - Stateless struct; produced by Lowerer::resolveMemberArrayField()
// Links: docs/bugs/basic_bugs.md (BUG-056, BUG-058, BUG-089, BUG-108)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Defines the value object returned by member-array resolution.
/// @details The result distinguishes dotted and implicit field access while
///          carrying the element type and optional object class required by
///          array load, store, scan, and ownership paths.

#pragma once

#include "frontends/basic/ast/NodeFwd.hpp"

#include <string>

namespace il::frontends::basic {

/// @brief Consolidated result of member array field resolution.
/// @details Captures whether a variable name refers to a class field, whether
///          that field is an array, whether the array elements are object-typed,
///          and the element class name when applicable. Produced by
///          Lowerer::resolveMemberArrayField().
struct MemberArrayInfo {
    bool isField = false;            ///< Name resolves to a class field.
    bool isArray = false;            ///< The field is an array type.
    bool isObjectArray = false;      ///< The array elements are object-typed.
    bool isDottedAccess = false;     ///< Access is through dotted syntax (obj.field).
    Type elementAstType = Type::I64; ///< AST type of array elements.
    std::string elementClassName;    ///< Class name of elements (if object array).
};

} // namespace il::frontends::basic
