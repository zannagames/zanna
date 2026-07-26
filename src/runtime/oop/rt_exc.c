//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/oop/rt_exc.c
// Purpose: Implements the runtime exception object type for the Zanna exception
//          handling system. Provides rt_exception allocation, message access,
//          and type-tag queries used by the VM and native EH instructions.
//
// Key invariants:
//   - Exception objects are heap-allocated and reference-counted.
//   - The message string is retained by the exception on creation and released
//     when the exception object is freed.
//   - Exception type tags are integer identifiers registered at startup.
//   - This built-in predicate recognizes the exact Exception class identifier.
//   - A NULL message is stored as the absence of a message.
//
// Ownership/Lifetime:
//   - Callers that throw an exception transfer ownership to the EH machinery.
//   - The EH machinery releases the exception after a catch handler returns.
//   - The message string is owned by the exception object, not the caller.
//
// Links: src/runtime/oop/rt_exc.h (public API),
//        src/runtime/oop/rt_type_registry.h (type tag registration)
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_exc.c
 * @brief Implements the built-in managed Exception object.
 * @details Exception construction allocates the stable built-in class payload,
 *          retains an optional managed message, installs finalization that
 *          releases that message, and supplies low-level message and exact
 *          class-identity queries for native exception handling.
 */

#include "rt_exc.h"
#include "rt_object.h"
#include "rt_string.h"
#include <stdlib.h>

/// @brief Internal exception object structure.
/// Layout: [vtable_ptr (8 bytes)][message (8 bytes)]
typedef struct {
    void *vtable;      ///< Vtable pointer (not used for Exception but reserved)
    rt_string message; ///< Exception message
} rt_exception_t;

/// @brief Finalizer for exception objects - releases the message string.
/// @param[in,out] obj Exception payload whose retained message is released.
static void exception_finalizer(void *obj) {
    rt_exception_t *exc = (rt_exception_t *)obj;
    if (exc->message) {
        rt_str_release_maybe(exc->message);
    }
}

/// @brief Allocate a built-in Exception and retain its optional message.
/// @param[in] msg Managed message String to retain, or NULL.
/// @return Caller-owned managed Exception with reference count one, or NULL on
///         allocation failure.
void *rt_exc_new(rt_string msg) {
    // Allocate exception object with class ID 1 (Exception)
    rt_exception_t *exc =
        (rt_exception_t *)rt_obj_new_i64(RT_EXCEPTION_CLASS_ID, sizeof(rt_exception_t));
    if (!exc)
        return NULL;

    // Initialize vtable pointer to NULL (Exception is a simple class)
    exc->vtable = NULL;

    // Store and retain the message
    exc->message = msg;
    if (msg) {
        rt_str_retain_maybe(msg);
    }

    // Set finalizer to release message when exception is freed
    rt_obj_set_finalizer(exc, exception_finalizer);

    return exc;
}

/// @brief Return the message stored in an Exception payload.
/// @param[in] exc Exception payload, or NULL.
/// @return Borrowed message handle, or NULL when @p exc or its message is NULL.
/// @warning This low-level accessor assumes every non-null pointer has the Exception layout.
rt_string rt_exc_get_message(void *exc) {
    if (!exc)
        return NULL;

    rt_exception_t *e = (rt_exception_t *)exc;
    return e->message;
}

/// @brief Test whether an object has the built-in Exception class identifier.
/// @param[in] obj Candidate managed object, or NULL.
/// @return 1 for an exact RT_EXCEPTION_CLASS_ID match; otherwise 0.
int64_t rt_exc_is_exception(void *obj) {
    if (!obj)
        return 0;
    return rt_obj_class_id(obj) == RT_EXCEPTION_CLASS_ID ? 1 : 0;
}
