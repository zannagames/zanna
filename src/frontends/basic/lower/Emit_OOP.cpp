//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/lower/Emit_OOP.cpp
// Purpose: Forwards object and STRING local cleanup requests from Lowerer to
//          the shared Emitter, and emits the parameter retain and slot move
//          that complete a procedure's ownership of its managed locals.
// Key invariants:
//   - Local cleanup covers BYVAL parameters, which are retained on entry, and
//     skips only the names the caller excludes.
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
/// @param excluded Names excluded from cleanup, such as a returned result slot.
void Lowerer::releaseObjectLocals(const std::unordered_set<std::string> &excluded) {
    emitter().releaseObjectLocals(excluded);
}

/// @copydoc Lowerer::releaseStringLocals()
void Lowerer::releaseStringLocals(const std::unordered_set<std::string> &excluded) {
    emitter().releaseStringLocals(excluded);
}

/// @copydoc Lowerer::retainOwnedParam()
void Lowerer::retainOwnedParam(Value incoming, Type ilType, bool isObject) {
    if (isObject) {
        requestHelper(RuntimeFeature::ObjRetainMaybe);
        emitCall("rt_obj_retain_maybe", {incoming});
    } else if (ilType.kind == Type::Kind::Str) {
        requireStrRetainMaybe();
        emitCall("rt_str_retain_maybe", {incoming});
    }
}

/// @copydoc Lowerer::takeOwnedSlot()
Value Lowerer::takeOwnedSlot(Value slot, Type ilType) {
    Value value = emitLoad(ilType, slot);
    emitStore(Type(Type::Kind::Ptr), slot, Value::null());
    return value;
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
