//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/rt_platform.h
// Purpose: Cross-platform preprocessor abstractions for the Zanna runtime, providing portable
// macros for thread-local storage, atomic operations, weak symbol linkage, and platform detection.
//
// Key invariants:
//   - RT_THREAD_LOCAL expands to the correct TLS keyword for each compiler/platform.
//   - RT_ATOMIC_* macros use C11 _Atomic on GCC/Clang and MSVC intrinsics on Windows.
//   - RT_WEAK uses __attribute__((weak)) on ELF targets and is empty on Mach-O/MSVC.
//   - Platform detection macros (RT_PLATFORM_WINDOWS etc.) are mutually exclusive.
//   - Compiler-specific diagnostics are hidden behind RT_* adapter macros.
//
// Ownership/Lifetime:
//   - All macros are pure preprocessor definitions; no runtime state is introduced.
//   - Including this header has no link-time side effects.
//
// Links: src/runtime/core/ (included by most runtime .c files)
//
//===----------------------------------------------------------------------===//
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

//===----------------------------------------------------------------------===//
// Platform Detection
//===----------------------------------------------------------------------===//

#if defined(_WIN32) || defined(_WIN64)
#define RT_PLATFORM_WINDOWS 1
#else
#define RT_PLATFORM_WINDOWS 0
#endif

#if defined(__APPLE__)
#define RT_PLATFORM_MACOS 1
#else
#define RT_PLATFORM_MACOS 0
#endif

#if defined(__linux__)
#define RT_PLATFORM_LINUX 1
#else
#define RT_PLATFORM_LINUX 0
#endif

//===----------------------------------------------------------------------===//
// Compiler Detection
//===----------------------------------------------------------------------===//

#if defined(_MSC_VER) && !defined(__clang__)
#define RT_COMPILER_MSVC 1
#else
#define RT_COMPILER_MSVC 0
#endif

#if defined(__GNUC__) || defined(__clang__)
#define RT_COMPILER_GCC_LIKE 1
#else
#define RT_COMPILER_GCC_LIKE 0
#endif

//===----------------------------------------------------------------------===//
// Thread-Local Storage
//===----------------------------------------------------------------------===//

#if RT_COMPILER_MSVC
#define RT_THREAD_LOCAL __declspec(thread)
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_THREADS__)
#define RT_THREAD_LOCAL _Thread_local
#else
#define RT_THREAD_LOCAL __thread
#endif

//===----------------------------------------------------------------------===//
// Weak Symbol Linkage
//===----------------------------------------------------------------------===//

#if RT_COMPILER_MSVC
// MSVC doesn't support weak linkage for functions. For data, use selectany.
// For functions, we define RT_WEAK as empty - test overrides work via link order.
#define RT_WEAK
#define RT_WEAK_DATA __declspec(selectany)
#elif RT_COMPILER_GCC_LIKE
#define RT_WEAK __attribute__((weak))
#define RT_WEAK_DATA __attribute__((weak))
#else
#define RT_WEAK
#define RT_WEAK_DATA
#endif

//===----------------------------------------------------------------------===//
// Compiler Diagnostics
//===----------------------------------------------------------------------===//

#if RT_COMPILER_MSVC
#define RT_SUPPRESS_SETJMP_WARNING_BEGIN __pragma(warning(push)) __pragma(warning(disable : 4611))
#define RT_SUPPRESS_SETJMP_WARNING_END __pragma(warning(pop))
#else
#define RT_SUPPRESS_SETJMP_WARNING_BEGIN
#define RT_SUPPRESS_SETJMP_WARNING_END
#endif

//===----------------------------------------------------------------------===//
// Atomic Operations
//===----------------------------------------------------------------------===//

#if RT_COMPILER_MSVC
#include <intrin.h>
#if defined(_M_IX86) || defined(_M_X64)
#include <immintrin.h>
#endif

// Memory ordering constants (matching GCC values for compatibility)
#define __ATOMIC_RELAXED 0
#define __ATOMIC_CONSUME 1
#define __ATOMIC_ACQUIRE 2
#define __ATOMIC_RELEASE 3
#define __ATOMIC_ACQ_REL 4
#define __ATOMIC_SEQ_CST 5

/// @brief Atomically load an 8-bit signed integer on MSVC platforms.
/// @param ptr Naturally aligned storage to load from.
/// @param order GCC-style memory-order constant accepted for source compatibility.
/// @return The value observed in @p ptr.
static inline int8_t rt_atomic_load_i8(const volatile int8_t *ptr, int order) {
    (void)order;
    int8_t value = *ptr;
#if defined(_M_ARM64)
    __dmb(_ARM64_BARRIER_ISH);
#else
    _ReadWriteBarrier();
#endif
    return value;
}

/// @brief Atomically store an 8-bit signed integer on MSVC platforms.
/// @param ptr Naturally aligned storage to update.
/// @param value Value to publish.
/// @param order GCC-style memory-order constant accepted for source compatibility.
static inline void rt_atomic_store_i8(volatile int8_t *ptr, int8_t value, int order) {
    (void)order;
#if defined(_M_ARM64)
    __dmb(_ARM64_BARRIER_ISH);
#else
    _ReadWriteBarrier();
#endif
    *ptr = value;
#if defined(_M_ARM64)
    __dmb(_ARM64_BARRIER_ISH);
#else
    _ReadWriteBarrier();
#endif
}

/// @brief Atomically load a 32-bit signed integer on MSVC platforms.
/// @details ARM64 builds use a CPU data-memory barrier because a compiler-only barrier is not
///          sufficient to preserve cross-thread visibility on that architecture.
/// @param ptr Naturally aligned storage to load from.
/// @param order GCC-style memory-order constant accepted for source compatibility.
/// @return The value observed in @p ptr.
static inline int rt_atomic_load_i32(const volatile int *ptr, int order) {
    (void)order;
    int value = *ptr;
#if defined(_M_ARM64)
    __dmb(_ARM64_BARRIER_ISH);
#else
    _ReadWriteBarrier();
#endif
    return value;
}

