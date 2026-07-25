//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/lower/Emit_OOP.cpp
// Purpose: Forwards object-local and object-parameter cleanup requests from
//          Lowerer to the shared Emitter.
// Key invariants:
//   - Local cleanup excludes names identified as parameters.
//   - Parameter cleanup considers only the caller-provided parameter set.
//   - Destructor dispatch and reference releases remain centralized in Emitter.
// Ownership/Lifetime:
//   - Name sets are borrowed for each call.
//   - Lowerer owns the Emitter and symbol state used for cleanup emission.
// Links: src/frontends/basic/lower/Emitter.hpp,
//        src/frontends/basic/NameMangler_OOP.hpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Forwards OOP lifetime management calls from the lowering layer to the
///        shared emitter implementation.
/// @details The lowering entry points call through to the emitter so ownership
///          transitions remain encapsulated.  Keeping the glue here avoids
///          leaking emitter headers throughout the lowering passes while still
///          documenting when runtime helpers are required.

#include "frontends/basic/Lowerer.hpp"
#include "frontends/basic/NameMangler_OOP.hpp"
#include "frontends/basic/lower/Emitter.hpp"

#include "zanna/il/Module.hpp"

#include <cassert>

using namespace il::core;

namespace il::frontends::basic {

/// @brief Release object-typed locals that go out of scope at the current point
///        in lowering.
///
/// @details Delegates to @ref il::frontends::basic::lower::Emitter::releaseObjectLocals
///          so the shared emitter can generate the necessary reference-counting
///          calls.  The wrapper exists to keep the @ref Lowerer API cohesive
///          while hiding the emitter type from most translation units.
///
/// @param paramNames Set of local names that require release operations.
void Lowerer::releaseObjectLocals(const std::unordered_set<std::string> &paramNames) {
    emitter().releaseObjectLocals(paramNames);
}

/// @brief Release object-typed parameters at the end of a procedure.
///
/// @details Invokes @ref il::frontends::basic::lower::Emitter::releaseObjectParams so
///          ownership semantics remain centralised in the emitter.  Parameters
///          are tracked separately from locals because they are initialised by
///          the caller and may have distinct lifetime guarantees.
///
/// @param paramNames Parameter identifiers that should be released.
void Lowerer::releaseObjectParams(const std::unordered_set<std::string> &paramNames) {
    emitter().releaseObjectParams(paramNames);
}

} // namespace il::frontends::basic
