//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/threads/rt_scheduler.c
// Purpose: Implements a poll-based task scheduler for the Zanna.Threads.Scheduler
//          class. Tasks are registered with a string name and a delay in
//          milliseconds; Poll returns a sequence of names whose due times have
//          elapsed. Does not use background threads.
//
// Key invariants:
//   - Tasks are stored as a singly-linked list; Poll scans and removes due ones.
//   - Due timestamps are computed from CLOCK_MONOTONIC to avoid wall-clock skew.
//   - Scheduling the same name twice replaces the previous due time.
//   - Poll removes and returns all tasks due at or before the current time.
//   - Scheduler operations are internally synchronized.
//   - Task name strings are retained by the scheduler until the task fires.
//
// Ownership/Lifetime:
//   - The scheduler object is heap-allocated and managed by the runtime GC.
//   - Each task entry retains a reference to its name string; the reference is
//     released when the task fires (transferred to the returned sequence).
//
// Links: src/runtime/threads/rt_scheduler.h (public API),
//        src/runtime/threads/rt_debounce.h (related delayed-execution concept)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements a synchronized, poll-driven named-delay scheduler.
/// @details Scheduling retains a unique task name and records an absolute
///          monotonic deadline plus an optional generation tag. Polling
///          atomically detaches all entries due at its timestamp and transfers
///          their retained names into a caller-owned sequence. No background
///          scheduler thread or callback execution is involved.

#include "rt_scheduler.h"

#include "rt_option.h"

#include "rt_internal.h"
#include "rt_object.h"
#include "rt_seq.h"
#include "rt_string.h"
#include "rt_string_internal.h"
#include "rt_threads.h"

#include <limits.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#endif

#include "rt_trap.h"

/// @brief Install a non-local recovery destination for runtime traps.
/// @param buf Jump buffer that receives control when a trap is raised.
void rt_trap_set_recovery(jmp_buf *buf);

/// @brief Remove the active runtime trap recovery destination.
void rt_trap_clear_recovery(void);

/// @brief Read the diagnostic associated with the current recovered trap.
/// @return Borrowed NUL-terminated diagnostic text, or NULL when unavailable.
const char *rt_trap_get_error(void);

//=============================================================================
// Time Helper
//=============================================================================

#if defined(_WIN32)
static INIT_ONCE g_sched_freq_once = INIT_ONCE_STATIC_INIT;
static LARGE_INTEGER g_sched_freq;

/// @brief One-shot initializer for the Win32 performance-counter frequency.
/// @details Same pattern used elsewhere in the threads subsystem: cache
///          `QueryPerformanceFrequency` once into `g_sched_freq` since it's invariant for the
///          lifetime of the system, and serialize the cache fill via `InitOnceExecuteOnce`.
/// @param once Win32 once-control passed by `InitOnceExecuteOnce`.
/// @param param Optional caller parameter; unused.
/// @param ctx Optional context output; unused.
/// @return `TRUE` after recording a usable frequency or a fallback marker.
static BOOL CALLBACK sched_freq_init(PINIT_ONCE once, PVOID param, PVOID *ctx) {
    (void)once;
    (void)param;
    (void)ctx;
    if (!QueryPerformanceFrequency(&g_sched_freq))
        g_sched_freq.QuadPart = 0;
    return TRUE;
}
#endif

/// @brief Read the current monotonic time in milliseconds.
/// @details Platform-portable wrapper. Win32 path uses `QueryPerformanceCounter` divided by
///          the cached frequency, with a split-modulo conversion to keep `int64_t` math
///          exact across long uptimes and falls back to `GetTickCount64` if QPC fails.
///          POSIX prefers `CLOCK_MONOTONIC` and falls back to `CLOCK_REALTIME`; it returns
///          0 only if every POSIX clock source fails.
/// @return Non-negative millisecond timestamp from the best available clock,
///         saturated to `INT64_MAX` where required.
static int64_t current_time_ms(void) {
#if defined(_WIN32)
    LARGE_INTEGER counter;
    InitOnceExecuteOnce(&g_sched_freq_once, sched_freq_init, NULL, NULL);
    if (g_sched_freq.QuadPart <= 0 || !QueryPerformanceCounter(&counter)) {
        ULONGLONG ticks = GetTickCount64();
        return ticks > (ULONGLONG)INT64_MAX ? INT64_MAX : (int64_t)ticks;
    }
    return (int64_t)((counter.QuadPart / g_sched_freq.QuadPart) * 1000LL +
                     ((counter.QuadPart % g_sched_freq.QuadPart) * 1000LL) / g_sched_freq.QuadPart);
#else
    struct timespec ts;
#ifdef CLOCK_MONOTONIC
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        return (int64_t)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
#endif
    if (clock_gettime(CLOCK_REALTIME, &ts) == 0)
        return (int64_t)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
    return 0;
#endif
}

