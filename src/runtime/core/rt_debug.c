//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/core/rt_debug.c
/// @file
/// @brief Implements immediate line-oriented stdout diagnostics.
///
// Purpose: Minimal debug-print helpers used by IL test harnesses, integration
//   tools, and golden-output tests. Functions write to stdout via the C stdio
//   layer and flush immediately, so deterministic traces are captured even when
//   the process terminates abruptly. The ABI is kept small and stable so
//   embedders can override the routines via linker substitution if needed.
//
// Key invariants:
//   - All output goes to stdout (not stderr) to be captured by golden-test
//     pipelines that redirect stdout.
//   - fflush(stdout) is called after every print to guarantee visibility
//     before any subsequent crash or trap.
//   - NULL string arguments are normalized to "" — callers need not guard.
//
// Ownership/Lifetime:
//   - No heap allocation. All functions are stateless wrappers.
//
// Links: src/runtime/core/rt_debug.h (public API)
//
//===----------------------------------------------------------------------===//

#include "rt_debug.h"

#include <stdio.h>

/// @brief Print a 32-bit integer followed by a newline.
/// @details Emits the decimal encoding of @p value with @c printf before forcing
///          a flush so the textual trace is observable even if the program
///          crashes immediately afterwards.  The function is intentionally tiny
///          to keep runtime dependencies minimal.
/// @param value Value to emit for diagnostic output.
/// @note Output and flush errors are intentionally ignored by this debug ABI.
void rt_println_i32(int32_t value) {
    printf("%d\n", value);
    fflush(stdout);
}

/// @brief Print a UTF-8 string followed by a newline.
/// @details Treats @p text as an optional pointer, normalising @c NULL to an
///          empty string so callers are spared from performing defensive checks.
///          Output is flushed immediately to keep debugger tooling responsive
///          and deterministic.
/// @param text Null-terminated string to print (may be null).
/// @note Bytes are forwarded unchanged through the C stdio layer; encoding is
///       a caller convention rather than validated by this function.
void rt_println_str(const char *text) {
    if (!text)
        text = "";
    printf("%s\n", text);
    fflush(stdout);
}
