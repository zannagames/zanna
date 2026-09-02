//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/oop/rt_class_layout.h
// Purpose: Per-class strong reference-slot maps (ADR 0315). The compiler
//          registers, at program entry, the byte offsets of every strong
//          object-typed field of every class it emitted; the cycle collector
//          traverses class instances through this map so object-to-object
//          cycles are reclaimable.
// Key invariants:
//   - Class ids are positive and dense (1..N); the table is indexed by id.
//   - A layout is written once by the entry prologue and read only afterwards
//     (the collector, the allocator); growth takes a spin lock, reads do not.
//   - Only RT_CLASS_SLOT_OBJ slots exist: strings and weak handles are never
//     members of a cycle and stay out of the map.
// Ownership/Lifetime:
//   - The table owns its slot arrays; rt_obj_class_layout_shutdown frees them.
//   - Callers receive borrowed views; the map is immutable after registration.
// Links: src/runtime/core/rt_gc.c (gc_zia_object_traverse),
//        src/runtime/oop/rt_object.c (rt_obj_new_i64),
//        src/frontends/zia/Lowerer_Decl_Types.cpp (emitClassLayoutInit),
//        docs/adr/0315-cycle-collection-of-class-instances.md
//
//===----------------------------------------------------------------------===//
#ifndef RT_CLASS_LAYOUT_H
#define RT_CLASS_LAYOUT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Slot kind: a strong object reference (the only kind registered).
#define RT_CLASS_SLOT_OBJ 1

/// @brief One class's strong reference slots, in byte offsets from the payload.
typedef struct rt_class_layout {
    int64_t *offsets; ///< Owned array of byte offsets (may be NULL when count is 0).
    int64_t count;    ///< Number of registered slots.
    int64_t capacity; ///< Declared slot count from rt_obj_class_layout_begin.
    int64_t declared; ///< 1 once rt_obj_class_layout_begin ran for this id.
} rt_class_layout_t;

/// @brief Declare (or redeclare) the layout of @p class_id with room for @p slot_count slots.
/// @details Grows the dense table as needed. A redeclaration resets the class's
///          slot list. Negative or zero ids and negative counts trap.
void rt_obj_class_layout_begin(int64_t class_id, int64_t slot_count);

/// @brief Append one strong slot to the layout declared by rt_obj_class_layout_begin.
/// @param class_id Positive class id previously passed to rt_obj_class_layout_begin.
/// @param offset Byte offset of the pointer field inside the object payload.
/// @param kind Must be RT_CLASS_SLOT_OBJ; other kinds trap.
void rt_obj_class_layout_add_slot(int64_t class_id, int64_t offset, int64_t kind);

/// @brief Does @p class_id have at least one registered strong slot?
/// @return 1 when instances of the class must be tracked by the cycle collector.
int8_t rt_obj_class_layout_has_ref_slots(int64_t class_id);

/// @brief Borrowed view of a class's layout, or NULL when none was registered.
const rt_class_layout_t *rt_obj_class_layout_get(int64_t class_id);

/// @brief Number of classes with a registered layout (diagnostics / tests).
int64_t rt_obj_class_layout_count(void);

/// @brief Free every layout (process shutdown / test isolation).
void rt_obj_class_layout_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* RT_CLASS_LAYOUT_H */