/// @brief Atomically store a 32-bit signed integer on MSVC platforms.
/// @param ptr Naturally aligned storage to update.
/// @param value Value to publish.
/// @param order GCC-style memory-order constant accepted for source compatibility.
static inline void rt_atomic_store_i32(volatile int *ptr, int value, int order) {
    (void)order;
#if defined(_M_ARM64)
    __dmb(_ARM64_BARRIER_ISH);
#else
    _ReadWriteBarrier();
#endif
    *ptr = value;
#if defined(_M_ARM64)
    __dmb(_ARM64_BARRIER_ISH);
#else
    _ReadWriteBarrier();
#endif
}

/// @brief Atomically exchange a 32-bit signed integer on MSVC platforms.
/// @param ptr Storage to update.
/// @param value Replacement value.
/// @param order GCC-style memory-order constant accepted for source compatibility.
/// @return The previous value stored in @p ptr.
static inline int rt_atomic_exchange_i32(volatile int *ptr, int value, int order) {
    (void)order;
    return _InterlockedExchange((volatile long *)ptr, value);
}

/// @brief Atomically compare-and-swap a 32-bit signed integer on MSVC platforms.
/// @param ptr Storage to update when it equals @p expected.
/// @param expected In/out expected value; receives the observed value on failure.
/// @param desired Value to store on success.
/// @param success_order Memory order for a successful exchange.
/// @param fail_order Memory order for a failed exchange.
/// @return 1 on success, 0 when @p expected did not match.
static inline int rt_atomic_compare_exchange_i32(
    volatile int *ptr, int *expected, int desired, int success_order, int fail_order) {
    (void)success_order;
    (void)fail_order;
    int old = _InterlockedCompareExchange((volatile long *)ptr, desired, *expected);
    if (old == *expected) {
        return 1;
    }
    *expected = old;
    return 0;
}

/// @brief Atomically add to a 32-bit signed integer on MSVC platforms.
/// @param ptr Storage to update.
/// @param value Increment to apply.
/// @param order GCC-style memory-order constant accepted for source compatibility.
/// @return The value stored in @p ptr before the addition.
static inline int rt_atomic_fetch_add_i32(volatile int *ptr, int value, int order) {
    (void)order;
    return _InterlockedExchangeAdd((volatile long *)ptr, value);
}

/// @brief Atomically subtract from a 32-bit signed integer on MSVC platforms.
/// @param ptr Storage to update.
/// @param value Decrement to apply.
/// @param order GCC-style memory-order constant accepted for source compatibility.
/// @return The value stored in @p ptr before the subtraction.
static inline int rt_atomic_fetch_sub_i32(volatile int *ptr, int value, int order) {
    (void)order;
    const uint32_t delta_bits = UINT32_C(0) - (uint32_t)value;
    return _InterlockedExchangeAdd((volatile long *)ptr, (long)delta_bits);
}

/// @brief Atomically load a 64-bit signed integer on MSVC platforms.
/// @param ptr Naturally aligned storage to load from.
/// @param order GCC-style memory-order constant accepted for source compatibility.
/// @return The value observed in @p ptr.
static inline int64_t rt_atomic_load_i64(const volatile int64_t *ptr, int order) {
    (void)order;
#if defined(_M_ARM64)
    int64_t value = *ptr;
    __dmb(_ARM64_BARRIER_ISH);
    return value;
#elif defined(_M_X64)
    int64_t value = *ptr;
    _ReadWriteBarrier();
    return value;
#else
    // 32-bit x86 needs interlocked read for 64-bit atomics
    return _InterlockedCompareExchange64((volatile long long *)ptr, 0, 0);
#endif
}

/// @brief Atomically store a 64-bit signed integer on MSVC platforms.
/// @param ptr Naturally aligned storage to update.
/// @param value Value to publish.
/// @param order GCC-style memory-order constant accepted for source compatibility.
static inline void rt_atomic_store_i64(volatile int64_t *ptr, int64_t value, int order) {
    (void)order;
#if defined(_M_ARM64)
    __dmb(_ARM64_BARRIER_ISH);
    *ptr = value;
    __dmb(_ARM64_BARRIER_ISH);
#elif defined(_M_X64)
    _ReadWriteBarrier();
    *ptr = value;
    _ReadWriteBarrier();
#else
    _InterlockedExchange64((volatile long long *)ptr, value);
#endif
}

/// @brief Atomically compare-and-swap a 64-bit signed integer on MSVC platforms.
/// @param ptr Storage to update when it equals @p expected.
/// @param expected In/out expected value; receives the observed value on failure.
/// @param desired Value to store on success.
/// @param success_order Memory order for a successful exchange.
/// @param fail_order Memory order for a failed exchange.
/// @return 1 on success, 0 when @p expected did not match.
static inline int rt_atomic_compare_exchange_i64(
    volatile int64_t *ptr, int64_t *expected, int64_t desired, int success_order, int fail_order) {
    (void)success_order;
    (void)fail_order;
    int64_t old = _InterlockedCompareExchange64((volatile long long *)ptr, desired, *expected);
    if (old == *expected) {
        return 1;
    }
    *expected = old;
    return 0;
}

/// @brief Atomically add to a 64-bit signed integer on MSVC platforms.
/// @param ptr Storage to update.
/// @param value Increment to apply.
/// @param order GCC-style memory-order constant accepted for source compatibility.
/// @return The value stored in @p ptr before the addition.
static inline int64_t rt_atomic_fetch_add_i64(volatile int64_t *ptr, int64_t value, int order) {
    (void)order;
    return _InterlockedExchangeAdd64((volatile long long *)ptr, value);
}

