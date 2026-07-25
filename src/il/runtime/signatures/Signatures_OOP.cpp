//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/il/runtime/signatures/Signatures_OOP.cpp
// Purpose: Placeholder for OOP runtime signatures. OOP functions are registered
//          via the extern registry in vm/OOPRuntime.cpp rather than through the
//          RuntimeDescriptor table, so this file intentionally registers no
//          signatures to avoid validation failures.
// Links: docs/oop.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Documents the intentionally empty debug OOP signature group.
/// @details OOP helpers use the VM extern registry rather than the runtime
///          descriptor registry validated by these signature modules.

#include "il/runtime/signatures/Registry.hpp"

namespace il::runtime::signatures {

/// @brief OOP signatures are registered via extern registry, not here.
/// @details OOP runtime functions like rt_get_class_vtable, rt_register_class_*,
///          and rt_register_interface_* are implemented in the C runtime and
///          registered with the VM's extern registry during initialization.
///          They don't use the RuntimeDescriptor table, so registering them
///          here would cause validation failures.
void register_oop_signatures() {
    // Intentionally empty - OOP functions use extern registry
}

} // namespace il::runtime::signatures
