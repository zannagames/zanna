//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/threads/rt_threads.h
// Purpose: Runtime thread and monitor primitives backing Zanna.Threads, providing thread creation,
// join, Sleep, and re-entrant FIFO-fair monitor operations.
//
// Key invariants:
//   - Monitor operations (Lock, Unlock, Wait, Notify, NotifyAll) are re-entrant.
//   - Monitor waiting is FIFO-fair; threads are notified in the order they waited.
//   - Thread handles and Safe* values are runtime-managed objects.
//   - rt_thread_sleep accepts milliseconds; accuracy depends on OS scheduling.
//
// Ownership/Lifetime:
//   - Thread handle objects are runtime-managed; join releases the OS thread handle.
//   - Safe* values are reference-counted; multiple readers/writers share ownership.
//
// Links: src/runtime/threads/rt_threads_common.c (shared implementation),
//        src/runtime/threads/rt_threads_posix.c and rt_threads_win.c (platform backends),
//        src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//
/// @file
/// @brief Declares core thread, monitor, SafeI64, gate, barrier, and RW-lock APIs.
/// @details This umbrella header exposes the low-level `Zanna.Threads`
///          primitives shared by higher-level async, pool, channel, and
///          synchronization components. Opaque callback entry points preserve
///          the generated-code ABI, while typed variants provide portable
///          native C integration and explicit managed-argument ownership.

#pragma once

#include "rt_string.h"
#include <stdint.h>

/// @brief Internal heap class identifiers for runtime-owned threading objects.
/// @details These are intentionally negative to avoid colliding with compiler-assigned
///          user type IDs and small positive collection IDs.
#define RT_THREAD_CLASS_ID INT64_C(-0x540101)
#define RT_SAFE_THREAD_CLASS_ID INT64_C(-0x540102)
#define RT_SAFE_I64_CLASS_ID INT64_C(-0x540103)
#define RT_GATE_CLASS_ID INT64_C(-0x540104)
#define RT_BARRIER_CLASS_ID INT64_C(-0x540105)
#define RT_RWLOCK_CLASS_ID INT64_C(-0x540106)
#define RT_CHANNEL_CLASS_ID INT64_C(-0x540107)
#define RT_CANCELLATION_CLASS_ID INT64_C(-0x540108)
#define RT_PROMISE_CLASS_ID INT64_C(-0x540109)
#define RT_FUTURE_CLASS_ID INT64_C(-0x54010A)
#define RT_THREADPOOL_CLASS_ID INT64_C(-0x54010B)
#define RT_CONCQUEUE_CLASS_ID INT64_C(-0x54010C)
#define RT_CONCMAP_CLASS_ID INT64_C(-0x54010D)
#define RT_SCHEDULER_CLASS_ID INT64_C(-0x54010E)
#define RT_DEBOUNCER_CLASS_ID INT64_C(-0x54010F)
#define RT_THROTTLER_CLASS_ID INT64_C(-0x540110)

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Native callback signature accepted by the typed thread-start APIs.
/// @details The callback runs exactly once on the newly created native thread.
///          The argument is borrowed unless the corresponding `Owned` start
///          variant is used. Native runtime code should prefer this type-safe
///          surface over the opaque IL compatibility entry points below.
/// @param arg Caller-supplied callback argument, which may be NULL.
typedef void (*rt_thread_entry_fn)(void *arg);

// =========================================================================
// Zanna.Threads.Monitor
// =========================================================================

/// @brief Acquire exclusive ownership of the object's monitor.
/// @details Blocks until the monitor can be acquired. The monitor is
///          re-entrant, so the same thread may call Enter multiple times,
///          but each Enter must be balanced with an Exit.
/// @param obj Object whose monitor to acquire (must be non-NULL).
void rt_monitor_enter(void *obj);

/// @brief Attempt to acquire the monitor without blocking.
/// @details Returns immediately with success if the monitor is available.
/// @param obj Object whose monitor to acquire (must be non-NULL).
/// @return 1 if acquired, 0 if not acquired.
int8_t rt_monitor_try_enter(void *obj);

