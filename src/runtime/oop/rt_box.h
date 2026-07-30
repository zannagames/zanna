//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/oop/rt_box.h
// Purpose: Boxing and unboxing primitives for storing primitive types (i64, f64, i1, str) in
// heap-allocated objects for use in generic collections.
//
// Key invariants:
//   - Boxed values carry a type tag: 0=i64, 1=f64, 2=i1, 3=str.
//   - rt_unbox_* traps if the type tag does not match the requested type.
//   - rt_box_try_to_* variants report mismatches without trapping.
//   - Boxed values participate in reference counting.
//   - rt_box_value_type boxes a struct/class by copying all fields into a heap object.
//
// Ownership/Lifetime:
//   - Boxed objects are heap-allocated with refcount 1; callers own the initial reference.
//   - Unboxing does not consume the boxed object; caller must release separately.
//
// Links: src/runtime/oop/rt_box.c (implementation), src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_box.h
 * @brief Declares primitive boxing and copied value-type boxing APIs.
 * @details The interface creates tagged managed boxes for integers, floating
 *          values, booleans, and Strings; supports strict or non-trapping
 *          unboxing; and records object or String fields inside copied value
 *          types for correct runtime ownership and equality behavior.
 */

#pragma once

#include "rt_string.h"
#include <stdint.h>

/// @brief Stable managed identity for primitive Box payloads.
#define RT_BOX_CLASS_ID INT64_C(-0x430101)
/// @brief Stable managed identity for compiler-allocated boxed value types.
#define RT_VALUE_TYPE_CLASS_ID INT64_C(-0x430102)

/// @brief Managed field kind for a general object reference.
#define RT_VALUE_FIELD_OBJ INT64_C(1)
/// @brief Managed field kind for a runtime String reference.
#define RT_VALUE_FIELD_STR INT64_C(2)

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Type tags discriminating boxed primitive values.
typedef enum rt_box_type {
    RT_BOX_I64 = 0,
    RT_BOX_F64 = 1,
    RT_BOX_I1 = 2,
    RT_BOX_STR = 3
} rt_box_type_t;

/// @brief Box a 64-bit integer.
/// @param val The integer value to box.
/// @return Heap-allocated boxed object (refcount = 1).
void *rt_box_i64(int64_t val);

/// @brief Box a 64-bit float.
/// @param val The float value to box.
/// @return Heap-allocated boxed object (refcount = 1).
void *rt_box_f64(double val);

/// @brief Box a boolean.
/// @param val The boolean value (0 = false, non-zero = true).
/// @return Heap-allocated boxed object (refcount = 1).
void *rt_box_i1(int64_t val);

/// @brief Box a boolean from the runtime `i1` ABI type.
/// @param val The boolean value (0 = false, non-zero = true).
/// @return Heap-allocated boxed object (refcount = 1).
void *rt_box_i1_bool(int8_t val);

/// @brief Box a string.
/// @param val The string to box.
/// @return Heap-allocated boxed object (refcount = 1).
void *rt_box_str(rt_string val);

/// @brief Unbox to integer.
/// @param box Boxed value (must be RT_BOX_I64).
/// @return The unboxed integer value.
/// @note Traps if box is NULL or wrong type.
int64_t rt_unbox_i64(void *box);

/// @brief Unbox to float.
/// @param box Boxed value (must be RT_BOX_F64).
/// @return The unboxed float value.
/// @note Traps if box is NULL or wrong type.
double rt_unbox_f64(void *box);

/// @brief Unbox to boolean.
/// @param box Boxed value (must be RT_BOX_I1).
/// @return The unboxed boolean (0 or 1).
/// @note Traps if box is NULL or wrong type.
int8_t rt_unbox_i1(void *box);

/// @brief Unbox to string.
/// @param box Boxed value (must be RT_BOX_STR).
/// @return The unboxed string (retained).
/// @note Traps if box is NULL or wrong type.
rt_string rt_unbox_str(void *box);

/// @brief Try to unbox to integer without trapping.
/// @param box Candidate boxed value.
/// @param out Receives the integer on success.
/// @return 1 on success, 0 for NULL, invalid box, wrong type, or NULL out.
int8_t rt_box_try_to_i64(void *box, int64_t *out);

/// @brief Try to unbox to float without trapping.
/// @param box Candidate boxed value.
/// @param out Receives the double on success.
/// @return 1 on success, 0 for NULL, invalid box, wrong type, or NULL out.
int8_t rt_box_try_to_f64(void *box, double *out);

/// @brief Try to unbox to boolean without trapping.
/// @param box Candidate boxed value.
/// @param out Receives 0 or 1 on success.
/// @return 1 on success, 0 for NULL, invalid box, wrong type, or NULL out.
int8_t rt_box_try_to_i1(void *box, int8_t *out);

