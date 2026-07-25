//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/core/rt_time.c
// Purpose: Implements cross-platform sleeping, millisecond/microsecond elapsed
// clocks, overflow-safe clock scaling, fallback monotonic ratcheting, and the
// Zanna.Time.Clock 64-bit wrappers.
//
// Key invariants:
//   - Sleep uses nanosleep() on POSIX and Sleep() on Windows; nanosleep is
//     retried automatically on EINTR to sleep the full requested duration.
//   - The tick counter prefers CLOCK_MONOTONIC (POSIX) or QueryPerformanceCounter
//     (Windows). POSIX falls back to CLOCK_REALTIME when the monotonic query fails;
//     the fallback reading is passed through a process-local atomic floor (CAS-max)
//     so the exposed sequence stays non-decreasing even if wall-clock time is
//     adjusted backward (VDOC-223). The monotonic fast path holds no shared state.
//   - Negative sleep durations are treated as 0 (no-op).
//   - Public operations are thread-safe. POSIX realtime fallbacks share
//     per-resolution atomic floor values; monotonic fast paths are stateless.
//
// Ownership/Lifetime:
//   - All functions operate on scalar integer values; no heap allocation is
//     performed.
//   - Only atomic fallback floors and platform clock state persist across calls.
//
// Links: src/runtime/core/rt_stopwatch.c (high-level Stopwatch class),
//        src/runtime/core/rt_countdown.c (Countdown timer class),
//        src/runtime/core/rt_datetime.c (wall-clock date/time operations)
//
//===----------------------------------------------------------------------===//
/// @file
/// @brief Portable sleeping, elapsed clocks, and monotonic fallback helpers.

#include "zanna/runtime/rt.h"

#include "rt_atomic_compat.h"

#include <limits.h>
#include <stdint.h>

/// @brief Add two signed 64-bit values with explicit overflow detection.
/// @param a Left operand.
/// @param b Right operand.
/// @param out Required output receiving the sum only on success.
/// @return One on overflow; zero after storing the exact sum.
static int rt_time_checked_add_i64(int64_t a, int64_t b, int64_t *out) {
    if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b))
        return 1;
    *out = a + b;
    return 0;
}

/// @brief Overflow-checked signed 64-bit multiplication. Returns 1 on overflow.
/// @details Uses `__builtin_mul_overflow` on GCC/Clang and a manual divide-bound
///          check on MSVC.
/// @param a Left factor.
/// @param b Right factor.
/// @param out Required output receiving the product only on success.
/// @return One on overflow; zero after storing the exact product.
static int rt_time_checked_mul_i64(int64_t a, int64_t b, int64_t *out) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_mul_overflow(a, b, out);
#else
    if (a == 0 || b == 0) {
        *out = 0;
        return 0;
    }
    if (a > 0) {
        if (b > 0) {
            if (a > INT64_MAX / b)
                return 1;
        } else if (b < INT64_MIN / a) {
            return 1;
        }
    } else {
        if (b > 0) {
            if (a < INT64_MIN / b)
                return 1;
        } else if (a < INT64_MAX / b) {
            return 1;
        }
    }
    *out = a * b;
    return 0;
#endif
}

/// @brief Compute `seconds * scale + fraction` with overflow trapping via `rt_trap_ovf`.
/// @details Used to convert a `(seconds, sub_second)` pair into a single integer time
///          value at a given resolution (e.g. `(s, ns/1e6) → ms` uses `scale = 1000`).
///          Either the multiply or the add overflowing surfaces the trap rather than
///          silently wrapping.
/// @param seconds Whole-second component.
/// @param scale Output units per second.
/// @param fraction Already-scaled subsecond component.
/// @return Combined signed value, or zero after a returning overflow trap.
static int64_t rt_time_scale_seconds(int64_t seconds, int64_t scale, int64_t fraction) {
    int64_t whole;
    int64_t result;
    if (rt_time_checked_mul_i64(seconds, scale, &whole) ||
        rt_time_checked_add_i64(whole, fraction, &result)) {
        rt_trap_ovf();
        return 0;
    }
    return result;
}

