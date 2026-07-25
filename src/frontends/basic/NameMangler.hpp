//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// This file declares the NameMangler class, which generates deterministic,
// unique names for IL symbols, temporaries, and basic blocks during the
// lowering process.
//
// This file re-exports the common NameMangler from the shared frontend library.
//
//===----------------------------------------------------------------------===//
/// @file NameMangler.hpp
/// @brief Exposes the shared deterministic name generator to the BASIC frontend.
/// @details BASIC uses the common frontend implementation directly, including
///          its configurable temporary prefix, per-hint block counters,
///          collision set, reset operation, and counter-overflow diagnostics.

#pragma once

#include "frontends/common/NameMangler.hpp"

namespace il::frontends::basic {

/// @brief Alias for the common NameMangler.
/// @details The aliased value type owns all counters and collision state; it
///          does not borrow AST, module, builder, or lowering objects.
using NameMangler = ::il::frontends::common::NameMangler;

} // namespace il::frontends::basic
