//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/oop/rt_oop.h
// Purpose: Defines the C ABI shared by Zanna language frontends for class
//          metadata, vtable dispatch, inheritance tests, interface tables,
//          safe casts, and the per-VM type registry.
//
// Key invariants:
//   - vptr is always at offset 0 in every object; this is a stable ABI invariant.
//   - vtable slot indices are compile-time constants set by the codegen layer.
//   - Classes and interfaces are registered before the registry is sealed;
//     subsequent registration attempts trap.
//   - Interface dispatch uses class/interface bindings to immutable itable arrays.
//
// Ownership/Lifetime:
//   - Registry state belongs to the active VM context and is released with it.
//   - Aggregate and C-string registration APIs borrow caller-owned metadata,
//     names, vtables, and itables for the registry lifetime; runtime-string
//     bridge APIs copy qualified names into registry-owned storage.
//   - Object instances are reference-counted separately; metadata is not refcounted.
//
// Links: src/runtime/oop/rt_oop_dispatch.c (vtable lookup),
//        src/runtime/oop/rt_type_registry.c (metadata and interface registry),
//        src/runtime/oop/rt_object.h (object allocation and lifetime)
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_oop.h
 * @brief Defines the shared native ABI for runtime object-oriented metadata.
 * @details The interface specifies object and class layouts, class and
 *          interface registration, virtual and interface dispatch, inheritance
 *          tests, safe casts, registry lifecycle, and bridge operations for
 *          compiler-emitted or managed metadata.
 */

#pragma once

#include "rt_string.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Compile-time class metadata descriptor.
/// @details Describes a single class type for runtime type identification,
///          virtual dispatch, and inheritance queries. Populated by the
///          compiler and registered during module initialization. Aggregate
///          registration borrows the descriptor and every pointer it contains.
typedef struct rt_class_info {
    int type_id;                      ///< Stable type id assigned by the compiler.
    const char *qname;                ///< Fully-qualified class name, e.g. "A.B.C".
    const struct rt_class_info *base; ///< Base class metadata, or NULL for root classes.
    void **vtable;                    ///< Array of function pointers (virtual method slots).
    uint32_t vtable_len;              ///< Number of slots in the vtable.
} rt_class_info;

/// @brief Runtime object header with vptr at offset 0.
/// @details Every heap-allocated object begins with this layout. Instance
///          fields follow immediately after the vptr (layout defined by the
///          compiler). The vtable storage is registry metadata and is not owned
///          by an individual object.
typedef struct rt_object {
    void **vptr; ///< Points into the class vtable (slot 0).
} rt_object;

/// @brief Register a class using an aggregate rt_class_info descriptor.
/// @details Adds borrowed class metadata to the active VM registry, keyed by
///          type ID and canonical vtable pointer. Repeating an identical
///          registration is harmless; conflicting IDs or vtables, invalid
///          metadata, allocation failure, or registration after sealing trap.
/// @param ci Borrowed class descriptor, including its borrowed name, base
///        metadata, and vtable; @c NULL is ignored.
/// @pre The descriptor and its referenced storage must remain valid for the
///      active registry's lifetime.
/// @thread_safety Serialized by the registry write lock until sealing.
void rt_register_class(const rt_class_info *ci);

/// @brief Resolve a virtual function pointer by slot index from an object.
/// @details Reads the object's vptr, requires that vtable to be registered,
///          bounds-checks @p slot against the class metadata's `vtable_len`,
///          and returns the stored function pointer without invoking it.
/// @param obj Instance pointer whose first field is an @ref rt_object vptr;
///        may be @c NULL.
/// @param slot 0-based vtable slot index assigned at compile time.
/// @return Borrowed function pointer stored at the slot, or @c NULL for a null
///         object/vptr, unregistered vtable, out-of-range slot, or null slot.
void *rt_get_vfunc(const rt_object *obj, uint32_t slot);

/// @brief Map a vtable pointer to its owning class metadata.
/// @details Looks up the vptr in the runtime registry and returns class info
///          when known. Used internally for bounds checking and diagnostic messages.
/// @param vptr Canonical vtable pointer for a class; may be @c NULL.
/// @return Borrowed class metadata, or @c NULL when @p vptr is null or unregistered.
const rt_class_info *rt_get_class_info_from_vptr(void **vptr);

/// @brief Interface metadata descriptor for registration.
/// @details Aggregate registration copies the scalar fields but borrows the
///          qualified-name pointer for the active registry's lifetime.
typedef struct {
    int iface_id;      ///< Stable interface id assigned by the compiler.
    const char *qname; ///< Fully-qualified interface name.
    int slot_count;    ///< Number of methods in the interface slot table.
} rt_iface_reg;

