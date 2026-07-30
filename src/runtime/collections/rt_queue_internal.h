//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/collections/rt_queue_internal.h
// Purpose: Shares the private Queue storage layout among runtime collection
//          implementation units.
// Key invariants: head/tail index a cap-sized circular buffer, len is bounded
//                 by cap, and the ownership flag controls retained graph edges.
// Ownership/Lifetime: This header defines no ownership; Queue instances own
//                     their native items allocation.
// Links: src/runtime/collections/rt_queue.c
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Private Queue layout and non-trapping identity predicate.

#pragma once

#include "rt_collection_ids.h"
#include "rt_object.h"

#include <stdint.h>

/// @brief Internal circular Queue payload.
typedef struct rt_queue_impl {
    int64_t len;          ///< Number of live elements.
    int64_t cap;          ///< Number of circular-buffer slots.
    int64_t head;         ///< Physical index of the logical front.
    int64_t tail;         ///< Physical index for the next push.
    void **items;         ///< Circular element storage.
    int8_t owns_elements; ///< Whether live slots own managed references.
} rt_queue_impl;

/// @brief Test whether @p obj is a live, completely sized Queue payload.
/// @param obj Candidate runtime payload.
/// @return One only for a valid Queue object with the full private layout.
static inline int8_t rt_queue_internal_is_valid(void *obj) {
    return obj && rt_obj_is_instance(obj, RT_QUEUE_CLASS_ID, sizeof(rt_queue_impl)) ? 1 : 0;
}