/// @brief Compute the absolute monotonic-clock millisecond timestamp at which `delay_ms` will
///        elapse from now.
/// @details Negative delays are clamped to zero so a misuse can't schedule a task in the past.
///          The overflow guard returns `INT64_MAX` instead of wrapping when `now + delay_ms`
///          would overflow — the resulting "essentially never due" timestamp is the safest
///          behaviour for absurdly large delays.
/// @param delay_ms Relative delay in milliseconds; negatives are treated as zero.
/// @return Saturating absolute due timestamp.
static int64_t due_time_from_now(int64_t delay_ms) {
    if (delay_ms < 0)
        delay_ms = 0;
    int64_t now = current_time_ms();
    if (delay_ms > INT64_MAX - now)
        return INT64_MAX;
    return now + delay_ms;
}

/// @brief Byte-equality comparator for two scheduler task names.
/// @details Used to find existing entries during `Schedule`/`Cancel`/`IsDue` lookups. Compares
///          length first to short-circuit on size mismatch, then `memcmp` on the underlying
///          buffers. Returns 1 on equality, 0 otherwise. Two NULLs compare as 0 (defensive —
///          callers should not pass NULL names but we don't want to claim equality if they do).
/// @param a First task-name String.
/// @param b Second task-name String.
/// @return One when both non-NULL names contain identical bytes, otherwise zero.
static int8_t scheduler_name_equals(rt_string a, rt_string b) {
    if (!a || !b)
        return 0;
    int64_t a_len = rt_string_len_bytes(a);
    int64_t b_len = rt_string_len_bytes(b);
    if (a_len != b_len)
        return 0;
    const char *a_data = rt_string_cstr(a);
    const char *b_data = rt_string_cstr(b);
    if (!a_data || !b_data)
        return a_data == b_data ? 1 : 0;
    return memcmp(a_data, b_data, (size_t)a_len) == 0 ? 1 : 0;
}

/// @brief Validate a non-null public task-name argument.
/// @param name Candidate runtime string.
/// @param diagnostic Operation-specific invalid-name message.
/// @return One for a live registered string, otherwise zero after trapping.
static int scheduler_name_valid(rt_string name, const char *diagnostic) {
    if (name && rt_string_is_handle(name))
        return 1;
    if (name)
        rt_trap(diagnostic);
    return 0;
}

//=============================================================================
// Internal Structures
//=============================================================================

/// @brief A single scheduled task entry.
typedef struct sched_entry {
    rt_string name;           ///< Retained task name string.
    int64_t due_time_ms;      ///< Absolute time when this task is due.
    int64_t generation;       ///< Caller-supplied identity (0 for plain Schedule).
    struct sched_entry *next; ///< Next entry in linked list.
} sched_entry;

/// @brief Internal scheduler data.
typedef struct {
    sched_entry *head; ///< Head of the linked list of entries.
    int64_t count;     ///< Number of entries in the list.
#if defined(_WIN32)
    CRITICAL_SECTION mutex;
#else
    pthread_mutex_t mutex;
#endif
} rt_scheduler_data;

#if defined(_WIN32)
#define SCHED_LOCK(data) EnterCriticalSection(&(data)->mutex)
#define SCHED_UNLOCK(data) LeaveCriticalSection(&(data)->mutex)
#else
#define SCHED_LOCK(data) pthread_mutex_lock(&(data)->mutex)
#define SCHED_UNLOCK(data) pthread_mutex_unlock(&(data)->mutex)
#endif

