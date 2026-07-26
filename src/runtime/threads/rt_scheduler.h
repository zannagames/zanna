//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/threads/rt_scheduler.h
// Purpose: Poll-based task scheduler for named delayed tasks using the monotonic clock, where
// duplicate task names replace the previous registration.
//
// Key invariants:
//   - Poll-based, not thread-based; rt_scheduler_poll must be called regularly.
//   - Uses the monotonic clock; immune to wall-clock adjustments.
//   - Task names are unique; registering a duplicate name replaces the previous task.
//   - Tasks fire at most once per registration; use rt_scheduler_schedule to re-queue.
//
// Ownership/Lifetime:
//   - Scheduler objects are heap-allocated runtime objects.
//   - Scheduled names are retained until cancellation, clearing, finalization,
//     replacement, or transfer into an owned Poll result sequence.
//
// Links: src/runtime/threads/rt_scheduler.c (implementation), src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//
/// @file
/// @brief Declares a synchronized, poll-driven scheduler for named deadlines.
/// @details Entries are unique by task-name bytes and contain a monotonic due
///          time plus a caller-defined generation. Scheduling performs no
///          callback execution and starts no background thread; callers poll
///          explicitly and receive due names in a caller-owned sequence.

#pragma once

#include "rt_string.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Create a new empty task scheduler.
/// @return New caller-owned Scheduler object, or NULL after allocation or
///         synchronization-initialization failure traps.
void *rt_scheduler_new(void);

/// @brief Schedule a named task with a delay in milliseconds.
/// @details Retains @p name and replaces the deadline and generation of an
///          existing byte-identical name. Plain scheduling records generation
///          zero. NULL scheduler or name is ignored.
/// @param sched Scheduler pointer.
/// @param name Borrowed task-name String retained by the scheduler.
/// @param delay_ms Delay from now; negative values are treated as zero.
void rt_scheduler_schedule(void *sched, rt_string name, int64_t delay_ms);

/// @brief Schedule a named task with a delay and a caller-supplied generation tag.
/// @details Like rt_scheduler_schedule, but stamps the entry with @p generation so a later
///          rt_scheduler_is_due_gen / rt_scheduler_generation_of can tell whether the entry that
///          fired is the one the caller queued or a newer one that superseded it. Re-scheduling a
///          name replaces both its due time and its generation. (Plain Schedule records generation 0.)
/// @param sched Scheduler pointer.
/// @param name Borrowed task-name String retained by the scheduler.
/// @param delay_ms Delay from now; negative values are treated as zero.
/// @param generation Caller-defined identity for this scheduling (e.g. a document revision).
void rt_scheduler_schedule_gen(void *sched, rt_string name, int64_t delay_ms, int64_t generation);

/// @brief Cancel a scheduled task by name.
/// @details Removes the byte-identical entry and releases its retained name.
///          NULL input and missing entries return zero.
/// @param sched Scheduler pointer.
/// @param name Task name to cancel.
/// @return 1 if a task was cancelled, 0 if not found.
int8_t rt_scheduler_cancel(void *sched, rt_string name);

/// @brief Check if a named task is due (its delay has elapsed).
/// @details This is a non-consuming point-in-time query; a due entry remains
///          scheduled until Poll, Cancel, Clear, replacement, or finalization.
/// @param sched Scheduler pointer.
/// @param name Task name to check.
/// @return 1 if due, 0 if not due or not found.
int8_t rt_scheduler_is_due(void *sched, rt_string name);

/// @brief Check if a named task is due AND still carries the given generation.
/// @details Equivalent to `rt_scheduler_is_due(sched, name) && rt_scheduler_generation_of(sched,
///          name) == generation`. Returns 0 when a newer rt_scheduler_schedule_gen replaced the
///          entry (superseded), so callers can discard stale revision-tagged work in one call.
/// @param sched Scheduler pointer.
/// @param name Task name to check.
/// @param generation The generation the caller expects (e.g. its current revision).
/// @return 1 if due and the generation matches, 0 otherwise.
int8_t rt_scheduler_is_due_gen(void *sched, rt_string name, int64_t generation);

/// @brief Return the generation currently scheduled for a named task.
/// @details This legacy scalar form cannot distinguish an unscheduled name
///          from a scheduled generation of -1; use the Option form when needed.
/// @param sched Scheduler pointer.
/// @param name Task name to query.
/// @return The entry's generation, or -1 if @p name is not scheduled.
int64_t rt_scheduler_generation_of(void *sched, rt_string name);

/// @brief GenerationOf as an Option: Some(generation) for a scheduled name
///        (even when the stored generation is -1), None otherwise.
/// @param sched Scheduler pointer.
/// @param name Task name to query.
/// @return Caller-owned `Some(Int64)` for a scheduled name, or the runtime None Option.
void *rt_scheduler_generation_of_option(void *sched, rt_string name);

/// @brief Poll for all due tasks and return their names as a Seq.
/// @details Uses one current-time snapshot and atomically removes every entry
///          at or before that deadline. Each retained name reference is
///          transferred into the owned result sequence. Result order follows
///          the scheduler's internal list and is not a chronological guarantee.
/// @param sched Scheduler pointer.
/// @return Caller-owned sequence of due task-name Strings, empty when none.
void *rt_scheduler_poll(void *sched);

/// @brief Get the number of pending tasks (due and not-yet-due).
/// @details Returns a synchronized point-in-time count; NULL returns zero.
/// @param sched Scheduler pointer.
/// @return Count of scheduled tasks.
int64_t rt_scheduler_pending(void *sched);

/// @brief Clear all scheduled tasks.
/// @details Atomically detaches every entry, then releases all retained names
///          outside the scheduler mutex. NULL is a no-op.
/// @param sched Scheduler pointer.
void rt_scheduler_clear(void *sched);

#ifdef __cplusplus
}
#endif
