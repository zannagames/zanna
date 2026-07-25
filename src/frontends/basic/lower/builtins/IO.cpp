//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/lower/builtins/IO.cpp
// Purpose: Registers the TIMER callback while leaving stateful file/channel
//          operations on their specialized statement and builtin paths.
// Key invariants: TIMER uses the generic registry rule; OPEN, CLOSE, INPUT#,
//                 PRINT#, and related operations are not registered here.
// Ownership/Lifetime: Registry callback pointers have process lifetime and
//                     borrow each per-call BuiltinLowerContext.
// Links: src/frontends/basic/lower/builtins/Registrars.hpp,
//        src/frontends/basic/BuiltinRegistry.hpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Registrar stub for BASIC file I/O builtin lowering.
/// @details Provides @ref registerIoBuiltins to keep the builtin registry shape
///          consistent even though file I/O lowering is handled by bespoke
///          routines elsewhere in the frontend.

#include "frontends/basic/BuiltinRegistry.hpp"
#include "frontends/basic/Lowerer.hpp"
#include "frontends/basic/lower/BuiltinCommon.hpp"
#include "frontends/basic/lower/builtins/Registrars.hpp"

namespace il::frontends::basic::lower::builtins {

namespace {
/// @brief Lower TIMER builtin using the generic rule-driven path.
/// @param ctx Active TIMER lowering context.
/// @return Rule-driven TIMER result.
Lowerer::RVal lowerTimerBuiltin(BuiltinLowerContext &ctx) {
    return lower::lowerGenericBuiltin(ctx);
}
} // namespace

/// @brief Registers the stateless TIMER builtin in the I/O family.
/// @details Stateful file/channel builtins continue to use bespoke lowering;
///          only TIMER is compatible with the generic rule-driven callback.
void registerIoBuiltins() {
    using Builtin = BuiltinCallExpr::Builtin;
    // Register TIMER builtin to use the generic lowering path
    register_builtin(getBuiltinInfo(Builtin::Timer).name, &lowerTimerBuiltin);
}
} // namespace il::frontends::basic::lower::builtins
