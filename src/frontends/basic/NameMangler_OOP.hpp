//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/NameMangler_OOP.hpp
// Purpose: Re-export OOP name mangling helpers from common library and add the
//          BASIC-only static member symbols.
// Key invariants: Mangled names remain stable and purely derived from inputs.
// Ownership/Lifetime: Returns freshly-allocated std::string instances owned by callers.
// Links: docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//

/// @file NameMangler_OOP.hpp
/// @brief Re-exports OOP name mangling helpers from the common frontend library.
/// @details These helpers provide a consistent naming convention for class
///          constructors, destructors, and methods so that later lowering
///          stages can rely on stable symbol identifiers irrespective of
///          declaration order or compilation session.

#pragma once

#include "frontends/common/NameMangler.hpp"

namespace il::frontends::basic {

/// @copydoc ::il::frontends::common::mangleClassCtor
using ::il::frontends::common::mangleClassCtor;
/// @copydoc ::il::frontends::common::mangleClassDtor
using ::il::frontends::common::mangleClassDtor;
/// @copydoc ::il::frontends::common::mangleIfaceBindThunk
using ::il::frontends::common::mangleIfaceBindThunk;
/// @copydoc ::il::frontends::common::mangleIfaceRegThunk
using ::il::frontends::common::mangleIfaceRegThunk;
/// @copydoc ::il::frontends::common::mangleMethod
using ::il::frontends::common::mangleMethod;
/// @copydoc ::il::frontends::common::mangleOopModuleInit
using ::il::frontends::common::mangleOopModuleInit;

/// @brief Name of a class's static constructor: "Class.__ctor$static".
/// @param className Qualified class name.
/// @return Static constructor symbol.
inline std::string mangleStaticCtor(std::string_view className) {
    return mangleClassCtor(className) + "$static";
}

/// @brief Name of a class's static destructor: "Class.__dtor$static".
/// @param className Qualified class name.
/// @return Static destructor symbol.
inline std::string mangleStaticDtor(std::string_view className) {
    return mangleClassDtor(className) + "$static";
}

/// @brief Name of the module finalizer that runs static destructors: "__mod_fini$oop".
/// @return Module finalizer symbol.
inline std::string mangleOopModuleFini() {
    return "__mod_fini$oop";
}

/// @brief Name of the module global backing a static field: "Class.__static.FIELD".
/// @details The `__static` segment keeps the global apart from the class's methods,
///          which are named "Class.Method".
/// @param className Qualified class name.
/// @param fieldName Field name as indexed.
/// @return Static field global symbol.
inline std::string mangleStaticField(std::string_view className, std::string_view fieldName) {
    std::string result;
    result.reserve(className.size() + fieldName.size() + 10);
    result.append(className);
    result.append(".__static.");
    result.append(fieldName);
    return result;
}

} // namespace il::frontends::basic
