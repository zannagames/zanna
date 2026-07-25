//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/assets/rt_gltf_trs.c
// Purpose: glTF node transform math: TRS<->matrix conversion, column<->row
//   major, orthonormalization, and local-matrix computation. Loading/validation
//   live in rt_gltf.c.
//
// Links: rt_gltf.h, rt_gltf_internal.h, rt_gltf.c
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_gltf_trs.c
 * @brief Implements glTF node transform conversion and local-matrix construction.
 * @details Provides column-major to row-major conversion, robust matrix decomposition,
 *          quaternion-based TRS composition, Gram-Schmidt basis repair, reflection-aware scale
 *          extraction, and bounded JSON-node transform lookup for the glTF scene importer.
 */

#include "rt_gltf.h"
#include "rt_asset.h"
#include "rt_canvas3d.h"
#include "rt_canvas3d_internal.h"
#include "rt_gif.h"
#include "rt_gltf_json.h"
#include "rt_mat4.h"
#include "rt_morphtarget3d.h"
#include "rt_pixels.h"
#include "rt_pixels_internal.h"
#include "rt_quat.h"
#include "rt_scene3d_internal.h"
#include "rt_skeleton3d.h"
#include "rt_skeleton3d_internal.h"
#include "rt_string.h"
#include "rt_textureasset3d.h"
#include "rt_vec3.h"
#include <ctype.h>
#include <errno.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rt_seq.h"
#include "rt_object.h"
#include "rt_gltf_internal.h"

/// @brief Write identity transform components to any provided output arrays.
/// @param[out] pos Optional three-component translation set to zero.
/// @param[out] quat Optional four-component quaternion set to `(0, 0, 0, 1)`.
/// @param[out] scale Optional three-component scale set to one.
static void gltf_write_identity_trs(double *pos, double *quat, double *scale) {
    if (pos) {
        pos[0] = 0.0;
        pos[1] = 0.0;
        pos[2] = 0.0;
    }
    if (quat) {
        quat[0] = 0.0;
        quat[1] = 0.0;
        quat[2] = 0.0;
        quat[3] = 1.0;
    }
    if (scale) {
        scale[0] = 1.0;
        scale[1] = 1.0;
        scale[2] = 1.0;
    }
}

/// @brief Square root guarded against non-finite or negative input (returns 0.0 for those).
/// @param value Candidate squared magnitude or trace term.
/// @return Square root of a positive finite value; zero otherwise.
static double gltf_sqrt_nonnegative(double value) {
    if (!isfinite(value) || value <= 0.0)
        return 0.0;
    return sqrt(value);
}

