//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/assets/rt_gltf_internal.h
// Purpose: Shared glTF JSON accessors used by rt_gltf.c and rt_gltf_trs.c.
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_gltf_internal.h
 * @brief Declares shared JSON-array and transform helpers for the internal glTF importer.
 * @details These declarations bridge `rt_gltf.c`, its included implementation fragments, and
 *          `rt_gltf_trs.c`. JSON values and arrays are borrowed runtime objects. Matrix buffers
 *          use sixteen doubles; glTF source matrices are column-major while runtime matrices and
 *          decomposed-node calculations use row-major storage.
 */

#pragma once

#include "rt_gltf_json.h"

/**
 * @brief Retrieve an array-valued property from a parsed JSON object.
 * @param obj Borrowed JSON object.
 * @param key NUL-terminated property name.
 * @return Borrowed property value, or NULL when absent; callers validate sequence behavior
 *         through `jarr_len`.
 */
void *jarr(void *obj, const char *key);

/**
 * @brief Return the number of entries in a parsed JSON array.
 * @param arr Borrowed JSON sequence, or NULL.
 * @return Runtime sequence length, or zero for NULL.
 */
int64_t jarr_len(void *arr);

/**
 * @brief Convert a parsed JSON value to a finite numeric value with a fallback.
 * @param value Borrowed JSON value.
 * @param def Default returned when @p value is absent or not numeric.
 * @return Parsed number or @p def.
 */
double jvalue_num(void *value, double def);

/**
 * @brief Decompose a row-major affine matrix into translation, rotation, and scale.
 * @param m Source sixteen-element row-major matrix.
 * @param pos Destination three-component translation.
 * @param quat Destination normalized XYZW rotation quaternion.
 * @param scale Destination three-component signed scale.
 */
void gltf_matrix_to_trs(const double *m, double *pos, double *quat, double *scale);

/**
 * @brief Transpose a glTF column-major matrix into runtime row-major storage.
 * @param src Source sixteen-element column-major matrix.
 * @param dst Destination sixteen-element row-major matrix that does not alias @p src.
 */
void gltf_matrix_column_major_to_row_major(const double *src, double *dst);

/**
 * @brief Resolve one glTF node's local transform as a row-major matrix.
 * @details Uses the explicit `matrix` property when present; otherwise composes the node's
 *          translation, rotation, and scale properties with glTF defaults.
 * @param nodes_arr Borrowed glTF JSON `nodes` array.
 * @param node_idx Node index to resolve.
 * @param out Destination for sixteen row-major matrix elements.
 * @return Non-zero when the node and all required transform values are valid.
 */
int gltf_node_local_matrix(void *nodes_arr, int32_t node_idx, double *out);
