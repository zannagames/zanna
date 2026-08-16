//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/core/rt_args.h
/// @file
/// @brief Declares context-scoped arguments and process environment services.
///
// Purpose: Process-wide command-line argument store and environment variable access, providing
// push/query semantics for argument strings and get/set/has helpers for environment variables.
//
// Key invariants:
//   - Argument indices are zero-based and contiguous.
//   - rt_args_get traps on out-of-range indices; callers must check rt_args_count first.
//   - Environment variable names must be non-empty strings.
//   - rt_cmdline returns a lossy display form joined by spaces without quoting.
//
// Ownership/Lifetime:
//   - Pushed string handles are retained by the store; bytes are not copied.
//   - rt_args_get returns a retained reference that the caller must release.
//   - rt_args_clear releases all stored references and resets the count to zero.
//   - Environment getters and rt_cmdline transfer a runtime string reference;
//     empty results may use the shared empty-string singleton.
//
// Links: src/runtime/core/rt_args.c (implementation), src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "rt_string.h"

/// @brief Remove all stored arguments and release their references.
/// @details Preserves item-array capacity and suppresses lazy host-argv import
///          when the effective store is the legacy context.
void rt_args_clear(void);

/// @brief Append an argument string to the store.
/// @details Retains @p s (no-op for NULL); callers retain ownership as well.
/// @param s Runtime string to append (may be NULL, treated as empty).
void rt_args_push(rt_string s);

/// @brief Return the number of stored arguments.
/// @return Argument count (non-negative).
/// @note A first read of an untouched legacy context may import host argv.
int64_t rt_args_count(void);

/// @brief Retrieve argument by zero-based index.
/// @details Returns a retained reference to the stored string; caller must
///          release it. Traps when @p index is out of range.
/// @param index Zero-based index in [0, rt_args_count()).
/// @return Retained runtime string.
rt_string rt_args_get(int64_t index);

/// @brief Return a single string joining all arguments separated by spaces.
/// @details No quoting or escaping is applied, and embedded null bytes are
///          truncated by C-string reconstruction.
/// @return New command-tail string, or the shared empty string when empty or
///         reconstruction fails.
rt_string rt_cmdline(void);

/// @brief Report whether the program is running as native code (not in the VM).
/// @return 1 when executing a native binary, 0 when running under the VM.
int64_t rt_env_is_native(void);

/// @brief Look up an environment variable by name.
/// @details Returns an empty string when the variable is missing. The name
///          must be non-empty and contain no null bytes.
/// @param name Environment variable to read.
/// @return New runtime string with the value, or shared empty string when unset.
/// @note Use rt_env_has_var() to distinguish unset from present-but-empty.
rt_string rt_env_get_var(rt_string name);

/// @brief Test whether an environment variable is present.
/// @details Treats empty values as "present"; returns 0 when missing. The
///          variable name must be non-empty and contain no null bytes.
/// @param name Environment variable to probe.
/// @return 1 when present, 0 when missing.
int64_t rt_env_has_var(rt_string name);

/// @brief Set or overwrite an environment variable.
/// @details Accepts empty values; overwrites existing entries. Variable
///          names must be non-empty. Cross-platform: uses setenv on POSIX
///          and SetEnvironmentVariable on Windows so empty values remain
///          present.
/// @param name Variable name to set.
/// @param value Desired value (may be empty). NULL treated as empty.
/// @note Names and values containing embedded null bytes trap.
void rt_env_set_var(rt_string name, rt_string value);

/// @brief Terminate the current process with the provided exit code.
/// @details Delegates to the platform termination primitive. Windows native PE
///          binaries bypass the CRT exit path.
/// @param code Exit status to report to the host OS.
/// @note Does not return; @p code is narrowed to the platform exit-status width.
void rt_env_exit(int64_t code);

// Flag and option parsing (ADR 0253).
/// @brief True when @p name appears as a standalone argument.
/// @param name Flag to look for, including any leading dashes.
/// @return Non-zero when present.
int8_t rt_args_has_flag(rt_string name);
/// @brief Value following @p name, accepting `--opt value` and `--opt=value`.
/// @param name Option to look for.
/// @param fallback Returned when absent or unvalued.
/// @return Owned value string.
rt_string rt_args_get_option(rt_string name, rt_string fallback);
/// @brief Integer value following @p name, or @p fallback.
/// @param name Option to look for.
/// @param fallback Returned when absent or unparseable.
/// @return Parsed integer or @p fallback.
int64_t rt_args_get_option_int(rt_string name, int64_t fallback);
/// @brief Arguments that are neither flags nor option values.
/// @details A bare `--` ends option processing; everything after it is
///          positional.
/// @return Owned Seq of positional argument strings.
void *rt_args_positionals(void);

#ifdef __cplusplus
}
#endif
