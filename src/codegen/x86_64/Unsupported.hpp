//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/Unsupported.hpp
// Purpose: Provides diagnostic helpers for unsupported Phase A features.
// Key invariants:
//   - phaseAUnsupported() never returns; it always throws std::runtime_error.
//   - The thrown message identifies the unsupported feature by name.
// Ownership/Lifetime:
//   - Header-only utility with no state; exceptions propagate to the caller.
// Links: src/codegen/x86_64/Backend.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include <stdexcept>
#include <string>

/// @file
/// @brief Defines the x86-64 backend's standardized unsupported-form error helper.

namespace zanna::codegen::x64 {

/// @brief Raises a standardized exception for a form the x86-64 backend rejects.
/// @details Prefixes the supplied feature or malformed-form description with
///          backend and implementation-phase context. Lowering and selection
///          code use this helper so unsupported paths report a consistent
///          diagnostic and cannot accidentally continue.
/// @param feature Null-terminated description of the rejected feature or form.
/// @pre @p feature points to a valid null-terminated string.
/// @throws std::runtime_error Always, with @p feature appended to the message
///         @c "x86-64 Phase A does not support: ".
[[noreturn]] inline void phaseAUnsupported(const char *feature) {
    throw std::runtime_error(std::string("x86-64 Phase A does not support: ") + feature);
}

} // namespace zanna::codegen::x64
