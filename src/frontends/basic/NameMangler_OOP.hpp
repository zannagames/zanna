//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/NameMangler_OOP.hpp
// Purpose: Re-export OOP name mangling helpers from common library.
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

} // namespace il::frontends::basic