/// @brief Dot product of two 3-component double vectors.
/// @param[in] a First three-component vector.
/// @param[in] b Second three-component vector.
/// @return Scalar dot product of @p a and @p b.
static double gltf_vec3_dot_local(const double *a, const double *b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

/// @brief Normalize a 3-vector in place.
/// @param[in,out] v Three-component vector to normalize.
/// @return Non-zero on success; zero, leaving @p v unchanged, for `NULL`, non-finite,
///         or near-zero input.
static int gltf_vec3_normalize_local(double *v) {
    double len;
    if (!v)
        return 0;
    if (!isfinite(v[0]) || !isfinite(v[1]) || !isfinite(v[2]))
        return 0;
    len = sqrt(gltf_vec3_dot_local(v, v));
    if (!isfinite(len) || len <= 1e-12)
        return 0;
    v[0] /= len;
    v[1] /= len;
    v[2] /= len;
    return 1;
}

/// @brief Cross product out = a × b of two 3-component double vectors.
/// @param[in] a First three-component vector.
/// @param[in] b Second three-component vector.
/// @param[out] out Three-component cross product; must not alias either input.
static void gltf_vec3_cross_local(const double *a, const double *b, double *out) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

/// @brief Gram-Schmidt orthonormalize three basis columns in place, substituting canonical
///   axes for any degenerate (non-normalizable) column so the result is always orthonormal.
/// @param[in,out] c0 First basis column, normalized or replaced with the X axis.
/// @param[in,out] c1 Second basis column, made orthogonal to @p c0 and normalized.
/// @param[out] c2 Third basis column rebuilt as the normalized cross product of the first two.
static void gltf_orthonormalize_columns(double *c0, double *c1, double *c2) {
    double dot01;
    if (!gltf_vec3_normalize_local(c0)) {
        c0[0] = 1.0;
        c0[1] = 0.0;
        c0[2] = 0.0;
    }
    dot01 = gltf_vec3_dot_local(c0, c1);
    c1[0] -= dot01 * c0[0];
    c1[1] -= dot01 * c0[1];
    c1[2] -= dot01 * c0[2];
    if (!gltf_vec3_normalize_local(c1)) {
        if (fabs(c0[0]) < 0.9) {
            c1[0] = 1.0;
            c1[1] = 0.0;
            c1[2] = 0.0;
        } else {
            c1[0] = 0.0;
            c1[1] = 1.0;
            c1[2] = 0.0;
        }
        dot01 = gltf_vec3_dot_local(c0, c1);
        c1[0] -= dot01 * c0[0];
        c1[1] -= dot01 * c0[1];
        c1[2] -= dot01 * c0[2];
        if (!gltf_vec3_normalize_local(c1)) {
            c1[0] = 0.0;
            c1[1] = 1.0;
            c1[2] = 0.0;
        }
    }
    gltf_vec3_cross_local(c0, c1, c2);
    if (!gltf_vec3_normalize_local(c2)) {
        c2[0] = 0.0;
        c2[1] = 0.0;
        c2[2] = 1.0;
    }
}

/// @brief Decompose a row-major 4x4 transform matrix into separate position, quaternion, and scale.
///
/// glTF nodes can store either a 16-element matrix or explicit
/// TRS — this helper converts the matrix form so the runtime
/// always works with TRS internally. Uses Shepperd's method
/// (largest-trace pivot) for the rotation extraction. Sheared authoring matrices
/// are reduced to the closest orthonormal rotation basis by Gram-Schmidt so
/// unsupported shear does not appear as a spurious node rotation.
/// @param[in] m Sixteen-element row-major transform matrix.
/// @param[out] pos Three-element translation vector.
/// @param[out] quat Four-element normalized quaternion in `(x, y, z, w)` order.
/// @param[out] scale Three-element scale vector, including one reflected axis when needed.
void gltf_matrix_to_trs(const double *m, double *pos, double *quat, double *scale) {
    double r00, r01, r02;
    double r10, r11, r12;
    double r20, r21, r22;
    double trace, s;
    double det;
    int flip_axis;
    if (!m || !pos || !quat || !scale)
        return;
    for (int i = 0; i < 16; i++) {
        if (!isfinite(m[i])) {
            gltf_write_identity_trs(pos, quat, scale);
            return;
        }
    }

    pos[0] = m[3];
    pos[1] = m[7];
    pos[2] = m[11];

    scale[0] = gltf_sqrt_nonnegative(m[0] * m[0] + m[4] * m[4] + m[8] * m[8]);
    scale[1] = gltf_sqrt_nonnegative(m[1] * m[1] + m[5] * m[5] + m[9] * m[9]);
    scale[2] = gltf_sqrt_nonnegative(m[2] * m[2] + m[6] * m[6] + m[10] * m[10]);
    if (!isfinite(scale[0]) || scale[0] <= 1e-12)
        scale[0] = 1.0;
    if (!isfinite(scale[1]) || scale[1] <= 1e-12)
        scale[1] = 1.0;
    if (!isfinite(scale[2]) || scale[2] <= 1e-12)
        scale[2] = 1.0;

    det = m[0] * (m[5] * m[10] - m[6] * m[9]) - m[1] * (m[4] * m[10] - m[6] * m[8]) +
          m[2] * (m[4] * m[9] - m[5] * m[8]);
    if (det < 0.0) {
        flip_axis = 0;
        if (m[0] < 0.0)
            flip_axis = 0;
        else if (m[5] < 0.0)
            flip_axis = 1;
        else if (m[10] < 0.0)
            flip_axis = 2;
        else {
            if (scale[1] > scale[flip_axis])
                flip_axis = 1;
            if (scale[2] > scale[flip_axis])
                flip_axis = 2;
        }
        scale[flip_axis] = -scale[flip_axis];
    }

    r00 = m[0] / scale[0];
    r10 = m[4] / scale[0];
    r20 = m[8] / scale[0];
    r01 = m[1] / scale[1];
    r11 = m[5] / scale[1];
    r21 = m[9] / scale[1];
    r02 = m[2] / scale[2];
    r12 = m[6] / scale[2];
    r22 = m[10] / scale[2];
    {
        double c0[3] = {r00, r10, r20};
        double c1[3] = {r01, r11, r21};
        double c2[3] = {r02, r12, r22};
        gltf_orthonormalize_columns(c0, c1, c2);
        r00 = c0[0];
        r10 = c0[1];
        r20 = c0[2];
        r01 = c1[0];
        r11 = c1[1];
        r21 = c1[2];
        r02 = c2[0];
        r12 = c2[1];
        r22 = c2[2];
    }

    trace = r00 + r11 + r22;
    if (trace > 0.0) {
        s = gltf_sqrt_nonnegative(trace + 1.0) * 2.0;
        if (s <= 1e-12) {
            quat[0] = quat[1] = quat[2] = 0.0;
            quat[3] = 1.0;
            return;
        }
        quat[3] = 0.25 * s;
        quat[0] = (r21 - r12) / s;
        quat[1] = (r02 - r20) / s;
        quat[2] = (r10 - r01) / s;
    } else if (r00 > r11 && r00 > r22) {
        s = gltf_sqrt_nonnegative(1.0 + r00 - r11 - r22) * 2.0;
        if (s <= 1e-12) {
            quat[0] = quat[1] = quat[2] = 0.0;
            quat[3] = 1.0;
            return;
        }
        quat[3] = (r21 - r12) / s;
        quat[0] = 0.25 * s;
        quat[1] = (r01 + r10) / s;
        quat[2] = (r02 + r20) / s;
    } else if (r11 > r22) {
        s = gltf_sqrt_nonnegative(1.0 + r11 - r00 - r22) * 2.0;
        if (s <= 1e-12) {
            quat[0] = quat[1] = quat[2] = 0.0;
            quat[3] = 1.0;
            return;
        }
        quat[3] = (r02 - r20) / s;
        quat[0] = (r01 + r10) / s;
        quat[1] = 0.25 * s;
        quat[2] = (r12 + r21) / s;
    } else {
        s = gltf_sqrt_nonnegative(1.0 + r22 - r00 - r11) * 2.0;
        if (s <= 1e-12) {
            quat[0] = quat[1] = quat[2] = 0.0;
            quat[3] = 1.0;
            return;
        }
        quat[3] = (r10 - r01) / s;
        quat[0] = (r02 + r20) / s;
        quat[1] = (r12 + r21) / s;
        quat[2] = 0.25 * s;
    }
    {
        double qlen =
            sqrt(quat[0] * quat[0] + quat[1] * quat[1] + quat[2] * quat[2] + quat[3] * quat[3]);
        if (!isfinite(qlen) || qlen <= 1e-12) {
            quat[0] = quat[1] = quat[2] = 0.0;
            quat[3] = 1.0;
        } else {
            quat[0] /= qlen;
            quat[1] /= qlen;
            quat[2] /= qlen;
            quat[3] /= qlen;
        }
    }
}

/// @brief Convert a glTF column-major matrix array into Zanna's row-major matrix layout.
/// @param[in] src Sixteen-element source matrix in glTF column-major order.
/// @param[out] dst Sixteen-element destination matrix in runtime row-major order.
/// @note @p src and @p dst must not alias because conversion is performed directly.
void gltf_matrix_column_major_to_row_major(const double *src, double *dst) {
    if (!src || !dst)
        return;
    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 4; col++)
            dst[row * 4 + col] = src[col * 4 + row];
    }
}

