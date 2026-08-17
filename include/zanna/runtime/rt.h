//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: include/zanna/runtime/rt.h
// Purpose: Provide a single stable umbrella include for the C runtime APIs.
// Key invariants: Aggregates only public rt_*.h headers in a deterministic order.
// Ownership/Lifetime: Header-only aggregator; does not own resources.
// Links: docs/internals/codemap.md, docs/internals/architecture.md
//
//===----------------------------------------------------------------------===//

/**
 * @file include/zanna/runtime/rt.h
 * @brief Aggregates the stable public C interfaces of the Zanna runtime.
 *
 * @details
 * Runtime consumers can include this header to obtain the public APIs for
 * strings, arrays, files, formatting, numeric operations, errors, threading,
 * traps, and related services in a deterministic order. The declarations
 * below add the clock and high-level file helpers that do not have a more
 * specialized public umbrella.
 *
 * The header is valid in C and C++. C++ translation units receive C linkage for
 * the directly declared functions. Resource ownership remains defined by each
 * component header and function contract; this aggregator owns no state.
 */

#pragma once

#include "rt_args.h"
#include "rt_array.h"
#include "rt_array_str.h"
#include "rt_compiled_pattern.h"
#include "rt_debug.h"
#include "rt_error.h"
#include "rt_file.h"
#include "rt_format.h"
#include "rt_fp.h"
#include "rt_heap.h"
#include "rt_int_format.h"
#include "rt_list.h"
#include "rt_math.h"
#include "rt_modvar.h"
#include "rt_numeric.h"
#include "rt_object.h" /* plain include; functions are no-ops when not linked */
#include "rt_random.h"
#include "rt_sb_bridge.h"
#include "rt_string.h"
#include "rt_string_builder.h"
#include "rt_threads.h"
#include "rt_trap.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Sleep the calling thread for approximately `ms` milliseconds.
 *
 * @param ms Requested duration. Negative values are clamped to zero.
 *
 * @note Scheduling and clock granularity may make the observed delay longer
 *       than requested. The function is thread-safe and blocks only the
 *       calling thread.
 */
void rt_sleep_ms(int32_t ms);

/**
 * @brief Read a monotonic clock in milliseconds.
 * @return Milliseconds since an unspecified epoch. Values are suitable for
 *         elapsed-time differences and do not track civil time.
 */
int64_t rt_timer_ms(void);

//=============================================================================
// Zanna.Time.Clock functions
//=============================================================================

/**
 * @brief Implement `Zanna.Time.Clock.Sleep` with a signed 64-bit argument.
 * @param ms Requested milliseconds, clamped to the range accepted by
 *           `rt_sleep_ms`; negative values become zero.
 *
 * @note The same scheduling and resolution limitations as `rt_sleep_ms`
 *       apply.
 */
void rt_clock_sleep(int64_t ms);

/**
 * @brief Implement `Zanna.Time.Clock.Ticks`.
 * @return Monotonic milliseconds since an unspecified epoch, using the same
 *         source as `rt_timer_ms`.
 */
int64_t rt_clock_ticks(void);

/**
 * @brief Implement high-resolution `Zanna.Time.Clock.TicksUs`.
 * @return Monotonic microseconds since an unspecified epoch.
 *
 * @note The numeric unit is microseconds; the underlying clock may have
 *       coarser effective resolution.
 */
int64_t rt_clock_ticks_us(void);

// --- High-level file helpers for Zanna.IO.File ---

/**
 * @brief Determine whether `path` resolves to an existing regular file.
 * @param path Borrowed runtime string containing a platform path.
 * @return One for an existing regular file; zero for directories, missing
 *         entries, malformed paths, or inspection failures.
 *
 * @note This predicate is intentionally non-trapping.
 */
int64_t rt_io_file_exists(rt_string path);

/**
 * @brief Read an entire file into a newly returned runtime string.
 * @param path Borrowed runtime string containing a platform path.
 * @return An owned runtime string containing every file byte. An empty file
 *         returns the shared empty-string handle.
 *
 * @note Invalid paths, non-regular files, allocation failure, file mutation
 *       during the read, and other I/O failures raise a runtime trap.
 *       Embedded NUL bytes are preserved.
 */
rt_string rt_io_file_read_all_text(rt_string path);

/**
 * @brief Atomically replace a file with the byte sequence in `contents`.
 * @param path Borrowed runtime string naming the destination.
 * @param contents Borrowed runtime string whose bytes are written verbatim.
 *
 * @note The implementation writes and synchronizes a temporary sidecar,
 *       then replaces the destination so readers see the old or new file,
 *       never a partial write. Existing permission bits are preserved.
 *       Invalid arguments and write failures raise a runtime trap.
 */
void rt_io_file_write_all_text(rt_string path, rt_string contents);

/**
 * @brief Attempt to unlink the filesystem entry at `path`.
 * @param path Borrowed runtime string containing the target path.
 *
 * @note A missing file is treated as success. Invalid paths, permission
 *       errors, and other unlink failures raise a runtime trap.
 */
void rt_io_file_delete(rt_string path);

#ifdef __cplusplus
} // extern "C"
#endif
