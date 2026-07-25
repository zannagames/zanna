//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/lower/MemberArrayResolver.cpp
// Purpose: Implements Lowerer::resolveMemberArrayField() — a single entry point
//          for the member array field resolution pattern that was previously
//          duplicated across four lowerer source files.
// Key invariants:
//   - Returns default (non-field) info when no class layout or field is found
//   - Local variables/parameters shadow implicit field arrays (BUG-108)
//   - Handles both dotted (obj.field) and implicit (field inside method) cases
// Ownership/Lifetime:
//   - Pure const query on Lowerer state; no side effects
// Links: docs/bugs/basic_bugs.md (BUG-056, BUG-058, BUG-089, BUG-108)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements class-field array resolution for BASIC lowering.

#include "frontends/basic/lower/MemberArrayResolver.hpp"
#include "frontends/basic/Lowerer.hpp"

namespace il::frontends::basic {

/// @brief Resolve member array field information for a variable name.
///
/// @details Consolidates the "is this a field? is it an array? is it
///          object-typed?" pattern that was duplicated across:
///
///          - RuntimeStatementLowerer_Assign.cpp (assignArrayElement)
///          - Emit_Expr.cpp (lowerArrayAccess)
///          - Lowerer_Expr.cpp (ArrayExpr visitor)
///          - Lower_OOP_Helpers.cpp (resolveObjectClass for ArrayExpr)
///
///          The resolution proceeds in two phases:
///
///          Phase 1 — Dotted access (BUG-056):
///            If the name contains '.', split into base and field, look up the
///            base object's class layout, and resolve the field. The element type
///            and object class name come from the field descriptor.
///
///          Phase 2 — Implicit field access (BUG-058, BUG-108):
///            If the name does NOT contain a dot, check whether it refers to a
///            field in the active class scope (during method lowering). However,
///            if a local variable or parameter with a materialized slot exists
///            for this name, the local shadows the implicit field (BUG-108).
///
///          In both phases, a non-empty objectClassName on the field indicates
///          that the array holds object references (BUG-089).
///
/// @param name Variable name to resolve.
/// @return Resolution flags and element metadata. A default result denotes a
///         non-field or unresolved name.
MemberArrayInfo Lowerer::resolveMemberArrayField(std::string_view name) const {
    MemberArrayInfo info;

    const bool isDotted = name.find('.') != std::string_view::npos;

    if (isDotted) {
        // Phase 1: Dotted member array (e.g., player.inventory)
        info.isDottedAccess = true;
        std::size_t dot = name.find('.');
        std::string baseName(name.substr(0, dot));
        std::string fieldName(name.substr(dot + 1));
        std::string klass = getSlotType(baseName).objectClass;
        if (const ClassLayout *layout = findClassLayout(klass)) {
            if (const ClassLayout::Field *fld = layout->findField(fieldName)) {
                info.isField = true;
                info.isArray = fld->isArray;
                info.elementAstType = fld->type;
                info.isObjectArray = !fld->objectClassName.empty();
                if (info.isObjectArray)
                    info.elementClassName = fld->objectClassName;
            }
        }
    } else {
        // Phase 2: Implicit field array inside a method (BUG-058)
        // Only applies when a field scope is active.
        if (isFieldInScope(name)) {
            // BUG-108: Local variables/parameters shadow implicit field arrays.
            // A local symbol with a materialized slot takes precedence.
            const SymbolInfo *localSym = findSymbol(name);
            bool localShadows = localSym && localSym->slotId.has_value();

            if (!localShadows) {
                const FieldScope *scope = activeFieldScope();
                if (scope && scope->layout) {
                    if (const ClassLayout::Field *fld = scope->layout->findField(name)) {
                        info.isField = true;
                        info.isArray = fld->isArray;
                        info.elementAstType = fld->type;
                        info.isObjectArray = !fld->objectClassName.empty();
                        if (info.isObjectArray)
                            info.elementClassName = fld->objectClassName;
                    }
                }
            }
        }
    }

    return info;
}

} // namespace il::frontends::basic
