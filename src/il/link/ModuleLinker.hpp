//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/link/ModuleLinker.hpp
// Purpose: Declares the IL module linker that merges multiple IL modules into
//          a single module, resolving Export/Import linkage pairs.
// Key invariants:
//   - Exactly one input module may contain a function named "main".
//   - Every Import-linkage function must resolve to an Export or Internal
//     definition in another module.
//   - Extern signatures must agree across modules.
// Ownership/Lifetime: Consumes input modules by move; returns a new merged module.
// Links: docs/adr/0003-il-linkage-and-module-linking.md,
//        il/core/Module.hpp, il/core/Linkage.hpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares the consuming IL module-linking interface.

#pragma once

#include "il/core/Module.hpp"

#include <string>
#include <vector>

namespace il::link {

/// @brief Result of linking multiple IL modules.
struct LinkResult {
    /// @brief The merged module (valid only when errors is empty).
    il::core::Module module;

    /// @brief Diagnostic messages from the linking process.
    /// @details Empty on success. Each entry describes a specific link error.
    std::vector<std::string> errors;

    /// @brief Check if linking succeeded.
    /// @return True exactly when no linking diagnostics were recorded.
    [[nodiscard]] bool succeeded() const {
        return errors.empty();
    }
};

/// @brief Merge multiple IL modules into a single module.
///
/// @details The linker performs the following steps:
///   1. Identify the entry module (the one containing "main").
///   2. Resolve Import-linkage functions against Export definitions.
///   3. Merge all functions, globals, and externs into one module.
///   4. Prefix Internal-linkage functions from non-entry modules to
///      avoid name collisions.
///   5. Inject calls to all module initializers in the merged "main".
///
/// @param modules Input modules to link (consumed by move).
/// @return LinkResult containing the merged module or error diagnostics.
LinkResult linkModules(std::vector<il::core::Module> modules);

} // namespace il::link