/// @brief Clamp a wall-clock fallback reading up to a process-local floor.
/// @details The CLOCK_REALTIME fallback (used only when CLOCK_MONOTONIC is
///          unavailable) can jump backward when the system clock is adjusted,
///          which would let elapsed calculations go negative. Routing the
///          fallback reading through a per-call-site atomic floor makes the
///          returned sequence non-decreasing: a lock-free CAS-max advances the
///          floor when the candidate is newer and otherwise returns the retained
///          floor (VDOC-223). The monotonic fast path never touches this state.
///          Shared with the Stopwatch and Countdown fallbacks so all three
///          Zanna.Time surfaces ratchet identically.
/// @param floor Process-local monotonic floor for this scale/call site.
/// @param candidate Freshly sampled fallback value.
/// @return The greater of the candidate and the retained floor.
/// @pre @p floor is non-null, suitably aligned, and remains live for concurrent
///      atomic access.
int64_t rt_time_monotonic_ratchet(int64_t *floor, int64_t candidate) {
    int64_t prev = __atomic_load_n(floor, __ATOMIC_RELAXED);
    while (candidate > prev) {
        if (__atomic_compare_exchange_n(floor, &prev, candidate, 0, __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED))
            return candidate;
    }
    return prev;
}

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/// @brief Narrow a `uint64_t` into `int64_t`, trapping when the value exceeds `INT64_MAX`.
/// @details Used to clip Win32 `GetTickCount64`/`QueryPerformanceCounter` returns into
///          the signed 64-bit range that the rest of the time machinery uses. Long-uptime
///          systems can produce values beyond `INT64_MAX` after many years of uptime.
/// @param value Unsigned platform counter value.
/// @return Exact signed representation, or zero after a returning overflow trap.
static int64_t rt_time_u64_to_i64(uint64_t value) {
    if (value > (uint64_t)INT64_MAX) {
        rt_trap_ovf();
        return 0;
    }
    return (int64_t)value;
}

/// @brief Suspends execution for the specified number of milliseconds (Windows).
///
/// Blocks the current thread for approximately the specified duration. The
/// actual sleep time may be slightly longer due to system scheduling.
///
/// **Usage example:**
/// ```
/// Print "Starting..."
/// Sleep(1000)           ' Wait 1 second
/// Print "Done!"
///
/// ' Animation delay
/// For i = 1 To 100
///     DrawFrame(i)
///     Sleep(16)          ' ~60 FPS (1000/60 ≈ 16ms)
/// Next
/// ```
///
/// @param ms Duration to sleep in milliseconds. Negative values are treated as 0.
///
/// @note Minimum resolution is typically 10-15ms on Windows.
/// @note Uses the Win32 Sleep() function.
///
/// @see rt_timer_ms For measuring elapsed time
void rt_sleep_ms(int32_t ms) {
    if (ms < 0)
        ms = 0;
    Sleep((DWORD)ms);
}

/// @brief Returns monotonic time in milliseconds (Windows).
///
/// Returns the number of milliseconds since an unspecified starting point.
/// The value increases monotonically and is not affected by system time
/// changes. Use this for measuring elapsed time between two points.
///
/// **Usage example:**
/// ```
/// Dim startTime = Timer()
/// DoSomeWork()
/// Dim endTime = Timer()
/// Print "Elapsed: " & (endTime - startTime) & " ms"
/// ```
///
/// @return Monotonic milliseconds since an arbitrary epoch.
///
/// @note Uses QueryPerformanceCounter for high resolution.
/// @note Falls back to GetTickCount64 if QPC is unavailable.
/// @note Never decreases, even if system time is changed.
///
/// @see rt_clock_ticks_us For microsecond resolution
int64_t rt_timer_ms(void) {
    // Use QueryPerformanceCounter for high-resolution monotonic time
    LARGE_INTEGER freq, counter;
    if (!QueryPerformanceFrequency(&freq) || freq.QuadPart == 0) {
        // Fallback to GetTickCount64 if QPC unavailable
        return rt_time_u64_to_i64((uint64_t)GetTickCount64());
    }

    if (!QueryPerformanceCounter(&counter))
        return rt_time_u64_to_i64((uint64_t)GetTickCount64());

    int64_t whole = counter.QuadPart / freq.QuadPart;
    int64_t remainder = counter.QuadPart % freq.QuadPart;
    int64_t fraction = (int64_t)(((long double)remainder * 1000.0L) / (long double)freq.QuadPart);
    return rt_time_scale_seconds(whole, 1000LL, fraction);
}