/// @brief Compose a row-major 4×4 transform matrix from separate translation, quaternion, and
/// scale.
/// @details Expands the unit quaternion `(x,y,z,w)` into the 3×3 rotation submatrix using the
///   standard double-angle identities (`xx = x*(2x)`, etc.), then multiplies each column by the
///   corresponding scale component and appends the translation in the rightmost column. The
///   bottom row is `[0, 0, 0, 1]`. This is the inverse of `gltf_matrix_to_trs` — it rebuilds
///   the TRS matrix after decomposition so we can accumulate world-space transforms during
///   the node-graph traversal.
/// @param[in] pos Three-element translation vector `[tx, ty, tz]`.
/// @param[in] quat Four-element quaternion `[x, y, z, w]`, normalized internally.
/// @param[in] scale Three-element scale vector `[sx, sy, sz]`.
/// @param[out] out Caller-supplied sixteen-element row-major matrix.
static void gltf_build_trs_matrix(const double *pos,
                                  const double *quat,
                                  const double *scale,
                                  double *out) {
    double x = quat[0], y = quat[1], z = quat[2], w = quat[3];
    double qlen = sqrt(x * x + y * y + z * z + w * w);
    if (!isfinite(qlen) || qlen <= 1e-12) {
        x = y = z = 0.0;
        w = 1.0;
    } else {
        x /= qlen;
        y /= qlen;
        z /= qlen;
        w /= qlen;
    }
    double x2 = x + x, y2 = y + y, z2 = z + z;
    double xx = x * x2, xy = x * y2, xz = x * z2;
    double yy = y * y2, yz = y * z2, zz = z * z2;
    double wx = w * x2, wy = w * y2, wz = w * z2;
    out[0] = (1.0 - (yy + zz)) * scale[0];
    out[1] = (xy - wz) * scale[1];
    out[2] = (xz + wy) * scale[2];
    out[3] = pos[0];
    out[4] = (xy + wz) * scale[0];
    out[5] = (1.0 - (xx + zz)) * scale[1];
    out[6] = (yz - wx) * scale[2];
    out[7] = pos[1];
    out[8] = (xz - wy) * scale[0];
    out[9] = (yz + wx) * scale[1];
    out[10] = (1.0 - (xx + yy)) * scale[2];
    out[11] = pos[2];
    out[12] = 0.0;
    out[13] = 0.0;
    out[14] = 0.0;
    out[15] = 1.0;
}

