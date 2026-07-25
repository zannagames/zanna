//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/collections/rt_seq_internal.h
// Purpose: Shared internal definitions for the Seq (dynamic array) runtime
//   implementation. Exposes the rt_seq_impl struct so that the core and
//   operations modules can both access sequence internals directly.
//
// Key invariants:
//   - This header is INTERNAL to the runtime — never include from public APIs.
//   - Only rt_seq.c and rt_seq_ops.c should include this header.
//   - The public API header (rt_seq.h) remains the sole interface for callers.
//
// Ownership/Lifetime:
//   - The rt_seq_impl struct is GC-managed via rt_obj_new_i64.
//   - The items array is malloc-managed and freed by the GC finalizer in rt_seq.c.
//
// Links: src/runtime/collections/rt_seq.h (public API),
//        src/runtime/collections/rt_seq.c (core operations),
//        src/runtime/collections/rt_seq_ops.c (sorting and functional operations)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Shares the private Seq storage layout across its implementation units.
/// @details This is not a public ABI header. Core mutation and higher-order
///          operations require direct access to the same length, capacity,
///          pointer allocation, and element-ownership flag; all external
///          callers must continue to use `rt_seq.h`.

#pragma once

#include "rt_seq.h"

#include <stdint.h>

/// @brief Internal sequence (dynamic array) implementation structure.
///
/// The Seq is implemented as a growable array that automatically expands when
/// its capacity is exceeded. This provides O(1) amortized append and O(1)
/// random access, making it the most versatile collection type.
///
/// **Memory layout:**
/// ```
/// Seq object (GC-managed):
///   +-----+-----+-------+
///   | len | cap | items |
///   |  5  | 16  | ----->|
///   +-----+-----+---|---+
///                   |
///                   v
/// items array (malloc'd):
///   +---+---+---+---+---+---+---+...+----+
///   | A | B | C | D | E | ? | ? |   | ?  |
///   +---+---+---+---+---+---+---+...+----+
///   [0]  [1] [2] [3] [4]          [cap-1]
///                     ^
///                     | len-1 = last valid index
/// ```
///
/// **Growth strategy:**
/// - Initial capacity: 16 elements
/// - When full, capacity doubles (16 → 32 → 64 → 128 → ...)
/// - This gives O(1) amortized time for Push operations
///
/// **Element ownership:**
/// By default (owns_elements=0), the Seq stores raw pointers and does NOT own
/// the elements. When owns_elements=1, the Seq retains elements on push and
/// releases them on finalize/clear/set-replace, enabling automatic lifetime
/// management via reference counting.
typedef struct rt_seq_impl {
    int64_t len;          ///< Number of elements currently in the sequence
    int64_t cap;          ///< Current capacity (allocated slots)
    void **items;         ///< Array of element pointers
    int8_t owns_elements; ///< 1 = retain on push, release on finalize/clear
} rt_seq_impl;