/// @brief Atomically subtract from a 64-bit signed integer on MSVC platforms.
/// @param ptr Storage to update.
/// @param value Decrement to apply.
/// @param order GCC-style memory-order constant accepted for source compatibility.
/// @return The value stored in @p ptr before the subtraction.
static inline int64_t rt_atomic_fetch_sub_i64(volatile int64_t *ptr, int64_t value, int order) {
    (void)order;
    const uint64_t delta_bits = UINT64_C(0) - (uint64_t)value;
    return _InterlockedExchangeAdd64((volatile long long *)ptr, (long long)delta_bits);
}

/// @brief Atomically load a size_t value on MSVC platforms.
/// @details `size_t` is handled separately from signed 64-bit storage because LLP64 Windows targets
///          give it a distinct ABI shape from `int64_t`.
/// @param ptr Naturally aligned storage to load from.
/// @param order GCC-style memory-order constant accepted for source compatibility.
/// @return The value observed in @p ptr.
static inline size_t rt_atomic_load_size(const volatile size_t *ptr, int order) {
    (void)order;
#if defined(_M_ARM64)
    size_t value = *ptr;
    __dmb(_ARM64_BARRIER_ISH);
    return value;
#elif defined(_M_X64)
    size_t value = *ptr;
    _ReadWriteBarrier();
    return value;
#else
    return (size_t)_InterlockedCompareExchange((volatile long *)ptr, 0, 0);
#endif
}

/// @brief Atomically store a size_t value on MSVC platforms.
/// @param ptr Naturally aligned storage to update.
/// @param value Value to publish.
/// @param order GCC-style memory-order constant accepted for source compatibility.
static inline void rt_atomic_store_size(volatile size_t *ptr, size_t value, int order) {
    (void)order;
#if defined(_M_ARM64)
    __dmb(_ARM64_BARRIER_ISH);
    *ptr = value;
    __dmb(_ARM64_BARRIER_ISH);
#elif defined(_M_X64)
    _ReadWriteBarrier();
    *ptr = value;
    _ReadWriteBarrier();
#else
    _InterlockedExchange((volatile long *)ptr, (long)value);
#endif
}

/// @brief Atomically compare-and-swap a size_t value on MSVC platforms.
/// @param ptr Storage to update when it equals @p expected.
/// @param expected In/out expected value; receives the observed value on failure.
/// @param desired Value to store on success.
/// @param success_order Memory order for a successful exchange.
/// @param fail_order Memory order for a failed exchange.
/// @return 1 on success, 0 when @p expected did not match.
static inline int rt_atomic_compare_exchange_size(
    volatile size_t *ptr, size_t *expected, size_t desired, int success_order, int fail_order) {
    (void)success_order;
    (void)fail_order;
#if defined(_M_X64) || defined(_M_ARM64)
    size_t old = (size_t)_InterlockedCompareExchange64(
        (volatile long long *)ptr, (long long)desired, (long long)*expected);
#else
    size_t old =
        (size_t)_InterlockedCompareExchange((volatile long *)ptr, (long)desired, (long)*expected);
#endif
    if (old == *expected) {
        return 1;
    }
    *expected = old;
    return 0;
}

/// @brief Atomically add to a size_t counter on MSVC platforms.
/// @param ptr Storage to update.
/// @param value Increment to apply.
/// @param order GCC-style memory-order constant accepted for source compatibility.
/// @return The value stored in @p ptr before the addition.
static inline size_t rt_atomic_fetch_add_size(volatile size_t *ptr, size_t value, int order) {
    (void)order;
#if defined(_M_X64) || defined(_M_ARM64)
    return (size_t)_InterlockedExchangeAdd64((volatile long long *)ptr, (long long)value);
#else
    return (size_t)_InterlockedExchangeAdd((volatile long *)ptr, (long)value);
#endif
}

/// @brief Atomically subtract from a size_t counter on MSVC platforms.
/// @param ptr Storage to update.
/// @param value Decrement to apply.
/// @param order GCC-style memory-order constant accepted for source compatibility.
/// @return The value stored in @p ptr before the subtraction.
static inline size_t rt_atomic_fetch_sub_size(volatile size_t *ptr, size_t value, int order) {
    (void)order;
    const size_t delta_bits = (size_t)0 - value;
#if defined(_M_X64) || defined(_M_ARM64)
    return (size_t)_InterlockedExchangeAdd64((volatile long long *)ptr, (long long)delta_bits);
#else
    return (size_t)_InterlockedExchangeAdd((volatile long *)ptr, (long)delta_bits);
#endif
}

/// @brief Atomically load a pointer on MSVC platforms.
/// @param ptr Address of pointer storage to load from.
/// @param order GCC-style memory-order constant accepted for source compatibility.
/// @return The pointer value observed in @p ptr.
static inline void *rt_atomic_load_ptr(void *const volatile *ptr, int order) {
    (void)order;
#if defined(_M_X64) || defined(_M_ARM64)
    return _InterlockedCompareExchangePointer((void *volatile *)ptr, NULL, NULL);
#else
    return (void *)_InterlockedCompareExchange((volatile long *)ptr, 0, 0);
#endif
}

/// @brief Atomically store a pointer on MSVC platforms.
/// @param ptr Address of pointer storage to update.
/// @param value Pointer value to publish.
/// @param order GCC-style memory-order constant accepted for source compatibility.
static inline void rt_atomic_store_ptr(void *volatile *ptr, void *value, int order) {
    (void)order;
#if defined(_M_X64) || defined(_M_ARM64)
    _InterlockedExchangePointer(ptr, value);
#else
    _InterlockedExchange((volatile long *)ptr, (long)value);
#endif
}

