//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/core/Global.hpp
// Purpose: Declares the Global struct -- module-scope variables and constants
//          in Zanna IL. Provides named, statically-allocated storage accessible
//          to all functions within a module (string literals, numeric constants,
//          lookup tables, runtime metadata).
// Key invariants:
//   - Global names must be unique among all globals owned by the module.
//   - Initializer type must match the declared type of the global.
//   - Non-empty init only for globals with constant values (e.g. UTF-8 strings).
// Ownership/Lifetime: Module owns Global structs by value in a std::vector.
//          Each Global owns its name string, type, and initializer data.
//          Global lifetime matches the containing module's lifetime.
// Links: docs/il/il-guide.md#reference, il/core/Type.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Declares module-scope IL storage objects and their linkage metadata.
 *
 * @details A `Global` owns its canonical name, declared type, serialized
 *          initializer, source-level const/initializer intent, and cross-module
 *          visibility. An optional interned name sidecar supports efficient
 *          module lookup without replacing the owned textual identity.
 */

#pragma once

#include "il/core/Linkage.hpp"
#include "il/core/Type.hpp"
#include "support/symbol.hpp"
#include <string>

namespace il::core {
/// @brief Module-scope variable or constant.
///
/// Globals provide named storage that is accessible to all functions within a
/// module. Each global carries its own identifier, declared type, and optional
/// initializer for constant data. The owning `Module` manages the lifetime of
/// these objects.
struct Global {
    /// @brief Identifier of the global within its module.
    /// @invariant Unique among all globals owned by the module.
    std::string name;

    /// @brief Declared IL type of the global.
    /// @invariant Must match the type of any provided initializer.
    Type type;

    /// @brief Serialized initializer data, if any.
    /// @invariant Non-empty only for globals with constant values (e.g. UTF-8
    /// string literals).
    std::string init;

    /// @brief Cross-module visibility for the IL linker.
    /// @details Defaults to Internal for backwards compatibility.
    /// @see ADR-0003, il/core/Linkage.hpp
    Linkage linkage = Linkage::Internal;

    /// @brief Whether source text declared this global with the `const` qualifier.
    /// @details String globals are immutable runtime string constants regardless
    ///          of this flag; the flag preserves round-trip intent for other
    ///          scalar globals.
    bool isConst = false;

    /// @brief Whether the source explicitly supplied an initializer.
    /// @details An empty string initializer is distinct from an omitted
    ///          initializer for textual round-tripping.
    bool hasInitializer = false;

    /// @brief Interned handle for @ref name within the owning Module.
    /// @details Invalid until populated by a Module helper or a construction
    ///          path such as the parser or IRBuilder.
    il::support::Symbol nameSymbol{};
};

} // namespace il::core
