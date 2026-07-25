//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/zia/ZiaLocationScope.hpp
// Purpose: RAII helper for managing source location context in Zia Lowerer.
// Key invariants: Restores previous location on scope exit.
// Ownership/Lifetime: Stack-based RAII, non-copyable, non-movable.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "support/source_location.hpp"

namespace il::frontends::zia {

class Lowerer;

/// @brief RAII helper to set and restore source location context in Lowerer.
/// @details Changes the lowerer's public source-location context on construction
///          and restores the captured value on destruction. The guard is
///          deliberately immobile so restoration always targets one lexical scope.
/// @invariant Restores original location on scope exit.
class ZiaLocationScope {
  public:
    /// @brief Install a temporary source location.
    /// @param lowerer Lowerer whose source-location context is changed.
    /// @param loc Location to expose until this guard is destroyed.
    ZiaLocationScope(Lowerer &lowerer, il::support::SourceLoc loc);

    /// @brief Restore the location that was active at construction.
    ~ZiaLocationScope();

    /// @brief Prevent copying a guard that owns a restoration obligation.
    ZiaLocationScope(const ZiaLocationScope &) = delete;

    /// @brief Prevent replacing a guard's lowerer and saved location by copy.
    ZiaLocationScope &operator=(const ZiaLocationScope &) = delete;

    /// @brief Prevent transferring the restoration obligation to another scope.
    ZiaLocationScope(ZiaLocationScope &&) = delete;

    /// @brief Prevent replacing a guard's lowerer and saved location by move.
    ZiaLocationScope &operator=(ZiaLocationScope &&) = delete;

  private:
    /// Lowerer whose location is restored when the guard leaves scope.
    Lowerer &lowerer_;

    /// Source location that was active before construction.
    il::support::SourceLoc previousLoc_;
};

} // namespace il::frontends::zia