/// @brief Register interface metadata.
/// @details Establishes interface identity and slot count in the active
///          registry. An identical repeated ID is ignored; conflicting or
///          invalid metadata, allocation failure, or registration after
///          sealing traps.
/// @param iface Borrowed interface descriptor; @c NULL is ignored. Its
///        qualified-name storage must outlive the registry.
void rt_register_interface(const rt_iface_reg *iface);

/// @brief Bind a class to an interface with a concrete itable slots array.
/// @details Stores the interface table in the registry, enabling interface dispatch
///          to call the correct method table for the given type. The itable_slots
///          array must have the registered interface's slot count. The class and
///          interface must already be registered. A repeated identical binding is
///          ignored; a conflicting binding or post-seal write traps.
/// @param type_id Concrete class type ID.
/// @param iface_id Registered interface ID to bind.
/// @param itable_slots Borrowed immutable function-pointer array; @c NULL is ignored
///        and non-NULL storage must outlive the registry.
void rt_bind_interface(int type_id, int iface_id, void **itable_slots);

/// @brief Resolve the dynamic type id of an object instance.
/// @details Reads the type id associated with the object's vptr for RTTI
///          and fast type comparisons. The object is validated before its vptr
///          is accessed.
/// @param obj Candidate object payload; may be @c NULL.
/// @return The compile-time-assigned type id of the object's dynamic type,
///         zero for @c NULL, or -1 for an invalid object or unregistered vtable.
int rt_typeid_of(void *obj);

/// @brief Check whether type_id is-a test_type_id (same type or subclass).
/// @details Consults class metadata and walks the inheritance chain to
///          determine if the type relationship holds.
/// @param type_id      The type to test.
/// @param test_type_id The target type to test against.
/// @return Non-zero if type_id is the same as or a subclass of test_type_id; 0 otherwise.
int8_t rt_type_is_a(int type_id, int test_type_id);

/// @brief Check whether a type implements an interface.
/// @details Searches the exact class binding and then each registered base
///          class, so an inherited interface implementation is recognized.
/// @param type_id  The class type id.
/// @param iface_id The interface id to check.
/// @return @c 1 if the type or an ancestor implements the interface; @c 0 for
///         no binding, unknown metadata, or negative IDs.
int8_t rt_type_implements(int type_id, int iface_id);

/// @brief Cast an object to a target class type, returning the object or NULL.
/// @details Performs a safe downcast by comparing the object's dynamic type id
///          and registered base chain against the target. The successful result
///          is the same borrowed pointer; this operation does not retain it.
/// @param obj Candidate object instance; may be @c NULL.
/// @param target_type_id The class type id to cast to.
/// @return The borrowed @p obj pointer if compatible; @c NULL for null/invalid
///         objects, negative or unknown targets, or incompatible types.
void *rt_cast_as(void *obj, int target_type_id);

/// @brief Cast an object to an interface, returning the object or NULL.
/// @details Searches bindings on the dynamic class and its base chain. The
///          successful result is the same borrowed pointer and is not retained.
/// @param obj Candidate object instance; may be @c NULL.
/// @param iface_id The interface id to cast to.
/// @return The borrowed @p obj pointer if compatible; @c NULL for null/invalid
///         objects, negative IDs, or a missing binding.
void *rt_cast_as_iface(void *obj, int iface_id);

/// @brief Lookup the interface table pointer for an object and interface.
/// @details Resolves the binding on the dynamic class or its nearest registered
///          ancestor for direct interface-slot invocation.
/// @param obj Candidate object instance; may be @c NULL.
/// @param iface_id The interface id to look up.
/// @return Borrowed interface method table, or @c NULL for invalid input,
///         unknown object metadata, or an unbound interface.
void **rt_itable_lookup(void *obj, int iface_id);

/// @brief Register an interface directly without an aggregate descriptor.
/// @details Simplifies compiler-emitted registration during module init by
///          accepting individual parameters instead of a struct. The qualified
///          name is borrowed for the registry lifetime.
/// @param iface_id   Stable interface id assigned by the compiler.
/// @param qname Borrowed fully-qualified interface name (for example,
///        `"Ns.IFace"`); may be @c NULL.
/// @param slot_count Number of method slots in the interface table.
void rt_register_interface_direct(int iface_id, const char *qname, int slot_count);

/// @brief Register an interface using a runtime string for the qualified name.
/// @details Validates the 64-bit identifiers/count, copies the runtime string's
///          bytes into registry-owned C-string storage, and registers the
///          descriptor. Invalid ranges or string handles trap.
/// @param iface_id   Stable interface id assigned by the compiler.
/// @param qname Borrowed runtime string containing the fully-qualified name;
///        may be @c NULL and need only remain live for the call.
/// @param slot_count Number of method slots in the interface table.
void rt_register_interface_direct_rs(int64_t iface_id, rt_string qname, int64_t slot_count);