/// @brief Validate that `sched` is a scheduler object, trapping on misuse.
/// @details Same shape as the `_require` helpers in sibling files. `trap_on_null` toggles
///          NULL-handling: 1 raises a trap, 0 silently returns NULL so callers like
///          `rt_scheduler_pending(NULL)` can return a no-op zero. A non-null pointer with
///          a wrong class id always traps — that's a programmer error worth surfacing.
/// @param sched Candidate Scheduler runtime object.
/// @param trap_on_null Whether a NULL handle should raise a runtime trap.
/// @return Valid scheduler payload, or NULL for tolerated or returning trap paths.
static rt_scheduler_data *scheduler_require(void *sched, int8_t trap_on_null) {
    if (!sched) {
        if (trap_on_null)
            rt_trap("Scheduler: null object");
        return NULL;
    }
    if (!rt_obj_is_instance(sched, RT_SCHEDULER_CLASS_ID, sizeof(rt_scheduler_data))) {
        rt_trap("Scheduler: invalid object");
        return NULL;
    }
    return (rt_scheduler_data *)sched;
}

/// @brief Drop one GC reference to @p sched and free it if the count hit zero.
/// @param sched Runtime object reference to release, or NULL.
static void scheduler_release_object(void *sched) {
    if (sched && rt_obj_release_check0(sched))
        rt_obj_free(sched);
}

/// @brief Preserve an active trap diagnostic in stable caller storage.
/// @param buffer Destination for the NUL-terminated diagnostic.
/// @param buffer_size Capacity of @p buffer in bytes.
/// @param fallback Diagnostic used when the trap supplied no usable text.
static void scheduler_save_trap_error(char *buffer, size_t buffer_size, const char *fallback) {
    const char *err = rt_trap_get_error();
    snprintf(buffer, buffer_size, "%s", err && err[0] ? err : fallback);
}

/// @brief Release task-name references and free every node in an entry chain.
/// @param e Head of the detached entry list, or NULL.
static void scheduler_free_entry_list(sched_entry *e) {
    while (e) {
        sched_entry *next = e->next;
        if (e->name)
            rt_string_unref(e->name);
        free(e);
        e = next;
    }
}

/// @brief Finalize a Scheduler and release its native and retained resources.
/// @details Detaches all entries under the scheduler mutex, releases their
///          task-name references, then destroys the platform mutex.
/// @param obj Scheduler object being finalized.
static void scheduler_finalizer(void *obj) {
    if (!obj)
        return;
    rt_scheduler_data *data = (rt_scheduler_data *)obj;

    SCHED_LOCK(data);
    sched_entry *e = data->head;
    data->head = NULL;
    data->count = 0;
    SCHED_UNLOCK(data);

    scheduler_free_entry_list(e);

#if defined(_WIN32)
    DeleteCriticalSection(&data->mutex);
#else
    pthread_mutex_destroy(&data->mutex);
#endif
}

//=============================================================================
// Public API
//=============================================================================

/// @brief Creates a new empty scheduler.
///
/// Allocates and initializes a Scheduler object with no pending tasks.
///
/// @return A new Scheduler object. Traps on allocation failure.
void *rt_scheduler_new(void) {
    rt_scheduler_data *data = (rt_scheduler_data *)rt_obj_new_i64(
        RT_SCHEDULER_CLASS_ID, (int64_t)sizeof(rt_scheduler_data));
    if (!data) {
        rt_trap("Scheduler: memory allocation failed");
        return NULL;
    }
    data->head = NULL;
    data->count = 0;
#if defined(_WIN32)
    if (!InitializeCriticalSectionEx(&data->mutex, 0, 0)) {
        if (rt_obj_release_check0(data))
            rt_obj_free(data);
        rt_trap("Scheduler: mutex initialization failed");
        return NULL;
    }
#else
    if (pthread_mutex_init(&data->mutex, NULL) != 0) {
        if (rt_obj_release_check0(data))
            rt_obj_free(data);
        rt_trap("Scheduler: mutex initialization failed");
        return NULL;
    }
#endif
    rt_obj_set_finalizer(data, scheduler_finalizer);
    return data;
}

