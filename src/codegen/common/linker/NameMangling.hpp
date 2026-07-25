//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/linker/NameMangling.hpp
// Purpose: Shared Mach-O/Darwin symbol name mangling utilities.
//          Centralizes the underscore-prefix convention so that all linker
//          and object file writer code uses a single mangling point.
// Key invariants:
//   - Local labels (starting with 'L' or '.') are NOT mangled
//   - Empty names are returned unchanged
//   - machoMangle() is idempotent only for non-mangled inputs
// Links: MachOWriter.cpp, MachOExeWriter.cpp, SymbolResolver.cpp,
//        NativeLinker.cpp, DeadStripPass.cpp
//
//===----------------------------------------------------------------------===//

/**
 * @file NameMangling.hpp
 * @brief Centralizes Darwin underscore mangling and platform-aware symbol lookup.
 */

#pragma once

#include "codegen/common/linker/LinkTypes.hpp"

#include <string>
#include <unordered_map>

namespace zanna::codegen::linker {

/// @brief Applies the Darwin/Mach-O external-symbol underscore convention.
/// @param name Logical symbol name.
/// @return @p name unchanged for empty names and local labels beginning with
///         `L` or `.`, otherwise a copy with one underscore prepended.
/// @note This function does not detect already-mangled external names.
inline std::string machoMangle(const std::string &name) {
    if (name.empty())
        return name;
    if (name[0] == 'L' || name[0] == '.')
        return name; // Local label — no mangling.
    return "_" + name;
}

/// @brief Looks up both logical and Darwin-mangled spellings of a symbol.
/// @tparam MapT Associative container exposing `find`, `end`, and string keys.
/// @param map Symbol map to search.
/// @param name Requested spelling.
/// @return Iterator to an exact, de-underscored, or underscore-prefixed match,
///         in that priority order, or `map.end()`.
template <typename MapT>
auto findWithMachoFallback(MapT &map, const std::string &name) -> decltype(map.find(name)) {
    auto it = map.find(name);
    if (it != map.end())
        return it;

    if (!name.empty() && name[0] == '_') {
        it = map.find(name.substr(1));
        if (it != map.end())
            return it;
    }

    return map.find("_" + name);
}

/// @brief Looks up a symbol with target-platform spelling rules.
/// @details macOS uses `findWithMachoFallback`; Linux and Windows require an
///          exact key because a leading underscore is semantically significant.
/// @tparam MapT Associative container exposing `find` and `end`.
/// @param map Symbol map to search.
/// @param name Requested symbol spelling.
/// @param platform Target platform.
/// @return Matching iterator or `map.end()`.
template <typename MapT>
auto findWithPlatformFallback(MapT &map, const std::string &name, LinkPlatform platform)
    -> decltype(map.find(name)) {
    if (platform == LinkPlatform::macOS)
        return findWithMachoFallback(map, name);
    return map.find(name);
}

} // namespace zanna::codegen::linker
