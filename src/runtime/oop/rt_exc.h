//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/oop/rt_exc.h
// Purpose: Runtime exception support for structured exception handling, providing exception object
// creation, message access, and type checking for catch handlers.
//
// Key invariants:
//   - Exception objects carry a class ID for type-based catch dispatch.
//   - Exception objects are reference-counted and contain a message string.
//   - rt_exc_is_exception compares the exact built-in Exception class ID.
//   - Exception objects follow normal refcount rules; callers must balance retain/release.
//
// Ownership/Lifetime:
//   - Exception objects start with refcount 1; caller owns the initial reference.
//   - Message strings are retained by the exception object.
//
// Links: src/runtime/oop/rt_exc.c (implementation), src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_exc.h
 * @brief Declares construction and inspection of built-in Exception objects.
 * @details Exceptions are reference-counted managed values with a stable class
 *          identifier and an optionally retained message String. Accessors
 *          borrow stored state, while creation returns one caller-owned
 *          reference for transfer to exception-handling machinery.
 */

#pragma once

#include "rt_string.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Exception class ID for type checking in exception handlers.
/// This is a well-known class ID for the built-in Exception class.
#define RT_EXCEPTION_CLASS_ID 1

/// @brief Create a new Exception object with the given message.
/// @param[in] msg Managed exception message retained by the object, or NULL.
/// @return Caller-owned managed Exception with reference count one, or NULL on
///         allocation failure.
void *rt_exc_new(rt_string msg);

/// @brief Get the message from an Exception object.
/// @param[in] exc Exception payload, or NULL.
/// @return Borrowed message String, or NULL when the receiver or message is NULL.
/// @warning This low-level accessor assumes every non-null pointer has the Exception layout.
rt_string rt_exc_get_message(void *exc);

/// @brief Check whether an object has the exact built-in Exception class ID.
/// @param[in] obj Candidate managed object, or NULL.
/// @return 1 for an exact Exception instance; otherwise 0.
int64_t rt_exc_is_exception(void *obj);

#ifdef __cplusplus
}
#endif
