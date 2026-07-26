//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/oop/rt_sb_bridge.c
// Purpose: Constructs the heap-managed wrapper used by
//          Zanna.Text.StringBuilder, placing native builder state after an
//          OOP-compatible leading vptr slot.
//
// Key invariants:
//   - Wrapper objects are allocated through rt_obj_new_i64.
//   - Returned handles follow the same retain/release ownership model as
//     other rt_object instances.
//   - The vptr field is at offset zero and builder state follows at the
//     platform-defined aligned offset reported by the C structure layout.
//   - Construction leaves the reserved vptr slot zero-initialized; bridge
//     methods operate directly on the embedded builder rather than dispatching it.
//
// Ownership/Lifetime:
//   - Callers receive a fresh reference (refcount=1) from bridge constructors.
//   - The installed finalizer releases any grown builder buffer before the
//     zero-reference wrapper allocation is reclaimed.
//
// Links: src/runtime/oop/rt_sb_bridge.h (public API),
//        src/runtime/oop/rt_object.h (object allocation and lifecycle),
//        src/runtime/core/rt_string_builder.h (embedded builder implementation)
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_sb_bridge.c
 * @brief Implements the managed wrapper for the native StringBuilder state.
 * @details The bridge allocates an object-compatible payload with a reserved
 *          leading vtable slot and an aligned embedded builder, initializes
 *          that builder in place, and installs a finalizer that releases any
 *          grown native buffer when the wrapper reaches zero references.
 */

#include "rt_object.h"
#include "rt_string_builder.h"
#include <stddef.h>

// The StringBuilder object layout:
// [offset 0]                    : native-width reserved vptr slot
// [offsetof(StringBuilder, builder)] : embedded rt_string_builder state
typedef struct {
    void *vptr;                // reserved native-width vtable pointer slot
    rt_string_builder builder; // embedded builder state
} StringBuilder;

/// @brief GC finalizer for the namespaced StringBuilder class.
/// @details Resets the embedded @ref rt_string_builder and frees its growable
///          byte buffer when the wrapper reaches finalization. The inline
///          buffer and reserved vptr slot are part of the enclosing payload and
///          are reclaimed with that payload after this callback returns.
/// @param obj StringBuilder wrapper being finalized; @c NULL is ignored.
static void rt_sb_finalize(void *obj) {
    if (!obj)
        return;
    StringBuilder *sb = (StringBuilder *)obj;
    rt_sb_free(&sb->builder);
}

/// @brief Allocate a new instance of the namespaced StringBuilder class.
///
/// @details This bridges the high-level `Zanna.Text.StringBuilder` class to a
///          runtime-managed object by allocating a reserved vptr slot followed
///          by an embedded @ref rt_string_builder payload. The allocation is
///          zero-filled with generic class ID zero; this constructor does not
///          install a vtable. It initializes the embedded builder in place and
///          installs @ref rt_sb_finalize before returning.
///
/// @return Caller-owned opaque StringBuilder wrapper, or @c NULL after an
///         allocation failure trap.
void *rt_sb_new(void) {
    // Allocate enough space for vptr + embedded builder
    const int64_t kClassId = 0;
    const int64_t kObjectSize = sizeof(StringBuilder);

    StringBuilder *sb = (StringBuilder *)rt_obj_new_i64(kClassId, kObjectSize);
    if (sb) {
        // Initialize the embedded builder
        rt_sb_init(&sb->builder);
        rt_obj_set_finalizer(sb, rt_sb_finalize);
    }
    return sb;
}