/// @brief Attempt to acquire the monitor with a timeout.
/// @details Waits up to @p ms milliseconds for the monitor. Negative values
///          are treated as zero (immediate check).
/// @param obj Object whose monitor to acquire (must be non-NULL).
/// @param ms Timeout in milliseconds.
/// @return 1 if acquired, 0 if timed out.
int8_t rt_monitor_try_enter_for(void *obj, int64_t ms);

/// @brief Release a previously acquired monitor.
/// @details Must be called once per successful Enter. Traps on misuse
///          (e.g., not the owning thread or unbalanced Exit).
/// @param obj Object whose monitor to release (must be non-NULL).
void rt_monitor_exit(void *obj);

/// @brief Wait on the monitor, releasing and re-acquiring it.
/// @details Releases the monitor, waits until signaled by Pause/PauseAll,
///          then re-acquires it before returning. The caller must own the
///          monitor when calling this function.
/// @param obj Object whose monitor to wait on (must be owned by caller).
void rt_monitor_wait(void *obj);

/// @brief Timed wait on the monitor with automatic re-acquisition.
/// @details Releases the monitor and waits up to @p ms milliseconds. The
///          monitor is re-acquired before returning. Negative timeouts are
///          treated as zero. The caller must own the monitor.
/// @param obj Object whose monitor to wait on (must be owned by caller).
/// @param ms Timeout in milliseconds.
/// @return 1 if signaled before timeout; 0 if timed out.
int8_t rt_monitor_wait_for(void *obj, int64_t ms);

/// @brief Wake one thread waiting on the monitor.
/// @details Moves the oldest waiter to the acquisition queue. The caller
///          must currently own the monitor and continues to hold it after
///          this call.
/// @param obj Object whose monitor to signal (must be owned by caller).
void rt_monitor_pause(void *obj);

/// @brief Wake all threads waiting on the monitor.
/// @details Moves all waiters to the acquisition queue so they will
///          re-acquire the monitor in FIFO order. The caller must own the
///          monitor and continues to hold it after this call.
/// @param obj Object whose monitor to signal (must be owned by caller).
void rt_monitor_pause_all(void *obj);

/// @brief Internal cleanup hook invoked when a monitored runtime object is freed.
/// @details Removes the monitor table entry for @p obj if one exists. This is
///          not part of the public Zanna.Threads surface.
/// @param obj Object whose monitor metadata should be discarded.
void rt_monitor_forget(void *obj);

// =========================================================================
// Zanna.Threads.Thread
// =========================================================================

/// @brief Start a native thread through the type-safe callback surface.
/// @details This is the preferred C/C++ API for runtime and embedding code. It
///          avoids converting a data pointer into a function pointer. The new
///          thread inherits the caller's runtime context, and the returned
///          runtime-managed handle supports the regular join/query functions.
/// @param entry Typed native callback. Must not be NULL.
/// @param arg Borrowed argument passed to @p entry; may be NULL.
/// @return Runtime-managed thread handle, or NULL after a reported failure.
void *rt_thread_start_fn(rt_thread_entry_fn entry, void *arg);

/// @brief Start a native thread and retain its managed callback argument.
/// @details Equivalent to @ref rt_thread_start_fn except that a managed object
///          or string passed as @p arg is retained until the callback returns.
///          Unmanaged native pointers preserve borrowed-pointer behavior.
/// @param entry Typed native callback. Must not be NULL.
/// @param arg Managed value to retain, unmanaged borrowed pointer, or NULL.
/// @return Runtime-managed thread handle, or NULL after a reported failure.
void *rt_thread_start_owned_fn(rt_thread_entry_fn entry, void *arg);

/// @brief Start a new thread executing the given entry function.
/// @details This opaque form is retained for the IL runtime ABI, whose `obj`
///          calling convention transports native entry addresses as object
///          values. Native C/C++ callers should use @ref rt_thread_start_fn.
///          The entry point is invoked with @p arg and inherits the caller's
///          runtime context.
/// @param entry Opaque ABI representation of a native thread callback.
/// @param arg Argument passed to the entry function.
/// @return Thread object handle, or NULL on allocation failure.
void *rt_thread_start(void *entry, void *arg);

