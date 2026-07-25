//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/rt_g3d_ref_slots.h
// Purpose: Shared retained-reference slot helpers for Graphics3D runtime
//   implementation objects.
//
// Key invariants:
//   - Helpers are NULL-safe for both the slot pointer and the slot contents.
//   - Every successful release clears the caller-owned slot to prevent double
//     drops on defensive repair/finalizer paths.
//   - Invalid raw pointers are cleared but not dereferenced or released; live
//     runtime heap/string handles are released through the normal object API.
//
// Ownership/Lifetime:
//   - Callers use these helpers only for slots that own a retained runtime
//     reference. The helpers consume that retained reference and leave NULL.
//
// Links: src/runtime/oop/rt_object.h, src/runtime/graphics/3d/rt_graphics3d_ids.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Defines null-safe retained-reference slot helpers for Graphics3D private state.
/// @details The inline helpers consume owned runtime references, clear borrowed wrong-class
///   corruption without dereferencing it, and report repairs so dependent caches can be
///   invalidated without risking double release.

#pragma once

#include "rt_graphics3d_ids.h"
#include "rt_object.h"

#include <stdint.h>

/// @brief Release one retained runtime reference slot and clear it.
/// @details This helper is intended for Graphics3D private fields that own a
///          retained object/string/array reference. It delegates to
///          @ref rt_obj_release_check0 so managed strings and heap payloads
///          follow the same lifetime rules as the rest of the runtime. Invalid
///          raw pointers are not released by the object API, but the slot is
///          still cleared so later repair/finalizer passes cannot dereference
///          stale private state.
/// @param slot_addr Address of the retained-reference slot to consume; may be NULL. The pointed-to
///          slot may have any runtime pointer type, not only `void *`.
/// @post When @p slot_addr is non-NULL, the referenced slot is NULL.
static inline void rt_g3d_ref_slot_release(void *slot_addr) {
    void **slot = (void **)slot_addr;
    void *ref;
    if (!slot || !*slot)
        return;
    ref = *slot;
    *slot = NULL;
    if (rt_obj_release_check0(ref))
        rt_obj_free(ref);
}

/// @brief Clear a stale private slot without releasing the referenced handle.
/// @details Wrong-class private fields are treated as memory-corruption sentinels rather than
///          proof of ownership: tests and defensive readers may place borrowed handles in private
///          slots to verify repair paths. This helper drops only the slot reference, leaving the
///          object refcount untouched so callers can safely clean up the borrowed handle later.
/// @param slot_addr Address of the pointer slot to clear; may be NULL. The pointed-to slot may
///          have any runtime pointer type.
/// @post When @p slot_addr is non-NULL, the referenced slot is NULL.
static inline void rt_g3d_ref_slot_clear_unowned(void *slot_addr) {
    void **slot = (void **)slot_addr;
    if (slot)
        *slot = NULL;
}

/// @brief Release a retained Graphics3D class slot, or clear wrong-class corruption.
/// @details Matching slots are owned retained references and are consumed through
///          @ref rt_g3d_ref_slot_release. Wrong-class slots are treated as borrowed corrupt
///          sentinels and cleared via @ref rt_g3d_ref_slot_clear_unowned so repair paths do not
///          release handles they did not retain.
/// @param slot_addr Address of the retained Graphics3D slot; may be NULL.
/// @param expected_class_id Runtime class id the slot is expected to contain.
/// @post When @p slot_addr is non-NULL, the referenced slot is NULL.
static inline void rt_g3d_ref_slot_release_class(void *slot_addr, int64_t expected_class_id) {
    void **slot = (void **)slot_addr;
    if (!slot || !*slot)
        return;
    if (!rt_g3d_has_class(*slot, expected_class_id)) {
        rt_g3d_ref_slot_clear_unowned(slot_addr);
        return;
    }
    rt_g3d_ref_slot_release(slot_addr);
}

/// @brief Repair a retained Graphics3D class slot when it no longer has the expected class.
/// @details Returns whether a repair occurred so callers can invalidate caches
///          that depended on the stale slot. Matching or empty slots are left
///          untouched; wrong-class slots are cleared without releasing.
/// @param slot_addr Address of the retained Graphics3D slot; may be NULL.
/// @param expected_class_id Runtime class id the slot is expected to contain.
/// @return 1 when the slot was non-NULL and got released/cleared; otherwise 0.
static inline int32_t rt_g3d_ref_slot_repair_class(void *slot_addr, int64_t expected_class_id) {
    void **slot = (void **)slot_addr;
    if (!slot || !*slot || rt_g3d_has_class(*slot, expected_class_id))
        return 0;
    rt_g3d_ref_slot_clear_unowned(slot_addr);
    return 1;
}

/// @brief Release and clear a retained slot when an external validity predicate failed.
/// @details Use this for retained slots whose valid payload may be a non-Graphics3D
///          runtime object, such as Pixels, or a small set of accepted classes.
///          Passing a non-zero @p supported leaves the slot untouched; passing
///          zero clears the slot without releasing because unsupported private state is treated as
///          borrowed corruption.
/// @param slot_addr Address of the retained slot; may be NULL.
/// @param supported Non-zero if the current slot contents are still valid.
/// @return 1 when the slot was released/cleared; otherwise 0.
static inline int32_t rt_g3d_ref_slot_release_if_unsupported(void *slot_addr, int supported) {
    void **slot = (void **)slot_addr;
    if (!slot || !*slot || supported)
        return 0;
    rt_g3d_ref_slot_clear_unowned(slot_addr);
    return 1;
}
