//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/system/rt_machine.h
// Purpose: System information queries for Zanna.System.Machine providing OS name, version,
// hostname, username, home/temp directories, CPU count, and memory statistics.
//
// Key invariants:
//   - OS values: 'linux', 'macos', 'windows', 'unknown'.
//   - All string-returning functions allocate new strings with refcount 1.
//   - Integer values (Cores, MemTotal, MemFree) are returned directly as int64_t.
//   - Queries are stateless; values may change between calls (e.g., MemFree).
//
// Ownership/Lifetime:
//   - Returned strings are caller-owned (refcnt == 1); callers must release them.
//   - No persistent state; all queries read from the OS on each call.
//
// Links: src/runtime/system/rt_machine.c (implementation), src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_machine.h
 * @brief Declares stateless machine and operating-system information queries.
 * @details String queries return fresh managed values for OS, host, user, and
 *          directory information, while numeric queries report CPU, memory,
 *          architecture-size, paging, and byte-order properties with
 *          platform-defined fallback values.
 */

#pragma once

#include "rt_string.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Get the operating system name.
/// @details Selects a stable lowercase identifier at compile time; it does not
///          probe mutable host state.
/// @return Newly allocated runtime string containing "linux", "macos",
///         "windows", or "unknown"; the caller must release it.
rt_string rt_machine_os(void);

/// @brief Get the operating system version string.
/// @details Uses the native Windows version API, macOS product-version sysctl,
///          Linux os-release VERSION_ID, or uname as the applicable fallback.
/// @return Newly allocated native-format version string, or "unknown" when all
///         probes fail; the caller must release it.
rt_string rt_machine_os_ver(void);

/// @brief Get the hostname of the machine.
/// @details Queries the operating system on every call and converts Windows
///          UTF-16 names to UTF-8.
/// @return Newly allocated hostname string, or "unknown" when the bounded query
///         fails; the caller must release it.
rt_string rt_machine_host(void);

/// @brief Get the current username.
/// @details Prefers the native account database/API and falls back to standard
///          username environment variables.
/// @return Newly allocated UTF-8 username, or "unknown" when every source
///         fails; the caller must release it.
rt_string rt_machine_user(void);

/// @brief Get the home directory path.
/// @details Windows checks USERPROFILE and then HOMEDRIVE plus HOMEPATH. POSIX
///          checks HOME and then the current passwd record.
/// @return Newly allocated UTF-8 path, or an empty string when unavailable; the
///         caller must release it.
rt_string rt_machine_home(void);

/// @brief Get the temporary directory path.
/// @details Uses GetTempPathW on Windows, removing a non-root trailing
///          separator. POSIX accepts an absolute existing TMPDIR, TMP, or TEMP
///          directory and otherwise uses `/tmp`.
/// @return Newly allocated UTF-8 temporary path; the caller must release it.
rt_string rt_machine_temp(void);

/// @brief Get the number of logical CPU cores.
/// @details Counts all Windows processor groups, uses the macOS logicalcpu
///          sysctl, and uses online POSIX processors. Linux additionally
///          restricts the host count by cgroup quota and cpuset controls.
/// @return Positive available logical-CPU count, with 1 as the safe fallback.
int64_t rt_machine_cores(void);

/// @brief Get a stable CPU architecture identifier.
/// @details Selects the identifier from compiler target macros rather than a
///          runtime OS query.
/// @return Newly allocated string containing "x86_64", "arm64", "x86", "arm",
///         "wasm32", or "unknown"; the caller must release it.
rt_string rt_machine_arch(void);

/// @brief Get the host memory page size in bytes.
/// @return Positive page size reported by Win32 or POSIX, or 4096 as the POSIX
///         fallback when unavailable.
int64_t rt_machine_page_size(void);

/// @brief Get the native pointer width in bits.
/// @return @c sizeof(void*) multiplied by eight, typically 32 or 64.
int64_t rt_machine_pointer_size(void);

/// @brief Get the total system memory in bytes.
/// @details Queries the platform's physical-memory capacity. On Linux, a finite
///          cgroup memory limit smaller than host RAM bounds the result.
/// @return Total or process-constrained capacity in bytes, saturated where
///         supported, or zero when unavailable.
int64_t rt_machine_mem_total(void);

/// @brief Get the free system memory in bytes.
/// @details Reports memory available for new work, including reclaimable memory
///          where the platform exposes it. Linux further bounds the result by
///          finite cgroup limit minus current usage.
/// @return Available or process-constrained estimate in bytes, saturated where
///         supported, or zero when unavailable.
int64_t rt_machine_mem_free(void);

/// @brief Get the system byte order.
/// @details Inspects the first byte of a known multibyte integer at runtime.
/// @return Newly allocated string containing "little" or "big"; the caller
///         must release it.
rt_string rt_machine_endian(void);

#ifdef __cplusplus
}
#endif
