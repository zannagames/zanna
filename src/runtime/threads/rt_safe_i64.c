//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/threads/rt_safe_i64.c
// Purpose: Implements a thread-safe int64 cell for the Zanna.Threads.SafeI64
//          class. Provides Get, Set, Add (returns new value), and
//          CompareExchange (CAS) operations synchronized via a monitor.
//
// Key invariants:
//   - All operations acquire the monitor before reading or writing the value.
//   - CompareExchange atomically reads, compares, conditionally writes, and
//     returns the pre-operation value in a single monitor-protected section.
//   - Add returns the value after the increment (post-increment semantics).
//   - The Windows path composes the object monitor with Interlocked 64-bit
//     primitives so individual accesses remain atomic on 32-bit Windows.
//   - The POSIX path protects the stored value through the object monitor.
//   - No busy-waiting; all blocking uses the monitor's condition variable.
//
// Ownership/Lifetime:
//   - SafeI64 objects are heap-allocated and managed by the runtime GC.
//   - Monitor state is associated with the cell address by the monitor runtime
//     and retired by normal runtime object finalization.
//
// Links: src/runtime/threads/rt_safe_i64.h (public API, via rt_threads.h),
//        src/runtime/threads/rt_monitor.h (underlying synchronization primitive),
//        src/runtime/threads/rt_threads.h (thread-related includes)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements the monitor-composable `Zanna.Threads.SafeI64` cell.
/// @details Every operation participates in the cell's object monitor, making
///          it atomic both alone and inside an explicit reentrant monitor
///          section. Windows additionally uses Interlocked 64-bit primitives
///          for safe access on 32-bit targets; POSIX reads and writes the value
///          while holding the monitor.

#include "rt_threads.h"

#include "rt_internal.h"
#include "rt_object.h"

#include <stdint.h>
#include <string.h>

/// @brief Reinterpret a uint64 bit pattern as int64 without UB.
/// @details Uses memcpy (well-defined type punning) rather than a signed cast,
///          whose result would be implementation-defined for values above
///          INT64_MAX — important since this backs atomic load results.
/// @param value Unsigned two's-complement bit pattern.
/// @return Signed integer with exactly the same object representation.
static int64_t safe_i64_from_u64(uint64_t value) {
    int64_t out;
    memcpy(&out, &value, sizeof(out));
    return out;
}

#if defined(_WIN32)

// =============================================================================
// Win32 backend — monitor-composable operations backed by Interlocked* intrinsics.
// The monitor keeps explicit Monitor.Enter(cell) sections portable with POSIX;
// the Interlocked calls keep each 64-bit operation atomic on 32-bit Windows.
// =============================================================================

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

typedef struct RtSafeI64Win {
    volatile LONG64 value;
} RtSafeI64Win;

/// @brief Validate and cast a SafeI64 handle for the Windows backend.
/// @details NULL input traps with @p what when supplied; a live runtime object
///          of any other class traps with the generic invalid-object diagnostic.
/// @param obj Candidate SafeI64 runtime object.
/// @param what Diagnostic used for NULL input.
/// @return Valid Windows SafeI64 payload, or NULL after a returning trap hook.
static RtSafeI64Win *require_safe_win(void *obj, const char *what) {
    if (!obj) {
        rt_trap(what ? what : "SafeI64: null object");
        return NULL;
    }
    if (!rt_obj_is_instance(obj, RT_SAFE_I64_CLASS_ID, sizeof(RtSafeI64Win))) {
        rt_trap("SafeI64: invalid object");
        return NULL;
    }
    return (RtSafeI64Win *)obj;
}

/// @brief Create a Windows SafeI64 cell with an initial value.
/// @param initial Initial signed 64-bit value.
/// @return New caller-owned SafeI64 object, or NULL after allocation traps.
void *rt_safe_i64_new(int64_t initial) {
    RtSafeI64Win *cell =
        (RtSafeI64Win *)rt_obj_new_i64(RT_SAFE_I64_CLASS_ID, (int64_t)sizeof(RtSafeI64Win));
    if (!cell)
        rt_trap("SafeI64.New: alloc failed");
    if (!cell)
        return NULL;
    cell->value = (LONG64)initial;
    return cell;
}

