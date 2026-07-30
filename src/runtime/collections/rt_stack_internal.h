//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/collections/rt_stack_internal.h
// Purpose: Shares the private Stack storage layout among runtime collection
//          implementation units.
// Key invariants: Live elements occupy items[0..len), cap covers len, and the
//                 ownership flag controls retained graph edges.
// Ownership/Lifetime: This header defines no ownership; Stack instances own
//                     their native items allocation.
// Links: src/runtime/collections/rt_stack.c
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Private Stack layout and non-trapping identity predicate.

#pragma once

#include "rt_collection_ids.h"
#include "rt_object.h"

#include <stdint.h>

/// @brief Internal contiguous Stack payload.
typedef struct rt_stack_impl {
    int64_t len;          ///< Number of live elements.
    int64_t cap;          ///< Number of allocated pointer slots.
    void **items;         ///< Bottom-to-top element storage.
    int8_t owns_elements; ///< Whether live slots own managed references.
} rt_stack_impl;

/// @brief Test whether @p obj is a live, completely sized Stack payload.
/// @param obj Candidate runtime payload.
/// @return One only for a valid Stack object with the full private layout.
static inline int8_t rt_stack_internal_is_valid(void *obj) {
    return obj && rt_obj_is_instance(obj, RT_STACK_CLASS_ID, sizeof(rt_stack_impl)) ? 1 : 0;
}
