//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/lower/common/BuiltinUtils.cpp
// Purpose: Materialise the registry-backed dispatcher used to lower BASIC
//          builtin calls.
// Key invariants: Each builtin name resolves to at most one handler and the
//                 registry initialises exactly once per process.
// Ownership/Lifetime: Relies on process-wide registration tables and does not
//                     allocate persistent state beyond handler bindings.
// Links: docs/tutorials/basic-tutorial.md, docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements one-time builtin callback registration and call dispatch.

#include "frontends/basic/lower/common/BuiltinUtils.hpp"

#include "frontends/basic/BuiltinRegistry.hpp"
#include "frontends/basic/DiagnosticEmitter.hpp"
#include "frontends/basic/lower/BuiltinCommon.hpp"
#include "frontends/basic/lower/builtins/Registrars.hpp"

#include <string>
#include <string_view>

namespace il::frontends::basic::lower::common {
namespace {
constexpr std::string_view kDiagMissingBuiltinEmitter = "B4004";

/// @brief Registers all builtin lowering handlers once per process.
/// @details Function-local static initialization provides thread-safe
///          initialization and preserves the declared family override order.
void ensureBuiltinHandlers() {
    static const bool initialized = [] {
        builtins::registerDefaultBuiltins();
        builtins::registerStringBuiltins();
        builtins::registerConversionBuiltins();
        builtins::registerMathBuiltins();
        builtins::registerArrayBuiltins();
        builtins::registerIoBuiltins();
        return true;
    }();
    (void)initialized;
}

} // namespace

/// @brief Lower a BASIC builtin call into IL.
/// @details Ensures the handler registry is initialised, constructs a
///          @ref BuiltinLowerContext, and dispatches to the registered handler.
///          When no handler is available the function emits a diagnostic (when
///          possible) and returns a zero-valued result so downstream passes can
///          continue operating.
/// @param lowerer Active lowering context used to emit IL.
/// @param call AST node describing the builtin invocation.
/// @return Lowered r-value or a zero substitute when no handler exists.
Lowerer::RVal lowerBuiltinCall(Lowerer &lowerer, const BuiltinCallExpr &call) {
    ensureBuiltinHandlers();

    BuiltinLowerContext ctx(lowerer, call);
    if (BuiltinHandler handler = find_builtin(ctx.info().name))
        return handler(ctx);

    if (auto *diag = lowerer.diagnosticEmitter()) {
        ctx.setCurrentLoc(call.loc);
        diag->emit(il::support::Severity::Error,
                   std::string(kDiagMissingBuiltinEmitter),
                   call.loc,
                   0,
                   "no emitter registered for builtin call");
    }

    return ctx.makeZeroResult();
}

/// @brief Expose handler initialisation for unit tests.
/// @details Invokes @ref ensureBuiltinHandlers so tests can rely on the same
///          registration logic as production code without duplicating
///          boilerplate.
void ensureBuiltinHandlersForTesting() {
    ensureBuiltinHandlers();
}

} // namespace il::frontends::basic::lower::common