/// @brief Atomically exchange a pointer on MSVC platforms.
/// @param ptr Address of pointer storage to update.
/// @param value Replacement pointer value.
/// @param order GCC-style memory-order constant accepted for source compatibility.
/// @return The previous pointer value.
static inline void *rt_atomic_exchange_ptr(void *volatile *ptr, void *value, int order) {
    (void)order;
#if defined(_M_X64) || defined(_M_ARM64)
    return _InterlockedExchangePointer(ptr, value);
#else
    return (void *)_InterlockedExchange((volatile long *)ptr, (long)value);
#endif
}

/// @brief Atomically compare-and-swap a pointer on MSVC platforms.
/// @param ptr Address of pointer storage to update when it equals @p expected.
/// @param expected In/out expected pointer; receives the observed value on failure.
/// @param desired Pointer value to store on success.
/// @param success_order Memory order for a successful exchange.
/// @param fail_order Memory order for a failed exchange.
/// @return 1 on success, 0 when @p expected did not match.
static inline int rt_atomic_compare_exchange_ptr(
    void *volatile *ptr, void **expected, void *desired, int success_order, int fail_order) {
    (void)success_order;
    (void)fail_order;
#if defined(_M_X64) || defined(_M_ARM64)
    void *old = _InterlockedCompareExchangePointer(ptr, desired, *expected);
#else
    void *old =
        (void *)_InterlockedCompareExchange((volatile long *)ptr, (long)desired, (long)*expected);
#endif
    if (old == *expected) {
        return 1;
    }
    *expected = old;
    return 0;
}

/// @brief Interpret a `double` storage address as the 64-bit slot required by Win32 atomics.
/// @details The Windows fallback for GCC-style `__atomic_load` / `__atomic_store` handles
///          floating-point values by atomically moving their IEEE-754 bit pattern through
///          `Interlocked*64`. All runtime call sites provide naturally aligned 8-byte
///          `double` storage; this helper centralizes the representation cast and keeps the
///          load/store helpers themselves free of repeated type-punning.
static inline volatile long long *rt_atomic_f64_bits_ptr(volatile double *ptr) {
    return (volatile long long *)(volatile void *)ptr;
}

/// @brief Atomically load a double by copying its IEEE-754 representation on MSVC platforms.
/// @param ptr Naturally aligned double storage.
/// @param out Receives the loaded value.
/// @param order GCC-style memory-order constant accepted for source compatibility.
static inline void rt_atomic_load_f64(const volatile double *ptr, double *out, int order) {
    (void)order;
    long long bits =
        _InterlockedCompareExchange64(rt_atomic_f64_bits_ptr((volatile double *)ptr), 0, 0);
    memcpy(out, &bits, sizeof(*out));
}

/// @brief Atomically store a double by copying its IEEE-754 representation on MSVC platforms.
/// @param ptr Naturally aligned double storage.
/// @param value Source value to publish.
/// @param order GCC-style memory-order constant accepted for source compatibility.
static inline void rt_atomic_store_f64(volatile double *ptr, const double *value, int order) {
    (void)order;
    long long bits;
    memcpy(&bits, value, sizeof(bits));
    _InterlockedExchange64(rt_atomic_f64_bits_ptr(ptr), bits);
}

// Map GCC-style atomic builtins to our functions
#define __atomic_load_n(ptr, order)                                                                \
    _Generic((ptr),                                                                                \
        volatile int8_t *: rt_atomic_load_i8,                                                      \
        const volatile int8_t *: rt_atomic_load_i8,                                                \
        int8_t *: rt_atomic_load_i8,                                                               \
        const int8_t *: rt_atomic_load_i8,                                                         \
        volatile int *: rt_atomic_load_i32,                                                        \
        const volatile int *: rt_atomic_load_i32,                                                  \
        int *: rt_atomic_load_i32,                                                                 \
        const int *: rt_atomic_load_i32,                                                           \
        volatile int64_t *: rt_atomic_load_i64,                                                    \
        const volatile int64_t *: rt_atomic_load_i64,                                              \
        int64_t *: rt_atomic_load_i64,                                                             \
        const int64_t *: rt_atomic_load_i64,                                                       \
        volatile size_t *: rt_atomic_load_size,                                                    \
        const volatile size_t *: rt_atomic_load_size,                                              \
        size_t *: rt_atomic_load_size,                                                             \
        const size_t *: rt_atomic_load_size)((ptr), (order))

#define __atomic_store_n(ptr, val, order)                                                          \
    _Generic((ptr),                                                                                \
        volatile int8_t *: rt_atomic_store_i8,                                                     \
        int8_t *: rt_atomic_store_i8,                                                              \
        volatile int *: rt_atomic_store_i32,                                                       \
        int *: rt_atomic_store_i32,                                                                \
        volatile int64_t *: rt_atomic_store_i64,                                                   \
        int64_t *: rt_atomic_store_i64,                                                            \
        volatile size_t *: rt_atomic_store_size,                                                   \
        size_t *: rt_atomic_store_size)((ptr), (val), (order))

#define __atomic_load(ptr, ret, order)                                                             \
    _Generic((ptr),                                                                                \
        volatile double *: rt_atomic_load_f64,                                                     \
        const volatile double *: rt_atomic_load_f64,                                               \
        double *: rt_atomic_load_f64,                                                              \
        const double *: rt_atomic_load_f64)((ptr), (ret), (order))

#define __atomic_store(ptr, val, order)                                                            \
    _Generic((ptr), volatile double *: rt_atomic_store_f64, double *: rt_atomic_store_f64)(        \
        (ptr), (val), (order))

#define __atomic_exchange_n(ptr, val, order)                                                       \
    _Generic((ptr),                                                                                \
        volatile int *: rt_atomic_exchange_i32,                                                    \
        int *: rt_atomic_exchange_i32,                                                             \
        void *volatile *: rt_atomic_exchange_ptr,                                                  \
        void **: rt_atomic_exchange_ptr)((ptr), (val), (order))

