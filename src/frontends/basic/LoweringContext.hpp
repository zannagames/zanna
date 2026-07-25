//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/LoweringContext.hpp
// Purpose: Declares state container used in BASIC-to-IL lowering.
// Key invariants: None.
// Ownership/Lifetime: Does not own referenced module.
// Links: docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//

/// @file LoweringContext.hpp
/// @brief Declares the legacy per-function BASIC lowering name cache.
/// @details LoweringContext associates source variable names and string values
///          with deterministic IL identifiers while retaining borrowed access
///          to the active builder and function. Name lookup alone emits no IR.

#pragma once

#include <string>
#include <unordered_map>

namespace il {

namespace core {
struct Function;
} // namespace core

namespace build {
class IRBuilder;
} // namespace build
} // namespace il

namespace il::frontends::basic {

/// @brief Tracks deterministic identifier mappings during BASIC lowering.
/// @details The object owns its maps and generated strings but only borrows the
///          builder and function supplied at construction.
/// @invariant Each variable name and each distinct string value has at most one
///            associated generated identifier per context.
class LoweringContext {
  public:
    /// @brief Create a context to lower into @p builder and populate @p func.
    /// @param builder Builder borrowed by the context.
    /// @param func Destination function borrowed by the context.
    /// @pre @p builder and @p func remain alive for this object's lifetime.
    LoweringContext(build::IRBuilder &builder, core::Function &func);

    /// @brief Get or assign the slot identifier for BASIC variable @p name.
    /// @details New entries use `%<name>_slot`; this operation does not emit a
    ///          stack allocation.
    /// @param name Source variable name used as the cache key.
    /// @return Stable slot identifier for @p name.
    std::string getOrCreateSlot(const std::string &name);

    /// @brief Get or assign the generated symbol for string @p value.
    /// @details New distinct values receive `.L0`, `.L1`, and so on; this
    ///          operation does not materialize a module global.
    /// @param value String contents used as the cache key.
    /// @return Stable generated symbol for @p value.
    std::string getOrAddString(const std::string &value);

  private:
    /// Borrowed IR builder associated with this context.
    build::IRBuilder &builder;

    /// Borrowed function associated with this context.
    core::Function &function;

    /// Owned mapping from BASIC variable names to textual slot identifiers.
    std::unordered_map<std::string, std::string> varSlots;

    /// Owned mapping from distinct string values to generated symbol names.
    std::unordered_map<std::string, std::string> strings;

    /// Next numeric suffix assigned to a previously unseen string value.
    unsigned nextStringId{0};
};

} // namespace il::frontends::basic