/// @brief Win32 SafeI64 read — implemented as `InterlockedCompareExchange64(.,0,0)` so the read
/// is atomic on 32-bit Windows builds (a plain `MOV` of a 64-bit value isn't atomic on x86).
/// @details The cell monitor is held around the interlocked read so this
///          operation composes with explicit monitor-protected transactions.
/// @param obj SafeI64 object to read.
/// @return Current value, or zero after an invalid-object trap returns.
int64_t rt_safe_i64_get(void *obj) {
    RtSafeI64Win *cell = require_safe_win(obj, "SafeI64.Get: null object");
    if (!cell)
        return 0;
    rt_monitor_enter(cell);
    // Acquire read via InterlockedCompareExchange64 (returns current value)
    int64_t value = (int64_t)InterlockedCompareExchange64(&cell->value, 0, 0);
    rt_monitor_exit(cell);
    return value;
}

/// @brief Replace a Windows SafeI64 value atomically.
/// @details The cell monitor is held around `InterlockedExchange64`.
/// @param obj SafeI64 object to update.
/// @param value New signed 64-bit value.
void rt_safe_i64_set(void *obj, int64_t value) {
    RtSafeI64Win *cell = require_safe_win(obj, "SafeI64.Set: null object");
    if (!cell)
        return;
    rt_monitor_enter(cell);
    InterlockedExchange64(&cell->value, (LONG64)value);
    rt_monitor_exit(cell);
}

/// @brief Win32 SafeI64 atomic add — returns the new value with two's-complement wraparound.
/// @details The cell monitor is held around `InterlockedExchangeAdd64`; unsigned
///          arithmetic computes a defined wrapped result from the returned old value.
/// @param obj SafeI64 object to update.
/// @param delta Signed increment, which may be negative.
/// @return Post-add value, or zero after an invalid-object trap returns.
int64_t rt_safe_i64_add(void *obj, int64_t delta) {
    RtSafeI64Win *cell = require_safe_win(obj, "SafeI64.Add: null object");
    if (!cell)
        return 0;
    rt_monitor_enter(cell);
    uint64_t old = (uint64_t)InterlockedExchangeAdd64(&cell->value, (LONG64)delta);
    int64_t value = safe_i64_from_u64(old + (uint64_t)delta);
    rt_monitor_exit(cell);
    return value;
}

/// @brief Win32 SafeI64 CAS — `InterlockedCompareExchange64` is one machine instruction (LOCK
/// CMPXCHG8B/CMPXCHG16B). Returns the value as it was BEFORE the swap; caller compares against
/// `expected` to determine whether the swap actually happened.
/// @details The cell monitor is held so the operation composes with other
///          monitor-protected SafeI64 accesses.
/// @param obj SafeI64 object to update.
/// @param expected Value required for replacement.
/// @param desired Value stored when the comparison succeeds.
/// @return Pre-operation value, or zero after an invalid-object trap returns.
int64_t rt_safe_i64_compare_exchange(void *obj, int64_t expected, int64_t desired) {
    RtSafeI64Win *cell = require_safe_win(obj, "SafeI64.CompareExchange: null object");
    if (!cell)
        return 0;
    rt_monitor_enter(cell);
    // Returns the old value (whether exchange happened or not)
    int64_t old =
        (int64_t)InterlockedCompareExchange64(&cell->value, (LONG64)desired, (LONG64)expected);
    rt_monitor_exit(cell);
    return old;
}

#else

/// @brief Internal structure for SafeI64.
///
/// Just holds a single int64 value. Thread safety is provided by using the
/// object's address as a monitor key (implicit monitor association).
typedef struct RtSafeI64 {
    int64_t value; ///< The stored value.
} RtSafeI64;

/// @brief Validates and casts a SafeI64 object pointer.
///
/// @param obj The object to validate.
/// @param what Error message context for trap.
///
/// @return The cast pointer, or NULL after trapping.
static RtSafeI64 *require_safe(void *obj, const char *what) {
    if (!obj) {
        rt_trap(what ? what : "SafeI64: null object");
        return NULL;
    }
    if (!rt_obj_is_instance(obj, RT_SAFE_I64_CLASS_ID, sizeof(RtSafeI64))) {
        rt_trap("SafeI64: invalid object");
        return NULL;
    }
    return (RtSafeI64 *)obj;
}

/// @brief Creates a new SafeI64 with an initial value.
///
/// Allocates and initializes a new thread-safe integer container. The returned
/// object is managed by Zanna's garbage collector.
///
/// **Example:**
/// ```
/// Dim counter = SafeI64.New(0)        ' Start at zero
/// Dim limit = SafeI64.New(1000)       ' Set a limit
/// Dim flags = SafeI64.New(&HFF)       ' Bit flags
/// ```
///
/// @param initial The initial value to store.
///
/// @return A new SafeI64 object, or NULL after trapping on allocation failure.
///
/// @note Thread-safe to call from any thread.
///
/// @see rt_safe_i64_get For reading the value
/// @see rt_safe_i64_set For writing the value
void *rt_safe_i64_new(int64_t initial) {
    RtSafeI64 *cell = (RtSafeI64 *)rt_obj_new_i64(RT_SAFE_I64_CLASS_ID, (int64_t)sizeof(RtSafeI64));
    if (!cell)
        rt_trap("SafeI64.New: alloc failed");
    if (!cell)
        return NULL;
    cell->value = initial;
    return cell;
}

