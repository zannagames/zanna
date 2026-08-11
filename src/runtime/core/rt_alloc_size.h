//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/core/rt_alloc_size.h
// Purpose: Portable overflow guards for `count * element_size` allocation math.
//
// Key invariants:
//   - A zero element size accepts any count because the product is zero.
//   - The guards compare 64-bit values against a runtime-computed bound, so they
//     stay meaningful on ILP32 targets without becoming tautologies that
//     compilers diagnose on LP64 targets.
//
// Ownership/Lifetime:
//   - Header-only helpers; own no memory and perform no allocation.
//
// Links: src/runtime/graphics/3d/rt_untrusted_count.h (byte-budget variant)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Provides overflow-safe element-count checks for allocation sizing.
/// @details Writing the guard as `(size_t)count > SIZE_MAX / sizeof(T)` is
/// tautologically false whenever `count` is narrower than `size_t`, which GCC
/// reports under `-Wtype-limits`. These helpers take the element size as a
/// runtime argument so the check keeps its meaning on 32-bit targets while
/// compiling warning-free everywhere.

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Check whether `count * elem_size` stays within @p byte_limit.
/// @details The bound is a runtime argument, so the division is not folded into
///          a compile-time constant that a narrower @p count can never reach.
/// @param count Element count from any unsigned or already-validated source.
/// @param elem_size Size of a single element in bytes; zero accepts any count.
/// @param byte_limit Largest byte total the caller can represent.
/// @return Nonzero when the product is within @p byte_limit.
static inline int rt_count_fits_bytes(uint64_t count, uint64_t elem_size, uint64_t byte_limit) {
    if (elem_size == 0u)
        return 1;
    return count <= byte_limit / elem_size;
}

/// @brief Check whether @p count elements of @p elem_size bytes fit in `size_t`.
/// @param count Element count from any unsigned or already-validated source.
/// @param elem_size Size of a single element in bytes; zero accepts any count.
/// @return Nonzero when `count * elem_size` is representable as `size_t`.
static inline int rt_alloc_count_ok(uint64_t count, size_t elem_size) {
    return rt_count_fits_bytes(count, (uint64_t)elem_size, (uint64_t)SIZE_MAX);
}

/// @brief Check whether `extra_bytes + count * elem_size` fits in `size_t`.
/// @details Used by parsers that prepend a fixed-size header to an element
///          array and must size both parts in one allocation.
/// @param count Element count from any unsigned or already-validated source.
/// @param elem_size Size of a single element in bytes; zero accepts any count.
/// @param extra_bytes Fixed byte count added to the element array; must not
///        exceed `SIZE_MAX`.
/// @return Nonzero when the total is representable as `size_t`.
static inline int rt_alloc_count_ok_with_extra(uint64_t count,
                                               size_t elem_size,
                                               size_t extra_bytes) {
    return rt_count_fits_bytes(count,
                               (uint64_t)elem_size,
                               (uint64_t)SIZE_MAX - (uint64_t)extra_bytes);
}

#ifdef __cplusplus
}
#endif
