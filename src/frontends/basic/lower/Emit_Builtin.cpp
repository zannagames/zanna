//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/lower/Emit_Builtin.cpp
// Purpose: Forwards Lowerer array storage, array cleanup, and temporary-value
//          lifetime operations to the stateful Emitter.
// Key invariants:
//   - Array ownership bookkeeping remains centralized in Emitter.
//   - Deferred temporary releases retain source order until emitted or cleared.
// Ownership/Lifetime:
//   - Value handles are copied into emitter bookkeeping; this layer owns no
//     runtime allocations itself.
//   - The Lowerer owns the Emitter used by every forwarding function.
// Links: src/frontends/basic/lower/Emitter.hpp,
//        src/frontends/basic/Lowerer.hpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements runtime helper emission forwarding for BASIC built-ins.
/// @details The functions in this translation unit act as the out-of-line glue
///          between `Lowerer` and the reusable emitter implementation.  Keeping
///          the forwarding logic here avoids including emitter internals in
///          every translation unit that instantiates `Lowerer`, while also
///          documenting the ownership conventions used for array-valued
///          temporaries.

#include "frontends/basic/Lowerer.hpp"
#include "frontends/basic/lower/Emitter.hpp"

#include "zanna/il/Module.hpp"

using namespace il::core;

namespace il::frontends::basic {

/// @brief Record a lowered array value into a stack slot owned by the procedure.
///
/// @details Lowered BASIC operations often yield temporary arrays that must be
///          retained so subsequent statements can access them.  The emitter is
///          responsible for pairing the store with the matching runtime retain
///          call; this forwarding helper simply hands the store request to that
///          component so all ownership tracking remains centralised.
///
/// @param slot Address where the array handle should be written.
/// @param value Array value produced by the lowering routine.
/// @param elementType BASIC element type used to select ownership behavior.
void Lowerer::storeArray(Value slot, Value value, AstType elementType) {
    emitter().storeArray(slot, value, elementType, /*isObjectArray=*/false);
}

/// @brief Records an array handle and explicitly identifies object arrays.
/// @param slot Address where the array handle is stored.
/// @param value Array handle produced by lowering.
/// @param elementType BASIC element type used for runtime metadata.
/// @param isObjectArray True when elements are object references requiring
///        object-aware ownership handling.
void Lowerer::storeArray(Value slot, Value value, AstType elementType, bool isObjectArray) {
    emitter().storeArray(slot, value, elementType, isObjectArray);
}

/// @brief Release any array locals that were materialised within the current procedure.
///
/// @details Array temporaries lowered from BASIC constructs require paired
///          release calls so the runtime can drop reference counts.  This
///          helper simply forwards to the shared emitter instance, which tracks
///          which local slots own arrays and emits the finaliser calls in a
///          deterministic order.
///
/// @param paramNames Name set describing parameters that must be preserved.
void Lowerer::releaseArrayLocals(const std::unordered_set<std::string> &paramNames) {
    emitter().releaseArrayLocals(paramNames);
}

/// @brief Request runtime releases for array parameters once a procedure exits.
///
/// @details Procedures that accept array arguments borrow ownership from the
///          caller.  Before returning, the lowering pipeline has to synthesise
///          release helpers so reference counts remain balanced.  Delegating the
///          actual emission to the central emitter guarantees that the
///          canonical release order is respected across all lowering sites.
///
/// @param paramNames Identifier set describing the formal parameters that
///        should be handed back to the runtime.
void Lowerer::releaseArrayParams(const std::unordered_set<std::string> &paramNames) {
    emitter().releaseArrayParams(paramNames);
}

/// @brief Defers release of a temporary string until the statement boundary.
/// @param v Runtime string handle whose ownership must be relinquished later.
void Lowerer::deferReleaseStr(Value v) {
    emitter().deferReleaseStr(v);
}

/// @brief Defers release of a temporary object until the statement boundary.
/// @param v Runtime object handle whose ownership must be relinquished later.
/// @param className Canonical class name used for destructor dispatch; may be
///        empty when no class-specific destruction is required.
void Lowerer::deferReleaseObj(Value v, const std::string &className) {
    emitter().deferReleaseObj(v, className);
}

/// @brief Emits releases for every deferred temporary and clears the queue.
void Lowerer::releaseDeferredTemps() {
    emitter().releaseDeferredTemps();
}

/// @brief Clears deferred-temporary bookkeeping without emitting releases.
/// @warning Callers use this only when ownership has been transferred or
///          cleanup is otherwise handled by a different control-flow path.
void Lowerer::clearDeferredTemps() {
    emitter().clearDeferredTemps();
}

} // namespace il::frontends::basic
