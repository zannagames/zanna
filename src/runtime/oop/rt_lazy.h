//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/oop/rt_lazy.h
// Purpose: Lazy type providing deferred computation until first access, evaluating a factory
// function once and caching the result for subsequent accesses.
//
// Key invariants:
//   - Suppliers must be pure because racing first accesses may evaluate twice.
//   - The computed value is cached; subsequent accesses return the cached value.
//   - Acquire/release publication makes a completed cached value visible across threads.
//   - rt_lazy_get returns the cached value after initialization.
//
// Ownership/Lifetime:
//   - Lazy objects are heap-allocated opaque pointers.
//   - Constructors return caller-owned Lazy objects.
//   - Object/String values passed to Of or Complete transfer ownership to Lazy.
//   - Accessors return borrowed cached values.
//
// Links: src/runtime/oop/rt_lazy.c (implementation), src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_lazy.h
 * @brief Declares deferred and precomputed managed Lazy-value operations.
 * @details Constructors accept native suppliers, VM bridge handles, or
 *          transferred object, String, and integer values. Accessors evaluate
 *          when necessary and return borrowed cached state, while completion
 *          and initialization queries support VM-managed evaluation.
 */

#pragma once

#include "rt_string.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//=========================================================================
// Lazy Creation
//=========================================================================

/// @brief Create a Lazy with a supplier function.
/// @param[in] supplier Borrowed pure function whose returned object ownership transfers to Lazy.
/// @return Caller-owned managed Lazy object, or NULL on allocation failure.
void *rt_lazy_new(void *(*supplier)(void));

/// @brief Create a deferred Lazy backed by an opaque managed-function handle.
/// @details VM-bridge only: the handle is not C-callable. The bridge intercepts
///          accessors, runs the handle itself, and publishes the result through
///          @ref rt_lazy_complete_obj. C-side evaluation of a handle-kind Lazy traps.
/// @param[in] handle Borrowed opaque managed-function handle.
/// @return Caller-owned managed Lazy object, or NULL on allocation failure.
void *rt_lazy_new_handle(void *handle);

/// @brief Fetch the pending managed-function handle of an unevaluated handle-kind Lazy.
/// @param[in] obj Lazy object, or NULL.
/// @return Borrowed handle, or NULL when evaluated or not handle-kind.
void *rt_lazy_pending_handle(void *obj);

/// @brief Publish the computed object payload for a handle-kind Lazy.
/// @param[in,out] obj Lazy object, or NULL.
/// @param[in] value Computed object whose ownership transfers to Lazy.
void rt_lazy_complete_obj(void *obj, void *value);

/// @brief Create an already-evaluated Lazy with a value.
/// @param[in] value Object whose ownership transfers to Lazy.
/// @return Caller-owned managed Lazy, or NULL on allocation failure.
void *rt_lazy_of(void *value);

/// @brief Create an already-evaluated Lazy with a string value.
/// @param[in] value Managed String whose ownership transfers to Lazy.
/// @return Caller-owned managed Lazy, or NULL on allocation failure.
void *rt_lazy_of_str(rt_string value);

/// @brief Create an already-evaluated Lazy with an i64 value.
/// @param[in] value Integer value to copy.
/// @return Caller-owned managed Lazy, or NULL on allocation failure.
void *rt_lazy_of_i64(int64_t value);

//=========================================================================
// Lazy Access
//=========================================================================

/// @brief Get the value, computing if necessary.
/// @param[in,out] obj Lazy object, or NULL.
/// @return Borrowed cached object value, or NULL.
void *rt_lazy_get(void *obj);

/// @brief Get the string value, computing if necessary.
/// @param[in,out] obj Lazy object, or NULL.
/// @return Borrowed cached String, or a constant empty String for other value kinds.
rt_string rt_lazy_get_str(void *obj);

/// @brief Get the i64 value, computing if necessary.
/// @param[in,out] obj Lazy object, or NULL.
/// @return Cached integer, or zero for other value kinds.
int64_t rt_lazy_get_i64(void *obj);

//=========================================================================
// Lazy State
//=========================================================================

/// @brief Check if the value has been computed.
/// @param[in] obj Lazy object, or NULL.
/// @return 1 if evaluated or NULL; otherwise 0.
int8_t rt_lazy_is_evaluated(void *obj);

/// @brief Force evaluation without returning value.
/// @param[in,out] obj Lazy object, or NULL.
void rt_lazy_force(void *obj);

//=========================================================================
// Transformation
//=========================================================================

/// @brief Eagerly transform a Lazy value and wrap the result in a new evaluated Lazy.
/// @param[in,out] obj Source Lazy.
/// @param[in] fn Borrowed transform callback.
/// @return Caller-owned transformed Lazy, or the original borrowed object for invalid arguments.
void *rt_lazy_map(void *obj, void *(*fn)(void *));

/// @brief Eagerly evaluate the source and invoke a callback returning another Lazy.
/// @param[in,out] obj Source Lazy.
/// @param[in] fn Borrowed callback returning a Lazy.
/// @return Callback result, or the original borrowed object for invalid arguments.
void *rt_lazy_flat_map(void *obj, void *(*fn)(void *));

/// @brief IL-compatible wrapper for @ref rt_lazy_new accepting an opaque supplier pointer.
/// @details Backs `Zanna.Functional.Lazy.New`, the public deferred-evaluation factory.
/// @param[in] supplier Opaque native supplier function pointer.
/// @return Result from rt_lazy_new().
void *rt_lazy_new_wrapper(void *supplier);

/// @brief IL-compatible wrapper for @ref rt_lazy_map accepting an opaque callback pointer.
/// @param[in,out] obj Source Lazy.
/// @param[in] fn Opaque native transform pointer.
/// @return Result from rt_lazy_map().
void *rt_lazy_map_wrapper(void *obj, void *fn);

/// @brief IL-compatible wrapper for @ref rt_lazy_flat_map accepting an opaque callback pointer.
/// @param[in,out] obj Source Lazy.
/// @param[in] fn Opaque native transform pointer.
/// @return Result from rt_lazy_flat_map().
void *rt_lazy_flat_map_wrapper(void *obj, void *fn);

#ifdef __cplusplus
}
#endif