/// @brief Register a class directly with its vtable and qualified name.
/// @details Allocates metadata and registers it in the per-VM registry so that
///          lookups by vtable pointer succeed and Object.ToString can render
///          the qualified name. This form registers a root class and borrows
///          both @p vtable and @p qname for the registry lifetime.
/// @param type_id     Stable class id assigned by the compiler.
/// @param vtable Borrowed canonical function-pointer array; @c NULL is ignored.
/// @param qname Borrowed fully-qualified class name; may be @c NULL.
/// @param vslot_count Number of vtable slots bound for this class.
void rt_register_class_direct(int type_id, void **vtable, const char *qname, int vslot_count);

/// @brief Register a class using a runtime string for the qualified name.
/// @details Root-class variant that copies the runtime string's bytes into
///          registry-owned storage. Invalid slot counts or string handles trap.
/// @param type_id     Stable class id assigned by the compiler.
/// @param vtable Borrowed canonical function-pointer array; @c NULL is ignored.
/// @param qname Borrowed runtime string containing the fully-qualified name;
///        may be @c NULL and need only remain live for the call.
/// @param vslot_count Number of vtable slots bound for this class.
void rt_register_class_direct_rs(int type_id, void **vtable, rt_string qname, int64_t vslot_count);

/// @brief Register a class with its base class type id.
/// @details Allocates metadata, sets the base pointer by looking up base_type_id
///          in the registry, and binds the class. The base must already be
///          registered. This C-string form borrows the name and vtable for the
///          active registry's lifetime.
/// @param type_id      Stable class id assigned by the compiler.
/// @param vtable Borrowed canonical function-pointer array; @c NULL is ignored.
/// @param qname Borrowed fully-qualified class name; may be @c NULL.
/// @param vslot_count  Number of vtable slots bound for this class.
/// @param base_type_id Type id of the base class, or -1 if none.
void rt_register_class_with_base(
    int type_id, void **vtable, const char *qname, int vslot_count, int base_type_id);

/// @brief Register a class with base class using a runtime string for the qualified name.
/// @details Copies the runtime string's bytes into registry-owned storage and
///          requires a nonnegative base ID to identify an already-registered
///          class. Invalid ranges or string handles trap.
/// @param type_id      Stable class id assigned by the compiler.
/// @param vtable Borrowed canonical function-pointer array; @c NULL is ignored.
/// @param qname Borrowed runtime string containing the fully-qualified name;
///        may be @c NULL and need only remain live for the call.
/// @param vslot_count  Number of vtable slots bound for this class.
/// @param base_type_id Type id of the base class, or -1 if none.
void rt_register_class_with_base_rs(
    int type_id, void **vtable, rt_string qname, int64_t vslot_count, int64_t base_type_id);

/// @brief Lookup the canonical vtable pointer for a class type id.
/// @details Allows the VM and runtime to resolve method tables by type id
///          without holding a direct reference to the vtable.
/// @param type_id Stable class id.
/// @return Borrowed canonical vtable pointer, or @c NULL for a negative or
///         unknown type ID.
void **rt_get_class_vtable(int type_id);

/// @brief Register an interface implementation table for a class.
/// @details Wraps rt_bind_interface with i64 parameters for IL compatibility.
///          Out-of-range IDs trap; a null table is ignored by the underlying
///          binding operation.
/// @param type_id  Class type id.
/// @param iface_id Interface id.
/// @param itable Borrowed immutable interface method table that must outlive
///        the registry; may be @c NULL.
void rt_register_interface_impl(int64_t type_id, int64_t iface_id, void **itable);

/// @brief Lookup the interface implementation table for a class type.
/// @details Looks up the binding by (type_id, iface_id), walking base classes
///          if needed. Enables interface assignment to resolve the itable at
///          compile-known types.
/// @param type_id  Class type id.
/// @param iface_id Interface id.
/// @return Borrowed interface method table, or @c NULL for out-of-range IDs or
///         when neither the class nor an ancestor has a binding.
void **rt_get_interface_impl(int64_t type_id, int64_t iface_id);

/// @brief Seal the type registry against further registration.
///
/// Call after all type and interface registration is complete (typically
/// at the end of module initialization). Subsequent registration attempts
/// trap, while reads continue through the registry's shared reader lock.
/// Calling without an active registry is a no-op; sealing an already sealed
/// registry is harmless.
///
/// @thread_safety Safe to call from any thread. The implementation holds the
///                write lock while publishing the immutable state with release
///                ordering, preventing writers from racing the transition.
void rt_type_registry_seal(void);

#ifdef __cplusplus
}
#endif