/// @brief Reads the current value thread-safely.
///
/// Acquires the monitor, reads the value, and releases the monitor.
///
/// **Example:**
/// ```
/// Dim value = cell.Get()
/// Print "Current value: " & value
/// ```
///
/// @param obj The SafeI64 object. Must not be NULL.
///
/// @return The current value, or 0 after trapping if obj is NULL.
///
/// @note Thread-safe.
/// @note Traps if obj is NULL.
int64_t rt_safe_i64_get(void *obj) {
    RtSafeI64 *cell = require_safe(obj, "SafeI64.Get: null object");
    if (!cell)
        return 0;
    rt_monitor_enter(cell);
    const int64_t v = cell->value;
    rt_monitor_exit(cell);
    return v;
}

/// @brief Sets the value thread-safely.
///
/// Acquires the monitor, sets the value, and releases the monitor.
///
/// **Example:**
/// ```
/// cell.Set(42)
/// cell.Set(0)  ' Reset
/// ```
///
/// @param obj The SafeI64 object. Must not be NULL.
/// @param value The new value to store.
///
/// @note Thread-safe.
/// @note Traps if obj is NULL.
void rt_safe_i64_set(void *obj, int64_t value) {
    RtSafeI64 *cell = require_safe(obj, "SafeI64.Set: null object");
    if (!cell)
        return;
    rt_monitor_enter(cell);
    cell->value = value;
    rt_monitor_exit(cell);
}

/// @brief Atomically adds to the value and returns the new value.
///
/// Acquires the monitor, adds delta to the current value, and returns the
/// new value. This is useful for counters that need to know the new value.
///
/// **Example:**
/// ```
/// ' Thread-safe increment
/// Dim newCount = counter.Add(1)
/// Print "Incremented to " & newCount
///
/// ' Decrement
/// counter.Add(-1)
///
/// ' Add multiple
/// counter.Add(10)
/// ```
///
/// @param obj The SafeI64 object. Must not be NULL.
/// @param delta The value to add (can be negative for subtraction).
///
/// @return The new value after addition, or 0 after trapping if obj is NULL.
///
/// @note Thread-safe.
/// @note Traps if obj is NULL.
/// @note Overflow wraps using two's-complement arithmetic.
int64_t rt_safe_i64_add(void *obj, int64_t delta) {
    RtSafeI64 *cell = require_safe(obj, "SafeI64.Add: null object");
    if (!cell)
        return 0;
    rt_monitor_enter(cell);
    uint64_t next = (uint64_t)cell->value + (uint64_t)delta;
    cell->value = safe_i64_from_u64(next);
    const int64_t out = cell->value;
    rt_monitor_exit(cell);
    return out;
}

/// @brief Atomically compares and conditionally exchanges the value.
///
/// If the current value equals `expected`, sets it to `desired`. Always
/// returns the value that was read (before any potential modification).
///
/// **Success check:** If the returned value equals `expected`, the exchange
/// happened. If not, another thread modified the value first.
///
/// **Example:**
/// ```
/// ' Try to increment from 5 to 6
/// Dim old = cell.CompareExchange(5, 6)
/// If old = 5 Then
///     Print "Successfully changed 5 to 6"
/// Else
///     Print "Value was " & old & ", not 5"
/// End If
///
/// ' CAS loop for complex updates
/// Dim current, newVal As Long
/// Do
///     current = cell.Get()
///     newVal = Transform(current)
/// Loop While cell.CompareExchange(current, newVal) <> current
/// ```
///
/// @param obj The SafeI64 object. Must not be NULL.
/// @param expected The value to compare against.
/// @param desired The value to set if comparison succeeds.
///
/// @return The value that was read (whether exchange happened or not).
///         Returns 0 after trapping if obj is NULL.
///
/// @note Thread-safe.
/// @note Traps if obj is NULL.
/// @note This is the building block for many lock-free algorithms.
int64_t rt_safe_i64_compare_exchange(void *obj, int64_t expected, int64_t desired) {
    RtSafeI64 *cell = require_safe(obj, "SafeI64.CompareExchange: null object");
    if (!cell)
        return 0;
    rt_monitor_enter(cell);
    const int64_t old = cell->value;
    if (old == expected)
        cell->value = desired;
    rt_monitor_exit(cell);
    return old;
}

#endif
