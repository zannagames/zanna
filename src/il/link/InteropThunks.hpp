//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/link/InteropThunks.hpp
// Purpose: Generate boolean conversion thunks at link time to bridge type
//          representation differences between Zia (i1) and BASIC (i64).
// Key invariants:
//   - i1→i64 uses Zext (not sign-extend, so true=1 not -1).
//   - i64→i1 uses IcmpNe vs 0 (any non-zero value maps to true).
//   - Thunks are only generated when an import/export pair has boolean mismatches.
// Ownership/Lifetime: Generates new Function objects owned by the caller.
// Links: docs/adr/0003-il-linkage-and-module-linking.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares boolean ABI interop thunk generation for the IL linker.

#pragma once

#include "il/core/Function.hpp"
#include "il/core/Module.hpp"

#include <string>
#include <vector>

namespace il::link {

/// @brief Information about a generated boolean thunk.
struct ThunkInfo {
    /// @brief Name of the thunk function.
    std::string thunkName;

    /// @brief Name of the original target function.
    std::string targetName;

    /// @brief The generated thunk function.
    il::core::Function thunk;
};

/// @brief Scan for boolean type mismatches between Import and Export function
///        pairs and generate conversion thunks.
///
/// @details For each pair where the Import declaration differs from the Export
///          definition in boolean type (i1 vs i64 in return type or parameters),
///          a wrapper function is generated that performs the conversion.
///          Variadic pairs and any non-boolean mismatch are not adapted.
///
/// @param importModule Module containing Import declarations.
/// @param exportModule Module containing Export definitions.
/// @return List of thunks to add to the merged module.
std::vector<ThunkInfo> generateBooleanThunks(const il::core::Module &importModule,
                                             const il::core::Module &exportModule);

} // namespace il::link