/// @brief Returns monotonic time in microseconds (Windows).
///
/// Returns the number of microseconds since an unspecified starting point.
/// This provides higher resolution than rt_timer_ms for precise timing needs.
///
/// **Usage example:**
/// ```
/// Dim start = Clock.TicksUs()
/// DoFastOperation()
/// Dim elapsed = Clock.TicksUs() - start
/// Print "Operation took " & elapsed & " microseconds"
/// ```
///
/// @return Monotonic microseconds since an arbitrary epoch.
///
/// @note Uses QueryPerformanceCounter for high resolution.
/// @note Falls back to GetTickCount64 * 1000 if QPC unavailable.
/// @note 1 millisecond = 1000 microseconds.
///
/// @see rt_timer_ms For millisecond resolution
int64_t rt_clock_ticks_us(void) {
    // Use QueryPerformanceCounter for high-resolution monotonic time
    LARGE_INTEGER freq, counter;
    if (!QueryPerformanceFrequency(&freq) || freq.QuadPart == 0) {
        // Fallback to GetTickCount64 (milliseconds) * 1000 for microseconds
        return rt_time_scale_seconds(rt_time_u64_to_i64((uint64_t)GetTickCount64()), 1000LL, 0);
    }

    if (!QueryPerformanceCounter(&counter))
        return rt_time_scale_seconds(rt_time_u64_to_i64((uint64_t)GetTickCount64()), 1000LL, 0);

    // Convert to microseconds using split division to avoid overflow:
    // (counter / freq) * 1000000 + (counter % freq) * 1000000 / freq
    int64_t whole = counter.QuadPart / freq.QuadPart;
    int64_t remainder = counter.QuadPart % freq.QuadPart;
    int64_t fraction =
        (int64_t)(((long double)remainder * 1000000.0L) / (long double)freq.QuadPart);
    return rt_time_scale_seconds(whole, 1000000LL, fraction);
}

#else
#include <errno.h>
#include <time.h>

/// @brief Suspends execution for the specified number of milliseconds (Unix).
///
/// Blocks the current thread for approximately the specified duration. On
/// Unix systems, this uses nanosleep() and automatically retries if the sleep
/// is interrupted by a signal (EINTR), ensuring the full duration is slept.
///
/// **Signal handling:**
/// Unlike some sleep implementations, this function guarantees the full
/// requested duration is slept, even if signals interrupt the process:
/// ```
/// Sleep(5000)   ' Always sleeps ~5 seconds, even with SIGALRM
/// ```
///
/// **Usage example:**
/// ```
/// Print "Countdown..."
/// For i = 5 To 1 Step -1
///     Print i
///     Sleep(1000)
/// Next
/// Print "Go!"
/// ```
///
/// @param ms Duration to sleep in milliseconds. Negative values are treated as 0.
///
/// @note Uses nanosleep() with automatic retry on EINTR.
/// @note Resolution is typically 1ms or better on modern systems.
///
/// @see rt_timer_ms For measuring elapsed time
void rt_sleep_ms(int32_t ms) {
    if (ms < 0)
        ms = 0;

    struct timespec req;
    req.tv_sec = ms / 1000;
    long nsec = (long)(ms % 1000) * 1000000L;
    req.tv_nsec = nsec;

    while (nanosleep(&req, &req) == -1 && errno == EINTR) {
        // Retry with remaining time in req.
    }
}

/// @brief Returns monotonic time in milliseconds (Unix).
///
/// Returns the number of milliseconds since an unspecified starting point
/// using the system's monotonic clock. The value is guaranteed to never
/// decrease, even if the system clock is adjusted.
///
/// **Clock selection:**
/// 1. CLOCK_MONOTONIC (preferred) - not affected by NTP or manual changes
/// 2. CLOCK_REALTIME (fallback) - may jump forward or backward with system time
///
/// **Usage example:**
/// ```
/// Dim start = Timer()
/// ' ... lengthy operation ...
/// Dim elapsed = Timer() - start
/// Print "Operation took " & elapsed & " ms"
/// ```
///
/// @return Milliseconds since the selected clock's epoch, or 0 on error. The
///         realtime fallback is ratcheted to stay non-decreasing.
///
/// @note Uses clock_gettime() with CLOCK_MONOTONIC.
/// @note Falls back to CLOCK_REALTIME if CLOCK_MONOTONIC unavailable; that
///       fallback is clamped to a process-local floor so it never decreases.
/// @note Resolution is typically 1ms or better.
///
/// @see rt_clock_ticks_us For microsecond resolution
int64_t rt_timer_ms(void) {
    // Use CLOCK_MONOTONIC for monotonic time since unspecified epoch
    struct timespec ts;

#ifdef CLOCK_MONOTONIC
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        // Convert to milliseconds: seconds * 1000 + nanoseconds / 1000000
        return rt_time_scale_seconds((int64_t)ts.tv_sec, 1000LL, (int64_t)ts.tv_nsec / 1000000LL);
    }
