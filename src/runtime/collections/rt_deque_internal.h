//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/collections/rt_deque_internal.h
// Purpose: Shares the private Deque storage layout among runtime collection
//          implementation units.
// Key invariants: front identifies logical index zero in a cap-sized circular
//                 buffer and 0 <= len <= cap.
// Ownership/Lifetime: This header defines no ownership; Deque instances retain
//                     every live element and own their native data allocation.
// Links: src/runtime/collections/rt_deque.c
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Private Deque layout and non-trapping identity predicate.

#pragma once

#include "rt_collection_ids.h"
#include "rt_object.h"

#include <stdint.h>

/// @brief Internal retained-element circular Deque payload.
typedef struct rt_deque_impl {
    void **data;   ///< Circular element storage.
    int64_t cap;   ///< Number of allocated slots.
    int64_t len;   ///< Number of live elements.
    int64_t front; ///< Physical index of logical element zero.
} rt_deque_impl;

/// @brief Compatibility alias used by the primary implementation unit.
typedef rt_deque_impl Deque;

/// @brief Test whether @p obj is a live, completely sized Deque payload.
/// @param obj Candidate runtime payload.
/// @return One only for a valid Deque object with the full private layout.
static inline int8_t rt_deque_internal_is_valid(void *obj) {
    return obj && rt_obj_is_instance(obj, RT_DEQUE_CLASS_ID, sizeof(rt_deque_impl)) ? 1 : 0;
}
