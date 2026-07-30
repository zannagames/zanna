//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/collections/rt_list_internal.h
// Purpose: Share List's private managed-array layout with collection adapters
//          that require sized identity checks or borrowed indexed access.
// Key invariants:
//   - This header is internal to the runtime and is not a public C ABI surface.
//   - The arr field is either null for an empty List or a managed RT_ELEM_OBJ array.
// Ownership/Lifetime:
//   - List owns one reference to arr; arr owns every populated element slot.
// Links: src/runtime/collections/rt_list.c,
//        src/runtime/collections/rt_iter.c
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Private List storage shared by tightly coupled runtime adapters.

#pragma once

#include "rt_collection_ids.h"
#include "rt_object.h"

#include <stdint.h>

/// @brief Internal List payload backed by a managed object array.
typedef struct rt_list_impl {
    void **vptr; ///< Vtable placeholder retained for object compatibility.
    void **arr;  ///< Managed RT_ELEM_OBJ array, or null while empty.
} rt_list_impl;

/// @brief Test whether @p obj is a live, completely sized List payload.
/// @param obj Candidate runtime payload.
/// @return One only for a valid List object with the full private layout.
static inline int8_t rt_list_internal_is_valid(void *obj) {
    return obj && rt_obj_is_instance(obj, RT_LIST_CLASS_ID, sizeof(rt_list_impl)) ? 1 : 0;
}
