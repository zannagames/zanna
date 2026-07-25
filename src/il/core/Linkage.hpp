//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/core/Linkage.hpp
// Purpose: Declares the Linkage enum for IL functions and globals, controlling
//          visibility across module boundaries for the IL module linker.
// Key invariants:
//   - Internal is the default, preserving backwards compatibility.
//   - Import-linkage functions must have no body (empty blocks vector).
//   - Export-linkage functions must have a body.
// Ownership/Lifetime: Pure enum, no runtime state.
// Links: docs/adr/0003-il-linkage-and-module-linking.md,
//        il/core/Function.hpp, il/core/Global.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

namespace il::core {

/// @brief Controls cross-module visibility of functions and globals.
///
/// @details Linkage determines how the IL module linker treats a definition:
///   - `Internal`: visible only within the defining module (default).
///   - `Export`: defined here, visible to other modules via the linker.
///   - `Import`: declared here, defined in another module (no body).
///
/// @see ADR-0003 for design rationale.
enum class Linkage {
    Internal, ///< Module-private (default for backwards compatibility).
    Export,   ///< Defined here, callable from other modules.
    Import    ///< Declared here, resolved by the linker from another module.
};

/// @brief Return a human-readable name for a Linkage value.
/// @param l Linkage enumerator to format.
/// @return Static lowercase linkage name, or `unknown` for an invalid value.
inline const char *linkageName(Linkage l) {
    switch (l) {
        case Linkage::Internal:
            return "internal";
        case Linkage::Export:
            return "export";
        case Linkage::Import:
            return "import";
    }
    return "unknown";
}

} // namespace il::core
