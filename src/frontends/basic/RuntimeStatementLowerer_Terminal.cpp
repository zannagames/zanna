//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/RuntimeStatementLowerer_Terminal.cpp
// Purpose: Implementation of terminal-related runtime statement lowering.
//          Handles BEEP, CLS, COLOR, LOCATE, CURSOR, ALTSCREEN, SLEEP statements.
// Key invariants: Terminal statements map to corresponding runtime helpers.
// Ownership/Lifetime: Borrows Lowerer reference; coordinates with parent.
// Links: docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//

/// @file RuntimeStatementLowerer_Terminal.cpp
/// @brief Implements terminal-control runtime calls for BASIC statements.
/// @details Each visitor applies the statement location, coerces/narrows any
///          operands to the runtime ABI, records the appropriate generated or
///          manual helper requirement, and emits one call.

#include "Lowerer.hpp"
#include "RuntimeCallHelpers.hpp"
#include "RuntimeStatementLowerer.hpp"

using namespace il::core;
using il::runtime::RuntimeFeature;

namespace il::frontends::basic {

/// @brief Lower the BASIC @c BEEP statement to a runtime helper call.
///
/// @details Emits a call to the bell/beep runtime function without arguments.
///          The current source location is preserved for diagnostics.
///
/// @param s AST node representing the @c BEEP statement.
void RuntimeStatementLowerer::visit(const BeepStmt &s) {
    RuntimeCallBuilder(lowerer_).at(s.loc).callHelperVoid(RuntimeFeature::TermBell, "rt_bell");
}

/// @brief Lower the BASIC @c CLS statement to a runtime helper call.
///
/// @details Emits a request for the terminal-clear helper and dispatches the
///          call without arguments.  The current source location is preserved so
///          diagnostics and debug traces attribute the call correctly.
///
/// @param s AST node representing the @c CLS statement.
void RuntimeStatementLowerer::visit(const ClsStmt &s) {
    RuntimeCallBuilder(lowerer_).at(s.loc).callHelperVoid(RuntimeFeature::TermCls, "rt_term_cls");
}

/// @brief Lower the BASIC @c COLOR statement to the runtime helper.
///
/// @details Evaluates the foreground and optional background expressions,
///          narrows them to 32-bit integers, requests the terminal-colour helper,
///          and emits the call.  Missing background arguments default to -1,
///          matching runtime semantics.
///
/// @param s AST node describing the @c COLOR statement.
/// @pre @p s has a non-null foreground expression.
void RuntimeStatementLowerer::visit(const ColorStmt &s) {
    auto fg = lowerer_.ensureI64(lowerer_.lowerExpr(*s.fg), s.loc);
    Value bgv = Value::constInt(-1);
    if (s.bg) {
        auto bg = lowerer_.ensureI64(lowerer_.lowerExpr(*s.bg), s.loc);
        bgv = bg.value;
    }

    RuntimeCallBuilder(lowerer_).at(s.loc).argNarrow32(fg.value).argNarrow32(bgv).callHelperVoid(
        RuntimeFeature::TermColor, "rt_term_color_i32");
}

/// @brief Lower the BASIC @c LOCATE statement that positions the cursor.
///
/// @details Evaluates the row and optional column expressions, coercing them to
///          I64 and then narrowing them to the I32 runtime ABI. A missing column
///          defaults to one. The runtime feature is recorded before the call.
///
/// @param s AST node describing the @c LOCATE statement.
/// @pre @p s has a non-null row expression.
void RuntimeStatementLowerer::visit(const LocateStmt &s) {
    auto row = lowerer_.ensureI64(lowerer_.lowerExpr(*s.row), s.loc);
    Value colv = Value::constInt(1);
    if (s.col) {
        auto col = lowerer_.ensureI64(lowerer_.lowerExpr(*s.col), s.loc);
        colv = col.value;
    }

    RuntimeCallBuilder(lowerer_).at(s.loc).argNarrow32(row.value).argNarrow32(colv).callHelperVoid(
        RuntimeFeature::TermLocate, "rt_term_locate_i32");
}

/// @brief Lower the BASIC @c CURSOR statement to control cursor visibility.
///
/// @details Emits a request for the terminal-cursor helper and dispatches the
///          call with either 1 (show) or 0 (hide) based on the parsed visibility
///          flag.  The current source location is preserved for diagnostics.
///
/// @param s AST node representing the @c CURSOR statement.
void RuntimeStatementLowerer::visit(const CursorStmt &s) {
    RuntimeCallBuilder(lowerer_)
        .at(s.loc)
        .argNarrow32(Value::constInt(s.visible ? 1 : 0))
        .callHelperVoid(RuntimeFeature::TermCursor, "rt_term_cursor_visible_i32");
}

/// @brief Lower the BASIC @c ALTSCREEN statement to control alternate screen buffer.
///
/// @details Emits a request for the terminal-altscreen helper and dispatches the
///          call with either 1 (enable) or 0 (disable) based on the parsed enable
///          flag.  The current source location is preserved for diagnostics.
///
/// @param s AST node representing the @c ALTSCREEN statement.
void RuntimeStatementLowerer::visit(const AltScreenStmt &s) {
    RuntimeCallBuilder(lowerer_)
        .at(s.loc)
        .argNarrow32(Value::constInt(s.enable ? 1 : 0))
        .callHelperVoid(RuntimeFeature::TermAltScreen, "rt_term_alt_screen_i32");
}

/// @brief Lower the BASIC SLEEP statement to the runtime helper.
///
/// @details Evaluates the duration expression, coerces it to a 32-bit integer,
///          records the manual sleep-helper requirement, and emits
///          `rt_sleep_ms`. This lowering layer performs no range clamp beyond
///          the narrowing conversion.
///
/// @param s AST node describing the SLEEP statement.
/// @pre @p s has a non-null duration expression.
void RuntimeStatementLowerer::visit(const SleepStmt &s) {
    auto ms = lowerer_.ensureI64(lowerer_.lowerExpr(*s.ms), s.loc);

    RuntimeCallBuilder(lowerer_)
        .at(s.loc)
        .argNarrow32(ms.value)
        .withManualHelper(&Lowerer::requireSleepMs)
        .call("rt_sleep_ms");
}

} // namespace il::frontends::basic
