//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/core/rt_output.h
// Purpose: Declares process-global runtime output routing, scoped capture,
// platform stdout delivery, and reference-counted conditional flushing.
//
// Key invariants:
//   - Initialization and batch-depth updates are safe for concurrent callers.
//   - Capture replaces stdout delivery for rt_output_str and rt_output_strn;
//     it does not capture unrelated writes made directly to stdout.
//   - Batch mode suppresses conditional POSIX flushes but does not own a
//     separate accumulation buffer or delay capture callbacks.
//   - Native Windows writes bypass CRT buffering, making flush operations no-ops.
//
// Ownership/Lifetime:
//   - Input byte ranges and capture contexts remain caller-owned.
//   - Capture callbacks must copy any bytes they need after returning.
//   - The POSIX stdout buffer is static module storage and must not be freed.
//
// Links: src/runtime/core/rt_output.c (implementation)
//
//===----------------------------------------------------------------------===//
/// @file
/// @brief Runtime stdout routing, capture-hook, and batching API.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Initialize output buffering for stdout.
/// @details On non-Windows platforms, one caller configures stdout with a
///          static 16 KiB full-buffering area and registers a normal-exit
///          flush when possible. Native Windows requires no CRT setup because
///          runtime writes target the OS handle. Calls are idempotent and
///          concurrent callers wait for the first initialization to finish.
void rt_output_init(void);

/// @brief Callback used to intercept runtime stdout writes.
/// @details When installed with @ref rt_output_set_capture_hook, every call to
///          @ref rt_output_str or @ref rt_output_strn is delivered to the
///          callback instead of the process stdout stream. The callback receives
///          exactly the bytes requested by the runtime print operation; the
///          buffer is not necessarily NUL-terminated. Implementations must copy
///          bytes they need after returning and must tolerate concurrent calls.
///          Callback execution occurs without the internal hook lock, permitting
///          nested output and hook replacement.
/// @param data Borrowed pointer to the byte range being written.
/// @param len Number of bytes available at @p data.
/// @param ctx Opaque context pointer supplied when the hook was installed.
typedef void (*rt_output_capture_fn)(const char *data, size_t len, void *ctx);

/// @brief Previously installed runtime output capture hook.
/// @details Returned from @ref rt_output_set_capture_hook so scoped embedders can
///          restore the exact prior hook after temporary capture ends. Both
///          fields are nullable; a null @c fn means runtime output writes to the
///          normal process stdout stream.
typedef struct rt_output_capture_hook {
    rt_output_capture_fn fn; ///< Capture callback, or NULL when capture is disabled.
    void *ctx;              ///< Opaque callback context associated with @c fn.
} rt_output_capture_hook;

/// @brief Replace the runtime stdout capture hook.
/// @details This is intended for embedders such as the REPL that need to capture
///          VM output without redirecting process file descriptors. The function
///          returns the previous hook so callers can restore it after execution.
///          Capture is process-global because runtime output is process-global;
///          callers should install it only around short, controlled VM
///          executions. Hook replacement is synchronized with snapshot reads,
///          but an invocation using an earlier snapshot may finish after this
///          function returns.
/// @param fn New capture callback, or NULL to disable capture.
/// @param ctx Caller-owned opaque context passed to @p fn.
/// @return The hook that was active before this call.
rt_output_capture_hook rt_output_set_capture_hook(rt_output_capture_fn fn, void *ctx);

/// @brief Route a NUL-terminated string to capture or stdout.
/// @details An installed hook receives the bytes instead of stdout. Otherwise
///          output is initialized and written without an implicit flush. A
///          null pointer is a no-op, and write failures are not reported.
/// @param s NUL-terminated string to write; may be `NULL`.
void rt_output_str(const char *s);

/// @brief Route an explicit byte range to capture or stdout.
/// @details Embedded NUL bytes are preserved. An installed hook replaces
///          stdout delivery; otherwise output is initialized and written
///          without an implicit flush. Null or empty ranges are no-ops, and
///          write failures are not reported.
/// @param s Byte range, not necessarily NUL-terminated.
/// @param len Number of bytes to write.
void rt_output_strn(const char *s, size_t len);

/// @brief Flush any buffered output to the terminal.
/// @details Calls `fflush(stdout)` on non-Windows platforms and ignores errors.
///          Native Windows output and capture callbacks have no module-owned
///          buffer to flush, so the function does nothing for those paths.
void rt_output_flush(void);

/// @brief Begin batch mode for output operations.
/// @details Atomically increments a process-global depth used by
///          @ref rt_output_flush_if_not_batch. Nested calls are supported.
///          Overflow at `INT_MAX` traps and leaves the depth unchanged.
void rt_output_begin_batch(void);

/// @brief End one level of batch mode and conditionally flush.
/// @details Decrements the batch mode reference count. When the count reaches
///          zero, non-Windows stdout is flushed. An unmatched call is a no-op.
void rt_output_end_batch(void);

/// @brief Check if batch mode is currently active.
/// @return One when the process-global batch depth is positive; otherwise zero.
int8_t rt_output_is_batch_mode(void);

/// @brief Flush output only if not in batch mode.
/// @details On non-Windows platforms, observes the atomic batch depth and
///          flushes stdout only when it is zero. The depth check is not coupled
///          atomically to the flush. Native Windows performs no flush.
void rt_output_flush_if_not_batch(void);

#ifdef __cplusplus
}
#endif