/// @brief Start a new thread and retain a runtime-managed argument for it.
/// @details Like @ref rt_thread_start, but a managed object or String is
///          retained until the entry function returns. Unmanaged native
///          pointers remain borrowed.
/// @param entry Opaque ABI representation of a native thread callback.
/// @param arg Managed value to retain, unmanaged borrowed pointer, or NULL.
/// @return Runtime-managed thread handle, or NULL after a reported failure.
void *rt_thread_start_owned(void *entry, void *arg);

/// @brief Join a thread, blocking until it finishes.
/// @details Traps if called on NULL or if a thread attempts to join itself.
///          Completion means the entry callback and inherited-context cleanup
///          have finished. A detached worker may drop its internal lifecycle
///          reference immediately after waking joiners, so native callers must
///          not interpret the return value of a subsequent memory release as a
///          synchronous native-thread reclamation guarantee.
/// @param thread Thread handle returned by rt_thread_start.
void rt_thread_join(void *thread);

/// @brief Attempt to join a thread without blocking.
/// @details Returns immediately with success if the thread has finished.
///          Traps on invalid usage (NULL or self-join).
/// @param thread Thread handle returned by rt_thread_start.
/// @return 1 if the thread has already finished, 0 if still running.
int8_t rt_thread_try_join(void *thread);

/// @brief Join a thread with a timeout.
/// @details Waits up to @p ms milliseconds for completion. Traps on invalid
///          usage (NULL or self-join).
/// @param thread Thread handle returned by rt_thread_start.
/// @param ms Timeout in milliseconds.
/// @return 1 if the thread finished before timeout, 0 if timed out.
int8_t rt_thread_join_for(void *thread, int64_t ms);

/// @brief Retrieve the runtime-assigned thread identifier.
/// @details IDs are monotonically increasing and remain stable for the
///          lifetime of the thread object.
/// @param thread Thread handle returned by rt_thread_start.
/// @return Thread identifier, or 0 on error.
int64_t rt_thread_get_id(void *thread);

/// @brief Check whether a thread is still running.
/// @details Returns 1 if the thread has not yet finished, 0 otherwise.
/// @param thread Thread handle returned by rt_thread_start.
/// @return 1 if alive, 0 if finished.
int8_t rt_thread_get_is_alive(void *thread);

/// @brief Sleep the current thread for the specified duration.
/// @details Negative values are treated as zero and large values are
///          clamped to an implementation-defined maximum.
/// @param ms Duration to sleep in milliseconds.
void rt_thread_sleep(int64_t ms);

/// @brief Yield the current thread's time slice.
/// @details Allows other runnable threads to execute without sleeping.
void rt_thread_yield(void);

// =========================================================================
// Zanna.Threads.Thread (safe start with error boundaries)
// =========================================================================

/// @brief Start a typed native callback inside a thread-local trap boundary.
/// @details Uncaught runtime traps are captured in the returned SafeThread
///          rather than aborting the process. The argument is borrowed.
/// @param entry Typed native callback. Must not be NULL.
/// @param arg Borrowed callback argument; may be NULL.
/// @return Runtime-managed SafeThread handle, or NULL after a reported failure.
void *rt_thread_start_safe_fn(rt_thread_entry_fn entry, void *arg);

/// @brief Start a trap-safe typed callback while retaining its managed argument.
/// @details Combines @ref rt_thread_start_safe_fn with the argument ownership
///          rules of @ref rt_thread_start_owned_fn.
/// @param entry Typed native callback. Must not be NULL.
/// @param arg Managed value to retain, unmanaged borrowed pointer, or NULL.
/// @return Runtime-managed SafeThread handle, or NULL after a reported failure.
void *rt_thread_start_safe_owned_fn(rt_thread_entry_fn entry, void *arg);

/// @brief Start a new thread with trap recovery (error boundary).
/// @details Opaque IL ABI adapter for @ref rt_thread_start_safe_fn. Like
///          rt_thread_start, it wraps the entry function in a
///          setjmp/longjmp recovery point. If the thread's code calls
///          rt_trap(), instead of terminating the process the error is
///          captured and the thread exits cleanly.
/// @param entry Function pointer representing the thread entry.
/// @param arg Argument passed to the entry function.
/// @return Thread object handle, or NULL on allocation failure.
void *rt_thread_start_safe(void *entry, void *arg);