#define __atomic_compare_exchange_n(ptr, expected, desired, weak, success, fail)                   \
    _Generic((ptr),                                                                                \
        volatile int *: rt_atomic_compare_exchange_i32,                                            \
        int *: rt_atomic_compare_exchange_i32,                                                     \
        volatile int64_t *: rt_atomic_compare_exchange_i64,                                        \
        int64_t *: rt_atomic_compare_exchange_i64,                                                 \
        volatile size_t *: rt_atomic_compare_exchange_size,                                        \
        size_t *: rt_atomic_compare_exchange_size,                                                 \
        void *volatile *: rt_atomic_compare_exchange_ptr,                                          \
        void **: rt_atomic_compare_exchange_ptr)((ptr), (expected), (desired), (success), (fail))

#define __atomic_fetch_add(ptr, val, order)                                                        \
    _Generic((ptr),                                                                                \
        volatile int *: rt_atomic_fetch_add_i32,                                                   \
        int *: rt_atomic_fetch_add_i32,                                                            \
        volatile int64_t *: rt_atomic_fetch_add_i64,                                               \
        int64_t *: rt_atomic_fetch_add_i64,                                                        \
        volatile size_t *: rt_atomic_fetch_add_size,                                               \
        size_t *: rt_atomic_fetch_add_size)((ptr), (val), (order))

#define __atomic_fetch_sub(ptr, val, order)                                                        \
    _Generic((ptr),                                                                                \
        volatile int *: rt_atomic_fetch_sub_i32,                                                   \
        int *: rt_atomic_fetch_sub_i32,                                                            \
        volatile int64_t *: rt_atomic_fetch_sub_i64,                                               \
        int64_t *: rt_atomic_fetch_sub_i64,                                                        \
        volatile size_t *: rt_atomic_fetch_sub_size,                                               \
        size_t *: rt_atomic_fetch_sub_size)((ptr), (val), (order))

/// @brief Atomically set a spinlock flag on MSVC platforms.
/// @param ptr Storage containing 0 when unlocked and nonzero when locked.
/// @param order GCC-style memory-order constant accepted for source compatibility.
/// @return Nonzero when the flag was already set, zero when this call acquired it.
static inline int rt_atomic_test_and_set(volatile int *ptr, int order) {
    (void)order;
    return _InterlockedExchange((volatile long *)ptr, 1) != 0;
}

/// @brief Atomically clear a spinlock flag on MSVC platforms.
/// @param ptr Storage to set back to 0.
/// @param order GCC-style memory-order constant accepted for source compatibility.
static inline void rt_atomic_clear(volatile int *ptr, int order) {
    (void)order;
#if defined(_M_ARM64)
    __dmb(_ARM64_BARRIER_ISH);
#else
    _ReadWriteBarrier();
#endif
    *ptr = 0;
#if defined(_M_ARM64)
    __dmb(_ARM64_BARRIER_ISH);
#else
    _ReadWriteBarrier();
#endif
}

#define __atomic_test_and_set(ptr, order) rt_atomic_test_and_set((volatile int *)(ptr), (order))
#define __atomic_clear(ptr, order) rt_atomic_clear((volatile int *)(ptr), (order))

/// @brief Emit a full thread fence on MSVC platforms.
/// @param order GCC-style memory-order constant accepted for source compatibility.
static inline void rt_atomic_thread_fence(int order) {
    (void)order;
    // Use _mm_mfence for full memory barrier on x86/x64
    // This is more portable than MemoryBarrier() which requires windows.h
#if defined(_M_X64) || defined(_M_IX86)
    _mm_mfence();
#elif defined(_M_ARM64)
    __dmb(_ARM64_BARRIER_SY);
#else
    _ReadWriteBarrier();
#endif
}

#define __atomic_thread_fence(order) rt_atomic_thread_fence(order)

#endif // RT_COMPILER_MSVC

#if !RT_COMPILER_MSVC
/// @brief Atomically load an 8-bit signed integer on GCC/Clang platforms.
/// @param ptr Naturally aligned storage to load from.
/// @param order GCC-style memory-order constant.
/// @return The value observed in @p ptr.
static inline int8_t rt_atomic_load_i8(const volatile int8_t *ptr, int order) {
    return __atomic_load_n(ptr, order);
}

/// @brief Atomically store an 8-bit signed integer on GCC/Clang platforms.
/// @param ptr Naturally aligned storage to update.
/// @param value Value to publish.
/// @param order GCC-style memory-order constant.
static inline void rt_atomic_store_i8(volatile int8_t *ptr, int8_t value, int order) {
    __atomic_store_n(ptr, value, order);
}

/// @brief Atomically load a 32-bit signed integer on GCC/Clang platforms.
/// @param ptr Naturally aligned storage to load from.
/// @param order GCC-style memory-order constant.
/// @return The value observed in @p ptr.
static inline int rt_atomic_load_i32(const volatile int *ptr, int order) {
    return __atomic_load_n(ptr, order);
}

/// @brief Atomically store a 32-bit signed integer on GCC/Clang platforms.
/// @param ptr Naturally aligned storage to update.
/// @param value Value to publish.
/// @param order GCC-style memory-order constant.
static inline void rt_atomic_store_i32(volatile int *ptr, int value, int order) {
    __atomic_store_n(ptr, value, order);
}

/// @brief Atomically exchange a 32-bit signed integer on GCC/Clang platforms.
/// @param ptr Storage to update.
/// @param value Replacement value.
/// @param order GCC-style memory-order constant.
/// @return The previous value stored in @p ptr.
static inline int rt_atomic_exchange_i32(volatile int *ptr, int value, int order) {
    return __atomic_exchange_n(ptr, value, order);
}

