//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/core/rt_printf_compat.h
// Purpose: Declares the runtime formatting entry point that forwards to libc
// vsnprintf by default and may be interposed where the build supports it.
//
// Key invariants:
//   - The default implementation forwards through a va_list to platform
//     vsnprintf without changing return or truncation semantics.
//   - The implementation is annotated RT_WEAK; actual interposition behavior
//     is compiler- and link-strategy-dependent.
//   - Callers must provide a valid format and matching promoted argument types.
//
// Ownership/Lifetime:
//   - No heap allocation; output is written into a caller-supplied buffer.
//   - The wrapper retains no buffer, format, argument, or process-global state.
//
// Links: src/runtime/core/rt_printf_compat.c (implementation),
//        src/runtime/rt_platform.h (RT_WEAK definition)
//
//===----------------------------------------------------------------------===//
/// @file
/// @brief Runtime snprintf-compatible formatting declaration.
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief snprintf-compatible formatting wrapper.
/// @details The default implementation forwards to platform `vsnprintf`
///          without validating arguments or normalizing platform behavior.
///          Its definition uses @ref RT_WEAK, allowing link-time replacement
///          only where the active toolchain/build supports that mechanism.
/// @param str Destination buffer accepted by `vsnprintf`; a null pointer is
///            valid only when permitted for the supplied @p size.
/// @param size Capacity of @p str in bytes, including any terminator.
/// @param fmt Non-null `printf`-style format string.
/// @param ... Arguments with promoted types matching @p fmt.
/// @return The exact platform `vsnprintf` result, normally the required
///         character count excluding the terminator or a negative error.
int rt_snprintf(char *str, size_t size, const char *fmt, ...);

#ifdef __cplusplus
}
#endif
