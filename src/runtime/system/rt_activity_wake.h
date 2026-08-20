//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/system/rt_activity_wake.h
// Purpose: Declare a ref-counted cross-thread wake callback shared by async runtimes.
// Key invariants:
//   - Invalidation waits for an in-flight callback and prevents later callbacks.
//   - Retained targets outlive every Process/PTy activity monitor that references them.
// Ownership/Lifetime:
//   - New returns one reference; Retain/Release balance additional owners.
// Links: src/runtime/system/rt_activity_wake.c,
//        src/runtime/system/rt_process.c,
//        src/runtime/graphics/gui/rt_gui_app.c,
//        docs/adr/0281-event-driven-process-pty-gui-wakes.md
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_activity_wake.h
 * @brief Declares a small lifetime-safe bridge from worker activity to event loops.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rt_activity_wake_target rt_activity_wake_target;
typedef void (*rt_activity_wake_callback)(void *context);

/// @brief Allocate an active wake target with one owned reference.
rt_activity_wake_target *rt_activity_wake_new(rt_activity_wake_callback callback, void *context);

/// @brief Retain one target for an asynchronous producer.
rt_activity_wake_target *rt_activity_wake_retain(rt_activity_wake_target *target);

/// @brief Invoke the callback unless the owner already invalidated it.
void rt_activity_wake_signal(rt_activity_wake_target *target);

/// @brief Disable future callbacks and wait for any current callback to return.
void rt_activity_wake_invalidate(rt_activity_wake_target *target);

/// @brief Release one reference and free the invalid target at zero references.
void rt_activity_wake_release(rt_activity_wake_target *target);

#ifdef __cplusplus
}
#endif
