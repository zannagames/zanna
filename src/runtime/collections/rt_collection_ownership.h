//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/collections/rt_collection_ownership.h
// Purpose: Provide status-returning managed-value retention for collection
//          mutations that must stop when an embedder trap hook returns.
// Key invariants:
//   - Managed strings and heap payloads are published only after a successful retain.
//   - Null and legacy unmanaged opaque pointers preserve the existing no-op retain semantics.
// Ownership/Lifetime:
//   - A successful managed-value call creates exactly one caller-owned reference.
//   - A failed call creates no reference and leaves the candidate value borrowed.
// Links: src/runtime/oop/rt_object.c, src/runtime/core/rt_heap.c,
//        src/runtime/core/rt_string_ops.c
//
//===----------------------------------------------------------------------===//

#pragma once

#include "rt_heap.h"
#include "rt_internal.h"
#include "rt_string.h"

#include <stdint.h>

/// @brief Retain a collection value and report whether ownership was acquired.
/// @details Mirrors the compatibility behavior of `rt_obj_retain_maybe`: null
///          and unmanaged opaque pointers are accepted without a retain.
///          Registered strings and heap payloads must acquire ownership before
///          the caller may publish or return them. Heap retention uses the
///          non-trapping primitive so a returning trap can be converted into an
///          explicit zero status.
/// @param value Candidate runtime string, heap payload, unmanaged opaque value,
///              or null.
/// @param failure Diagnostic raised when a heap payload cannot be retained.
/// @return One when publication is safe; zero after a returning retain trap.
static inline int8_t rt_collection_retain_checked(void *value, const char *failure) {
    if (!value)
        return 1;
    if (rt_string_is_handle(value))
        return rt_string_ref((rt_string)value) ? 1 : 0;
    if (!rt_heap_is_payload(value))
        return 1;

    int32_t status = rt_heap_try_retain_live(value);
    if (status == 1 || status == 2)
        return 1;
    rt_trap(failure ? failure : "Collection: value retain failed");
    return 0;
}
