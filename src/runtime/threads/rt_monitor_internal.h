//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/threads/rt_monitor_internal.h
// Purpose: Shared declarations and platform-neutral helpers for the monitor
//   (mutual-exclusion) runtime, whose implementation is split by platform into
//   rt_monitor_win.c (SRWLOCK + CONDITION_VARIABLE) and rt_monitor_posix.c
//   (pthread). Both translation units include this header for the common
//   includes, trap-recovery hooks, and the two small shared helpers.
//
// Key invariants:
//   - Helpers are static inline: one internal-linkage copy per TU, no exported
//     symbol, no source duplication.
//
// Ownership/Lifetime:
//   - monitor_release_enter_ref drops a GC ref taken while entering a monitor.
//
// Links: rt_monitor_win.c, rt_monitor_posix.c, rt_threads.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares helpers shared by the POSIX and Windows monitor backends.
/// @details The helpers preserve runtime trap diagnostics across cleanup and
///          release temporary object references acquired while entering a
///          monitor. They are inline so each platform translation unit keeps
///          an internal copy without exporting additional ABI.

#pragma once

#include "rt_threads.h"

#include "rt_internal.h"
#include "rt_object.h"

#include <errno.h>
#include <limits.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/// @brief Install a non-local recovery destination for runtime traps.
/// @param buf Jump buffer that receives control when a trap is raised.
void rt_trap_set_recovery(jmp_buf *buf);

/// @brief Remove the active runtime trap recovery destination.
void rt_trap_clear_recovery(void);

/// @brief Read the diagnostic associated with the current recovered trap.
/// @return Borrowed NUL-terminated diagnostic text, or NULL when unavailable.
const char *rt_trap_get_error(void);

/// @brief Release the temporary managed reference held during monitor entry.
/// @details NULL is ignored; a reference reaching zero is finalized through
///          the runtime object allocator.
/// @param obj Runtime object reference to release, or NULL.
static inline void monitor_release_enter_ref(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief Copy the active trap diagnostic into stable caller storage.
/// @details Empty diagnostics use @p fallback, or the monitor-entry default
///          when no fallback is supplied. A NULL or zero-capacity destination
///          is ignored.
/// @param buffer Destination for the NUL-terminated diagnostic.
/// @param buffer_size Capacity of @p buffer in bytes.
/// @param fallback Optional diagnostic used when the trap has no message.
static inline void monitor_save_trap_error(char *buffer, size_t buffer_size, const char *fallback) {
    if (!buffer || buffer_size == 0)
        return;
    const char *err = rt_trap_get_error();
    if (!err || !*err)
        err = fallback ? fallback : "Monitor.Enter: failed";
    snprintf(buffer, buffer_size, "%s", err);
}