/// @brief Compute the local-space 4×4 matrix for a glTF node.
/// @details A glTF node stores its transform as either a 16-element column-major `"matrix"` array
///   or as separate `"translation"`, `"rotation"`, and `"scale"` arrays. This function handles
///   both forms: when a `"matrix"` field is present it is transposed from glTF's column-major
///   convention to Zanna's row-major layout via `gltf_matrix_column_major_to_row_major`; otherwise
///   the three TRS arrays are read (defaulting to identity when absent) and reassembled into a
///   matrix by `gltf_build_trs_matrix`. Returns 1 on success, 0 if the node index is out of
///   range or required data is missing.
/// @param[in] nodes_arr JSON array of glTF node objects.
/// @param node_idx Zero-based node index.
/// @param[out] out Caller-supplied sixteen-element row-major result matrix.
/// @return Non-zero on success; zero if the node is inaccessible or @p out is `NULL`.
int gltf_node_local_matrix(void *nodes_arr, int32_t node_idx, double *out) {
    void *node_json;
    void *matrix_arr;
    void *translation;
    void *rotation;
    void *scale_arr;
    double pos[3] = {0.0, 0.0, 0.0};
    double quat[4] = {0.0, 0.0, 0.0, 1.0};
    double scale[3] = {1.0, 1.0, 1.0};
    if (!nodes_arr || !out || node_idx < 0 || node_idx >= jarr_len(nodes_arr))
        return 0;
    node_json = rt_seq_get(nodes_arr, node_idx);
    if (!node_json)
        return 0;
    matrix_arr = jarr(node_json, "matrix");
    if (matrix_arr && jarr_len(matrix_arr) >= 16) {
        double m[16];
        for (int i = 0; i < 16; i++)
            m[i] = jvalue_num(rt_seq_get(matrix_arr, (int64_t)i), i % 5 == 0 ? 1.0 : 0.0);
        gltf_matrix_column_major_to_row_major(m, out);
        return 1;
    }
    translation = jarr(node_json, "translation");
    rotation = jarr(node_json, "rotation");
    scale_arr = jarr(node_json, "scale");
    for (int i = 0; i < 3; i++) {
        if (translation && jarr_len(translation) > i)
            pos[i] = jvalue_num(rt_seq_get(translation, i), pos[i]);
        if (scale_arr && jarr_len(scale_arr) > i)
            scale[i] = jvalue_num(rt_seq_get(scale_arr, i), scale[i]);
    }
    for (int i = 0; i < 4; i++) {
        if (rotation && jarr_len(rotation) > i)
            quat[i] = jvalue_num(rt_seq_get(rotation, i), quat[i]);
    }
    for (int i = 0; i < 3; i++) {
        if (!isfinite(pos[i]))
            pos[i] = 0.0;
        if (!isfinite(scale[i]))
            scale[i] = 1.0;
    }
    if (!isfinite(quat[0]) || !isfinite(quat[1]) || !isfinite(quat[2]) ||
        !isfinite(quat[3])) {
        quat[0] = 0.0;
        quat[1] = 0.0;
        quat[2] = 0.0;
        quat[3] = 1.0;
    }
    gltf_build_trs_matrix(pos, quat, scale, out);
    return 1;
}
