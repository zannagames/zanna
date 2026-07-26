//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/oop/rt_oop_dispatch.c
// Purpose: Implements virtual method dispatch (vtable lookup) for the Zanna OOP
//          runtime. Objects carry a vptr to their class vtable; dispatch reads
//          and returns the function pointer at a requested slot for the caller
//          to invoke with the appropriate generated signature.
//
// Key invariants:
//   - Slot 0 is Object.ToString, slot 1 is Object.Equals, slot 2 is GetHashCode;
//     class-specific overrides start at slot 3.
//   - NULL object, NULL vptr, or out-of-range slot returns NULL (not a trap).
//   - Registered vtable contents and lengths remain immutable while dispatch is active.
//   - Vtable lookups are read-only and fully thread-safe after registration.
//
// Ownership/Lifetime:
//   - The active per-VM type registry borrows compiler-emitted vtable storage.
//   - The returned function pointer is borrowed; lookup creates no ownership.
//
// Links: src/runtime/oop/rt_oop.h (public API and object layout),
//        src/runtime/oop/rt_type_registry.c (class and vtable registration),
//        src/runtime/oop/rt_object.h (object allocation and vptr layout)
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_oop_dispatch.c
 * @brief Implements bounds-checked virtual function lookup.
 * @details Dispatch reads the fixed leading vtable pointer from an object,
 *          resolves immutable class metadata through the active type registry,
 *          validates the compiler-assigned slot, and returns a borrowed
 *          function pointer without invoking it.
 */

#include "rt_oop.h"

/// @brief Look up a virtual function pointer from an object's vtable.
///
/// @details Retrieves the function pointer at a specific slot in the object's
///          registered vtable. This is the core lookup for virtual method
///          dispatch; the slot index corresponds to the virtual method's
///          compiler-assigned position in the class hierarchy.
///
/// **Dispatch sequence:**
/// ```
/// obj.Method()
///   ↓
/// rt_get_vfunc(obj, METHOD_SLOT)
///   ↓
/// obj->vptr[slot] → function pointer
///   ↓
/// Indirect call through function pointer
/// ```
///
/// **Safety behavior:**
/// - NULL object → returns NULL (no crash)
/// - NULL vptr → returns NULL (uninitialized object)
/// - Unknown class → returns NULL (no bounds check possible)
/// - Out of bounds slot → returns NULL (prevents buffer overread)
///
/// @param obj Object whose vtable is queried. May be NULL.
/// @param slot Zero-based vtable slot index to fetch.
///
/// @return Borrowed function pointer at the slot, or @c NULL for a null object
///         or vptr, unregistered vtable, out-of-range slot, or null slot value.
///
/// @note Callers must check for NULL before calling the returned pointer.
/// @note O(1) time complexity (array index lookup).
void *rt_get_vfunc(const rt_object *obj, uint32_t slot) {
    if (!obj || !obj->vptr)
        return (void *)0;

    // Bounds check: retrieve class info and validate slot index
    const rt_class_info *ci = rt_get_class_info_from_vptr(obj->vptr);
    if (!ci)
        return (void *)0; // Unknown class, cannot validate

    if (slot >= ci->vtable_len)
        return (void *)0; // Out of bounds

    return obj->vptr[slot];
}
