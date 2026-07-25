//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/LoweringContext.cpp
// Purpose: Provide the stateful helpers used when lowering BASIC surface syntax
//          into IL instructions and blocks.
// Key invariants: Slot names, block labels, and string identifiers are stable
//                 and deterministic within a compilation unit.
// Links: docs/internals/codemap/basic.md
//
//===----------------------------------------------------------------------===//

/// @file LoweringContext.cpp
/// @brief Implements BASIC variable-slot and string-symbol name caches.
/// @details The helpers in this file assign deterministic textual identifiers;
///          they do not emit allocations, globals, blocks, or instructions.
///          Callers remain responsible for materializing each returned name in
///          the destination function or module.

#include "frontends/basic/LoweringContext.hpp"
#include "zanna/il/IRBuilder.hpp"
#include "zanna/il/Module.hpp"

namespace il::frontends::basic {

/// @brief Construct a lowering context for a BASIC function.
/// @details Stores non-owning references to the active builder and destination
///          function and initializes empty per-context name caches.
/// @param builder Builder associated with the lowering operation.
/// @param func Function associated with the lowering operation.
/// @pre @p builder and @p func outlive the constructed context.
LoweringContext::LoweringContext(build::IRBuilder &builder, core::Function &func)
    : builder(builder), function(func) {}

/// @brief Retrieve the cached slot identifier for @p name or assign one.
/// @details A first lookup records the textual identifier `%<name>_slot`;
///          subsequent lookups return the same string. This method does not
///          emit an `alloca` or otherwise modify the destination function.
/// @param name BASIC variable identifier.
/// @return The stable slot identifier associated with @p name in this context.
std::string LoweringContext::getOrCreateSlot(const std::string &name) {
    auto it = varSlots.find(name);
    if (it != varSlots.end())
        return it->second;
    std::string slot = "%" + name + "_slot";
    varSlots[name] = slot;
    return slot;
}

/// @brief Retrieve the cached symbol for string @p value or assign one.
/// @details A distinct value receives the next `.L<N>` name, beginning at
///          `.L0`; repeated values reuse their existing name without advancing
///          the counter. This method records only the mapping and does not add a
///          global string to an IL module.
/// @param value String value used as the cache key.
/// @return The stable generated symbol associated with @p value in this context.
std::string LoweringContext::getOrAddString(const std::string &value) {
    auto it = strings.find(value);
    if (it != strings.end())
        return it->second;
    std::string name = ".L" + std::to_string(nextStringId++);
    strings[value] = name;
    return name;
}

} // namespace il::frontends::basic