/// @brief Atomically compare-and-swap a 32-bit signed integer on GCC/Clang platforms.
/// @param ptr Storage to update when it equals @p expected.
/// @param expected In/out expected value; receives the observed value on failure.
/// @param desired Value to store on success.
/// @param success_order Memory order for a successful exchange.
/// @param fail_order Memory order for a failed exchange.
/// @return 1 on success, 0 when @p expected did not match.
static inline int rt_atomic_compare_exchange_i32(
    volatile int *ptr, int *expected, int desired, int success_order, int fail_order) {
    return __atomic_compare_exchange_n(ptr, expected, desired, 0, success_order, fail_order);
}

/// @brief Atomically add to a 32-bit signed integer on GCC/Clang platforms.
/// @param ptr Storage to update.
/// @param value Increment to apply.
/// @param order GCC-style memory-order constant.
/// @return The value stored in @p ptr before the addition.
static inline int rt_atomic_fetch_add_i32(volatile int *ptr, int value, int order) {
    return __atomic_fetch_add(ptr, value, order);
}

/// @brief Atomically subtract from a 32-bit signed integer on GCC/Clang platforms.
/// @param ptr Storage to update.
/// @param value Decrement to apply.
/// @param order GCC-style memory-order constant.
/// @return The value stored in @p ptr before the subtraction.
static inline int rt_atomic_fetch_sub_i32(volatile int *ptr, int value, int order) {
    return __atomic_fetch_sub(ptr, value, order);
}

/// @brief Atomically load a 64-bit signed integer on GCC/Clang platforms.
/// @param ptr Naturally aligned storage to load from.
/// @param order GCC-style memory-order constant.
/// @return The value observed in @p ptr.
static inline int64_t rt_atomic_load_i64(const volatile int64_t *ptr, int order) {
    return __atomic_load_n(ptr, order);
}

/// @brief Atomically add to a 64-bit signed integer on GCC/Clang platforms.
/// @param ptr Storage to update.
/// @param value Increment to apply.
/// @param order GCC-style memory-order constant.
/// @return The value stored in @p ptr before the addition.
static inline int64_t rt_atomic_fetch_add_i64(volatile int64_t *ptr, int64_t value, int order) {
    return __atomic_fetch_add(ptr, value, order);
}

/// @brief Atomically load a size_t value on GCC/Clang platforms.
/// @param ptr Naturally aligned storage to load from.
/// @param order GCC-style memory-order constant.
/// @return The value observed in @p ptr.
static inline size_t rt_atomic_load_size(const volatile size_t *ptr, int order) {
    return __atomic_load_n(ptr, order);
}

/// @brief Atomically store a size_t value on GCC/Clang platforms.
/// @param ptr Naturally aligned storage to update.
/// @param value Value to publish.
/// @param order GCC-style memory-order constant.
static inline void rt_atomic_store_size(volatile size_t *ptr, size_t value, int order) {
    __atomic_store_n(ptr, value, order);
}

/// @brief Atomically compare-and-swap a size_t value on GCC/Clang platforms.
/// @param ptr Storage to update when it equals @p expected.
/// @param expected In/out expected value; receives the observed value on failure.
/// @param desired Value to store on success.
/// @param success_order Memory order for a successful exchange.
/// @param fail_order Memory order for a failed exchange.
/// @return 1 on success, 0 when @p expected did not match.
static inline int rt_atomic_compare_exchange_size(
    volatile size_t *ptr, size_t *expected, size_t desired, int success_order, int fail_order) {
    return __atomic_compare_exchange_n(ptr, expected, desired, 0, success_order, fail_order);
}

/// @brief Atomically add to a size_t counter on GCC/Clang platforms.
/// @param ptr Naturally aligned storage to update.
/// @param value Increment to apply with modulo-size_t arithmetic.
/// @param order GCC-style memory-order constant.
/// @return The value stored in @p ptr before the addition.
static inline size_t rt_atomic_fetch_add_size(volatile size_t *ptr, size_t value, int order) {
    return __atomic_fetch_add(ptr, value, order);
}

/// @brief Atomically subtract from a size_t counter on GCC/Clang platforms.
/// @param ptr Naturally aligned storage to update.
/// @param value Decrement to apply with modulo-size_t arithmetic.
/// @param order GCC-style memory-order constant.
/// @return The value stored in @p ptr before the subtraction.
static inline size_t rt_atomic_fetch_sub_size(volatile size_t *ptr, size_t value, int order) {
    return __atomic_fetch_sub(ptr, value, order);
}

/// @brief Atomically load a pointer on GCC/Clang platforms.
/// @param ptr Address of pointer storage to load from.
/// @param order GCC-style memory-order constant.
/// @return The pointer value observed in @p ptr.
static inline void *rt_atomic_load_ptr(void *const volatile *ptr, int order) {
    return __atomic_load_n(ptr, order);
}

/// @brief Atomically exchange a pointer on GCC/Clang platforms.
/// @param ptr Address of pointer storage to update.
/// @param value Replacement pointer value.
/// @param order GCC-style memory-order constant.
/// @return The previous pointer value.
static inline void *rt_atomic_exchange_ptr(void *volatile *ptr, void *value, int order) {
    return __atomic_exchange_n(ptr, value, order);
}

/// @brief Atomically compare-and-swap a pointer on GCC/Clang platforms.
/// @param ptr Address of pointer storage to update when it equals @p expected.
/// @param expected In/out expected pointer; receives the observed value on failure.
/// @param desired Pointer value to store on success.
/// @param success_order Memory order for a successful exchange.
/// @param fail_order Memory order for a failed exchange.
/// @return 1 on success, 0 when @p expected did not match.
static inline int rt_atomic_compare_exchange_ptr(
    void *volatile *ptr, void **expected, void *desired, int success_order, int fail_order) {
    return __atomic_compare_exchange_n(ptr, expected, desired, 0, success_order, fail_order);
}

/// @brief Atomically load a double by copying its IEEE-754 representation.
/// @param ptr Naturally aligned double storage.
/// @param out Receives the loaded value.
/// @param order GCC-style memory-order constant.
static inline void rt_atomic_load_f64(const volatile double *ptr, double *out, int order) {
    __atomic_load(ptr, out, order);
}