/// @brief Start a safe thread and retain a runtime-managed argument for it.
/// @details Like @ref rt_thread_start_safe, but retains a managed object or
///          String until the entry returns or traps. Unmanaged pointers remain
///          borrowed.
/// @param entry Opaque ABI representation of a native thread callback.
/// @param arg Managed value to retain, unmanaged borrowed pointer, or NULL.
/// @return Runtime-managed SafeThread handle, or NULL after a reported failure.
void *rt_thread_start_safe_owned(void *entry, void *arg);

/// @brief Check whether a safe-started thread exited with a trap error.
/// @param thread Thread handle returned by rt_thread_start_safe.
/// @return 1 if the thread trapped, 0 otherwise.
int8_t rt_thread_has_error(void *thread);

/// @brief Get the error message if the thread trapped.
/// @param thread Thread handle returned by rt_thread_start_safe.
/// @return Error message string, or empty string if no error.
rt_string rt_thread_get_error(void *thread);

/// @brief Join the underlying thread of a safe-started thread.
/// @param thread Thread handle returned by rt_thread_start_safe.
void rt_thread_safe_join(void *thread);

/// @brief Get the thread ID of a safe-started thread.
/// @param thread Thread handle returned by rt_thread_start_safe.
/// @return Thread identifier, or 0 on error.
int64_t rt_thread_safe_get_id(void *thread);

/// @brief Check whether a safe-started thread is still running.
/// @param thread Thread handle returned by rt_thread_start_safe.
/// @return 1 if alive, 0 if finished.
int8_t rt_thread_safe_is_alive(void *thread);

// =========================================================================
// Zanna.Threads.SafeI64
// =========================================================================

/// @brief Create a thread-safe 64-bit integer cell.
/// @details The cell provides monitor-backed atomic-style operations.
/// @param initial Initial value to store.
/// @return Opaque SafeI64 object pointer, or NULL on failure.
void *rt_safe_i64_new(int64_t initial);

/// @brief Retrieve the current value of the SafeI64.
/// @details The read is synchronized so it is safe across threads.
/// @param obj SafeI64 object pointer.
/// @return Current integer value.
int64_t rt_safe_i64_get(void *obj);

/// @brief Set the SafeI64 to a new value.
/// @details The write is synchronized so it is safe across threads.
/// @param obj SafeI64 object pointer.
/// @param value New value to store.
void rt_safe_i64_set(void *obj, int64_t value);

/// @brief Atomically add @p delta and return the new value.
/// @details The operation is synchronized and safe across threads.
/// @param obj SafeI64 object pointer.
/// @param delta Amount to add (may be negative).
/// @return Updated value after addition.
int64_t rt_safe_i64_add(void *obj, int64_t delta);

/// @brief Compare-and-swap the stored value.
/// @details If the current value equals @p expected, it is replaced with
///          @p desired. The previous value is always returned so callers
///          can detect whether the swap occurred.
/// @param obj SafeI64 object pointer.
/// @param expected Expected current value.
/// @param desired Value to store if the expectation matches.
/// @return The value observed before the swap.
int64_t rt_safe_i64_compare_exchange(void *obj, int64_t expected, int64_t desired);

// =========================================================================
// Zanna.Threads.Gate
// =========================================================================

/// @brief Create a gate (counting semaphore) with initial permits.
/// @details Permits must be non-negative. The gate is FIFO-fair when
///          multiple threads are waiting.
/// @param permits Initial permit count.
/// @return Opaque Gate object pointer, or NULL on failure.
void *rt_gate_new(int64_t permits);

/// @brief Enter the gate, blocking until a permit is available.
/// @details Decrements the permit count when entry is granted.
/// @param gate Gate object pointer.
void rt_gate_enter(void *gate);

/// @brief Attempt to enter the gate without blocking.
/// @details Succeeds only when no waiters exist and a permit is available.
/// @param gate Gate object pointer.
/// @return 1 if a permit was acquired, 0 otherwise.
int8_t rt_gate_try_enter(void *gate);

/// @brief Attempt to enter the gate with a timeout.
/// @details Waits up to @p ms milliseconds; negative values are treated as
///          zero for an immediate check.
/// @param gate Gate object pointer.
/// @param ms Timeout in milliseconds.
/// @return 1 if a permit was acquired, 0 if timed out.
int8_t rt_gate_try_enter_for(void *gate, int64_t ms);

