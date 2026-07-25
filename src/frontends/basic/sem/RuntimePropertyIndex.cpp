//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/sem/RuntimePropertyIndex.cpp
// Purpose: Implement a case-insensitive property index derived from the runtime
//          class catalog for BASIC semantic analysis.
// Key invariants:
//   * Composite keys normalize both class and property names.
//   * Each seed operation replaces the previous snapshot atomically from the
//     caller's perspective.
//   * A missing setter always makes a property read-only.
// Ownership: The index copies all catalog strings and returns copied property
//            records; it retains no pointers into the runtime catalog.
// References: docs/internals/codemap/basic.md
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Implements runtime property catalog indexing for BASIC.
//
//===----------------------------------------------------------------------===//

#include "frontends/basic/sem/RuntimePropertyIndex.hpp"
#include "il/runtime/classes/RuntimeClasses.hpp"

#include <cctype>

namespace il::frontends::basic {

/// @brief Normalize an identifier for case-insensitive property lookup.
/// @param s Class or property spelling to normalize.
/// @return Lowercase copy of @p s.
std::string RuntimePropertyIndex::toLower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s)
        out.push_back(static_cast<char>(std::tolower(c)));
    return out;
}

/// @brief Build the composite lookup key for a runtime property.
/// @details Normalizes both components and separates them with a character that
///          cannot be confused with the dotted class name.
/// @param cls Fully qualified runtime class name.
/// @param prop Runtime property name.
/// @return Canonical `class|property` lookup key.
std::string RuntimePropertyIndex::keyFor(std::string_view cls, std::string_view prop) {
    std::string key;
    key.reserve(cls.size() + prop.size() + 1);
    key.append(toLower(cls));
    key.push_back('|');
    key.append(toLower(prop));
    return key;
}

/// @brief Replace the index contents with properties from a runtime class catalog.
/// @details Copies each property's type and accessor targets. A property is
///          marked read-only when the catalog says so or when no setter target
///          is supplied.
/// @param classes Runtime classes whose property metadata should be indexed.
void RuntimePropertyIndex::seed(const std::vector<il::runtime::RuntimeClass> &classes) {
    map_.clear();
    for (const auto &cls : classes) {
        for (const auto &p : cls.properties) {
            RuntimePropertyInfo info;
            info.type = p.type ? p.type : "";
            info.getter = p.getter ? p.getter : "";
            info.setter = p.setter ? p.setter : "";
            info.readonly = p.readonly || info.setter.empty();
            map_[keyFor(cls.qname, p.name)] = std::move(info);
        }
    }
}

/// @brief Look up a runtime property without regard to source spelling case.
/// @param classQName Fully qualified receiver class name.
/// @param propName Property name on the receiver.
/// @return A copied property record, or std::nullopt when the key is absent.
std::optional<RuntimePropertyInfo> RuntimePropertyIndex::find(std::string_view classQName,
                                                              std::string_view propName) const {
    auto it = map_.find(keyFor(classQName, propName));
    if (it == map_.end())
        return std::nullopt;
    return it->second;
}

/// @brief Access the process-wide runtime property index.
/// @return Reference to the lazily initialized singleton.
RuntimePropertyIndex &runtimePropertyIndex() {
    static RuntimePropertyIndex idx;
    return idx;
}

} // namespace il::frontends::basic
