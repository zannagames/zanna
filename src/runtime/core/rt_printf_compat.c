//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/core/rt_printf_compat.c
// Purpose: Provides the runtime's overridable, snprintf-compatible formatting
//          entry point and isolates its varargs forwarding to libc vsnprintf.
//
// Key invariants:
//   - rt_snprintf forwards its buffer, bound, format, and variadic arguments
//     directly to the platform vsnprintf implementation.
//   - Return, truncation, termination, encoding-error, and invalid-format
//     behavior is exactly the behavior of that platform implementation.
//   - RT_WEAK controls link-time interposition where the selected compiler
//     supports it; on compilers where it expands to nothing this is an ordinary
//     definition and replacement remains a build/link concern.
//   - The wrapper owns no mutable state and performs no stream I/O.
//
// Ownership/Lifetime:
//   - Writes into a caller-supplied buffer; no heap allocation is performed.
//   - No state is retained between calls.
//
// Links: src/runtime/core/rt_printf_compat.h (public API),
//        src/runtime/core/rt_int_format.c (uses rt_snprintf),
//        src/runtime/core/rt_output.c (higher-level output buffering)
//
//===----------------------------------------------------------------------===//
/// @file
/// @brief Default implementation of the runtime snprintf compatibility entry.

#include "rt_printf_compat.h"
#include "rt_platform.h"

#include <stdarg.h>
#include <stdio.h>

/// @brief snprintf-compatible formatting wrapper with weak linkage.
/// @details Initializes a `va_list`, forwards it once to `vsnprintf`, closes
///          the list, and returns libc's result unchanged. No argument
///          validation or portability normalization is added. @ref RT_WEAK
///          permits strong-symbol interposition only on toolchains where that
///          platform macro provides the required linkage semantics.
/// @param str Destination buffer accepted by `vsnprintf`; may be `NULL` only
///            when the platform permits it for the supplied @p size.
/// @param size Capacity of @p str in bytes, including any terminator.
/// @param fmt Non-null `printf`-style format string.
/// @param ... Arguments whose promoted types must match @p fmt.
/// @return The exact `vsnprintf` result: normally the number of characters
///         required excluding the terminator, or a negative platform error.
RT_WEAK int rt_snprintf(char *str, size_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
#if !defined(_MSC_VER)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#endif
    int written = vsnprintf(str, size, fmt, ap);
#if !defined(_MSC_VER)
#pragma GCC diagnostic pop
#endif
    va_end(ap);
    return written;
}
