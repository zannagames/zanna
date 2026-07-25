//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/core/rt_log.h
// Purpose: Simple leveled logging for the Zanna.Diagnostics.Log namespace, writing timestamped
// messages to stderr with DEBUG/INFO/WARN/ERROR levels and a configurable minimum level filter.
//
// Key invariants:
//   - Log levels are ordered: DEBUG(0) < INFO(1) < WARN(2) < ERROR(3) < OFF(4).
//   - Messages below the current minimum level are silently discarded.
//   - Output format is: [LEVEL] YYYY-MM-DD HH:MM:SS message, written to stderr.
//   - Message bytes are length-aware; embedded NUL and control bytes are escaped.
//   - Each accepted log call is emitted as one complete physical log line.
//   - The default minimum level is INFO (1).
//
// Ownership/Lifetime:
//   - Log functions do not retain input strings; callers retain ownership.
//   - The global minimum level is process-wide atomic state.
//
// Links: src/runtime/core/rt_log.c (implementation), src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares process-wide filtered logging for `Zanna.Diagnostics.Log`.
/// @details Accepted messages are emitted to standard error as one serialized
///   physical line with local timestamp and escaped control bytes.

#pragma once

#include "rt_string.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Ordered logging threshold and message-severity values.
typedef enum {
    /// Most verbose diagnostic messages.
    RT_LOG_DEBUG = 0,
    /// Normal operational information and the default threshold.
    RT_LOG_INFO = 1,
    /// Recoverable or potentially problematic conditions.
    RT_LOG_WARN = 2,
    /// Serious failures that do not themselves terminate execution.
    RT_LOG_ERROR = 3,
    /// Threshold that suppresses every message severity.
    RT_LOG_OFF = 4,
} rt_log_level_t;

/// @brief Log a debug message.
/// @details Emits only when the current threshold is `RT_LOG_DEBUG`.
/// @param message Borrowed runtime string; NULL or invalid handles log an empty message.
void rt_log_debug(rt_string message);

/// @brief Log an info message.
/// @details Emits when the current threshold is DEBUG or INFO.
/// @param message Borrowed runtime string; NULL or invalid handles log an empty message.
void rt_log_info(rt_string message);

/// @brief Log a warning message.
/// @details Emits when the current threshold is DEBUG, INFO, or WARN.
/// @param message Borrowed runtime string; NULL or invalid handles log an empty message.
void rt_log_warn(rt_string message);

/// @brief Log an error message.
/// @details Emits at every threshold except OFF. Logging does not trap or
///   terminate the process.
/// @param message Borrowed runtime string; NULL or invalid handles log an empty message.
void rt_log_error(rt_string message);

/// @brief Get the current log level.
/// @return Atomically observed threshold from `RT_LOG_DEBUG` through `RT_LOG_OFF`.
int64_t rt_log_level(void);

/// @brief Set the log level.
/// @details The process-global atomic threshold affects subsequent logging on
///   every thread.
/// @param level Requested threshold; values below DEBUG clamp to DEBUG and
///   values above OFF clamp to OFF.
void rt_log_set_level(int64_t level);

/// @brief Check if a log level is enabled.
/// @param level Candidate message severity.
/// @return 1 when @p level is at least the current threshold and strictly below
///   OFF; invalid values and OFF return 0.
int8_t rt_log_enabled(int64_t level);

/// @brief Get the DEBUG level constant.
/// @return `RT_LOG_DEBUG` (0).
int64_t rt_log_level_debug(void);

/// @brief Get the INFO level constant.
/// @return `RT_LOG_INFO` (1).
int64_t rt_log_level_info(void);

/// @brief Get the WARN level constant.
/// @return `RT_LOG_WARN` (2).
int64_t rt_log_level_warn(void);

/// @brief Get the ERROR level constant.
/// @return `RT_LOG_ERROR` (3).
int64_t rt_log_level_error(void);

/// @brief Get the OFF level constant.
/// @return `RT_LOG_OFF` (4).
int64_t rt_log_level_off(void);

#ifdef __cplusplus
}
#endif
