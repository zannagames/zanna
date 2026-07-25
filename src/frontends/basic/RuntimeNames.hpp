//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
///
/// @file RuntimeNames.hpp
/// @brief BASIC frontend runtime name constants.
/// @details Imports generated canonical runtime names and defines compatibility
///          aliases used by BASIC lowering. Each pointer refers to
///          process-lifetime string-literal storage.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "il/runtime/RuntimeNames.hpp"

namespace il::frontends::basic::runtime {

/// Make generated canonical runtime constants directly visible to BASIC lowering.
using namespace il::runtime::names;

/// Compatibility name for integer-to-string conversion.
inline constexpr const char *kStringFromInt = kCoreConvertToStringInt;
/// Compatibility name for floating-point-to-string conversion.
inline constexpr const char *kStringFromDouble = kCoreConvertToStringDouble;

/// Short alias for the canonical conversion-to-double helper name.
inline constexpr const char *kConvertToDouble = kCoreConvertToDouble;
/// Short alias for the canonical conversion-to-I64 helper name.
inline constexpr const char *kConvertToInt = kCoreConvertToInt64;

/// Short alias for the canonical fallible double parser name.
inline constexpr const char *kParseDouble = kCoreParseTryDouble;
/// Short alias for the canonical fallible I64 parser name.
inline constexpr const char *kParseInt64 = kCoreParseTryInt;
/// Legacy C-string double parser symbol.
inline constexpr const char *kParseDoubleCStr = "rt_parse_double";
/// Legacy C-string I64 parser symbol.
inline constexpr const char *kParseInt64CStr = "rt_parse_int64";
/// Raw string-field splitting runtime symbol.
inline constexpr const char *kStringSplitFieldsRaw = "rt_str_split_fields";

} // namespace il::frontends::basic::runtime
