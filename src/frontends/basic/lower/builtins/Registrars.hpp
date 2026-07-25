//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/lower/builtins/Registrars.hpp
// Purpose: Declares callback-registration entry points for BASIC builtin
//          lowering families.
// Key invariants: Each function mutates the process-wide callback registry only
//                 for the builtin family it owns.
// Ownership/Lifetime: Registered function pointers have static lifetime and
//                     borrow BuiltinLowerContext for each invocation.
// Links: src/frontends/basic/lower/common/BuiltinUtils.cpp,
//        src/frontends/basic/lower/BuiltinCommon.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

/// @file
/// @brief Declares per-family BASIC builtin lowering registrars.

namespace il::frontends::basic::lower::builtins {
/// @brief Populate the builtin registry with the core scalar operations.
/// @details Installs the default generic callback used by builtin names that do
///          not have a more specialized family handler.
void registerDefaultBuiltins();

/// @brief Register builtin handlers that operate on BASIC strings.
/// @details Binds the callback-driven dispatcher for STR$, INKEY$, and GETKEY$;
///          other string builtins retain the default rule-driven callback.
void registerStringBuiltins();

/// @brief Register conversion intrinsic handlers such as VAL and STR$.
/// @details Binds numeric conversion calls that require conversion-specific
///          dispatch or guarded runtime paths.
void registerConversionBuiltins();

/// @brief Register math intrinsic lowering handlers.
/// @details Binds registry-driven trigonometric, exponential, rounding, random,
///          sign, and related mathematical operations.
void registerMathBuiltins();

/// @brief Invoke the array-family registration extension point.
/// @details Currently installs no callbacks because array operations are
///          lowered by dedicated expression and statement paths.
void registerArrayBuiltins();

/// @brief Register generic callbacks owned by the I/O builtin family.
/// @details Currently binds TIMER; stateful file and console operations remain
///          on dedicated lowering paths.
void registerIoBuiltins();
} // namespace il::frontends::basic::lower::builtins
