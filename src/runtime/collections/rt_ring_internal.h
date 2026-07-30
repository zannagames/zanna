//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/collections/rt_ring_internal.h
// Purpose: Share Ring's private circular-buffer layout with collection
//          adapters that require sized identity checks and borrowed indexing.
// Key invariants:
//   - This header is internal to the runtime and is not a public C ABI surface.
//   - count never exceeds capacity; head identifies logical index zero.
// Ownership/Lifetime:
//   - items is native storage owned by Ring; live slots are retained only when
//     owns_elements is nonzero.
// Links: src/runtime/collections/rt_ring.c,
//        src/runtime/collections/rt_iter.c
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Private Ring storage shared by tightly coupled runtime adapters.

#pragma once

#include "rt_collection_ids.h"
#include "rt_object.h"

#include <stddef.h>
#include <stdint.h>

/// @brief Internal fixed-capacity circular-buffer payload.
typedef struct rt_ring_impl {
    void **vptr;          ///< Vtable placeholder.
    void **items;         ///< Native array of element pointers.
    size_t capacity;      ///< Maximum number of live elements.
    size_t head;          ///< Physical slot for logical index zero.
    size_t count;         ///< Number of live elements.
    int8_t owns_elements; ///< Whether live slots own managed references.
} rt_ring_impl;

/// @brief Test whether @p obj is a live, completely sized Ring payload.
/// @param obj Candidate runtime payload.
/// @return One only for a valid Ring object with the full private layout.
static inline int8_t rt_ring_internal_is_valid(void *obj) {
    return obj && rt_obj_is_instance(obj, RT_RING_CLASS_ID, sizeof(rt_ring_impl)) ? 1 : 0;
}
