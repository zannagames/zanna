//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/NativeEHLowering.hpp
// Purpose: Lower IL EH markers into native-friendly control flow before backend
//          lowering.
// Key invariants:
//   - After lowering no structured EH marker or handler token block remains.
//   - The setjmp call matches the target platform's C runtime convention.
// Ownership/Lifetime:
//   - Rewrites the caller-owned module in place; no state outlives a call.
// Links: src/codegen/common/NativeEHLowering.cpp, src/runtime/core/rt_io.c
//
//===----------------------------------------------------------------------===//

#pragma once

#include "il/core/Module.hpp"

#include <optional>
#include <string>

/// @file
/// @brief Declares structured-EH normalization for native code generation.

namespace zanna::codegen::common {

/// @brief The platform `setjmp` entry point native EH frames call.
enum class NativeSetjmpVariant {
    /// `setjmp(env)`: glibc never saves the signal mask.
    Plain,
    /// `_setjmp(env)` on Darwin. BSD `setjmp` saves the signal mask with a
    /// `sigprocmask` syscall on every `try` entry; the runtime's
    /// `rt_native_eh_*` helpers unwind with the matching `_longjmp` there.
    DarwinMaskFree,
    /// `setjmp(env, NULL)` on Windows. The CRT stores the second argument as the
    /// jump buffer's frame, and `longjmp` runs an SEH unwind toward any non-null
    /// frame. A null frame restores the saved context directly, as `longjmp`
    /// does on the other platforms.
    WindowsNullFrame,
};

/// @brief Rewrite structured EH into ordinary IL calls/branches for native codegen.
///
/// Replaces `eh.push`/`eh.pop` with runtime frame management and `setjmp`,
/// assigns synthetic resume-site identifiers around potentially trapping
/// instructions, converts handler error/resume-token parameters to native
/// pointer/integer types, and expands resume operations into validated dispatch
/// control flow. Required runtime externs are inserted with exact ABI checks.
///
/// @param[in,out] module IL module whose functions and extern table may be rewritten.
/// @param setjmpVariant Platform `setjmp` symbol and argument convention.
/// @return `true` when at least one function contained structured EH markers.
/// @throws No C++ exception intentionally; malformed lowering invariants report
///         an internal compiler error and abort.
bool lowerNativeEh(il::core::Module &module,
                   NativeSetjmpVariant setjmpVariant = NativeSetjmpVariant::Plain);

/// @brief Report the first structured EH marker that survived native EH lowering.
/// @details Native backends expect @ref lowerNativeEh to erase all structured EH
///          markers and handler resume-token block shapes before IL optimization
///          and MIR lowering. A non-empty result is a backend-lowering bug.
/// @param module Lowered IL module to audit.
/// @return Description of the first residual marker/handler shape, including
///         function and block names, or `std::nullopt` when clean.
[[nodiscard]] std::optional<std::string> findResidualStructuredEh(const il::core::Module &module);

} // namespace zanna::codegen::common
