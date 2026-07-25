//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/ScopeTracker.hpp
// Purpose: Backward-compatible alias re-exporting the shared ScopeTracker.
//          New code should include frontends/common/ScopeTracker.hpp directly.
// Key invariants: Delegates entirely to common::ScopeTracker; adds no new state.
// Ownership/Lifetime: Header-only alias; no owned resources.
// Links: frontends/common/ScopeTracker.hpp
//
//===----------------------------------------------------------------------===//
/// @file ScopeTracker.hpp
/// @brief Re-exports the shared lexical scope tracker for BASIC compatibility.
/// @details The aliased value type owns its scope stack, copied name bindings,
///          and unique-name counter, and supplies an RAII scope guard.

#pragma once

#include "frontends/common/ScopeTracker.hpp"

namespace il::frontends::basic {

/// @brief Backward-compatible alias for the shared scope tracker.
/// @details The aliased type tracks scope nesting and symbol visibility during
///          semantic analysis and lowering, resolves from inner to outer scope,
///          and generates deterministic suffixed local names. Prefer the common
///          namespace in new code.
using ScopeTracker = ::il::frontends::common::ScopeTracker;

} // namespace il::frontends::basic