/// @brief Schedules a named task with a delay in milliseconds.
///
/// Records a task that will become due after the specified delay. If a task
/// with the same name already exists, it is replaced with the new delay.
///
/// @param sched Scheduler pointer.
/// @param name Task name string. Ignored if NULL.
/// @param delay_ms Delay in milliseconds from now. Negative values treated as 0.
/// @param generation Caller-supplied identity recorded on the entry (0 for plain Schedule).
static void scheduler_schedule_impl(void *sched,
                                    rt_string name,
                                    int64_t delay_ms,
                                    int64_t generation) {
    if (!sched || !name)
        return;
    if (!scheduler_name_valid(name, "Scheduler.Schedule: invalid task name"))
        return;
    rt_scheduler_data *data = scheduler_require(sched, 0);
    if (!data)
        return;
    rt_obj_retain_maybe(sched);

    int64_t due = due_time_from_now(delay_ms);
    rt_string retained_name = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[256];
        scheduler_save_trap_error(
            saved_error, sizeof(saved_error), "Scheduler.Schedule: name retain failed");
        rt_trap_clear_recovery();
        scheduler_release_object(sched);
        rt_trap(saved_error);
        return;
    }
    retained_name = rt_string_ref(name);
    rt_trap_clear_recovery();

    SCHED_LOCK(data);

    // Check for existing entry with the same name and update it
    sched_entry *e = data->head;
    while (e) {
        if (scheduler_name_equals(e->name, name)) {
            e->due_time_ms = due;
            e->generation = generation;
            SCHED_UNLOCK(data);
            rt_string_unref(retained_name);
            scheduler_release_object(sched);
            return;
        }
        e = e->next;
    }

    // Create new entry
    if (data->count == INT64_MAX) {
        SCHED_UNLOCK(data);
        rt_string_unref(retained_name);
        scheduler_release_object(sched);
        rt_trap("Scheduler.Schedule: task count overflow");
        return;
    }
    sched_entry *entry = (sched_entry *)malloc(sizeof(sched_entry));
    if (!entry) {
        SCHED_UNLOCK(data);
        rt_string_unref(retained_name);
        scheduler_release_object(sched);
        rt_trap("Scheduler.Schedule: memory allocation failed");
        return;
    }
    entry->name = retained_name;
    entry->due_time_ms = due;
    entry->generation = generation;
    entry->next = data->head;
    data->head = entry;
    data->count++;
    SCHED_UNLOCK(data);
    scheduler_release_object(sched);
}

/// @brief Schedule or replace a named task using generation zero.
/// @param sched Scheduler object to update.
/// @param name Borrowed non-NULL task-name String retained by the scheduler.
/// @param delay_ms Relative delay in milliseconds; negatives become zero.
void rt_scheduler_schedule(void *sched, rt_string name, int64_t delay_ms) {
    scheduler_schedule_impl(sched, name, delay_ms, 0);
}

/// @brief Schedule or replace a named task with a generation tag.
/// @param sched Scheduler object to update.
/// @param name Borrowed non-NULL task-name String retained by the scheduler.
/// @param delay_ms Relative delay in milliseconds; negatives become zero.
/// @param generation Opaque caller-defined identity stored with the entry.
void rt_scheduler_schedule_gen(void *sched, rt_string name, int64_t delay_ms, int64_t generation) {
    scheduler_schedule_impl(sched, name, delay_ms, generation);
}

/// @brief Cancels a scheduled task by name.
///
/// Removes the first task matching the given name from the scheduler.
///
/// @param sched Scheduler pointer.
/// @param name Task name to cancel.
/// @return 1 if a task was found and cancelled, 0 if not found.
int8_t rt_scheduler_cancel(void *sched, rt_string name) {
    if (!sched || !name)
        return 0;
    if (!scheduler_name_valid(name, "Scheduler.Cancel: invalid task name"))
        return 0;
    rt_scheduler_data *data = scheduler_require(sched, 0);
    if (!data)
        return 0;
    rt_obj_retain_maybe(sched);

    SCHED_LOCK(data);
    sched_entry **pp = &data->head;
    while (*pp) {
        if (scheduler_name_equals((*pp)->name, name)) {
            sched_entry *e = *pp;
            *pp = e->next;
            data->count--;
            SCHED_UNLOCK(data);
            scheduler_release_object(sched);
            rt_string_unref(e->name);
            free(e);
            return 1;
        }
        pp = &(*pp)->next;
    }
    SCHED_UNLOCK(data);
    scheduler_release_object(sched);
    return 0;
}