/// @brief Try to unbox to string without trapping for structural mismatches.
/// @param box Candidate boxed value.
/// @param out Receives a retained string reference on success.
/// @return 1 on success; 0 for NULL, invalid box, wrong type, NULL out, or a
///         returning contained-string retain trap. On failure, @p out is NULL.
int8_t rt_box_try_to_str(void *box, rt_string *out);

/// @brief Convert to integer Option without exposing an out pointer.
/// @param[in] box Candidate managed box.
/// @return Caller-owned Some<i64> for a matching box, otherwise caller-owned None.
void *rt_box_to_i64_option(void *box);

/// @brief Convert to float Option without exposing an out pointer.
/// @param[in] box Candidate managed box.
/// @return Caller-owned Some<f64> for a matching box, otherwise caller-owned None.
void *rt_box_to_f64_option(void *box);

/// @brief Convert to boolean Option without exposing an out pointer.
/// @param[in] box Candidate managed box.
/// @return Caller-owned Some<i1> for a matching box, otherwise caller-owned None.
void *rt_box_to_i1_option(void *box);

/// @brief Convert to string Option without exposing an out pointer.
/// @param[in] box Candidate managed box.
/// @return Caller-owned Some<String> for a matching box, caller-owned None for
///         mismatch, or NULL after a returning retain/allocation trap.
void *rt_box_to_str_option(void *box);

/// @brief Get the type tag of a boxed value.
/// @param box Boxed value.
/// @return Type tag (0=i64, 1=f64, 2=i1, 3=str), or -1 if NULL.
int64_t rt_box_type(void *box);

/// @brief Check if a boxed value equals an integer.
/// @param box Boxed value.
/// @param val Integer to compare.
/// @return 1 if equal, 0 otherwise.
int64_t rt_box_eq_i64(void *box, int64_t val);

/// @brief Check if a boxed value equals a float.
/// @param box Boxed value.
/// @param val Float to compare.
/// @return 1 if equal, 0 otherwise.
int64_t rt_box_eq_f64(void *box, double val);

/// @brief Check if a boxed value equals a string.
/// @param box Boxed value.
/// @param val String to compare.
/// @return 1 if equal, 0 otherwise.
int64_t rt_box_eq_str(void *box, rt_string val);

/// @brief Allocate heap memory for boxing a value type (struct).
/// @param size Size in bytes to allocate.
/// @return Heap-allocated zero-initialized memory.
/// @note Size 0 is valid and produces a managed empty value-type object.
/// @note Negative sizes trap.
/// @note The compiler copies struct fields into this memory and registers
/// managed fields with rt_box_value_type_add_field().
void *rt_box_value_type(int64_t size);

/// @brief Register a managed field inside a boxed value type.
/// @param obj ValueType object returned from rt_box_value_type().
/// @param offset Byte offset of the pointer-sized field.
/// @param kind RT_VALUE_FIELD_OBJ or RT_VALUE_FIELD_STR.
/// @param retain_now Retain the current field value immediately when non-zero.
/// @note Re-registering the same offset with the same kind is a no-op and does not retain or
/// release the current slot value.
/// @note Re-registering the same offset with a different kind traps.
void rt_box_value_type_add_field(void *obj, int64_t offset, int64_t kind, int8_t retain_now);

/// @brief Content-aware hash for an element.
/// Boxed values (RT_ELEM_BOX) are hashed by content using FNV-1a;
/// non-boxed objects fall back to pointer identity hashing.
/// @param elem Element pointer (may be NULL).
/// @return Hash value.
size_t rt_box_hash(void *elem);

/// @brief Content-aware equality for two elements.
/// Boxed values (RT_ELEM_BOX) are compared by content (tag + data);
/// boxed floating-point NaN values compare equal to each other to keep
/// equality compatible with canonical NaN hashing.
/// non-boxed objects fall back to pointer identity.
/// @param a First element (may be NULL).
/// @param b Second element (may be NULL).
/// @return 1 if equal, 0 otherwise.
int8_t rt_box_equal(void *a, void *b);

/// @brief Total, transitive default sort order shared by the collections.
/// @details Ranks by type class (NULL < numeric < string < other), compares
///          within class by value (boxed i64/i1 exactly; f64 with NaN last;
///          raw or boxed strings lexicographically), and falls back to a
///          well-defined uintptr_t pointer order for other objects.
/// @param[in] a First collection element.
/// @param[in] b Second collection element.
/// @return Negative, zero, or positive for a<b, a==b, a>b.
int64_t rt_box_default_sort_compare(void *a, void *b);

#ifdef __cplusplus
}
#endif
