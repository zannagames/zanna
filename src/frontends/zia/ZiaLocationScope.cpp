//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: frontends/zia/ZiaLocationScope.cpp
// Purpose: RAII guard that sets/restores source location on the Zia lowerer.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements temporary Zia lowerer source-location scoping.
/// @details Construction saves and replaces the active location; destruction
///          restores it so nested lowering operations preserve diagnostic
///          context without manual cleanup.

#include "frontends/zia/ZiaLocationScope.hpp"
#include "frontends/zia/Lowerer.hpp"

namespace il::frontends::zia {

/// @brief Install a temporary source location on a lowerer.
/// @param lowerer Lowerer whose current diagnostic/emission location is changed.
/// @param loc Source location to expose for the lifetime of this guard.
/// @post `lowerer.sourceLocation()` equals @p loc until this guard is destroyed.
ZiaLocationScope::ZiaLocationScope(Lowerer &lowerer, il::support::SourceLoc loc)
    : lowerer_(lowerer), previousLoc_(lowerer.sourceLocation()) {
    lowerer_.setSourceLocation(loc);
}

/// @brief Restore the source location captured by the constructor.
/// @post The guarded lowerer again exposes its pre-construction source location.
ZiaLocationScope::~ZiaLocationScope() {
    lowerer_.setSourceLocation(previousLoc_);
}

} // namespace il::frontends::zia
