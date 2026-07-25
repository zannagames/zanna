//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: frontends/basic/builtins/MathBuiltins.hpp
// Purpose: Declares helpers for registering BASIC math builtins in the central
//          builtin registry (info, scan, and lowering tables).
// Key invariants: Registration writes entries at the dense index matching each
//                 BuiltinCallExpr::Builtin enumerator; it never resizes tables.
// Ownership/Lifetime: Functions mutate caller-provided tables in-place.
// Links: docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//

#pragma once

#include <span>

#include "frontends/basic/BuiltinRegistry.hpp"

/// @file
/// @brief Declares registration functions for BASIC mathematical builtins.
/// @details Each function fills one projection of the central builtin
///          registry. Callers must supply a dense span large enough to index
///          every mathematical BuiltinCallExpr::Builtin enumerator.

namespace il::frontends::basic::builtins {

/// @brief Populate builtin metadata for math helpers (INT/FIX/ABS/etc.).
/// @param infos Dense table indexed by BuiltinCallExpr::Builtin.
/// @post Slots associated with every supported mathematical builtin contain
///       their source spelling and optional semantic-analysis callback.
void registerMathBuiltinInfos(std::span<BuiltinInfo> infos);

/// @brief Populate scan rules describing math builtin traversal.
/// @param rules Dense table indexed by BuiltinCallExpr::Builtin.
/// @post Mathematical builtin slots describe result inference, argument
///       traversal, and runtime feature tracking for semantic scanning.
void registerMathBuiltinScanRules(std::span<BuiltinScanRule> rules);

/// @brief Populate lowering rules describing math builtin emission.
/// @param rules Dense table indexed by BuiltinCallExpr::Builtin.
/// @post Mathematical builtin slots describe coercions, runtime calls, result
///       types, and feature tracking used by IL lowering.
void registerMathBuiltinLoweringRules(std::span<BuiltinLoweringRule> rules);

} // namespace il::frontends::basic::builtins
