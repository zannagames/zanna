//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/LocationScope.hpp
// Purpose: RAII helper for managing source location context in Lowerer.
// Key invariants: Restores previous location on scope exit.
// Ownership/Lifetime: Stack-based RAII, non-copyable, non-movable.
// Links: docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//

/**
 * @file LocationScope.hpp
 * @brief Declares scoped source-location replacement for BASIC IL emission.
 *
 * LocationScope supports nested guards: each instance captures the location
 * visible at its own construction and restores that value at destruction.
 */

#pragma once

#include "support/source_location.hpp"

namespace il::frontends::basic {

/// BASIC-to-IL lowering state whose current location is scoped by LocationScope.
class Lowerer;

/// @brief RAII helper to set and restore source location context in Lowerer.
/// @details Automatically sets Lowerer::curLoc to a new location on construction
///          and restores the previous location on destruction. This eliminates
///          manual curLoc assignments throughout lowering visitor methods.
/// @invariant Restores original location on scope exit.
/// @invariant The referenced Lowerer outlives the guard.
///
/// Usage example:
/// @code
/// void Lowerer::visit(const BeepStmt &s) {
///     LocationScope loc(*this, s.loc);
///     // curLoc is now set to s.loc
///     requestHelper(RuntimeFeature::TermBell);
///     emitCallRet(Type(Type::Kind::Void), "rt_bell", {});
/// } // curLoc is automatically restored here
/// @endcode
class LocationScope {
  public:
    /// @brief Construct a location scope that sets Lowerer::curLoc.
    /// @param lowerer The lowerer instance whose curLoc will be managed.
    /// @param loc The new source location to set.
    /// @pre @p lowerer must remain alive until this object is destroyed.
    /// @post Lowerer::sourceLocation() equals @p loc.
    LocationScope(Lowerer &lowerer, il::support::SourceLoc loc);

    /// @brief Restore the previous source location.
    /// @post Lowerer::sourceLocation() equals the value saved at construction.
    ~LocationScope();

    /// @brief Copy construction is disabled because each guard restores one scope.
    LocationScope(const LocationScope &) = delete;
    /// @brief Copy assignment is disabled for the borrowed lowerer binding.
    LocationScope &operator=(const LocationScope &) = delete;
    /// @brief Move construction is disabled to preserve lexical restoration order.
    LocationScope(LocationScope &&) = delete;
    /// @brief Move assignment is disabled to preserve lexical restoration order.
    LocationScope &operator=(LocationScope &&) = delete;

  private:
    Lowerer &lowerer_;                   ///< Reference to the lowerer
    il::support::SourceLoc previousLoc_; ///< Location to restore on destruction
};

} // namespace il::frontends::basic
