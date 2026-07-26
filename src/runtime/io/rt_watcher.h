//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/io/rt_watcher.h
// Purpose: Poll-based file system watcher for Zanna.IO.Watcher, monitoring files and directories
// for changes via native OS facilities and exposing queued events through Poll/PollFor.
//
// Key invariants:
//   - Events are queued internally and retrieved synchronously via Poll/PollFor.
//   - Watching is non-recursive on all platforms.
//   - The watcher must be started with rt_watcher_start before events fire.
//   - EventPath returns the full watched file path for file watches; directory watches
//     return the changed child path when the platform reports one, otherwise the watched path.
//   - The fixed 64-event queue coalesces excess events into an overflow marker.
//   - Stop clears the complete current event epoch even when the native backend
//     has already become inactive after a terminal event.
//   - Every instance is bound to its construction thread; all public instance
//     operations trap when called from another thread.
//
// Ownership/Lifetime:
//   - Watcher objects are heap-allocated and GC-managed.
//   - The watcher retains copies of the watched path and any queued event paths.
//
// Links: src/runtime/io/rt_watcher.c (implementation), src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//
#pragma once

#include "rt_string.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Discriminant tag for file system events reported by a Watcher.
typedef enum {
    RT_WATCH_EVENT_NONE = 0,
    RT_WATCH_EVENT_CREATED = 1,
    RT_WATCH_EVENT_MODIFIED = 2,
    RT_WATCH_EVENT_DELETED = 3,
    RT_WATCH_EVENT_RENAMED = 4,
    RT_WATCH_EVENT_OVERFLOW = 5,
} rt_watch_event_t;

/// @brief Create a new inactive watcher for an existing file or directory.
/// @details Retains @p path and, for a file watch, derives retained parent and
///          leaf strings transactionally. Partial construction is finalized if
///          any managed allocation traps. No native watch handle is opened
///          until @ref rt_watcher_start. The result is permanently bound to
///          the calling thread because native wait state and the event ring are
///          intentionally not protected by a monitor.
/// @param path Valid non-empty runtime path naming an existing entry.
/// @return Owned opaque Watcher, or NULL after a validation/allocation trap.
void *rt_watcher_new(rt_string path);

/// @brief Get the watched path.
/// @details Traps unless called from the construction thread. The retained
///          result remains valid independently of the Watcher reference.
/// @param obj Opaque Watcher object pointer.
/// @return Owned retained reference to the path being watched, or a fresh
///         empty string after invalid use.
rt_string rt_watcher_get_path(void *obj);

/// @brief Check if the watcher is actively watching.
/// @details Traps unless called from the construction thread. Terminal backend
///          events may retire the native watch before Stop is called.
/// @param obj Opaque Watcher object pointer.
/// @return 1 if watching, 0 otherwise.
int8_t rt_watcher_get_is_watching(void *obj);

/// @brief Start a fresh event epoch for file system changes.
/// @details Clears stale queued/last events from a retired backend. Unsupported
///          platforms and invalid usage trap; transient native handle, watch,
///          or registration exhaustion leaves IsWatching false so callers can
///          fall back to rescanning. Traps unless called from the construction
///          thread.
/// @param obj Opaque Watcher object pointer.
void rt_watcher_start(void *obj);

/// @brief Stop watching and clear all queued and last-event state.
/// @details Idempotent even when a terminal backend event already made the
///          watcher inactive. Traps unless called from the construction thread.
/// @param obj Opaque Watcher object pointer.
void rt_watcher_stop(void *obj);

/// @brief Poll for a file system event (non-blocking).
/// @details Returns the oldest queued event before consulting the native
///          backend and traps unless called from the construction thread.
/// @param obj Opaque Watcher object pointer.
/// @return Event type (RT_WATCH_EVENT_*), including OVERFLOW, or NONE if unavailable.
int64_t rt_watcher_poll(void *obj);

/// @brief Poll for a file system event with timeout.
/// @details A negative timeout waits indefinitely, zero is non-blocking, and a
///          positive timeout is a monotonic upper bound across interrupted
///          POSIX waits. A queued event returns immediately. Traps unless
///          called from the construction thread.
/// @param obj Opaque Watcher object pointer.
/// @param ms Maximum milliseconds to wait.
/// @return Event type (RT_WATCH_EVENT_*), or 0 if timeout.
int64_t rt_watcher_poll_for(void *obj, int64_t ms);

/// @brief Get the path of the file that triggered the last event.
/// @details Traps unless called from the construction thread.
/// @param obj Opaque Watcher object pointer.
/// @return Owned retained event path, or traps if no event has been polled in
///         the current Start/Stop epoch.
rt_string rt_watcher_event_path(void *obj);

/// @brief Get the type of the last polled event.
/// @details Traps unless called from the construction thread.
/// @param obj Opaque Watcher object pointer.
/// @return Event type (RT_WATCH_EVENT_*), or NONE when no event has been polled.
int64_t rt_watcher_event_type(void *obj);

/// @brief Get the number of dropped events represented by the last overflow event.
/// @details Returns zero when no event has been polled or when the last event was
///          not RT_WATCH_EVENT_OVERFLOW. Internal-ring loss is counted exactly
///          and saturates at INT64_MAX; native/backend loss is -1 because the
///          operating system does not report an exact count. Traps unless
///          called from the construction thread.
/// @param obj Opaque Watcher object pointer.
/// @return Coalesced dropped event count for the last overflow marker.
int64_t rt_watcher_event_overflow_count(void *obj);

/// @brief Return RT_WATCH_EVENT_NONE through the property-dispatch ABI.
/// @param self Unused receiver.
/// @return @ref RT_WATCH_EVENT_NONE.
int64_t rt_watcher_event_none(void *self);
/// @brief Return RT_WATCH_EVENT_CREATED through the property-dispatch ABI.
/// @param self Unused receiver.
/// @return @ref RT_WATCH_EVENT_CREATED.
int64_t rt_watcher_event_created(void *self);
/// @brief Return RT_WATCH_EVENT_MODIFIED through the property-dispatch ABI.
/// @param self Unused receiver.
/// @return @ref RT_WATCH_EVENT_MODIFIED.
int64_t rt_watcher_event_modified(void *self);
/// @brief Return RT_WATCH_EVENT_DELETED through the property-dispatch ABI.
/// @param self Unused receiver.
/// @return @ref RT_WATCH_EVENT_DELETED.
int64_t rt_watcher_event_deleted(void *self);
/// @brief Return RT_WATCH_EVENT_RENAMED through the property-dispatch ABI.
/// @param self Unused receiver.
/// @return @ref RT_WATCH_EVENT_RENAMED.
int64_t rt_watcher_event_renamed(void *self);
/// @brief Return RT_WATCH_EVENT_OVERFLOW through the property-dispatch ABI.
/// @param self Unused receiver.
/// @return @ref RT_WATCH_EVENT_OVERFLOW.
int64_t rt_watcher_event_overflow(void *self);

#ifdef __cplusplus
}
#endif
