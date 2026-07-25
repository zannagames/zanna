//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/core/rt_debug.h
/// @file
/// @brief Declares immediate line-oriented stdout diagnostic helpers.
///
// Purpose: Debug-oriented printing helpers providing deterministic, line-oriented output of i32 and
// C-string values for IL golden tests and runtime diagnostics.
//
// Key invariants:
//   - Each call appends a newline and flushes stdout immediately.
//   - Output is locale-independent and platform-deterministic.
//   - NULL strings are treated as empty (zero-length output followed by newline).
//   - These functions bypass the output buffering layer for immediate visibility.
//
// Ownership/Lifetime:
//   - Functions accept plain values or C pointers with no ownership transfer.
//   - No runtime string objects are used; no heap allocation occurs.
//
// Links: src/runtime/core/rt_debug.c (implementation)
//
//===----------------------------------------------------------------------===//
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Print a signed 32-bit integer followed by a newline.
/// @param value Value to print in decimal.
/// @note Flushes stdout and ignores output/flush errors.
void rt_println_i32(int32_t value);

/// @brief Print a C string followed by a newline.
/// @param text Null-terminated string; treated as empty when null.
/// @note Flushes stdout; the byte encoding is not validated.
void rt_println_str(const char *text);

#ifdef __cplusplus
}
#endif