/// @brief Atomically store a double by copying its IEEE-754 representation.
/// @param ptr Naturally aligned double storage.
/// @param value Source value to publish.
/// @param order GCC-style memory-order constant.
static inline void rt_atomic_store_f64(volatile double *ptr, const double *value, int order) {
    double tmp = *value;
    __atomic_store(ptr, &tmp, order);
}

/// @brief Atomically set a spinlock flag on GCC/Clang platforms.
/// @param ptr Storage containing 0 when unlocked and nonzero when locked.
/// @param order GCC-style memory-order constant.
/// @return Nonzero when the flag was already set, zero when this call acquired it.
static inline int rt_atomic_test_and_set(volatile int *ptr, int order) {
    return __atomic_exchange_n(ptr, 1, order) != 0;
}

/// @brief Atomically clear a spinlock flag on GCC/Clang platforms.
/// @param ptr Storage to set back to 0.
/// @param order GCC-style memory-order constant.
static inline void rt_atomic_clear(volatile int *ptr, int order) {
    __atomic_store_n(ptr, 0, order);
}
#endif

/// @brief Atomically load an unsigned 64-bit value.
/// @param ptr Naturally aligned unsigned 64-bit storage.
/// @param order GCC-style memory-order constant.
/// @return The value observed in @p ptr.
static inline uint64_t rt_atomic_load_u64(const volatile uint64_t *ptr, int order) {
    (void)order;
#if RT_COMPILER_MSVC
    return (uint64_t)_InterlockedCompareExchange64((volatile long long *)ptr, 0, 0);
#else
    return __atomic_load_n(ptr, order);
#endif
}

/// @brief Atomically store an unsigned 64-bit value.
/// @param ptr Naturally aligned unsigned 64-bit storage.
/// @param value Value to publish.
/// @param order GCC-style memory-order constant.
static inline void rt_atomic_store_u64(volatile uint64_t *ptr, uint64_t value, int order) {
    (void)order;
#if RT_COMPILER_MSVC
    _InterlockedExchange64((volatile long long *)ptr, (long long)value);
#else
    __atomic_store_n(ptr, value, order);
#endif
}

/// @brief Atomically compare and exchange an unsigned 64-bit value.
/// @details The caller supplies the expected value by address. On success,
///          @p desired replaces it and the function returns nonzero. On
///          failure, @p expected receives the value that was actually
///          observed. This explicit unsigned helper avoids the duplicate
///          `_Generic` associations that `uint64_t` and `size_t` can create on
///          LLP64 Windows targets.
/// @param ptr Naturally aligned unsigned 64-bit storage to update.
/// @param expected In/out expected value; updated with the observed value on
///        failure.
/// @param desired Replacement value written when the comparison succeeds.
/// @param success_order GCC-style memory order for a successful exchange.
/// @param fail_order GCC-style memory order for a failed comparison.
/// @return Nonzero when @p desired was stored; zero otherwise.
static inline int rt_atomic_compare_exchange_u64(volatile uint64_t *ptr,
                                                 uint64_t *expected,
                                                 uint64_t desired,
                                                 int success_order,
                                                 int fail_order) {
    (void)success_order;
    (void)fail_order;
#if RT_COMPILER_MSVC
    uint64_t old = (uint64_t)_InterlockedCompareExchange64(
        (volatile long long *)ptr, (long long)desired, (long long)*expected);
    if (old == *expected)
        return 1;
    *expected = old;
    return 0;
#else
    return __atomic_compare_exchange_n(ptr, expected, desired, 0, success_order, fail_order);
#endif
}

/// @brief Atomically add @p value to an unsigned 64-bit counter and return the previous value.
/// @details This helper exists for runtime cache-identity counters that are deliberately modeled as
///          wrapping `uint64_t` sequences. GCC/Clang accept `__atomic_fetch_add` on `uint64_t`
///          directly, but the MSVC compatibility layer maps GCC-style builtins with `_Generic` and
///          cannot safely add a `uint64_t *` arm on LLP64 targets where `uint64_t` and `size_t` may
///          be compatible types. Centralizing the operation here avoids duplicate `_Generic`
///          associations while still using native interlocked 64-bit instructions on MSVC.
/// @param ptr Naturally aligned unsigned 64-bit counter storage.
/// @param value Increment to apply; arithmetic wraps modulo 2^64, matching unsigned C semantics.
/// @param order GCC-style memory-order constant. Current call sites use relaxed identity
/// allocation;
///              stronger values are accepted and mapped to the platform atomic primitive.
/// @return The value stored in @p ptr before the addition.
static inline uint64_t rt_atomic_fetch_add_u64(volatile uint64_t *ptr, uint64_t value, int order) {
    (void)order;
#if RT_COMPILER_MSVC
    return (uint64_t)_InterlockedExchangeAdd64((volatile long long *)ptr, (long long)value);
#else
    return __atomic_fetch_add(ptr, value, order);
#endif
}

//===----------------------------------------------------------------------===//
// Windows POSIX Compatibility
//===----------------------------------------------------------------------===//

#if RT_PLATFORM_WINDOWS

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

// Avoid conflicts with Windows headers
#ifdef Type
#undef Type
#endif

#include <direct.h>
#include <io.h>
#include <process.h>
#include <windows.h>

// POSIX-like type definitions
#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
// LOW-1: Use 64-bit ssize_t on both 32-bit and 64-bit Windows.
// The legacy 32-bit definition (int) limited I/O return values to ±2 GB and
// caused sign-extension hazards when assigning to 64-bit targets.  Using
// long long uniformly matches POSIX semantics regardless of pointer width.
typedef long long ssize_t;
#endif

// POSIX function mappings
#define access _access
#define getcwd _getcwd
#define chdir _chdir
#define mkdir(path, mode) _mkdir(path)
#define rmdir _rmdir
#define unlink _unlink
#define fileno _fileno
#define isatty _isatty
#define strdup _strdup
#define getpid _getpid