/// @brief Checks if a named task is due.
///
/// Returns true if the named task exists and its due time has passed.
///
/// @param sched Scheduler pointer.
/// @param name Task name to check.
/// @return 1 if due, 0 if not due or not found.
int8_t rt_scheduler_is_due(void *sched, rt_string name) {
    if (!sched || !name)
        return 0;
    if (!scheduler_name_valid(name, "Scheduler.IsDue: invalid task name"))
        return 0;
    rt_scheduler_data *data = scheduler_require(sched, 0);
    if (!data)
        return 0;
    rt_obj_retain_maybe(sched);
    int64_t now = current_time_ms();

    SCHED_LOCK(data);
    sched_entry *e = data->head;
    while (e) {
        if (scheduler_name_equals(e->name, name)) {
            int8_t due = now >= e->due_time_ms ? 1 : 0;
            SCHED_UNLOCK(data);
            scheduler_release_object(sched);
            return due;
        }
        e = e->next;
    }
    SCHED_UNLOCK(data);
    scheduler_release_object(sched);
    return 0;
}

/// @brief Checks if a named task is due and still carries the given generation.
///
/// Returns true only if the named task exists, its due time has passed, and its
/// stored generation equals @p generation. A newer rt_scheduler_schedule_gen
/// replaces the entry, so a query for a superseded generation returns 0.
///
/// @param sched Scheduler pointer.
/// @param name Task name to check.
/// @param generation The generation the caller expects.
/// @return 1 if due and the generation matches, 0 otherwise.
int8_t rt_scheduler_is_due_gen(void *sched, rt_string name, int64_t generation) {
    if (!sched || !name)
        return 0;
    if (!scheduler_name_valid(name, "Scheduler.IsDueGen: invalid task name"))
        return 0;
    rt_scheduler_data *data = scheduler_require(sched, 0);
    if (!data)
        return 0;
    rt_obj_retain_maybe(sched);
    int64_t now = current_time_ms();

    SCHED_LOCK(data);
    sched_entry *e = data->head;
    while (e) {
        if (scheduler_name_equals(e->name, name)) {
            int8_t due = (now >= e->due_time_ms && e->generation == generation) ? 1 : 0;
            SCHED_UNLOCK(data);
            scheduler_release_object(sched);
            return due;
        }
        e = e->next;
    }
    SCHED_UNLOCK(data);
    scheduler_release_object(sched);
    return 0;
}

/// @brief Returns the generation currently scheduled for a named task.
///
/// @param sched Scheduler pointer.
/// @param name Task name to query.
/// @return The entry's generation, or -1 if @p name is not scheduled.
int64_t rt_scheduler_generation_of(void *sched, rt_string name) {
    if (!sched || !name)
        return -1;
    if (!scheduler_name_valid(name, "Scheduler.GenerationOf: invalid task name"))
        return -1;
    rt_scheduler_data *data = scheduler_require(sched, 0);
    if (!data)
        return -1;
    rt_obj_retain_maybe(sched);

    SCHED_LOCK(data);
    sched_entry *e = data->head;
    while (e) {
        if (scheduler_name_equals(e->name, name)) {
            int64_t gen = e->generation;
            SCHED_UNLOCK(data);
            scheduler_release_object(sched);
            return gen;
        }
        e = e->next;
    }
    SCHED_UNLOCK(data);
    scheduler_release_object(sched);
    return -1;
}

/// @brief GenerationOf as an Option (VDOC-130): Some(generation) when the
///        name is scheduled — including a stored generation of -1 — and None
///        for a null name/scheduler or an unscheduled name, so -1 stays
///        usable as ordinary data.
/// @param sched Scheduler object to query.
/// @param name Task-name String to find.
/// @return Caller-owned `Some(Int64)` for a scheduled name, or the runtime None Option.
void *rt_scheduler_generation_of_option(void *sched, rt_string name) {
    if (!sched || !name)
        return rt_option_none();
    if (!scheduler_name_valid(name, "Scheduler.GenerationOfOption: invalid task name"))
        return rt_option_none();
    rt_scheduler_data *data = scheduler_require(sched, 0);
    if (!data)
        return rt_option_none();
    rt_obj_retain_maybe(sched);

    SCHED_LOCK(data);
    sched_entry *e = data->head;
    while (e) {
        if (scheduler_name_equals(e->name, name)) {
            int64_t gen = e->generation;
            SCHED_UNLOCK(data);
            scheduler_release_object(sched);
            return rt_option_some_i64(gen);
        }
        e = e->next;
    }
    SCHED_UNLOCK(data);
    scheduler_release_object(sched);
    return rt_option_none();
}

