//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/text/rt_yaml_internal.h
// Purpose: Shared YAML value classifiers/release used by the parser (rt_yaml.c)
//   and the emitter (rt_yaml_format.c).
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Private YAML runtime-value classification and scalar conversion API.
/// @details The parser and emitter share these helpers to distinguish opaque
///          runtime objects without exposing implementation-specific container
///          layouts through the public header.

#pragma once

#include "rt_box.h"
#include "rt_map.h"
#include "rt_object.h"
#include "rt_seq.h"

/// @brief Minimum payload required before an object can be classified as a Seq.
#define YAML_SEQ_MIN_PAYLOAD (sizeof(int64_t) * 2 + sizeof(void *) + sizeof(int8_t))
/// @brief Minimum payload required before an object can be classified as a Map.
#define YAML_MAP_MIN_PAYLOAD (sizeof(void *) * 2 + sizeof(size_t) * 2)


/// @brief Drop a temporary runtime-object reference.
/// @param obj Runtime-managed object; null is ignored.
static inline void yaml_release(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief True if `obj` is a boxed primitive (Int/Bool/Float/String wrapper).
///
/// YAML scalars become boxed primitives; sequences/mappings stay
/// as plain GC objects. The class-id check distinguishes the two.
/// @param obj Candidate runtime object.
/// @return `true` for a non-string object with a recognized box type.
static inline bool yaml_is_boxed(void *obj) {
    return obj && !rt_string_is_handle(obj) && rt_box_type(obj) >= 0;
}

/// @brief Test whether an object is a sufficiently large runtime sequence.
/// @param obj Candidate runtime object.
/// @return `true` only for a Seq handle accepted by the runtime instance check.
static inline bool yaml_is_sequence(void *obj) {
    if (!obj || rt_string_is_handle(obj) || yaml_is_boxed(obj))
        return false;
    return rt_obj_is_instance(obj, RT_SEQ_CLASS_ID, YAML_SEQ_MIN_PAYLOAD);
}

/// @brief Test whether an object is a sufficiently large runtime mapping.
/// @param obj Candidate runtime object.
/// @return `true` only for a Map handle accepted by the runtime instance check.
static inline bool yaml_is_mapping(void *obj) {
    if (!obj || rt_string_is_handle(obj) || yaml_is_boxed(obj))
        return false;
    return rt_obj_is_instance(obj, RT_MAP_CLASS_ID, YAML_MAP_MIN_PAYLOAD);
}

/// @brief Convert a plain YAML scalar span to its strongest runtime type.
/// @details Recognizes null, booleans, supported integers, finite decimal
///          floats, and YAML special floats; all other spans become strings.
/// @param str Plain-scalar byte span.
/// @param len Scalar length in bytes.
/// @return Owned boxed primitive or string, or null for the YAML null value.
void *parse_scalar(const char *str, size_t len);
