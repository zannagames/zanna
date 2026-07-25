//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//

// File: src/frontends/basic/LocationScope.cpp
// Purpose: Implements scoped replacement of the BASIC lowerer's source location.
// Key invariants: Destruction restores the location captured at construction.
// Ownership/Lifetime: The scope borrows a Lowerer that must outlive it.
// Links: src/frontends/basic/LocationScope.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file LocationScope.cpp
 * @brief Implements the source-location RAII guard used during BASIC lowering.
 */

#include "frontends/basic/LocationScope.hpp"
#include "frontends/basic/Lowerer.hpp"

namespace il::frontends::basic {

/// @brief Construct a location scope that sets Lowerer's source location.
/// @details Uses the public sourceLocation() accessor instead of direct member
///          access, eliminating the need for LocationScope to be a friend of
///          Lowerer. Saves the previous location for restoration on destruction.
/// @param lowerer The lowerer instance whose source location will be managed.
/// @param loc The new source location to set.
/// @pre @p lowerer must outlive this guard.
/// @post @p lowerer reports @p loc as its current source location.
LocationScope::LocationScope(Lowerer &lowerer, il::support::SourceLoc loc)
    : lowerer_(lowerer), previousLoc_(lowerer.sourceLocation()) {
    lowerer_.setSourceLocation(loc);
}

/// @brief Restore the source location captured by the constructor.
/// @post The borrowed lowerer again reports the saved source location.
LocationScope::~LocationScope() {
    lowerer_.setSourceLocation(previousLoc_);
}

} // namespace il::frontends::basic
