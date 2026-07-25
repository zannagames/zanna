//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
///
/// @file LowererTypeLayout.cpp
/// @brief Implements registration of Zia struct, class, and interface layouts.
///
/// @details Registration transfers completed metadata into the module-local
///          registry. Registering an existing name replaces its prior record.
///
//===----------------------------------------------------------------------===//

#include "frontends/zia/LowererTypeLayout.hpp"

namespace il::frontends::zia {

/// @brief Register or replace a value-type layout.
/// @param name Qualified value-type name used as the registry key.
/// @param info Completed field and method metadata to store.
void LowererTypeLayout::registerValueType(const std::string &name, StructTypeInfo info) {
    structTypes_[name] = std::move(info);
}

/// @brief Register or replace an entity-type layout.
/// @param name Qualified class name used as the registry key.
/// @param info Completed field, method, and dispatch metadata to store.
void LowererTypeLayout::registerEntityType(const std::string &name, ClassTypeInfo info) {
    classTypes_[name] = std::move(info);
}

/// @brief Register or replace an interface-type layout.
/// @param name Qualified interface name used as the registry key.
/// @param info Completed method and slot metadata to store.
void LowererTypeLayout::registerInterfaceType(const std::string &name, InterfaceTypeInfo info) {
    interfaceTypes_[name] = std::move(info);
}

} // namespace il::frontends::zia
