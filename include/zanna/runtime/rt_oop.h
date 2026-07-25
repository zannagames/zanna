//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: include/zanna/runtime/rt_oop.h
// Purpose: Public OOP runtime ABI for interface registration, binding, and RTTI.
// Key invariants: type_id and iface_id are process-local, assigned at module load;
//                 objects embed a vptr at offset 0; interface slots follow
//                 interface declaration order and bind per (class, interface).
// Ownership/Lifetime: Class/interface metadata is owned by the runtime registry
//                     for the duration of the process. Callers must ensure any
//                     itable slot arrays passed to bind are live for as long as
//                     the class remains loaded.
// Links: docs/specs/object-layout.md, src/runtime/oop/rt_oop.h,
//        src/runtime/oop/rt_type_registry.c
//
//===----------------------------------------------------------------------===//

/**
 * @file include/zanna/runtime/rt_oop.h
 * @brief Declares the public C ABI for interface registration and runtime casts.
 *
 * @details
 * This installed façade exposes the subset of the object runtime needed to
 * register interface metadata, associate class/interface method tables, query
 * dynamic class relationships, and perform null-on-failure casts.
 *
 * Type and interface identifiers are meaningful only within the active runtime
 * registry. Objects passed to query functions must obey the Zanna object-layout
 * ABI, with a registered vtable pointer stored at payload offset zero.
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Describes one interface to the runtime type registry.
     *
     * The descriptor itself is inspected synchronously and may be transient.
     * The registry retains the `qname` pointer rather than copying its bytes, so
     * that character storage must remain valid for the registry lifetime.
     */
    typedef struct rt_iface_reg
    {
        int iface_id;      ///< Non-negative, process-local interface identifier.
        const char *qname; ///< Persistent fully-qualified name, such as `Ns.IFace`.
        int slot_count;    ///< Non-negative number of itable method slots.
    } rt_iface_reg;

    /**
     * @brief Register or verify an interface descriptor.
     *
     * Registration is idempotent when an existing identifier has the same name
     * and slot count. Conflicting duplicate identifiers, negative metadata, and
     * registration after the registry is sealed raise a runtime trap.
     *
     * @param iface Borrowed descriptor. A null pointer is ignored.
     *
     * @pre `iface->qname` remains valid for the lifetime of the active registry
     *      when a new entry is installed.
     */
    void rt_register_interface(const rt_iface_reg *iface);

    /**
     * @brief Bind a registered class to an interface implementation table.
     *
     * A repeated binding is idempotent only when it supplies the same table
     * pointer. Unknown class or interface identifiers, conflicting duplicate
     * bindings, and mutation after registry sealing raise a runtime trap.
     *
     * @param type_id Process-local identifier of a registered class.
     * @param iface_id Identifier previously passed to
     *                 `rt_register_interface`.
     * @param itable_slots Borrowed array containing the interface's declared
     *                     number of function-pointer slots. Null is ignored.
     *
     * @pre `itable_slots` remains valid for the lifetime of the active registry.
     */
    void rt_bind_interface(int type_id, int iface_id, void **itable_slots);

    /**
     * @brief Resolve an object's dynamic class identifier from its vtable.
     * @param obj Borrowed object payload with a vtable pointer at offset zero,
     *            or null.
     * @return The registered type identifier, zero for a null object, or
     *         negative one when the object has no registered vtable.
     */
    int rt_typeid_of(void *obj);

    /**
     * @brief Test a same-class or derived-class relationship.
     * @param type_id Registered dynamic or candidate class identifier.
     * @param test_type_id Registered target class identifier.
     * @return Nonzero when `type_id` equals or transitively derives from
     *         `test_type_id`; otherwise zero.
     *
     * @note Negative and unknown identifiers compare false.
     */
    int rt_type_is_a(int type_id, int test_type_id);

    /**
     * @brief Test whether a class or one of its ancestors binds an interface.
     * @param type_id Registered class identifier to query.
     * @param iface_id Registered interface identifier to locate.
     * @return Nonzero when the class inheritance chain supplies a binding;
     *         otherwise zero.
     *
     * @note Negative and unknown identifiers compare false.
     */
    int rt_type_implements(int type_id, int iface_id);

    /**
     * @brief Perform a null-on-failure cast to a target class.
     * @param obj Borrowed object payload, or null.
     * @param target_type_id Desired registered class identifier.
     * @return The unchanged borrowed `obj` pointer when its dynamic class is the
     *         target or derives from it; otherwise null.
     *
     * @note A successful cast does not retain or otherwise change ownership.
     */
    void *rt_cast_as(void *obj, int target_type_id);

    /**
     * @brief Perform a null-on-failure cast to an implemented interface.
     * @param obj Borrowed object payload, or null.
     * @param iface_id Desired registered interface identifier.
     * @return The unchanged borrowed `obj` pointer when its dynamic class or an
     *         ancestor binds the interface; otherwise null.
     *
     * @note A successful cast does not return an itable and does not change
     *       ownership; dispatch resolves the table separately.
     */
    void *rt_cast_as_iface(void *obj, int iface_id);

#ifdef __cplusplus
} // extern "C"
#endif
