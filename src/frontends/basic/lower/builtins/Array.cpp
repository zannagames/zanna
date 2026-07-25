//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/lower/builtins/Array.cpp
// Purpose: Provides the array-family registrar hook for the callback-based
//          BASIC builtin lowering registry.
// Key invariants: Array operations remain on their dedicated lowering paths;
//                 this registrar therefore installs no callbacks.
// Ownership/Lifetime: Owns no state and performs no allocation.
// Links: src/frontends/basic/lower/builtins/Registrars.hpp,
//        src/frontends/basic/lower/MemberArrayResolver.cpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Registrar stub for BASIC array builtin lowering.
/// @details Exposes @ref registerArrayBuiltins so the shared builtin registry
///          remains uniform even though array-specific lowerings have not been
///          implemented yet.

#include "frontends/basic/lower/builtins/Registrars.hpp"

namespace il::frontends::basic::lower::builtins {
/// @brief Install array builtin lowering rules into the shared registry.
/// @details The lowering pipeline invokes a registrar for every builtin domain
///          during initialisation.  Array intrinsics currently lower through
///          generic expression handling, so there is nothing to register yet.
///          We nevertheless keep this hook so that:
///            - Higher level code can unconditionally call every registrar
///              without guarding for feature availability.
///            - Tooling that inspects the registry sees a slot for array
///              builtins and can surface follow-up work accordingly.
///            - Future developers have a documented entry point when specialised
///              array lowering becomes necessary.
///          The body intentionally remains a no-op.
void registerArrayBuiltins() {
    // No array-specific builtin lowering is routed through the shared registry.
}
} // namespace il::frontends::basic::lower::builtins
