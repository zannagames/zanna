//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/system/rt_activity_watch.h
// Purpose: Declares the process-wide activity watcher shared by Process and PTY.
//
// Key invariants:
//   - One worker services every registered nonblocking readiness probe.
//   - A registration emits at most one wake until explicitly rearmed.
//   - Unregister waits until no worker can call the registration's probe.
//
// Ownership/Lifetime:
//   - Register retains the wake target and returns an owned registration.
//   - Unregister releases both and must precede destruction of probe context.
//
// Links: src/runtime/system/rt_activity_watch.c,
//        docs/adr/0281-event-driven-process-pty-gui-wakes.md
//
//===----------------------------------------------------------------------===//

#pragma once

#include "rt_activity_wake.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rt_activity_watch rt_activity_watch;
typedef int (*rt_activity_probe)(void *context);

/// @brief Register a nonblocking readiness probe with the shared worker.
rt_activity_watch *rt_activity_watch_register(rt_activity_probe probe,
                                              void *context,
                                              rt_activity_wake_target *target);

/// @brief Permit one subsequent signal after the consumer drained activity.
void rt_activity_watch_rearm(rt_activity_watch *watch);

/// @brief Remove and release a registration, waiting out an in-flight probe.
void rt_activity_watch_unregister(rt_activity_watch *watch);

#ifdef __cplusplus
}
#endif
