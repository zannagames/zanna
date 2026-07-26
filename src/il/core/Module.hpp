//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/core/Module.hpp
// Purpose: Declares the Module struct -- the top-level container for an IL
//          compilation unit aggregating externs, globals, and function
//          definitions. Tracks version and optional target triple.
// Key invariants:
//   - Function, extern, and global names must be unique within the module.
//   - version defaults to ZANNA_IL_VERSION_STR for new modules.
// Ownership/Lifetime: Module owns all contained entities by value through
//          std::vector containers. Movable efficiently; copying is expensive
//          (deep copy of all functions). Most code works with Module by
//          reference or pointer.
// Links: docs/il/il-guide.md#reference, il/core/Extern.hpp,
//        il/core/Function.hpp, il/core/Global.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Declares the top-level container for one IL compilation unit.
 *
 * @details A `Module` owns its format/target metadata, external declarations,
 *          globals, function definitions, and identifier interner. Textual
 *          names remain authoritative for diagnostics and serialization;
 *          module-scoped symbol sidecars provide stable, compact identities
 *          for analyses and transformations.
 */

#pragma once

#include "il/core/Extern.hpp"
#include "il/core/Function.hpp"
#include "il/core/Global.hpp"
#include "support/string_interner.hpp"
#include "zanna/version.hpp"
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace il::core {

/// @brief IL module aggregating externs, globals, and functions.
struct Module {
    /// @brief Module format version string.
    ///
    /// Defaults to configured IL spec version for newly constructed modules and
    /// may be overwritten by parsers when reading serialized IL.
    std::string version = ZANNA_IL_VERSION_STR;

    /// @brief Optional target triple directive associated with the module.
    ///
    /// Absent by default; populated when a `target "triple"` directive is
    /// encountered during parsing or assigned programmatically.
    std::optional<std::string> target;

    /// @brief Declared external functions available to the module.
    ///
    /// Starts empty and is populated as extern declarations are processed.
    std::vector<Extern> externs;

    /// @brief Global variable declarations.
    ///
    /// Initialized empty; parsers append entries for each global encountered.
    std::vector<Global> globals;

    /// @brief Function definitions contained in the module.
    ///
    /// Begins empty and receives entries as functions are defined or parsed.
    std::vector<Function> functions;

    /// @brief Module-owned storage for interned IR identifiers.
    /// @details Symbols are only meaningful in the context of the Module that
    ///          created them. The interner owns stable copies of function names,
    ///          extern names, global names, block labels, callees, and branch
    ///          target labels so analyses can compare compact handles instead
    ///          of repeatedly hashing strings.
    il::support::StringInterner symbols{};

    /// @brief Intern an identifier in this module's symbol table.
    /// @param text Identifier text to intern. Empty input returns an invalid
    ///        symbol.
    /// @return Stable symbol handle owned by @ref symbols, or an invalid symbol
    ///         if @p text is empty or the interner is full.
    [[nodiscard]] il::support::Symbol internIdentifier(std::string_view text);

    /// @brief Look up the text for a module-owned symbol.
    /// @param symbol Symbol previously returned by @ref internIdentifier.
    /// @return Interned text, or an empty view when @p symbol is invalid for
    ///         this module.
    [[nodiscard]] std::string_view lookupIdentifier(il::support::Symbol symbol) const;

    /// @brief Test whether a symbol belongs to this module's interner.
    /// @param symbol Symbol handle to validate.
    /// @return True when @p symbol is nonzero and names an interned string in
    ///         this module.
    [[nodiscard]] bool containsIdentifier(il::support::Symbol symbol) const noexcept;

    /// @brief Populate every identifier sidecar from the current string fields.
    /// @details This pass is idempotent and safe after manual IR construction,
    ///          parsing, linking, or transformations that rewrite names. It
    ///          interns module declarations, function names, block labels,
    ///          direct callees, and branch target labels while preserving the
    ///          existing string fields for diagnostics and serialization.
    void internOwnedIdentifiers();
};

} // namespace il::core
