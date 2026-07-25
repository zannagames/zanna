//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/types/TypeMapping.hpp
// Purpose: Declare the scalar IL-to-BASIC type mapping used when importing
//          procedure and runtime signatures.
// Key invariants:
//   * I16, I32, I64, and opaque pointers map to BASIC Type::I64.
//   * F64, string, and I1 map to their corresponding scalar BASIC types.
//   * Void and unsupported IL kinds return std::nullopt.
// Ownership: The mapping is stateless and retains no reference to its input.
// References: docs/internals/codemap/basic.md
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Declares scalar IL-to-BASIC signature type translation.
/// @details This boundary deliberately represents opaque pointers as integer
///          handles; object identity is refined by later semantic/OOP layers.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "frontends/basic/ast/NodeFwd.hpp"
#include "il/core/Type.hpp"
#include <optional>

namespace il::frontends::basic::types {

/// @brief Map an IL core type to the corresponding BASIC AST scalar type.
/// @details Translates integer, floating-point, string, boolean, and pointer
///          kinds into the frontend's canonical scalar types. Void and any IL
///          kind without a BASIC scalar representation return std::nullopt.
/// @param ilType The IL core type to translate.
/// @return The corresponding BASIC type, or std::nullopt if unsupported.
std::optional<Type> mapIlToBasic(const il::core::Type &ilType);

} // namespace il::frontends::basic::types