/// @brief Release a single permit back to the gate.
/// @details Wakes the oldest waiter if one exists.
/// @param gate Gate object pointer.
void rt_gate_leave(void *gate);

/// @brief Release multiple permits back to the gate.
/// @details Increments the permit count by @p count and wakes waiters in
///          FIFO order as permits become available.
/// @param gate Gate object pointer.
/// @param count Number of permits to release (must be non-negative).
void rt_gate_leave_many(void *gate, int64_t count);

/// @brief Query the current number of available permits.
/// @details The value is returned under the gate's internal lock.
/// @param gate Gate object pointer.
/// @return Current permit count.
int64_t rt_gate_get_permits(void *gate);

// =========================================================================
// Zanna.Threads.Barrier
// =========================================================================

/// @brief Create a barrier with the given number of parties.
/// @details All parties must call Arrive before the barrier releases.
/// @param parties Number of participants (must be >= 1).
/// @return Opaque Barrier object pointer, or NULL on failure.
void *rt_barrier_new(int64_t parties);

/// @brief Arrive at the barrier and wait for other parties.
/// @details Returns the arrival index for this generation (0-based). The
///          barrier releases all parties when the count is reached and then
///          resets for the next generation.
/// @param barrier Barrier object pointer.
/// @return Arrival index in the current generation.
int64_t rt_barrier_arrive(void *barrier);

/// @brief Reset the barrier to the next generation.
/// @details Traps if threads are currently waiting at the barrier.
/// @param barrier Barrier object pointer.
void rt_barrier_reset(void *barrier);

/// @brief Get the number of parties required to release the barrier.
/// @param barrier Barrier object pointer.
/// @return Party count.
int64_t rt_barrier_get_parties(void *barrier);

/// @brief Get the number of parties currently waiting.
/// @param barrier Barrier object pointer.
/// @return Count of waiting threads in the current generation.
int64_t rt_barrier_get_waiting(void *barrier);

// =========================================================================
// Zanna.Threads.RwLock
// =========================================================================

/// @brief Create a new reader-writer lock.
/// @details The lock prefers waiting writers to avoid starvation.
/// @return Opaque RwLock object pointer, or NULL on failure.
void *rt_rwlock_new(void);

/// @brief Acquire the lock for shared (reader) access.
/// @details Blocks while a writer is active or waiting. Multiple readers
///          may hold the lock concurrently.
/// @param lock RwLock object pointer.
void rt_rwlock_read_enter(void *lock);

/// @brief Release a previously acquired read lock.
/// @details Traps if called without a matching read enter.
/// @param lock RwLock object pointer.
void rt_rwlock_read_exit(void *lock);

/// @brief Acquire the lock for exclusive (writer) access.
/// @details Writer acquisition is re-entrant for the owning thread and
///          blocks until no readers or writers are active.
/// @param lock RwLock object pointer.
void rt_rwlock_write_enter(void *lock);

/// @brief Release a previously acquired write lock.
/// @details Traps if called by a non-owner or without a matching enter.
/// @param lock RwLock object pointer.
void rt_rwlock_write_exit(void *lock);

/// @brief Attempt to acquire a read lock without blocking.
/// @details Succeeds only if no writer is active or waiting.
/// @param lock RwLock object pointer.
/// @return 1 if acquired, 0 otherwise.
int8_t rt_rwlock_try_read_enter(void *lock);

/// @brief Attempt to acquire a write lock without blocking.
/// @details Succeeds only if no readers or writers are active. If the
///          calling thread already owns the write lock, recursion is
///          incremented and the call succeeds.
/// @param lock RwLock object pointer.
/// @return 1 if acquired, 0 otherwise.
int8_t rt_rwlock_try_write_enter(void *lock);

/// @brief Query the number of active readers.
/// @param lock RwLock object pointer.
/// @return Count of active reader locks.
int64_t rt_rwlock_get_readers(void *lock);

/// @brief Check whether a writer currently holds the lock.
/// @param lock RwLock object pointer.
/// @return 1 if a writer is active, 0 otherwise.
int8_t rt_rwlock_get_is_write_locked(void *lock);

#ifdef __cplusplus
}
#endif