/// @brief Polls for all due tasks.
///
/// Returns a Seq of task name strings for all tasks whose due time has
/// passed. Due tasks are removed from the scheduler.
///
/// @param sched Scheduler pointer.
/// @return Seq of due task name strings. Empty seq if none due.
void *rt_scheduler_poll(void *sched) {
    if (!sched)
        return rt_seq_new_owned();
    rt_scheduler_data *data = scheduler_require(sched, 0);
    if (!data)
        return rt_seq_new_owned();
    rt_obj_retain_maybe(sched);
    int64_t now = current_time_ms();
    sched_entry *due_head = NULL;
    sched_entry *due_tail = NULL;
    int64_t due_count = 0;

    SCHED_LOCK(data);
    for (sched_entry *e = data->head; e; e = e->next) {
        if (now >= e->due_time_ms)
            due_count++;
    }
    SCHED_UNLOCK(data);

    void *result = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[256];
        scheduler_save_trap_error(
            saved_error, sizeof(saved_error), "Scheduler.Poll: result allocation failed");
        rt_trap_clear_recovery();
        scheduler_release_object(sched);
        rt_trap(saved_error);
        return rt_seq_new_owned();
    }
    result = rt_seq_with_capacity_owned(due_count > 0 ? due_count : 1);
    rt_trap_clear_recovery();

    SCHED_LOCK(data);
    sched_entry **pp = &data->head;
    while (*pp) {
        if (now >= (*pp)->due_time_ms) {
            sched_entry *e = *pp;
            *pp = e->next;
            e->next = NULL;
            if (due_tail)
                due_tail->next = e;
            else
                due_head = e;
            due_tail = e;
            data->count--;
        } else {
            pp = &(*pp)->next;
        }
    }
    SCHED_UNLOCK(data);
    scheduler_release_object(sched);

    sched_entry *volatile remaining = due_head;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[256];
        scheduler_save_trap_error(
            saved_error, sizeof(saved_error), "Scheduler.Poll: result append failed");
        rt_trap_clear_recovery();
        scheduler_free_entry_list((sched_entry *)remaining);
        scheduler_release_object(result);
        rt_trap(saved_error);
        return rt_seq_new_owned();
    }
    while (due_head) {
        sched_entry *next = due_head->next;
        rt_seq_push_raw(result, (void *)due_head->name);
        free(due_head);
        due_head = next;
        remaining = due_head;
    }
    rt_trap_clear_recovery();
    return result;
}

/// @brief Gets the number of pending tasks.
///
/// @param sched Scheduler pointer.
/// @return Count of tasks in the scheduler (both due and not-yet-due).
int64_t rt_scheduler_pending(void *sched) {
    if (!sched)
        return 0;
    rt_scheduler_data *data = scheduler_require(sched, 0);
    if (!data)
        return 0;
    rt_obj_retain_maybe(sched);
    SCHED_LOCK(data);
    int64_t count = data->count;
    SCHED_UNLOCK(data);
    scheduler_release_object(sched);
    return count;
}

/// @brief Clears all scheduled tasks.
///
/// Removes all tasks from the scheduler, freeing associated memory.
///
/// @param sched Scheduler pointer.
void rt_scheduler_clear(void *sched) {
    if (!sched)
        return;
    rt_scheduler_data *data = scheduler_require(sched, 0);
    if (!data)
        return;
    rt_obj_retain_maybe(sched);

    SCHED_LOCK(data);
    sched_entry *e = data->head;
    data->head = NULL;
    data->count = 0;
    SCHED_UNLOCK(data);
    scheduler_release_object(sched);

    scheduler_free_entry_list(e);
}