// Access mode flags
#ifndef F_OK
#define F_OK 0
#endif
#ifndef R_OK
#define R_OK 4
#endif
#ifndef W_OK
#define W_OK 2
#endif
#ifndef X_OK
#define X_OK 1 // Note: Windows doesn't really have execute permission
#endif

// String functions
#define strcasecmp _stricmp
#define strncasecmp _strnicmp

// File I/O — use 64-bit variant for >2GB file support
#define lseek _lseeki64

// POSIX file type macros (Windows doesn't have these)
#ifndef S_ISREG
#define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
#endif
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#endif
#ifndef S_ISBLK
#define S_ISBLK(m) (0) // Windows doesn't have block devices
#endif
#ifndef S_ISCHR
#define S_ISCHR(m) (((m) & _S_IFMT) == _S_IFCHR)
#endif
#ifndef S_ISFIFO
#define S_ISFIFO(m) (((m) & _S_IFMT) == _S_IFIFO)
#endif

// Path separator
#define RT_PATH_SEPARATOR '\\'
#define RT_PATH_SEPARATOR_STR "\\"

/// @brief Return milliseconds since Unix epoch (Windows implementation).
/// @details Converts Windows FILETIME (100-ns ticks since 1601) to Unix epoch ms.
static inline int64_t rt_windows_time_ms(void) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    // Convert 100-nanosecond intervals since 1601 to milliseconds since Unix epoch
    uint64_t time = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    // Subtract difference between 1601 and 1970 (in 100-ns intervals)
    time -= 116444736000000000ULL;
    return (int64_t)(time / 10000);
}

/// @brief Return microseconds since Unix epoch (Windows implementation).
static inline int64_t rt_windows_time_us(void) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t time = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    time -= 116444736000000000ULL;
    return (int64_t)(time / 10);
}

/// @brief Sleep for @p ms milliseconds (Windows implementation).
static inline void rt_windows_sleep_ms(int64_t ms) {
    if (ms > 0)
        Sleep((DWORD)ms);
}

#else // POSIX systems (macOS, Linux)

#include <sys/time.h>
#include <unistd.h>

#define RT_PATH_SEPARATOR '/'
#define RT_PATH_SEPARATOR_STR "/"

#endif // RT_PLATFORM_WINDOWS

//===----------------------------------------------------------------------===//
// Thread-Safe Time Functions
//===----------------------------------------------------------------------===//

#include <string.h>
#include <time.h>

/// @brief Thread-safe version of localtime().
/// @param timer Pointer to time_t value to convert.
/// @param result Pointer to struct tm to store the result.
/// @return Pointer to result on success, NULL on failure.
static inline struct tm *rt_localtime_r(const time_t *timer, struct tm *result) {
    if (!timer || !result)
        return NULL;
    memset(result, 0, sizeof(*result));
#if RT_PLATFORM_WINDOWS
    // Windows localtime_s has reversed parameter order and returns errno_t
    if (localtime_s(result, timer) == 0)
        return result;
    return NULL;
#else
    return localtime_r(timer, result);
#endif
}

/// @brief Thread-safe version of gmtime().
/// @param timer Pointer to time_t value to convert.
/// @param result Pointer to struct tm to store the result.
/// @return Pointer to result on success, NULL on failure.
static inline struct tm *rt_gmtime_r(const time_t *timer, struct tm *result) {
#if RT_PLATFORM_WINDOWS
    // Windows gmtime_s has reversed parameter order and returns errno_t
    if (gmtime_s(result, timer) == 0)
        return result;
    return NULL;
#else
    return gmtime_r(timer, result);
#endif
}

/// @brief Thread-safe version of strtok().
/// Maps to strtok_r on POSIX and strtok_s on Windows (same signature).
/// @param str   String to tokenize, or NULL to continue from last call.
/// @param delim Delimiter characters.
/// @param saveptr Caller-provided pointer used to store tokenizer state.
/// @return Pointer to next token, or NULL when no more tokens remain.
static inline char *rt_strtok_r(char *str, const char *delim, char **saveptr) {
#if RT_PLATFORM_WINDOWS
    return strtok_s(str, delim, saveptr);
#else
    return strtok_r(str, delim, saveptr);
#endif
}

//===----------------------------------------------------------------------===//
// Main-Thread Assertion
//===----------------------------------------------------------------------===//

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Record the current thread as the "main" thread.
///
/// May be called during runtime initialization or by an embedder that must
/// override constructor-time capture. Concurrent probes are serialized with
/// the override, although callers must still coordinate changes with any
/// thread-affine GUI or input subsystem.
void rt_set_main_thread(void);

/// @brief Check whether the calling thread is the main thread.
/// @return Non-zero if called from the main thread, zero otherwise.
int8_t rt_is_main_thread(void);

/// @brief Internal assertion helper — do not call directly.
/// @see RT_ASSERT_MAIN_THREAD
void rt_assert_main_thread_(const char *file, int line);

#ifdef __cplusplus
}
#endif

/// @def RT_ASSERT_MAIN_THREAD()
/// Traps with a diagnostic if called from a non-main thread.
#if defined(RT_NO_MAIN_THREAD_ASSERT)
#define RT_ASSERT_MAIN_THREAD() ((void)0)
#else
#define RT_ASSERT_MAIN_THREAD() rt_assert_main_thread_(__FILE__, __LINE__)
#endif

//===----------------------------------------------------------------------===//
// Format Attribute
//===----------------------------------------------------------------------===//

#if RT_COMPILER_GCC_LIKE
#define RT_PRINTF_FORMAT(fmt_idx, first_arg) __attribute__((format(printf, fmt_idx, first_arg)))
#else
#define RT_PRINTF_FORMAT(fmt_idx, first_arg)
#endif