#endif

    // Fallback to CLOCK_REALTIME if CLOCK_MONOTONIC unavailable. Ratchet the
    // wall-clock reading so the exposed sequence never decreases (VDOC-223).
    if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
        static int64_t floor_ms = 0;
        int64_t candidate =
            rt_time_scale_seconds((int64_t)ts.tv_sec, 1000LL, (int64_t)ts.tv_nsec / 1000000LL);
        return rt_time_monotonic_ratchet(&floor_ms, candidate);
    }

    // Last resort: return 0 if all clock sources fail
    return 0;
}

/// @brief Returns monotonic time in microseconds (Unix).
///
/// Returns the number of microseconds since an unspecified starting point.
/// This provides 1000x higher resolution than rt_timer_ms for precise timing.
///
/// **Usage example:**
/// ```
/// Dim start = Clock.TicksUs()
/// FastFunction()
/// Dim elapsed = Clock.TicksUs() - start
/// Print "Function took " & elapsed & " μs"
/// ```
///
/// @return Microseconds since the selected clock's epoch, or 0 on error. The
///         realtime fallback is ratcheted to stay non-decreasing.
///
/// @note Uses clock_gettime() with CLOCK_MONOTONIC.
/// @note Falls back to CLOCK_REALTIME if CLOCK_MONOTONIC unavailable; that
///       fallback is clamped to a process-local floor so it never decreases.
/// @note 1 millisecond = 1,000 microseconds.
/// @note Typical resolution is 1 microsecond on modern systems.
///
/// @see rt_timer_ms For millisecond resolution
int64_t rt_clock_ticks_us(void) {
    // Use CLOCK_MONOTONIC for monotonic time since unspecified epoch
    struct timespec ts;

#ifdef CLOCK_MONOTONIC
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        // Convert to microseconds: seconds * 1000000 + nanoseconds / 1000
        return rt_time_scale_seconds((int64_t)ts.tv_sec, 1000000LL, (int64_t)ts.tv_nsec / 1000LL);
    }
#endif

    // Fallback to CLOCK_REALTIME if CLOCK_MONOTONIC unavailable. Ratchet the
    // wall-clock reading so the exposed sequence never decreases (VDOC-223).
    if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
        static int64_t floor_us = 0;
        int64_t candidate =
            rt_time_scale_seconds((int64_t)ts.tv_sec, 1000000LL, (int64_t)ts.tv_nsec / 1000LL);
        return rt_time_monotonic_ratchet(&floor_us, candidate);
    }

    // Last resort: return 0 if all clock sources fail
    return 0;
}
#endif

//=============================================================================
// Zanna.Time.Clock wrappers (i64 interface)
//=============================================================================

/// @brief Suspends execution for the specified number of milliseconds (64-bit).
///
/// High-level wrapper around rt_sleep_ms that accepts 64-bit durations. Values
/// outside the valid range are clamped. This is the function exposed to Zanna
/// BASIC code via the Clock class.
///
/// **Value clamping:**
/// - Negative values → 0 (no sleep)
/// - Values > INT32_MAX → INT32_MAX (~24 days)
///
/// **Usage example:**
/// ```
/// ' Sleep for 2.5 seconds
/// Clock.Sleep(2500)
///
/// ' Sleep using a 64-bit variable
/// Dim duration As Long = 1000
/// Clock.Sleep(duration)
/// ```
///
/// @param ms Duration to sleep in milliseconds (clamped to int32 range).
///
/// @note Delegates to rt_sleep_ms after clamping.
/// @note Maximum sleep is about 24.8 days (INT32_MAX milliseconds).
///
/// @see rt_sleep_ms For the underlying implementation
/// @see rt_clock_ticks For measuring elapsed time
void rt_clock_sleep(int64_t ms) {
    // Clamp to int32_t range for rt_sleep_ms
    if (ms < 0)
        ms = 0;
    if (ms > INT32_MAX)
        ms = INT32_MAX;
    rt_sleep_ms((int32_t)ms);
}

/// @brief Returns monotonic time in milliseconds (Clock wrapper).
///
/// High-level wrapper that returns the current monotonic time in milliseconds.
/// This is the function exposed to Zanna BASIC code as Clock.Ticks().
///
/// **Usage example:**
/// ```
/// Dim start = Clock.Ticks()
/// DoWork()
/// Dim elapsed = Clock.Ticks() - start
/// Print "Work took " & elapsed & " ms"
/// ```
///
/// @return Monotonic milliseconds since an arbitrary epoch.
///
/// @note Simply delegates to rt_timer_ms.
///
/// @see rt_timer_ms For implementation details
/// @see rt_clock_ticks_us For microsecond resolution
int64_t rt_clock_ticks(void) {
    return rt_timer_ms();
}
