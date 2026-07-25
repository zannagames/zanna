//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/StringRetainPolicy.hpp
// Purpose: Decide when backends may skip the defensive rt_str_retain_maybe on
//          a string-typed call result.
// Key invariants:
//   - Elision is legal only when a direct callee transfers an owned reference:
//     IL-defined functions do so by convention and registered runtime helpers
//     do so when classifyRuntimeOwnership reports returnsOwned.
//   - Indirect callees keep the retain; unregistered direct names follow the
//     IL-defined-function ownership convention.
//   - Load-path retains carry ownership only for spending uses such as
//     consuming callees and returns. Borrow-only loads may elide them when the
//     backend proves the source slot remains live through the use window.
// Ownership/Lifetime: Header-only policy; no state beyond the env hatch.
// Links: docs/il/il-passes.md, src/il/runtime/RuntimeOwnership.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file StringRetainPolicy.hpp
 * @brief Centralizes native-backend decisions for eliding redundant string
 *        retains.
 *
 * These helpers encode the ownership contract shared by native backends and
 * provide `ZANNA_NO_RETAIN_ELIDE` as a diagnostic escape hatch.
 */

#pragma once

#include "il/runtime/RuntimeOwnership.hpp"
#include "il/runtime/RuntimeSignatures.hpp"

#include <cstdlib>
#include <string_view>

namespace zanna::codegen {

/// @brief True when the backend may skip the defensive retain on the string
///        result of a call to @p callee.
/// @details The retain exists to mirror the VM's "every register def owns a
///          reference" model without its balancing frame-teardown release, so
///          on every balanced frontend path it is a pure leak. When the callee
///          is classified as transferring ownership of its result, the
///          transferred reference already keeps the value alive until the
///          frontend's release/consume, and the retain can be dropped.
///          `ZANNA_NO_RETAIN_ELIDE=1` restores the unconditional retain.
/// @param callee Direct callee name.  An empty name represents an indirect
///               call and is never eligible.
/// @return `true` for an unregistered direct name, which follows the
///         IL-defined-function transfer convention, or for a registered
///         runtime helper classified as returning ownership; otherwise
///         `false`.
[[nodiscard]] inline bool shouldElideStringResultRetain(std::string_view callee) {
    if (callee.empty())
        return false;
    if (std::getenv("ZANNA_NO_RETAIN_ELIDE") != nullptr)
        return false;
    // Registered runtime helpers transfer when classified returnsOwned.
    // Everything else is an IL-defined function (module or cross-module),
    // and the IL string return convention is transfer-everywhere: frontends
    // mint the caller's owned reference on every return path and the VM's
    // return-retain enforces the same. Indirect calls (empty callee) keep
    // the retain above.
    if (il::runtime::findRuntimeSignature(callee) == nullptr)
        return true;
    return il::runtime::classifyRuntimeOwnership(callee).returnsOwned;
}

/// @brief Tests whether a callee is an explicit string-release entry point.
/// @param callee Direct C ABI or namespace-qualified runtime callee name.
/// @return `true` when @p callee is one of the recognized release aliases.
[[nodiscard]] inline bool isStringReleaseCallee(std::string_view callee) {
    return callee == "rt_str_release_maybe" || callee == "rt_str_release" ||
           callee == "rt_memory_release_str" || callee == "Zanna.String.ReleaseMaybe" ||
           callee == "Zanna.Memory.ReleaseStr" || callee == "Zanna.Runtime.Unsafe.ReleaseStr";
}

/// @brief Tests whether a callee explicitly creates another owned string reference.
/// @param callee Direct C ABI or namespace-qualified runtime callee name.
/// @return `true` when @p callee is one of the recognized retain aliases.
[[nodiscard]] inline bool isStringRetainCallee(std::string_view callee) {
    return callee == "rt_str_retain" || callee == "rt_str_retain_maybe" ||
           callee == "rt_memory_retain_str" || callee == "Zanna.String.RetainMaybe" ||
           callee == "Zanna.Memory.RetainStr" || callee == "Zanna.Runtime.Unsafe.RetainStr";
}

/// @brief True when the load-path retain for a string load may be skipped.
/// @details The defensive load retain exists so consuming callees and
///          explicit releases can spend a reference without destroying the
///          slot's own one. When the loaded value's ONLY use is an explicit
///          release call, the retain+release pair is a native no-op that
///          silently keeps the displaced value alive forever; skipping the
///          retain lets the frontend's release actually free it. Any other
///          use keeps the retain.
/// @return `true` unless retain elision has been disabled through the
///         `ZANNA_NO_RETAIN_ELIDE` environment variable.
[[nodiscard]] inline bool shouldElideLoadRetainForRelease() {
    return std::getenv("ZANNA_NO_RETAIN_ELIDE") == nullptr;
}

} // namespace zanna::codegen
