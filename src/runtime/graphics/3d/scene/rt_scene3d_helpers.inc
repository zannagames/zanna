//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/scene/rt_scene3d_helpers.inc
// Purpose: Scene3D transform, transactional ownership, node lifecycle, light,
//   and bounds helpers included by rt_scene3d.c.
// Key invariants:
//   - Owner propagation completes traversal allocation before mutating nodes.
//   - Transform and bounds helpers sanitize non-finite private state.
//   - Preserve-world hierarchy edits accept only exactly representable local TRS.
// Ownership/Lifetime:
//   - Traversal arrays are native allocations released in the same helper call.
//   - Runtime object pointers are borrowed unless a helper explicitly retains them.
// Links: rt_scene3d.c, rt_scene3d_internal.h, rt_scene3d_node.c,
//   docs/adr/0166-exact-scenenode-world-matrix-assignment.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements shared Scene3D transform, ownership, bounds, light, and PVS helpers.
/// @details This include supplies allocation-safe iterative traversals, exact
/// preserve-world TRS decomposition, lazy transform propagation, binding sync,
/// geometry revision tracking, world-bounds construction, spatial query
/// primitives, and portal visibility setup for the Scene3D implementation.

/*==========================================================================
 * Helpers
 *=========================================================================*/

/// @brief Build a TRS matrix: Translate * Rotate * Scale (row-major).
/// Quaternion (x,y,z,w) is expanded inline to avoid allocating a Mat4.
/// @param pos Borrowed three-component translation, or `NULL` for zero.
/// @param quat Borrowed four-component quaternion, or `NULL` for identity.
/// @param scl Borrowed three-component scale, or `NULL` for unit scale.
/// @param out Output receiving sixteen row-major matrix lanes.
static void build_trs_matrix(const double *pos,
                             const double *quat,
                             const double *scl,
                             double *out) {
    double p[3] = {pos ? pos[0] : 0.0, pos ? pos[1] : 0.0, pos ? pos[2] : 0.0};
    double q[4] = {
        quat ? quat[0] : 0.0, quat ? quat[1] : 0.0, quat ? quat[2] : 0.0, quat ? quat[3] : 1.0};
    double s[3] = {scl ? scl[0] : 1.0, scl ? scl[1] : 1.0, scl ? scl[2] : 1.0};
    scene3d_sanitize_vec3d(p, 0.0);
    scene3d_sanitize_scale3(s);
    scene3d_quat_normalize_local(q);
    double x = q[0], y = q[1], z = q[2], w = q[3];
    double x2 = x + x, y2 = y + y, z2 = z + z;
    double xx = x * x2, xy = x * y2, xz = x * z2;
    double yy = y * y2, yz = y * z2, zz = z * z2;
    double wx = w * x2, wy = w * y2, wz = w * z2;

    double sx = s[0], sy = s[1], sz = s[2];

    /* R * S (rotation columns scaled) */
    out[0] = (1.0 - (yy + zz)) * sx;
    out[1] = (xy - wz) * sy;
    out[2] = (xz + wy) * sz;
    out[3] = p[0];

    out[4] = (xy + wz) * sx;
    out[5] = (1.0 - (xx + zz)) * sy;
    out[6] = (yz - wx) * sz;
    out[7] = p[1];

    out[8] = (xz - wy) * sx;
    out[9] = (yz + wx) * sy;
    out[10] = (1.0 - (xx + yy)) * sz;
    out[11] = p[2];

    out[12] = 0.0;
    out[13] = 0.0;
    out[14] = 0.0;
    out[15] = 1.0;
}

/// @brief Multiply two 4x4 row-major matrices: out = a * b.
/// @param a Borrowed left matrix.
/// @param b Borrowed right matrix.
/// @param out Output receiving the finite product or identity on invalid input.
static void mat4d_mul(const double *a, const double *b, double *out) {
    double tmp[16];
    if (!out)
        return;
    if (!scene3d_matrix_is_finite(a) || !scene3d_matrix_is_finite(b)) {
        mat4d_identity(out);
        return;
    }
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            double v = a[r * 4 + 0] * b[0 * 4 + c] + a[r * 4 + 1] * b[1 * 4 + c] +
                       a[r * 4 + 2] * b[2 * 4 + c] + a[r * 4 + 3] * b[3 * 4 + c];
            tmp[r * 4 + c] = scene3d_clamp_abs_or(v, r == c ? 1.0 : 0.0);
        }
    }
    memcpy(out, tmp, sizeof(tmp));
}

/// @brief Write an identity 4x4 row-major matrix into @p out.
/// @param out Output receiving sixteen identity-matrix lanes.
void mat4d_identity(double *out) {
    if (!out)
        return;
    memset(out, 0, sizeof(double) * 16);
    out[0] = 1.0;
    out[5] = 1.0;
    out[10] = 1.0;
    out[15] = 1.0;
}

/// @brief Invert a row-major 4x4 matrix using the cofactor expansion.
///
/// Returns 0 on success and writes the inverse into `out`. Returns -1
/// if the matrix is singular (`|det| < 1e-12`); `out` is then untouched.
/// Used to derive parent-from-world transforms and bind-pose inverses.
/// @param m Borrowed finite row-major matrix.
/// @param out Output receiving the inverse on success.
/// @return Zero on success, or `-1` for invalid, singular, or non-finite input.
static int mat4d_invert(const double *m, double *out) {
    double inv[16];
    double det;
    double inv_det;
    if (!out || !scene3d_matrix_is_finite(m))
        return -1;
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] +
             m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] -
             m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] +
             m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] -
              m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] -
             m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] +
             m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] -
             m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] +
              m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] +
             m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] -
             m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] +
              m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] -
              m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] -
             m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] +
             m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] -
              m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] +
              m[8] * m[1] * m[6] - m[8] * m[2] * m[5];

    for (int i = 0; i < 16; i++) {
        if (!isfinite(inv[i]))
            return -1;
    }
    det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (!isfinite(det) || fabs(det) < 1e-12)
        return -1;

    inv_det = 1.0 / det;
    for (int i = 0; i < 16; i++) {
        out[i] = inv[i] * inv_det;
        if (!isfinite(out[i]))
            return -1;
    }
    return 0;
}

/// @brief Set `out` to the identity quaternion `(0, 0, 0, 1)` — no rotation.
/// @param out Output receiving four quaternion components.
static void quat_identity(double *out) {
    if (!out)
        return;
    out[0] = 0.0;
    out[1] = 0.0;
    out[2] = 0.0;
    out[3] = 1.0;
}

/// @brief Renormalise `q` so |q|=1; defaults to identity if it's degenerate.
/// @param q Mutable four-component quaternion; `NULL` is accepted.
void scene3d_quat_normalize_local(double *q) {
    double max_abs;
    double x;
    double y;
    double z;
    double w;
    double len_sq;
    double inv_len;
    if (!q)
        return;
    if (!isfinite(q[0]) || !isfinite(q[1]) || !isfinite(q[2]) || !isfinite(q[3])) {
        quat_identity(q);
        return;
    }
    max_abs = fmax(fmax(fabs(q[0]), fabs(q[1])), fmax(fabs(q[2]), fabs(q[3])));
    if (!isfinite(max_abs) || max_abs < 1e-24) {
        quat_identity(q);
        return;
    }
    x = q[0] / max_abs;
    y = q[1] / max_abs;
    z = q[2] / max_abs;
    w = q[3] / max_abs;
    len_sq = x * x + y * y + z * z + w * w;
    if (!isfinite(len_sq) || len_sq < 1e-20) {
        quat_identity(q);
        return;
    }
    inv_len = 1.0 / sqrt(len_sq);
    q[0] = x * inv_len;
    q[1] = y * inv_len;
    q[2] = z * inv_len;
    q[3] = w * inv_len;
}

/// @brief Square root helper for quaternion extraction, treating invalid terms as zero.
/// @param value Candidate squared quaternion term.
/// @return Square root for a positive finite value, otherwise zero.
static double scene3d_sqrt_nonnegative(double value) {
    if (!isfinite(value) || value <= 0.0)
        return 0.0;
    return sqrt(value);
}

/// @brief Quaternion conjugate for unit-rotation inverse.
/// @param q Borrowed unit quaternion, or `NULL` for identity.
/// @param out Output receiving the conjugate.
static void quat_conjugate_local(const double *q, double *out) {
    if (!out)
        return;
    if (!q) {
        quat_identity(out);
        return;
    }
    out[0] = -q[0];
    out[1] = -q[1];
    out[2] = -q[2];
    out[3] = q[3];
}

/// @brief Quaternion product `out = a * b`.
/// @param a Borrowed left quaternion.
/// @param b Borrowed right quaternion.
/// @param out Output receiving the normalized product or identity.
static void quat_mul_local(const double *a, const double *b, double *out) {
    double ax;
    double ay;
    double az;
    double aw;
    double bx;
    double by;
    double bz;
    double bw;
    if (!out)
        return;
    if (!a || !b) {
        quat_identity(out);
        return;
    }
    ax = a[0];
    ay = a[1];
    az = a[2];
    aw = a[3];
    bx = b[0];
    by = b[1];
    bz = b[2];
    bw = b[3];
    out[0] = aw * bx + ax * bw + ay * bz - az * by;
    out[1] = aw * by - ax * bz + ay * bw + az * bx;
    out[2] = aw * bz + ax * by - ay * bx + az * bw;
    out[3] = aw * bw - ax * bx - ay * by - az * bz;
    scene3d_quat_normalize_local(out);
}

/// @brief Transform a point by a row-major 4x4 matrix with translation in column 3.
/// @param m Borrowed finite row-major matrix.
/// @param point Borrowed three-component point.
/// @param out Output receiving the sanitized transformed point.
static void mat4d_transform_point(const double *m, const double *point, double *out) {
    double p[3] = {point ? point[0] : 0.0, point ? point[1] : 0.0, point ? point[2] : 0.0};
    double x;
    double y;
    double z;
    if (!out)
        return;
    scene3d_sanitize_vec3d(p, 0.0);
    if (!scene3d_matrix_is_finite(m)) {
        out[0] = p[0];
        out[1] = p[1];
        out[2] = p[2];
        return;
    }
    x = m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3];
    y = m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7];
    z = m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11];
    out[0] = scene3d_clamp_abs_or(x, 0.0);
    out[1] = scene3d_clamp_abs_or(y, 0.0);
    out[2] = scene3d_clamp_abs_or(z, 0.0);
}

/// @brief Extract a unit quaternion from the rotation part of a row-major matrix.
///
/// Picks the largest of four possible diagonal terms and uses
/// the corresponding extraction formula — Shepperd's method —
/// for numerical stability across the full sphere of orientations.
/// @param m00 Rotation row 0, column 0.
/// @param m01 Rotation row 0, column 1.
/// @param m02 Rotation row 0, column 2.
/// @param m10 Rotation row 1, column 0.
/// @param m11 Rotation row 1, column 1.
/// @param m12 Rotation row 1, column 2.
/// @param m20 Rotation row 2, column 0.
/// @param m21 Rotation row 2, column 1.
/// @param m22 Rotation row 2, column 2.
/// @param out Output receiving a normalized quaternion.
static void quat_from_matrix_rows(double m00,
                                  double m01,
                                  double m02,
                                  double m10,
                                  double m11,
                                  double m12,
                                  double m20,
                                  double m21,
                                  double m22,
                                  double *out) {
    double trace;
    if (!out)
        return;
    trace = m00 + m11 + m22;
    if (trace > 0.0) {
        double s = scene3d_sqrt_nonnegative(trace + 1.0) * 2.0;
        if (s <= 1e-12) {
            quat_identity(out);
            return;
        }
        out[3] = 0.25 * s;
        out[0] = (m21 - m12) / s;
        out[1] = (m02 - m20) / s;
        out[2] = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        double s = scene3d_sqrt_nonnegative(1.0 + m00 - m11 - m22) * 2.0;
        if (s <= 1e-12) {
            quat_identity(out);
            return;
        }
        out[3] = (m21 - m12) / s;
        out[0] = 0.25 * s;
        out[1] = (m01 + m10) / s;
        out[2] = (m02 + m20) / s;
    } else if (m11 > m22) {
        double s = scene3d_sqrt_nonnegative(1.0 + m11 - m00 - m22) * 2.0;
        if (s <= 1e-12) {
            quat_identity(out);
            return;
        }
        out[3] = (m02 - m20) / s;
        out[0] = (m01 + m10) / s;
        out[1] = 0.25 * s;
        out[2] = (m12 + m21) / s;
    } else {
        double s = scene3d_sqrt_nonnegative(1.0 + m22 - m00 - m11) * 2.0;
        if (s <= 1e-12) {
            quat_identity(out);
            return;
        }
        out[3] = (m10 - m01) / s;
        out[0] = (m02 + m20) / s;
        out[1] = (m12 + m21) / s;
        out[2] = 0.25 * s;
    }
    scene3d_quat_normalize_local(out);
}

/// @brief Strip translation/scale and call `quat_from_matrix_rows` to recover the rotation
/// quaternion.
///
/// Used when reading back world-space orientation after a node's
/// world matrix has been composed from parent transforms — we need
/// the rotation back as a quaternion for further composition.
/// @param m Borrowed finite row-major transform matrix.
/// @param basis Output receiving an orthonormal three-by-three basis.
/// @return Nonzero when a finite non-degenerate rotation basis was recovered.
static int scene_extract_rotation_basis(const double *m, double basis[9]) {
    double rx;
    double ry;
    double rz;
    double ux;
    double uy;
    double uz;
    double fx;
    double fy;
    double fz;
    double rlen;
    double ulen;
    double flen;
    double det;
    if (!m || !basis)
        return 0;
    rx = m[0];
    ry = m[4];
    rz = m[8];
    ux = m[1];
    uy = m[5];
    uz = m[9];
    fx = m[2];
    fy = m[6];
    fz = m[10];
    rlen = sqrt(rx * rx + ry * ry + rz * rz);
    ulen = sqrt(ux * ux + uy * uy + uz * uz);
    flen = sqrt(fx * fx + fy * fy + fz * fz);
    if (!isfinite(rlen) || !isfinite(ulen) || !isfinite(flen) || rlen < 1e-12 || ulen < 1e-12 ||
        flen < 1e-12)
        return 0;
    rx /= rlen;
    ry /= rlen;
    rz /= rlen;
    ux /= ulen;
    uy /= ulen;
    uz /= ulen;
    fx /= flen;
    fy /= flen;
    fz /= flen;
    det = rx * (uy * fz - uz * fy) - ux * (ry * fz - rz * fy) + fx * (ry * uz - rz * uy);
    if (!isfinite(det))
        return 0;
    if (det < 0.0) {
        if (rlen >= ulen && rlen >= flen) {
            rx = -rx;
            ry = -ry;
            rz = -rz;
        } else if (ulen >= flen) {
            ux = -ux;
            uy = -uy;
            uz = -uz;
        } else {
            fx = -fx;
            fy = -fy;
            fz = -fz;
        }
    }
    basis[0] = rx;
    basis[1] = ux;
    basis[2] = fx;
    basis[3] = ry;
    basis[4] = uy;
    basis[5] = fy;
    basis[6] = rz;
    basis[7] = uz;
    basis[8] = fz;
    return 1;
}

/// @brief Extract a unit quaternion from a world matrix's rotation, normalizing the basis
///   first; falls back to identity when the matrix has no recoverable rotation.
/// @param m Borrowed world matrix.
/// @param out Output receiving a normalized quaternion or identity.
static void quat_from_world_matrix(const double *m, double *out) {
    double basis[9];
    if (!out)
        return;
    if (!scene_extract_rotation_basis(m, basis)) {
        quat_identity(out);
        return;
    }
    quat_from_matrix_rows(basis[0],
                          basis[1],
                          basis[2],
                          basis[3],
                          basis[4],
                          basis[5],
                          basis[6],
                          basis[7],
                          basis[8],
                          out);
}

/// @brief Push @p node onto an iterative-traversal node stack, growing it geometrically.
/// @details Used by walk functions that prefer iterative traversal over recursion
///   (avoids stack overflow on deep scene graphs).
///   Returns 1 on success or no-op (NULL node), 0 on overflow / OOM.
/// @param stack Address of the caller-owned mutable node-pointer stack.
/// @param count In/out populated entry count.
/// @param capacity In/out allocated entry capacity.
/// @param node Borrowed candidate node to validate and append.
/// @return Nonzero on append or safe no-op, zero on growth failure.
int scene_node_stack_push(rt_scene_node3d ***stack,
                          size_t *count,
                          size_t *capacity,
                          rt_scene_node3d *node) {
    rt_scene_node3d *checked;
    if (!stack || !count || !capacity || !node)
        return 1;
    checked = scene_node3d_checked(node);
    if (!checked)
        return 1;
    if (*count >= *capacity) {
        if (!scene3d_grow_stack_storage((void **)stack, capacity, sizeof(**stack)))
            return 0;
    }
    (*stack)[(*count)++] = checked;
    return 1;
}

/// @brief `scene_node_stack_push` for read-only `const` traversals.
/// @details Same growth contract; the const variant is needed because the C type
///   system disallows aliasing a `const T**` as a `T**` even when only reads happen.
/// @param stack Address of the caller-owned immutable-node stack.
/// @param count In/out populated entry count.
/// @param capacity In/out allocated entry capacity.
/// @param node Borrowed node to append.
/// @return Nonzero on append or safe no-op, zero on growth failure.
static int scene_node_const_stack_push(const rt_scene_node3d ***stack,
                                       size_t *count,
                                       size_t *capacity,
                                       const rt_scene_node3d *node) {
    if (!stack || !count || !capacity || !node)
        return 1;
    if (*count >= *capacity) {
        if (!scene3d_grow_stack_storage((void **)stack, capacity, sizeof(**stack)))
            return 0;
    }
    (*stack)[(*count)++] = node;
    return 1;
}

static int32_t g_scene3d_test_owner_growth_failure_after = -1;

/// @brief Configure deterministic owner-transaction allocation fault injection for tests.
/// @param successful_growths Number of successful growths allowed before failure, or `-1` to
/// disable.
void rt_scene3d_test_set_owner_growth_failure(int32_t successful_growths) {
    g_scene3d_test_owner_growth_failure_after = successful_growths < -1 ? -1 : successful_growths;
}

/// @brief Grow one owner transaction, honoring the deterministic test-only failure countdown.
/// @param transaction Borrowed transaction whose native node array may grow.
/// @return `1` when the buffer can accept another node, otherwise `0` without changing it.
static int scene_owner_transaction_grow(scene_owner_transaction *transaction) {
    if (!transaction)
        return 0;
    if (g_scene3d_test_owner_growth_failure_after == 0)
        return 0;
    if (g_scene3d_test_owner_growth_failure_after > 0)
        g_scene3d_test_owner_growth_failure_after--;
    return scene3d_grow_stack_storage(
        (void **)&transaction->nodes, &transaction->capacity, sizeof(transaction->nodes[0]));
}

/// @brief Append one validated node to a preflight owner transaction.
/// @param transaction Borrowed transaction to extend.
/// @param node Borrowed validated node; `NULL` is a successful no-op.
/// @return Nonzero on append/no-op, or zero on size or allocation failure.
static int scene_owner_transaction_append(scene_owner_transaction *transaction,
                                          rt_scene_node3d *node) {
    if (!transaction || !node)
        return 1;
    if (transaction->count >= RT_SCENE3D_MAX_OWNER_TRANSACTION_NODES)
        return 0;
    if (transaction->count >= transaction->capacity && !scene_owner_transaction_grow(transaction))
        return 0;
    transaction->nodes[transaction->count++] = node;
    return 1;
}

/// @brief Collect a complete subtree breadth-first before publishing any owner changes.
/// @param node Borrowed subtree root; invalid or null roots produce an empty transaction.
/// @param transaction Output transaction owning the collected pointer array.
/// @return Nonzero when preflight collection completes, otherwise zero with storage discarded.
int scene_node_owner_transaction_prepare(rt_scene_node3d *node,
                                         scene_owner_transaction *transaction) {
    size_t cursor = 0;
    if (!transaction)
        return 0;
    memset(transaction, 0, sizeof(*transaction));
    node = scene_node3d_checked(node);
    if (!node)
        return 1;
    if (!scene_owner_transaction_append(transaction, node))
        goto fail;
    while (cursor < transaction->count) {
        rt_scene_node3d *current = transaction->nodes[cursor++];
        int32_t child_count = scene3d_node_child_count(current);
        for (int32_t i = 0; i < child_count; ++i) {
            rt_scene_node3d *child = scene_node3d_checked(current->children[i]);
            if (child && !scene_owner_transaction_append(transaction, child))
                goto fail;
        }
    }
    return 1;

fail:
    scene_node_owner_transaction_discard(transaction);
    return 0;
}

/// @brief Release a prepared owner transaction without publishing changes.
/// @param transaction Borrowed transaction whose native storage is freed and reset.
void scene_node_owner_transaction_discard(scene_owner_transaction *transaction) {
    if (!transaction)
        return;
    free(transaction->nodes);
    memset(transaction, 0, sizeof(*transaction));
}

/// @brief Publish one prepared owner assignment without allocation or failure paths.
/// @param transaction Borrowed prepared complete-subtree transaction.
/// @param owner Borrowed Scene3D assigned to every collected node, or `NULL`.
void scene_node_owner_transaction_assign(scene_owner_transaction *transaction, rt_scene3d *owner) {
    if (!transaction)
        return;
    for (size_t i = 0; i < transaction->count; ++i)
        transaction->nodes[i]->owner_scene = owner;
}

/// @brief Publish one prepared conditional owner clear without allocation.
/// @param transaction Borrowed prepared complete-subtree transaction.
/// @param owner Borrowed owner cleared only from matching nodes.
void scene_node_owner_transaction_clear(scene_owner_transaction *transaction, rt_scene3d *owner) {
    if (!transaction || !owner)
        return;
    for (size_t i = 0; i < transaction->count; ++i) {
        if (transaction->nodes[i]->owner_scene == owner)
            transaction->nodes[i]->owner_scene = NULL;
    }
}

/// @brief Clear matching descendant owners while preserving the transaction root owner.
/// @param transaction Borrowed prepared subtree transaction.
/// @param owner Borrowed owner cleared from entries after the root.
void scene_node_owner_transaction_clear_descendants(scene_owner_transaction *transaction,
                                                    rt_scene3d *owner) {
    if (!transaction || !owner)
        return;
    for (size_t i = 1; i < transaction->count; ++i) {
        if (transaction->nodes[i]->owner_scene == owner)
            transaction->nodes[i]->owner_scene = NULL;
    }
}

/// @brief Transactionally assign one owner to a complete subtree.
/// @param node Borrowed subtree root.
/// @param owner Borrowed Scene3D assigned to every node, or `NULL`.
/// @return Nonzero after complete assignment or an empty no-op; zero on preflight failure.
int scene_node_assign_owner_recursive(rt_scene_node3d *node, rt_scene3d *owner) {
    scene_owner_transaction transaction;
    if (!node)
        return 1;
    if (!scene_node_owner_transaction_prepare(node, &transaction)) {
        rt_trap("Scene3D: owner traversal stack allocation failed");
        return 0;
    }
    scene_node_owner_transaction_assign(&transaction, owner);
    scene_node_owner_transaction_discard(&transaction);
    return 1;
}

/// @brief Transactionally clear a matching owner from a complete subtree.
/// @param node Borrowed subtree root.
/// @param owner Borrowed owner cleared only where it matches.
/// @return Nonzero after complete clearing or an empty no-op; zero on preflight failure.
int scene_node_clear_owner_recursive(rt_scene_node3d *node, rt_scene3d *owner) {
    scene_owner_transaction transaction;
    if (!node || !owner)
        return 1;
    if (!scene_node_owner_transaction_prepare(node, &transaction)) {
        rt_trap("Scene3D: owner traversal stack allocation failed");
        return 0;
    }
    scene_node_owner_transaction_clear(&transaction, owner);
    scene_node_owner_transaction_discard(&transaction);
    return 1;
}

/// @brief Mark a node's local world transform dirty.
///
/// Descendants do not need eager dirty propagation. Each node tracks the parent
/// world-matrix revision it last consumed, so a child lazily notices parent
/// changes during `recompute_world_matrix` without allocating a traversal stack.
/// @param node Borrowed node whose world transform and spatial bounds become stale.
void mark_dirty(rt_scene_node3d *node) {
    if (!node)
        return;
    node->world_dirty = 1;
    scene3d_mark_spatial_refit_dirty(node->owner_scene);
}

/// @brief Recompute a single node's world matrix after its parent has already been refreshed.
/// @details This is the non-recursive body used by `recompute_world_matrix`. It keeps
///          the existing parent-revision dirty check, but assumes parent matrices are
///          current so deep scene graphs do not consume the C call stack.
/// @param node Borrowed node whose parent is already current.
static void recompute_world_matrix_single(rt_scene_node3d *node) {
    double local[16];
    uint32_t parent_revision = 0;
    if (!node)
        return;
    scene3d_repair_node_transform(node);

    if (node->parent) {
        parent_revision = node->parent->world_revision;
        if (node->parent_world_revision_seen != parent_revision)
            node->world_dirty = 1;
    }

    if (!node->world_dirty && scene3d_matrix_is_finite(node->world_matrix))
        return;

    build_trs_matrix(node->position, node->rotation, node->scale_xyz, local);
    {
        double next_world[16];
        if (node->parent)
            mat4d_mul(node->parent->world_matrix, local, next_world);
        else
            memcpy(next_world, local, sizeof(double) * 16);
        if (!scene3d_matrix_is_finite(next_world))
            mat4d_identity(next_world);
        node->parent_world_revision_seen = parent_revision;
        node->world_dirty = 0;
        /* Bump the revision only when the matrix actually changed: children
         * compare parent_world_revision_seen and the spatial index refits by
         * revision, so a recompute that lands on the identical matrix (parent
         * refreshed to the same pose, redundant dirty) must not cascade
         * recomputes and BVH refits through the whole subtree. */
        if (memcmp(next_world, node->world_matrix, sizeof(next_world)) != 0) {
            memcpy(node->world_matrix, next_world, sizeof(next_world));
            node->world_revision =
                node->world_revision == UINT32_MAX ? 1u : node->world_revision + 1u;
        }
    }
}

/// @brief Recompute the world matrix if local or parent state changed.
/// @details Walks the parent chain into a scene-owned reusable stack, then refreshes from the
///          highest ancestor down to @p node. This preserves lazy parent revision
///          tracking while avoiding recursive calls and per-call heap allocation on normal
///          scene-owned nodes.
/// @param node Borrowed node whose ancestor chain and world matrix are refreshed.
void recompute_world_matrix(rt_scene_node3d *node) {
    rt_scene_node3d **stack = NULL;
    size_t count = 0;
    size_t capacity = 0;
    rt_scene_node3d *current = scene_node3d_checked(node);
    rt_scene3d *owner_scene = current ? scene3d_checked(current->owner_scene) : NULL;
    if (owner_scene) {
        while (current) {
            rt_scene_node3d *parent;
            if (!scene_node_stack_push(&owner_scene->world_matrix_stack,
                                       &count,
                                       &owner_scene->world_matrix_stack_capacity,
                                       current)) {
                rt_trap("Scene3D: world-matrix traversal stack allocation failed");
                return;
            }
            parent = scene_node3d_checked(current->parent);
            if (!parent)
                break;
            current = parent;
        }
        while (count > 0)
            recompute_world_matrix_single(owner_scene->world_matrix_stack[--count]);
        return;
    }
    while (current) {
        rt_scene_node3d *parent;
        if (!scene_node_stack_push(&stack, &count, &capacity, current)) {
            rt_trap("Scene3D: world-matrix traversal stack allocation failed");
            free(stack);
            return;
        }
        parent = scene_node3d_checked(current->parent);
        if (!parent)
            break;
        current = parent;
    }
    while (count > 0)
        recompute_world_matrix_single(stack[--count]);
    free(stack);
}

/// @brief Length of one affine basis column without overflowing intermediate squares.
/// @param x Basis-column X component.
/// @param y Basis-column Y component.
/// @param z Basis-column Z component.
/// @return Stable Euclidean length, zero for zero input, or NaN for invalid input.
static double scene3d_preserve_basis_length(double x, double y, double z) {
    double max_abs = fmax(fabs(x), fmax(fabs(y), fabs(z)));
    if (!isfinite(max_abs) || max_abs <= 0.0)
        return max_abs == 0.0 ? 0.0 : NAN;
    x /= max_abs;
    y /= max_abs;
    z /= max_abs;
    return max_abs * sqrt(x * x + y * y + z * z);
}

/// @brief Floating-point equality used to prove a TRS decomposition is lossless.
/// @details The absolute floor handles values around zero. The scaled DBL_EPSILON
///          term permits ordinary inversion/quaternion round-off without accepting
///          visible shear or a materially different authored transform.
/// @param a Borrowed first sixteen-lane matrix.
/// @param b Borrowed second sixteen-lane matrix.
/// @return Nonzero when every lane agrees within the losslessness tolerance.
int scene3d_preserve_matrix_equal(const double a[16], const double b[16]) {
    if (!scene3d_matrix_is_finite(a) || !scene3d_matrix_is_finite(b))
        return 0;
    for (int i = 0; i < 16; ++i) {
        double magnitude = fmax(1.0, fmax(fabs(a[i]), fabs(b[i])));
        double tolerance = 1e-10 + 4096.0 * DBL_EPSILON * magnitude;
        if (fabs(a[i] - b[i]) > tolerance)
            return 0;
    }
    return 1;
}

/// @brief Derive an exact local TRS for one requested complete world matrix.
/// @details SceneNode stores TRS rather than a general affine matrix. The inverse
///          parent transform may therefore yield shear, which cannot be represented
///          without changing the requested world pose. This helper rejects singular
///          parents, projective matrices, degenerate bases, shear, and decomposition
///          drift before callers mutate node state. A negative determinant is carried
///          by the largest-magnitude scale axis so reflected transforms remain exact.
/// @param parent Borrowed prospective parent, or `NULL` for a root transform.
/// @param desired_world Borrowed complete requested world matrix.
/// @param out_position Output receiving exact local translation.
/// @param out_rotation Output receiving exact local unit quaternion.
/// @param out_scale Output receiving exact local scale, including reflection sign.
/// @return Nonzero only when rebuilding the derived TRS reproduces the requested world matrix.
int scene_node_compute_local_trs_for_world_matrix(rt_scene_node3d *parent,
                                                  const double desired_world[16],
                                                  double out_position[3],
                                                  double out_rotation[4],
                                                  double out_scale[3]) {
    double inverse_parent[16];
    double local[16];
    double rebuilt_local[16];
    double rebuilt_world[16];
    double basis[9];
    double lengths[3];
    double dot_xy;
    double dot_xz;
    double dot_yz;
    double determinant;
    int reflected_axis = 0;

    if (!desired_world || !out_position || !out_rotation || !out_scale ||
        !scene3d_matrix_is_finite(desired_world))
        return 0;
    if (parent) {
        recompute_world_matrix(parent);
        if (!scene3d_matrix_is_finite(parent->world_matrix) ||
            mat4d_invert(parent->world_matrix, inverse_parent) != 0)
            return 0;
        mat4d_mul(inverse_parent, desired_world, local);
    } else {
        memcpy(local, desired_world, sizeof(local));
    }

    if (fabs(local[12]) > 1e-10 || fabs(local[13]) > 1e-10 || fabs(local[14]) > 1e-10 ||
        fabs(local[15] - 1.0) > 1e-10)
        return 0;
    lengths[0] = scene3d_preserve_basis_length(local[0], local[4], local[8]);
    lengths[1] = scene3d_preserve_basis_length(local[1], local[5], local[9]);
    lengths[2] = scene3d_preserve_basis_length(local[2], local[6], local[10]);
    if (!isfinite(lengths[0]) || !isfinite(lengths[1]) || !isfinite(lengths[2]) ||
        lengths[0] <= 1e-12 || lengths[1] <= 1e-12 || lengths[2] <= 1e-12)
        return 0;

    basis[0] = local[0] / lengths[0];
    basis[1] = local[1] / lengths[1];
    basis[2] = local[2] / lengths[2];
    basis[3] = local[4] / lengths[0];
    basis[4] = local[5] / lengths[1];
    basis[5] = local[6] / lengths[2];
    basis[6] = local[8] / lengths[0];
    basis[7] = local[9] / lengths[1];
    basis[8] = local[10] / lengths[2];
    dot_xy = basis[0] * basis[1] + basis[3] * basis[4] + basis[6] * basis[7];
    dot_xz = basis[0] * basis[2] + basis[3] * basis[5] + basis[6] * basis[8];
    dot_yz = basis[1] * basis[2] + basis[4] * basis[5] + basis[7] * basis[8];
    if (!isfinite(dot_xy) || !isfinite(dot_xz) || !isfinite(dot_yz) || fabs(dot_xy) > 1e-10 ||
        fabs(dot_xz) > 1e-10 || fabs(dot_yz) > 1e-10)
        return 0;

    determinant = basis[0] * (basis[4] * basis[8] - basis[5] * basis[7]) -
                  basis[1] * (basis[3] * basis[8] - basis[5] * basis[6]) +
                  basis[2] * (basis[3] * basis[7] - basis[4] * basis[6]);
    if (!isfinite(determinant) || fabs(fabs(determinant) - 1.0) > 1e-10)
        return 0;
    if (determinant < 0.0) {
        if (lengths[1] > lengths[reflected_axis])
            reflected_axis = 1;
        if (lengths[2] > lengths[reflected_axis])
            reflected_axis = 2;
        lengths[reflected_axis] = -lengths[reflected_axis];
        basis[reflected_axis] = -basis[reflected_axis];
        basis[3 + reflected_axis] = -basis[3 + reflected_axis];
        basis[6 + reflected_axis] = -basis[6 + reflected_axis];
    }

    out_position[0] = local[3];
    out_position[1] = local[7];
    out_position[2] = local[11];
    out_scale[0] = lengths[0];
    out_scale[1] = lengths[1];
    out_scale[2] = lengths[2];
    quat_from_matrix_rows(basis[0],
                          basis[1],
                          basis[2],
                          basis[3],
                          basis[4],
                          basis[5],
                          basis[6],
                          basis[7],
                          basis[8],
                          out_rotation);
    build_trs_matrix(out_position, out_rotation, out_scale, rebuilt_local);
    if (!scene3d_preserve_matrix_equal(local, rebuilt_local))
        return 0;
    if (!parent)
        return scene3d_preserve_matrix_equal(desired_world, rebuilt_local);
    mat4d_mul(parent->world_matrix, rebuilt_local, rebuilt_world);
    return scene3d_preserve_matrix_equal(desired_world, rebuilt_world);
}

/// @brief Derive the local TRS that preserves one child's complete world matrix.
/// @param new_parent Borrowed prospective parent.
/// @param child Borrowed child whose current world matrix is preserved.
/// @param out_position Output receiving exact local translation.
/// @param out_rotation Output receiving exact local unit quaternion.
/// @param out_scale Output receiving exact local scale.
/// @return Nonzero when the preserved world matrix has an exact parent-relative TRS.
int scene_node_compute_preserved_local_trs(rt_scene_node3d *new_parent,
                                           rt_scene_node3d *child,
                                           double out_position[3],
                                           double out_rotation[4],
                                           double out_scale[3]) {
    double desired_world[16];

    if (!new_parent || !child || !out_position || !out_rotation || !out_scale)
        return 0;
    recompute_world_matrix(child);
    if (!scene3d_matrix_is_finite(child->world_matrix))
        return 0;
    memcpy(desired_world, child->world_matrix, sizeof(desired_world));
    return scene_node_compute_local_trs_for_world_matrix(
        new_parent, desired_world, out_position, out_rotation, out_scale);
}

/// @brief Count nodes in a subtree (including the root).
/// @param node Borrowed subtree root.
/// @return Non-negative node count, saturating at `INT32_MAX`.
static int32_t count_subtree(const rt_scene_node3d *node) {
    const rt_scene_node3d **stack = NULL;
    size_t count = 0;
    size_t capacity = 0;
    int32_t total = 0;
    const rt_scene_node3d *root = scene_node3d_checked_const(node);
    if (!root)
        return 0;
    if (!scene_node_const_stack_push(&stack, &count, &capacity, root))
        return INT32_MAX;
    while (count > 0) {
        const rt_scene_node3d *current = stack[--count];
        if (total == INT32_MAX) {
            free(stack);
            return INT32_MAX;
        }
        total++;
        for (int32_t i = 0, child_count = scene3d_node_child_count(current); i < child_count; i++) {
            const rt_scene_node3d *child = scene_node3d_checked(current->children[i]);
            if (!scene_node_const_stack_push(&stack, &count, &capacity, child)) {
                free(stack);
                return INT32_MAX;
            }
        }
    }
    free(stack);
    return total;
}

/// @brief Compose `node->world_matrix` from its ancestors and read the translation column.
/// @param node Borrowed SceneNode3D handle.
/// @param x Output receiving world X, initialized to zero.
/// @param y Output receiving world Y, initialized to zero.
/// @param z Output receiving world Z, initialized to zero.
void scene_node_get_world_position(rt_scene_node3d *node, double *x, double *y, double *z) {
    if (!x || !y || !z) {
        return;
    }
    *x = 0.0;
    *y = 0.0;
    *z = 0.0;
    if (!node)
        return;
    recompute_world_matrix(node);
    *x = scene3d_clamp_abs_or(node->world_matrix[3], 0.0);
    *y = scene3d_clamp_abs_or(node->world_matrix[7], 0.0);
    *z = scene3d_clamp_abs_or(node->world_matrix[11], 0.0);
}

/// @brief Read this node's world-space rotation as a quaternion (composing parent rotations).
/// @param node Borrowed SceneNode3D handle.
/// @param out_quat Output receiving the unit world quaternion or identity.
void scene_node_get_world_rotation(rt_scene_node3d *node, double *out_quat) {
    if (!out_quat) {
        return;
    }
    quat_identity(out_quat);
    if (!node)
        return;
    recompute_world_matrix(node);
    quat_from_world_matrix(node->world_matrix, out_quat);
}

/// @brief Set a node's world-space TRS, working backward through parents to update local TRS.
///
/// Inverts the parent chain's world matrix and applies it to the
/// requested world transform so the resulting local transform
/// produces the requested world pose. Used by physics → scene
/// sync to apply rigid-body world poses to scene nodes.
/// @param node Borrowed node whose local position/rotation are updated.
/// @param world_pos Borrowed requested world-space position.
/// @param world_quat Borrowed requested world-space quaternion.
static void scene_node_set_world_transform(rt_scene_node3d *node,
                                           const double *world_pos,
                                           const double *world_quat) {
    double world_rot[4];
    double inv_parent[16];
    if (!node || !world_pos || !world_quat)
        return;
    world_rot[0] = world_quat[0];
    world_rot[1] = world_quat[1];
    world_rot[2] = world_quat[2];
    world_rot[3] = world_quat[3];
    scene3d_quat_normalize_local(world_rot);

    if (!node->parent) {
        node->position[0] = scene3d_clamp_abs_or(world_pos[0], 0.0);
        node->position[1] = scene3d_clamp_abs_or(world_pos[1], 0.0);
        node->position[2] = scene3d_clamp_abs_or(world_pos[2], 0.0);
        node->rotation[0] = world_rot[0];
        node->rotation[1] = world_rot[1];
        node->rotation[2] = world_rot[2];
        node->rotation[3] = world_rot[3];
        mark_dirty(node);
        return;
    }

    recompute_world_matrix(node->parent);
    {
        double safe_world_pos[3] = {scene3d_clamp_abs_or(world_pos[0], 0.0),
                                    scene3d_clamp_abs_or(world_pos[1], 0.0),
                                    scene3d_clamp_abs_or(world_pos[2], 0.0)};
        if (mat4d_invert(node->parent->world_matrix, inv_parent) == 0) {
            mat4d_transform_point(inv_parent, safe_world_pos, node->position);
        } else {
            memcpy(node->position, safe_world_pos, sizeof(node->position));
        }
    }
    {
        double parent_world_rot[4];
        double inv_parent_rot[4];
        scene_node_get_world_rotation(node->parent, parent_world_rot);
        quat_conjugate_local(parent_world_rot, inv_parent_rot);
        quat_mul_local(inv_parent_rot, world_rot, node->rotation);
    }
    node->position[0] = scene3d_clamp_abs_or(node->position[0], 0.0);
    node->position[1] = scene3d_clamp_abs_or(node->position[1], 0.0);
    node->position[2] = scene3d_clamp_abs_or(node->position[2], 0.0);
    node->scale_xyz[0] = scene3d_scale_or_unit(node->scale_xyz[0]);
    node->scale_xyz[1] = scene3d_scale_or_unit(node->scale_xyz[1]);
    node->scale_xyz[2] = scene3d_scale_or_unit(node->scale_xyz[2]);

    mark_dirty(node);
}

/// @brief Rotate a 3-vector by a unit quaternion: v' = v + 2*w*(q x v) + 2*(q x (q x v)).
/// @param q Borrowed unit quaternion.
/// @param v Borrowed three-component vector.
/// @param out Output receiving the rotated vector.
static void scene_node_quat_rotate_vec(const double q[4], const double v[3], double out[3]) {
    double tx = 2.0 * (q[1] * v[2] - q[2] * v[1]);
    double ty = 2.0 * (q[2] * v[0] - q[0] * v[2]);
    double tz = 2.0 * (q[0] * v[1] - q[1] * v[0]);

    out[0] = v[0] + q[3] * tx + (q[1] * tz - q[2] * ty);
    out[1] = v[1] + q[3] * ty + (q[2] * tx - q[0] * tz);
    out[2] = v[2] + q[3] * tz + (q[0] * ty - q[1] * tx);
}

/// @brief Drive a socketed node's world transform from its bone binding.
/// @details World pose = parentWorld x bonePose x socketOffset, composed as
///   position + rotation (bone scale is normalized away; socket children do
///   not inherit skeletal scale). The bone pose is model-space, so the node
///   must be parented under the node that renders the skinned model for the
///   composition to land in the right space.
/// @param node Borrowed socketed node whose local transform is updated.
static void scene_node_apply_bone_socket(rt_scene_node3d *node) {
    void *animator;
    double bone_pos[3];
    double bone_quat[4];
    double parent_pos[3] = {0.0, 0.0, 0.0};
    double parent_quat[4];
    double model_pos[3];
    double model_quat[4];
    double rotated[3];
    double world_pos[3];
    double world_quat[4];

    if (!node)
        return;
    animator = rt_g3d_checked_or_null(node->socket_animator, RT_G3D_ANIMCONTROLLER3D_CLASS_ID);
    if (!animator)
        return;
    if (!rt_anim_controller3d_get_bone_pose(
            animator, (int64_t)node->socket_bone, bone_pos, bone_quat))
        return;

    /* Socket offset in bone space -> model space. */
    scene_node_quat_rotate_vec(bone_quat, node->socket_offset_pos, rotated);
    model_pos[0] = bone_pos[0] + rotated[0];
    model_pos[1] = bone_pos[1] + rotated[1];
    model_pos[2] = bone_pos[2] + rotated[2];
    quat_mul_local(bone_quat, node->socket_offset_quat, model_quat);

    /* Model space -> world space through the parent (skinned model) node.
     * The parent's world SCALE must apply to the model-space offset: a
     * centimetre-rigged model under an entity-level unit scale otherwise
     * lands its socketed props ~30x away from the hand (ZB-15 — no bat or
     * glove was ever visible). Per-axis scale composes before rotation
     * (world = T * R * S * model). */
    quat_identity(parent_quat);
    if (node->parent) {
        double psx = 1.0;
        double psy = 1.0;
        double psz = 1.0;
        scene_node_get_world_position(node->parent, &parent_pos[0], &parent_pos[1], &parent_pos[2]);
        scene_node_get_world_rotation(node->parent, parent_quat);
        if (rt_scene_node3d_get_world_scale_components(node->parent, &psx, &psy, &psz)) {
            model_pos[0] *= psx;
            model_pos[1] *= psy;
            model_pos[2] *= psz;
        }
    }
    scene_node_quat_rotate_vec(parent_quat, model_pos, rotated);
    world_pos[0] = parent_pos[0] + rotated[0];
    world_pos[1] = parent_pos[1] + rotated[1];
    world_pos[2] = parent_pos[2] + rotated[2];
    quat_mul_local(parent_quat, model_quat, world_quat);
    scene_node_set_world_transform(node, world_pos, world_quat);
}

/// @brief Shift an entire root-level subtree by subtracting a world-space delta.
/// @param node Borrowed root-level subtree node.
/// @param delta Borrowed three-component origin shift.
static void scene_node_rebase_root_child(rt_scene_node3d *node, const double delta[3]) {
    double world_pos[3];
    double world_rot[4];
    if (!node || !delta)
        return;
    node = scene_node3d_checked(node);
    if (!node)
        return;
    scene_node_get_world_position(node, &world_pos[0], &world_pos[1], &world_pos[2]);
    scene_node_get_world_rotation(node, world_rot);
    world_pos[0] = scene3d_clamp_abs_or(world_pos[0] - delta[0], 0.0);
    world_pos[1] = scene3d_clamp_abs_or(world_pos[1] - delta[1], 0.0);
    world_pos[2] = scene3d_clamp_abs_or(world_pos[2] - delta[2], 0.0);
    scene_node_set_world_transform(node, world_pos, world_rot);
}

/// @brief Move the node by the per-frame "root motion" delta produced by the bound animator.
///
/// Animations whose root bone moves (walk cycles, jumps) report the
/// per-frame translation/rotation as a delta; here we apply it to
/// the node's local transform so the character actually traverses.
/// @param node Borrowed node whose bound animator root motion is consumed.
static void scene_node_apply_root_motion(rt_scene_node3d *node) {
    void *delta;
    void *delta_rot;
    void *node_rot;
    void *combined_rot;
    void *animator =
        node ? rt_g3d_checked_or_null(node->bound_animator, RT_G3D_ANIMCONTROLLER3D_CLASS_ID)
             : NULL;
    if (!node || !animator)
        return;
    delta = rt_anim_controller3d_consume_root_motion(animator);
    delta_rot = rt_anim_controller3d_consume_root_motion_rotation(animator);
    if (!delta) {
        scene3d_release_ref(&delta_rot);
        return;
    }
    node->position[0] = scene3d_clamp_abs_or(scene3d_clamp_abs_or(node->position[0], 0.0) +
                                                 scene3d_clamp_abs_or(rt_vec3_x(delta), 0.0),
                                             0.0);
    node->position[1] = scene3d_clamp_abs_or(scene3d_clamp_abs_or(node->position[1], 0.0) +
                                                 scene3d_clamp_abs_or(rt_vec3_y(delta), 0.0),
                                             0.0);
    node->position[2] = scene3d_clamp_abs_or(scene3d_clamp_abs_or(node->position[2], 0.0) +
                                                 scene3d_clamp_abs_or(rt_vec3_z(delta), 0.0),
                                             0.0);
    if (delta_rot) {
        node_rot =
            rt_quat_new(node->rotation[0], node->rotation[1], node->rotation[2], node->rotation[3]);
        combined_rot = node_rot ? rt_quat_mul(node_rot, delta_rot) : NULL;
        if (combined_rot) {
            node->rotation[0] = rt_quat_x(combined_rot);
            node->rotation[1] = rt_quat_y(combined_rot);
            node->rotation[2] = rt_quat_z(combined_rot);
            node->rotation[3] = rt_quat_w(combined_rot);
            scene3d_quat_normalize_local(node->rotation);
        }
        scene3d_release_ref(&combined_rot);
        scene3d_release_ref(&node_rot);
        scene3d_release_ref(&delta_rot);
    }
    if (rt_obj_release_check0(delta))
        rt_obj_free(delta);
    mark_dirty(node);
}

/// @brief Derive a node-attached camera's eye/forward/up basis from the current world matrix.
/// @param node Borrowed node whose attached camera pose is synchronized.
static void scene_node_sync_camera(rt_scene_node3d *node) {
    rt_camera3d *camera;
    double eye[3];
    double forward[3];
    double up[3];
    double target[3];
    if (!node)
        return;
    camera = (rt_camera3d *)rt_g3d_checked_or_null(node->camera, RT_G3D_CAMERA3D_CLASS_ID);
    if (!camera)
        return;
    recompute_world_matrix(node);
    eye[0] = node->world_matrix[3];
    eye[1] = node->world_matrix[7];
    eye[2] = node->world_matrix[11];
    forward[0] = -node->world_matrix[2];
    forward[1] = -node->world_matrix[6];
    forward[2] = -node->world_matrix[10];
    up[0] = node->world_matrix[1];
    up[1] = node->world_matrix[5];
    up[2] = node->world_matrix[9];
    target[0] = eye[0] + forward[0];
    target[1] = eye[1] + forward[1];
    target[2] = eye[2] + forward[2];
    rt_camera3d_look_at_components(
        camera, eye[0], eye[1], eye[2], target[0], target[1], target[2], up[0], up[1], up[2]);
}

/// @brief Walk the subtree synchronising node transforms with bound bodies / animators.
///
/// Per node, depending on `sync_mode`:
///   - `BODY_TO_NODE`: copies the rigid body's world pose into the node.
///   - `NODE_TO_BODY`: pushes the node's transform back into the body.
///   - root motion: bumps the local TRS by the animator's per-frame delta.
/// Then recurses into children. The traversal stack persists on @p scene so the
/// once-per-tick whole-tree walk stops paying a malloc/free per frame; on trap
/// paths the buffer is simply left in place for the next call.
/// @param scene Borrowed owning scene providing reusable traversal storage.
/// @param node Borrowed subtree root.
/// @param dt Simulation delta forwarded to bound animators.
static void scene_node_sync_recursive(rt_scene3d *scene, rt_scene_node3d *node, double dt) {
    size_t count = 0;
    if (!node || !scene)
        return;
    if (!scene_node_stack_push(&scene->sync_stack, &count, &scene->sync_stack_capacity, node)) {
        rt_trap("Scene3D.SyncBindings: traversal stack allocation failed");
        return;
    }
    while (count > 0) {
        rt_scene_node3d *current = scene->sync_stack[--count];
        int64_t mode;
        int pull_from_body;
        int push_to_body;
        int body_is_kinematic;
        void *node_animator =
            rt_g3d_checked_or_null(current->bound_node_animator, RT_G3D_NODEANIMATOR3D_CLASS_ID);
        void *animator =
            rt_g3d_checked_or_null(current->bound_animator, RT_G3D_ANIMCONTROLLER3D_CLASS_ID);

        if (animator && current->bound_animator_scene_update)
            rt_anim_controller3d_update(animator, dt);
        if (node_animator)
            node_animator_update((rt_node_animator3d *)node_animator, dt);

        mode = current->sync_mode;
        body_is_kinematic = current->bound_body ? rt_body3d_is_kinematic(current->bound_body) : 0;
        pull_from_body = current->bound_body &&
                         (mode == RT_SCENE_NODE3D_SYNC_NODE_FROM_BODY ||
                          (mode == RT_SCENE_NODE3D_SYNC_TWO_WAY_KINEMATIC && !body_is_kinematic));
        push_to_body = current->bound_body &&
                       (mode == RT_SCENE_NODE3D_SYNC_BODY_FROM_NODE ||
                        (mode == RT_SCENE_NODE3D_SYNC_TWO_WAY_KINEMATIC && body_is_kinematic));

        if (current->socket_animator) {
            /* Bone sockets supersede body/root-motion sync for this node. */
            scene_node_apply_bone_socket(current);
            push_to_body = 0;
        } else if (pull_from_body) {
            double world_pos[3];
            double world_quat[4];
            rt_body3d_get_pose_raw(current->bound_body, world_pos, world_quat, NULL);
            scene_node_set_world_transform(current, world_pos, world_quat);
        } else if (animator &&
                   (mode == RT_SCENE_NODE3D_SYNC_NODE_FROM_ANIMATOR_ROOT_MOTION || push_to_body)) {
            scene_node_apply_root_motion(current);
        }

        if (push_to_body) {
            double world_pos[3];
            double world_quat[4];
            scene_node_get_world_position(current, &world_pos[0], &world_pos[1], &world_pos[2]);
            scene_node_get_world_rotation(current, world_quat);
            rt_body3d_set_position(current->bound_body, world_pos[0], world_pos[1], world_pos[2]);
            {
                void *quat =
                    rt_quat_new(world_quat[0], world_quat[1], world_quat[2], world_quat[3]);
                rt_body3d_set_orientation(current->bound_body, quat);
                scene3d_release_ref(&quat);
            }
        }

        scene_node_sync_camera(current);

        for (int32_t i = scene3d_node_child_count(current) - 1; i >= 0; i--) {
            if (!scene_node_stack_push(&scene->sync_stack,
                                       &count,
                                       &scene->sync_stack_capacity,
                                       current->children[i])) {
                rt_trap("Scene3D.SyncBindings: traversal stack allocation failed");
                return;
            }
        }
    }
}

/// @brief True if `target` appears anywhere in the subtree rooted at `root`.
/// Used to prevent cycles when reparenting (don't re-attach a node under one of its descendants).
/// @param root Borrowed subtree root.
/// @param target Borrowed candidate descendant.
/// @return Nonzero when the validated target occurs in the subtree.
int node_contains(const rt_scene_node3d *root, const rt_scene_node3d *target) {
    const rt_scene_node3d **stack = NULL;
    size_t count = 0;
    size_t capacity = 0;
    root = scene_node3d_checked_const(root);
    target = scene_node3d_checked_const(target);
    if (!root || !target)
        return 0;
    if (!scene_node_const_stack_push(&stack, &count, &capacity, root))
        return root == target;
    while (count > 0) {
        const rt_scene_node3d *current = stack[--count];
        if (current == target) {
            free(stack);
            return 1;
        }
        for (int32_t i = 0, child_count = scene3d_node_child_count(current); i < child_count; i++) {
            const rt_scene_node3d *child = scene_node3d_checked(current->children[i]);
            if (!scene_node_const_stack_push(&stack, &count, &capacity, child)) {
                rt_trap("SceneNode3D: traversal stack allocation failed");
                free(stack);
                return 0;
            }
        }
    }
    free(stack);
    return 0;
}

/// @brief Compute the local-space AABB of `mesh` by min/maxing every vertex position.
/// Cached on the mesh so subsequent calls are O(1).
/// @param mesh Borrowed Mesh3D payload, or `NULL`.
/// @param out_min Optional output receiving local minimum bounds.
/// @param out_max Optional output receiving local maximum bounds.
/// @param out_radius Optional output receiving cached bounding-sphere radius.
void scene_mesh_bounds(rt_mesh3d *mesh, float out_min[3], float out_max[3], float *out_radius) {
    if (!mesh) {
        if (out_min)
            out_min[0] = out_min[1] = out_min[2] = 0.0f;
        if (out_max)
            out_max[0] = out_max[1] = out_max[2] = 0.0f;
        if (out_radius)
            *out_radius = 0.0f;
        return;
    }
    rt_mesh3d_refresh_bounds(mesh);
    if (out_min) {
        out_min[0] = mesh->aabb_min[0];
        out_min[1] = mesh->aabb_min[1];
        out_min[2] = mesh->aabb_min[2];
    }
    if (out_max) {
        out_max[0] = mesh->aabb_max[0];
        out_max[1] = mesh->aabb_max[1];
        out_max[2] = mesh->aabb_max[2];
    }
    if (out_radius)
        *out_radius = mesh->bsphere_radius;
}

/// @brief Mix an arbitrary 64-bit value into a non-zero node geometry signature.
/// @details Spatial entries use this compact signature to notice mesh, LOD, and impostor metadata
///   changes without retaining a larger snapshot. It is not cryptographic; it only needs to make
///   ordinary scene-edit collisions unlikely enough for BVH refit invalidation.
/// @param signature Existing nonzero signature state.
/// @param value Arbitrary value to mix.
/// @return Updated nonzero 32-bit signature.
static uint32_t scene3d_mix_signature_u64(uint32_t signature, uint64_t value) {
    uint32_t lo = (uint32_t)(value & 0xffffffffu);
    uint32_t hi = (uint32_t)(value >> 32u);
    signature ^= lo + 0x9e3779b9u + (signature << 6) + (signature >> 2);
    signature ^= hi + 0x85ebca6bu + (signature << 6) + (signature >> 2);
    return signature ? signature : 1u;
}

/// @brief Convert a double to bits for compact geometry-signature mixing.
/// @param value Candidate floating-point value; non-finite values canonicalize to zero.
/// @return Canonicalized IEEE-754 bit pattern.
static uint64_t scene3d_double_bits_for_signature(double value) {
    union {
        double d;
        uint64_t u;
    } bits;

    bits.d = isfinite(value) ? value : 0.0;
    return bits.u;
}

/// @brief Mix one mesh identity and geometry revision into a non-zero node geometry signature.
/// @details Spatial entries store a compact signature instead of retaining every mesh
///          pointer snapshot. Including the pointer catches replacement by a different mesh whose
///          geometry revision happens to match the prior one.
/// @param signature Existing signature state.
/// @param mesh_obj Borrowed candidate Mesh3D handle.
/// @return Updated nonzero signature including identity and geometry revision.
static uint32_t scene3d_mix_mesh_revision(uint32_t signature, void *mesh_obj) {
    rt_mesh3d *mesh =
        rt_g3d_has_class(mesh_obj, RT_G3D_MESH3D_CLASS_ID) ? (rt_mesh3d *)mesh_obj : NULL;
    uint32_t revision = mesh ? mesh->geometry_revision : 0u;
    signature = scene3d_mix_signature_u64(signature, (uint64_t)(uintptr_t)mesh_obj);
    signature = scene3d_mix_signature_u64(signature, (uint64_t)revision);
    return signature;
}

/// @brief Combine the geometry revisions of all drawable meshes owned by @p node.
/// @details Includes the primary mesh, every LOD mesh, and the generated impostor
///          mesh. The result changes when any referenced mesh's CPU geometry changes,
///          allowing the spatial index to refit stale bounds even if the node transform
///          revision did not change.
/// @param node Borrowed node whose base, LOD, and impostor metadata are mixed.
/// @return Nonzero geometry signature, or zero for an invalid node.
uint32_t scene_node_geometry_revision_signature(rt_scene_node3d *node) {
    uint32_t signature = 2166136261u;
    if (!node)
        return 0;
    signature = scene3d_mix_mesh_revision(signature, node->mesh);
    signature = scene3d_mix_signature_u64(signature, (uint64_t)scene3d_node_lod_count(node));
    signature = scene3d_mix_signature_u64(signature, (uint64_t)(uint8_t)node->auto_lod_enabled);
    signature = scene3d_mix_signature_u64(
        signature, scene3d_double_bits_for_signature(node->auto_lod_screen_error_px));
    for (int32_t i = 0, lod_count = scene3d_node_lod_count(node); i < lod_count; i++) {
        signature = scene3d_mix_mesh_revision(signature, node->lod_levels[i].mesh);
        signature = scene3d_mix_signature_u64(
            signature, scene3d_double_bits_for_signature(node->lod_levels[i].distance));
    }
    signature = scene3d_mix_signature_u64(signature, (uint64_t)(uint8_t)node->has_impostor);
    signature = scene3d_mix_signature_u64(
        signature, scene3d_double_bits_for_signature(node->impostor_distance));
    if (node->has_impostor)
        signature = scene3d_mix_mesh_revision(signature, node->impostor_mesh);
    return signature ? signature : 1u;
}

/// @brief Refresh the node's cached primary-mesh local bounds after mesh geometry changes.
/// @details SceneNode3D stores an AABB/radius for APIs and impostor sizing, but Mesh3D
///          geometry can be mutated after assignment. This helper compares the mesh's
///          geometry revision to the node's last cached revision and recomputes only
///          when necessary. Non-mesh or missing meshes reset the cache to an empty AABB.
/// @param node Borrowed node whose primary mesh-bound cache is refreshed.
void scene_node_refresh_mesh_bounds(rt_scene_node3d *node) {
    rt_mesh3d *mesh;
    if (!node)
        return;
    mesh = rt_g3d_has_class(node->mesh, RT_G3D_MESH3D_CLASS_ID) ? (rt_mesh3d *)node->mesh : NULL;
    if (!mesh) {
        node->aabb_min[0] = node->aabb_min[1] = node->aabb_min[2] = 0.0f;
        node->aabb_max[0] = node->aabb_max[1] = node->aabb_max[2] = 0.0f;
        node->bsphere_radius = 0.0f;
        node->mesh_bounds_revision = 0;
        return;
    }
    if (node->mesh_bounds_revision == mesh->geometry_revision && !mesh->bounds_dirty)
        return;
    scene_mesh_bounds(mesh, node->aabb_min, node->aabb_max, &node->bsphere_radius);
    node->mesh_bounds_revision = mesh->geometry_revision ? mesh->geometry_revision : 1u;
}

/// @brief Multiply a local-space point by the node's world matrix; result lands in `out`.
/// @param world_matrix Borrowed finite row-major transform.
/// @param local Borrowed local-space float point.
/// @param out Output receiving a safely narrowed world-space point.
static void scene_world_point(const double *world_matrix, const float local[3], float out[3]) {
    double p[3];
    double x;
    double y;
    double z;
    if (!world_matrix || !local || !out)
        return;
    if (!scene3d_matrix_is_finite(world_matrix)) {
        out[0] = out[1] = out[2] = 0.0f;
        return;
    }
    p[0] = scene3d_clamp_abs_or((double)local[0], 0.0);
    p[1] = scene3d_clamp_abs_or((double)local[1], 0.0);
    p[2] = scene3d_clamp_abs_or((double)local[2], 0.0);
    x = world_matrix[0] * p[0] + world_matrix[1] * p[1] + world_matrix[2] * p[2] + world_matrix[3];
    y = world_matrix[4] * p[0] + world_matrix[5] * p[1] + world_matrix[6] * p[2] + world_matrix[7];
    z = world_matrix[8] * p[0] + world_matrix[9] * p[1] + world_matrix[10] * p[2] +
        world_matrix[11];
    out[0] = scene3d_float_or_zero(scene3d_clamp_abs_or(x, 0.0));
    out[1] = scene3d_float_or_zero(scene3d_clamp_abs_or(y, 0.0));
    out[2] = scene3d_float_or_zero(scene3d_clamp_abs_or(z, 0.0));
}

/// @brief Multiply a local-space point by the node's world matrix without narrowing to float.
/// @details LOD and impostor selection can sit on tight distance thresholds. Keeping the world
///          center and camera position in double precision avoids frame-to-frame selection jitter
///          in large-world scenes where float truncation erases the low bits near the camera.
/// @param world_matrix Borrowed finite row-major transform.
/// @param local Borrowed local-space float point.
/// @param out Output receiving a double-precision world-space point.
static void scene_world_point_d(const double *world_matrix, const float local[3], double out[3]) {
    double p[3];
    double x;
    double y;
    double z;
    if (!world_matrix || !local || !out)
        return;
    if (!scene3d_matrix_is_finite(world_matrix)) {
        out[0] = out[1] = out[2] = 0.0;
        return;
    }
    p[0] = scene3d_clamp_abs_or((double)local[0], 0.0);
    p[1] = scene3d_clamp_abs_or((double)local[1], 0.0);
    p[2] = scene3d_clamp_abs_or((double)local[2], 0.0);
    x = world_matrix[0] * p[0] + world_matrix[1] * p[1] + world_matrix[2] * p[2] + world_matrix[3];
    y = world_matrix[4] * p[0] + world_matrix[5] * p[1] + world_matrix[6] * p[2] + world_matrix[7];
    z = world_matrix[8] * p[0] + world_matrix[9] * p[1] + world_matrix[10] * p[2] +
        world_matrix[11];
    out[0] = scene3d_clamp_abs_or(x, 0.0);
    out[1] = scene3d_clamp_abs_or(y, 0.0);
    out[2] = scene3d_clamp_abs_or(z, 0.0);
}

/// @brief Normalize a float[3] vector in-place, substituting a caller-supplied fallback
///        direction when the vector is degenerate (zero-length or non-finite magnitude).
/// @details Used to normalize light direction vectors that have been transformed into
///   world space via the node's 3×3 rotation sub-matrix. A scale component in the
///   matrix can inflate or shrink the transformed direction, so re-normalization is
///   always required. The 1e-8 threshold guards against divide-by-near-zero; the
///   (0, 0, -1) fallback used by the caller makes a degenerate light point forward
///   rather than disappear.
/// @param v          float[3] vector to normalize in-place; must not be NULL.
/// @param fallback_x X component of the replacement direction on degenerate input.
/// @param fallback_y Y component of the replacement direction on degenerate input.
/// @param fallback_z Z component of the replacement direction on degenerate input.
static void scene_normalize_f32_vec3(float v[3],
                                     float fallback_x,
                                     float fallback_y,
                                     float fallback_z) {
    float len;
    if (!v)
        return;
    len = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (!isfinite(len) || len <= 1e-8f) {
        v[0] = fallback_x;
        v[1] = fallback_y;
        v[2] = fallback_z;
        return;
    }
    v[0] /= len;
    v[1] /= len;
    v[2] /= len;
}

/// @brief Transform a local vector by a finite world matrix's linear 3x3 portion.
/// @param world_matrix Borrowed finite row-major transform.
/// @param local Borrowed local-space direction or basis vector.
/// @param out Output receiving the transformed vector without translation.
static void scene_world_vector_d(const double world_matrix[16],
                                 const double local[3],
                                 double out[3]) {
    if (!world_matrix || !local || !out || !scene3d_matrix_is_finite(world_matrix)) {
        if (out)
            out[0] = out[1] = out[2] = 0.0;
        return;
    }
    out[0] = scene3d_clamp_abs_or(
        world_matrix[0] * local[0] + world_matrix[1] * local[1] + world_matrix[2] * local[2], 0.0);
    out[1] = scene3d_clamp_abs_or(
        world_matrix[4] * local[0] + world_matrix[5] * local[1] + world_matrix[6] * local[2], 0.0);
    out[2] = scene3d_clamp_abs_or(
        world_matrix[8] * local[0] + world_matrix[9] * local[1] + world_matrix[10] * local[2], 0.0);
}

/// @brief Normalize a double vector and return its original length, or zero on degeneration.
/// @param value Mutable three-component vector.
/// @return Original finite length after normalization, or zero on degeneration.
static double scene_normalize_f64_vec3(double value[3]) {
    double scale;
    double length;
    if (!value)
        return 0.0;
    scale = fmax(fabs(value[0]), fmax(fabs(value[1]), fabs(value[2])));
    if (!isfinite(scale) || scale <= 1e-12)
        return 0.0;
    length = scale * sqrt((value[0] / scale) * (value[0] / scale) +
                          (value[1] / scale) * (value[1] / scale) +
                          (value[2] / scale) * (value[2] / scale));
    if (!isfinite(length) || length <= 1e-12)
        return 0.0;
    value[0] /= length;
    value[1] /= length;
    value[2] /= length;
    return length;
}

/// @brief Conservative maximum axis scale of a world matrix.
/// @param world_matrix Borrowed finite row-major transform.
/// @return Largest linear-basis length, defaulting to one for invalid/degenerate matrices.
static double scene_world_max_scale(const double world_matrix[16]) {
    double maximum = 0.0;
    if (!world_matrix || !scene3d_matrix_is_finite(world_matrix))
        return 1.0;
    for (int column = 0; column < 3; column++) {
        double axis[3] = {world_matrix[column], world_matrix[4 + column], world_matrix[8 + column]};
        double length = scene_normalize_f64_vec3(axis);
        if (length > maximum)
            maximum = length;
    }
    return isfinite(maximum) && maximum > 1e-12 ? maximum : 1.0;
}

/// @brief Transform a node-attached light into world space for the current draw snapshot.
/// @details Copies all authored fields, transforms position and direction,
///          orthonormalizes area-light bases, and scales physical extents/range
///          conservatively without mutating the retained source light.
/// @param node Borrowed node supplying the world transform.
/// @param src Borrowed attached local-space light payload.
/// @param dst Output receiving an independent world-space snapshot.
static void scene_transform_node_light(rt_scene_node3d *node,
                                       const rt_light3d *src,
                                       rt_light3d *dst) {
    float local_pos[3];
    float world_pos[3];
    float world_dir[3];
    double basis[9];
    double local_dir[3];
    if (!node || !src || !dst)
        return;
    recompute_world_matrix(node);
    memcpy(dst, src, sizeof(*dst));
    local_pos[0] = (float)src->position[0];
    local_pos[1] = (float)src->position[1];
    local_pos[2] = (float)src->position[2];
    scene_world_point(node->world_matrix, local_pos, world_pos);
    dst->position[0] = world_pos[0];
    dst->position[1] = world_pos[1];
    dst->position[2] = world_pos[2];

    local_dir[0] = scene3d_finite_or(src->direction[0], 0.0);
    local_dir[1] = scene3d_finite_or(src->direction[1], 0.0);
    local_dir[2] = scene3d_finite_or(src->direction[2], -1.0);
    if (scene_extract_rotation_basis(node->world_matrix, basis)) {
        world_dir[0] =
            (float)(basis[0] * local_dir[0] + basis[1] * local_dir[1] + basis[2] * local_dir[2]);
        world_dir[1] =
            (float)(basis[3] * local_dir[0] + basis[4] * local_dir[1] + basis[5] * local_dir[2]);
        world_dir[2] =
            (float)(basis[6] * local_dir[0] + basis[7] * local_dir[1] + basis[8] * local_dir[2]);
    } else {
        world_dir[0] = (float)local_dir[0];
        world_dir[1] = (float)local_dir[1];
        world_dir[2] = (float)local_dir[2];
    }
    scene_normalize_f32_vec3(world_dir, 0.0f, 0.0f, -1.0f);
    dst->direction[0] = world_dir[0];
    dst->direction[1] = world_dir[1];
    dst->direction[2] = world_dir[2];

    if (src->type == 4) {
        double transformed_u[3];
        double transformed_v[3];
        double transformed_direction[3];
        double source_u[3] = {src->basis_u[0], src->basis_u[1], src->basis_u[2]};
        double source_v[3] = {src->basis_v[0], src->basis_v[1], src->basis_v[2]};
        double source_direction[3] = {src->direction[0], src->direction[1], src->direction[2]};
        double scale_u;
        double scale_v;
        double projection;
        double orientation;
        scene_world_vector_d(node->world_matrix, source_u, transformed_u);
        scene_world_vector_d(node->world_matrix, source_v, transformed_v);
        scene_world_vector_d(node->world_matrix, source_direction, transformed_direction);
        scale_u = scene_normalize_f64_vec3(transformed_u);
        if (scale_u <= 0.0) {
            transformed_u[0] = 1.0;
            transformed_u[1] = transformed_u[2] = 0.0;
            scale_u = 1.0;
        }
        projection = transformed_v[0] * transformed_u[0] + transformed_v[1] * transformed_u[1] +
                     transformed_v[2] * transformed_u[2];
        transformed_v[0] -= projection * transformed_u[0];
        transformed_v[1] -= projection * transformed_u[1];
        transformed_v[2] -= projection * transformed_u[2];
        scale_v = scene_normalize_f64_vec3(transformed_v);
        if (scale_v <= 0.0) {
            transformed_v[0] = 0.0;
            transformed_v[1] = 1.0;
            transformed_v[2] = 0.0;
            projection = transformed_v[0] * transformed_u[0] + transformed_v[1] * transformed_u[1] +
                         transformed_v[2] * transformed_u[2];
            transformed_v[0] -= projection * transformed_u[0];
            transformed_v[1] -= projection * transformed_u[1];
            transformed_v[2] -= projection * transformed_u[2];
            if (scene_normalize_f64_vec3(transformed_v) <= 0.0) {
                transformed_v[0] = 0.0;
                transformed_v[1] = 0.0;
                transformed_v[2] = 1.0;
            }
            scale_v = 1.0;
        }
        transformed_direction[0] =
            transformed_u[1] * transformed_v[2] - transformed_u[2] * transformed_v[1];
        transformed_direction[1] =
            transformed_u[2] * transformed_v[0] - transformed_u[0] * transformed_v[2];
        transformed_direction[2] =
            transformed_u[0] * transformed_v[1] - transformed_u[1] * transformed_v[0];
        scene_normalize_f64_vec3(transformed_direction);
        orientation = transformed_direction[0] * world_dir[0] +
                      transformed_direction[1] * world_dir[1] +
                      transformed_direction[2] * world_dir[2];
        if (orientation < 0.0) {
            transformed_direction[0] = -transformed_direction[0];
            transformed_direction[1] = -transformed_direction[1];
            transformed_direction[2] = -transformed_direction[2];
        }
        memcpy(dst->basis_u, transformed_u, sizeof(transformed_u));
        memcpy(dst->basis_v, transformed_v, sizeof(transformed_v));
        memcpy(dst->direction, transformed_direction, sizeof(transformed_direction));
        dst->width = scene3d_clamp_abs_or(src->width * scale_u, 1.0);
        dst->height = scene3d_clamp_abs_or(src->height * scale_v, 1.0);
    }
    if (src->type == 5 || src->type == 6)
        dst->radius =
            scene3d_clamp_abs_or(src->radius * scene_world_max_scale(node->world_matrix), 1.0);
    if (src->type >= 4 && src->type <= 6)
        dst->range =
            scene3d_clamp_abs_or(src->range * scene_world_max_scale(node->world_matrix), 1.0);
}

/// @brief Recursively collect world-space lights from visible nodes into the draw snapshot.
/// @details Traverses the subtree rooted at @p node, skipping invisible nodes entirely
///   (their children are also skipped, consistent with visibility culling elsewhere).
///   For each visible node that carries a light, `scene_transform_node_light` writes
///   the world-space copy into @p storage[*io_count] and adds its address to
///   @p out_lights[*io_count] before incrementing the counter. Collection stops when
///   the hardware light limit `VGFX3D_MAX_LIGHTS` is reached; excess lights are silently
///   dropped, which is the standard GPU behavior for too many dynamic lights.
/// @param node       Root of the subtree to search.
/// @param storage    Pre-allocated array of at least VGFX3D_MAX_LIGHTS rt_light3d structs
///                   used as scratch space for world-space copies.
/// @param out_lights Output pointer array; receives addresses into @p storage.
/// @param io_count   In/out: current light count; incremented for each light found.
static void scene_collect_node_lights(rt_scene_node3d *node,
                                      rt_light3d *storage,
                                      rt_light3d **out_lights,
                                      int32_t *io_count) {
    rt_scene_node3d **stack = NULL;
    size_t count = 0;
    size_t capacity = 0;
    if (!node || !storage || !out_lights || !io_count)
        return;
    if (!scene_node_stack_push(&stack, &count, &capacity, node)) {
        rt_trap("Scene3D.Draw: light traversal stack allocation failed");
        return;
    }
    while (count > 0 && *io_count < VGFX3D_MAX_LIGHTS) {
        rt_scene_node3d *current = stack[--count];
        if (!current->visible)
            continue;
        if (current->light) {
            int32_t index = *io_count;
            scene_transform_node_light(
                current, (const rt_light3d *)current->light, &storage[index]);
            out_lights[index] = &storage[index];
            *io_count = index + 1;
            if (*io_count >= VGFX3D_MAX_LIGHTS)
                break;
        }
        for (int32_t i = scene3d_node_child_count(current) - 1; i >= 0; i--) {
            if (!scene_node_stack_push(&stack, &count, &capacity, current->children[i])) {
                rt_trap("Scene3D.Draw: light traversal stack allocation failed");
                free(stack);
                return;
            }
        }
    }
    free(stack);
}

/// @brief Initialise a min/max pair so subsequent point inserts grow a valid AABB.
/// @param out_min Optional output set to positive float maxima.
/// @param out_max Optional output set to negative float maxima.
void scene_bounds_reset(float out_min[3], float out_max[3]) {
    if (out_min) {
        out_min[0] = FLT_MAX;
        out_min[1] = FLT_MAX;
        out_min[2] = FLT_MAX;
    }
    if (out_max) {
        out_max[0] = -FLT_MAX;
        out_max[1] = -FLT_MAX;
        out_max[2] = -FLT_MAX;
    }
}

/// @brief Reset an AABB to the empty state (min = +DBL_MAX, max = -DBL_MAX) ready for accumulation.
/// @param out_min Optional output set to positive double maxima.
/// @param out_max Optional output set to negative double maxima.
void scene_bounds_reset_d(double out_min[3], double out_max[3]) {
    if (out_min) {
        out_min[0] = DBL_MAX;
        out_min[1] = DBL_MAX;
        out_min[2] = DBL_MAX;
    }
    if (out_max) {
        out_max[0] = -DBL_MAX;
        out_max[1] = -DBL_MAX;
        out_max[2] = -DBL_MAX;
    }
}

/// @brief Expand @p bounds_min/@p bounds_max to include @p point.
/// @param bounds_min Mutable accumulated float minimum.
/// @param bounds_max Mutable accumulated float maximum.
/// @param point Borrowed point to include.
static void scene_bounds_include_point(float bounds_min[3],
                                       float bounds_max[3],
                                       const float point[3]) {
    if (!bounds_min || !bounds_max || !point)
        return;
    for (int i = 0; i < 3; i++) {
        if (point[i] < bounds_min[i])
            bounds_min[i] = point[i];
        if (point[i] > bounds_max[i])
            bounds_max[i] = point[i];
    }
}

/// @brief Expand an AABB in place to contain a point.
/// @param bounds_min Mutable accumulated double minimum.
/// @param bounds_max Mutable accumulated double maximum.
/// @param point Borrowed point sanitized before inclusion.
void scene_bounds_include_point_d(double bounds_min[3],
                                  double bounds_max[3],
                                  const double point[3]) {
    double p[3];
    if (!bounds_min || !bounds_max || !point)
        return;
    p[0] = scene3d_clamp_abs_or(point[0], 0.0);
    p[1] = scene3d_clamp_abs_or(point[1], 0.0);
    p[2] = scene3d_clamp_abs_or(point[2], 0.0);
    for (int i = 0; i < 3; i++) {
        if (p[i] < bounds_min[i])
            bounds_min[i] = p[i];
        if (p[i] > bounds_max[i])
            bounds_max[i] = p[i];
    }
}

/// @brief Transform a local AABB by a matrix into a world AABB by bounding its eight rotated
/// corners.
/// @param obj_min Borrowed local-space minimum corner.
/// @param obj_max Borrowed local-space maximum corner.
/// @param world_matrix Borrowed finite row-major transform.
/// @param out_min Output receiving world-space minimum bounds.
/// @param out_max Output receiving world-space maximum bounds.
/// @return Nonzero when all eight corners produce valid bounds, otherwise zero.
int scene3d_transform_aabb_d(const float obj_min[3],
                             const float obj_max[3],
                             const double world_matrix[16],
                             double out_min[3],
                             double out_max[3]) {
    if (!out_min || !out_max)
        return 0;
    scene_bounds_reset_d(out_min, out_max);
    if (!obj_min || !obj_max || !world_matrix) {
        out_min[0] = out_min[1] = out_min[2] = 0.0;
        out_max[0] = out_max[1] = out_max[2] = 0.0;
        return 0;
    }
    for (int i = 0; i < 3; i++) {
        if (!isfinite(obj_min[i]) || !isfinite(obj_max[i])) {
            out_min[0] = out_min[1] = out_min[2] = 0.0;
            out_max[0] = out_max[1] = out_max[2] = 0.0;
            return 0;
        }
    }
    if (!scene3d_matrix_is_finite(world_matrix)) {
        out_min[0] = out_min[1] = out_min[2] = 0.0;
        out_max[0] = out_max[1] = out_max[2] = 0.0;
        return 0;
    }
    float safe_min[3];
    float safe_max[3];
    for (int axis = 0; axis < 3; axis++) {
        if (obj_min[axis] <= obj_max[axis]) {
            safe_min[axis] = obj_min[axis];
            safe_max[axis] = obj_max[axis];
        } else {
            safe_min[axis] = obj_max[axis];
            safe_max[axis] = obj_min[axis];
        }
    }
    for (int corner = 0; corner < 8; corner++) {
        double cx = (corner & 1) ? (double)safe_max[0] : (double)safe_min[0];
        double cy = (corner & 2) ? (double)safe_max[1] : (double)safe_min[1];
        double cz = (corner & 4) ? (double)safe_max[2] : (double)safe_min[2];
        double p[3];
        p[0] = scene3d_clamp_abs_or(world_matrix[0] * cx + world_matrix[1] * cy +
                                        world_matrix[2] * cz + world_matrix[3],
                                    0.0);
        p[1] = scene3d_clamp_abs_or(world_matrix[4] * cx + world_matrix[5] * cy +
                                        world_matrix[6] * cz + world_matrix[7],
                                    0.0);
        p[2] = scene3d_clamp_abs_or(world_matrix[8] * cx + world_matrix[9] * cy +
                                        world_matrix[10] * cz + world_matrix[11],
                                    0.0);
        scene_bounds_include_point_d(out_min, out_max, p);
    }
    return 1;
}

/// @brief Transform the 8 corners of a local AABB and union them into @p bounds_min/max.
/// @param bounds_min Mutable accumulated float minimum.
/// @param bounds_max Mutable accumulated float maximum.
/// @param local_min Borrowed local-space AABB minimum.
/// @param local_max Borrowed local-space AABB maximum.
/// @param local_to_root Borrowed transform from local to queried-root space.
static void scene_bounds_include_aabb(float bounds_min[3],
                                      float bounds_max[3],
                                      const float local_min[3],
                                      const float local_max[3],
                                      const double *local_to_root) {
    float local_corner[3];
    float root_corner[3];
    if (!bounds_min || !bounds_max || !local_min || !local_max || !local_to_root)
        return;
    for (int xi = 0; xi < 2; xi++) {
        for (int yi = 0; yi < 2; yi++) {
            for (int zi = 0; zi < 2; zi++) {
                local_corner[0] = xi ? local_max[0] : local_min[0];
                local_corner[1] = yi ? local_max[1] : local_min[1];
                local_corner[2] = zi ? local_max[2] : local_min[2];
                scene_world_point(local_to_root, local_corner, root_corner);
                scene_bounds_include_point(bounds_min, bounds_max, root_corner);
            }
        }
    }
}

/// @brief Traversal frame pairing a node with its transform into queried-root space.
typedef struct {
    /// Borrowed subtree node.
    rt_scene_node3d *node;
    /// Row-major matrix mapping node-local coordinates to root-local coordinates.
    double node_to_root[16];
} scene_bounds_stack_item_t;

/// @brief Push (node, node→root matrix) onto a growable explicit traversal stack.
/// @details Used instead of recursion so deep scene hierarchies cannot overflow
///          the C stack. Capacity doubles on demand (seeded at 64) via an
///          overflow-guarded realloc.
/// @param stack Address of the caller-owned stack allocation.
/// @param count In/out populated item count.
/// @param capacity In/out allocated item capacity.
/// @param node Borrowed node to append.
/// @param node_to_root Borrowed transform copied into the new frame.
/// @return 1 on success — and also on NULL/missing arguments, treated as a no-op;
///         0 only when capacity growth overflows or realloc fails.
static int scene_bounds_stack_push(scene_bounds_stack_item_t **stack,
                                   size_t *count,
                                   size_t *capacity,
                                   rt_scene_node3d *node,
                                   const double *node_to_root) {
    rt_scene_node3d *checked;
    if (!stack || !count || !capacity || !node || !node_to_root)
        return 1;
    checked = scene_node3d_checked(node);
    if (!checked)
        return 1;
    if (*count >= *capacity) {
        if (!scene3d_grow_stack_storage((void **)stack, capacity, sizeof(**stack)))
            return 0;
    }
    (*stack)[*count].node = checked;
    memcpy((*stack)[*count].node_to_root, node_to_root, sizeof(double) * 16);
    (*count)++;
    return 1;
}

/// @brief Compute a node subtree's local-space AABB relative to the queried root node.
///
/// @param node Current subtree node.
/// @param node_to_root Row-major matrix mapping @p node local space into the queried root's local
/// space.
/// @param out_min Running subtree minimum.
/// @param out_max Running subtree maximum.
/// @return 1 if the subtree contributed any mesh bounds, otherwise 0.
int scene_node_collect_subtree_bounds(rt_scene_node3d *node,
                                      const double *node_to_root,
                                      float out_min[3],
                                      float out_max[3]) {
    int has_bounds = 0;
    scene_bounds_stack_item_t *stack = NULL;
    size_t count = 0;
    size_t capacity = 0;
    if (!node || !node_to_root || !out_min || !out_max)
        return 0;

    if (!scene_bounds_stack_push(&stack, &count, &capacity, node, node_to_root)) {
        rt_trap("SceneNode3D.GetAabb: traversal stack allocation failed");
        return 0;
    }
    while (count > 0) {
        scene_bounds_stack_item_t item = stack[--count];
        void *mesh = rt_g3d_checked_or_null(item.node->mesh, RT_G3D_MESH3D_CLASS_ID);
        if (mesh) {
            float mesh_min[3];
            float mesh_max[3];
            scene_mesh_bounds((rt_mesh3d *)mesh, mesh_min, mesh_max, NULL);
            scene_bounds_include_aabb(out_min, out_max, mesh_min, mesh_max, item.node_to_root);
            has_bounds = 1;
        }
        for (int32_t i = scene3d_node_child_count(item.node) - 1; i >= 0; i--) {
            rt_scene_node3d *child = scene_node3d_checked(item.node->children[i]);
            if (!child)
                continue;
            double child_local[16];
            double child_to_root[16];
            build_trs_matrix(child->position, child->rotation, child->scale_xyz, child_local);
            mat4d_mul(item.node_to_root, child_local, child_to_root);
            if (!scene_bounds_stack_push(&stack, &count, &capacity, child, child_to_root)) {
                rt_trap("SceneNode3D.GetAabb: traversal stack allocation failed");
                free(stack);
                return has_bounds;
            }
        }
    }
    free(stack);

    return has_bounds;
}

/// @brief Depth-first search for a node whose `name` matches `target` (NULL on miss).
/// @details Reuses the owning scene's query traversal stack when the subtree is
///   scene-attached, so repeated per-frame `Find` calls do not heap-allocate.
///   Detached subtrees (e.g. scene-less model rigs) fall back to a local stack.
/// @param node Borrowed subtree root.
/// @param target Borrowed null-terminated exact name.
/// @return Borrowed first matching node in depth-first order, or `NULL`.
rt_scene_node3d *find_by_name(rt_scene_node3d *node, const char *target) {
    rt_scene_node3d **local_stack = NULL;
    size_t local_capacity = 0;
    rt_scene_node3d ***stack_ref = &local_stack;
    size_t *capacity_ref = &local_capacity;
    size_t count = 0;
    rt_scene_node3d *found = NULL;
    if (!node || !target)
        return NULL;
    if (node->owner_scene) {
        stack_ref = &node->owner_scene->query_traversal_stack;
        capacity_ref = &node->owner_scene->query_traversal_stack_capacity;
    }
    if (!scene_node_stack_push(stack_ref, &count, capacity_ref, node)) {
        rt_trap("SceneNode3D.Find: traversal stack allocation failed");
        return NULL;
    }
    while (count > 0) {
        rt_scene_node3d *current = (*stack_ref)[--count];
        if (current->name) {
            if (!rt_string_is_handle(current->name)) {
                current->name = NULL;
            } else {
                const char *s = rt_string_cstr(current->name);
                if (s && strcmp(s, target) == 0) {
                    found = current;
                    break;
                }
            }
        }
        for (int32_t i = scene3d_node_child_count(current) - 1; i >= 0; i--) {
            if (!scene_node_stack_push(stack_ref, &count, capacity_ref, current->children[i])) {
                rt_trap("SceneNode3D.Find: traversal stack allocation failed");
                free(local_stack);
                return NULL;
            }
        }
    }
    free(local_stack);
    return found;
}

/// @brief Read a boxed Vec3 into a double[3], trapping with @p trap_message on a non-Vec3 handle.
/// @param obj Borrowed opaque handle expected to contain a Vec3.
/// @param out Output receiving sanitized components.
/// @param trap_message Message used when type validation fails.
/// @return Nonzero after a successful read, otherwise zero.
int scene3d_read_vec3d(void *obj, double out[3], const char *trap_message) {
    if (!out)
        return 0;
    if (!rt_g3d_is_vec3(obj)) {
        rt_trap(trap_message);
        return 0;
    }
    out[0] = scene3d_clamp_abs_or(rt_vec3_x(obj), 0.0);
    out[1] = scene3d_clamp_abs_or(rt_vec3_y(obj), 0.0);
    out[2] = scene3d_clamp_abs_or(rt_vec3_z(obj), 0.0);
    return 1;
}

/// @brief Get a node's own mesh AABB in world space. Transform-only nodes return false.
/// @param node Borrowed SceneNode3D handle.
/// @param world_min Output receiving world-space minimum bounds.
/// @param world_max Output receiving world-space maximum bounds.
/// @return Nonzero for a valid mesh node with transformed bounds.
int scene3d_node_world_mesh_aabb(rt_scene_node3d *node, double world_min[3], double world_max[3]) {
    float local_min[3];
    float local_max[3];
    rt_mesh3d *mesh;
    if (!node || !node->mesh || !world_min || !world_max)
        return 0;
    mesh = (rt_mesh3d *)rt_g3d_checked_or_null(node->mesh, RT_G3D_MESH3D_CLASS_ID);
    if (!mesh)
        return 0;
    recompute_world_matrix(node);
    scene_mesh_bounds(mesh, local_min, local_max, NULL);
    if (!scene3d_transform_aabb_d(local_min, local_max, node->world_matrix, world_min, world_max))
        return 0;
    scene3d_canonicalize_aabb_d(world_min, world_max);
    return 1;
}

/// @brief Whether two AABBs overlap on all three axes.
/// @param a_min Borrowed minimum corner of box A.
/// @param a_max Borrowed maximum corner of box A.
/// @param b_min Borrowed minimum corner of box B.
/// @param b_max Borrowed maximum corner of box B.
/// @return Nonzero when the canonicalized closed boxes overlap or touch.
int scene3d_aabb_intersects_aabb(const double a_min[3],
                                 const double a_max[3],
                                 const double b_min[3],
                                 const double b_max[3]) {
    double amin[3] = {a_min ? a_min[0] : 0.0, a_min ? a_min[1] : 0.0, a_min ? a_min[2] : 0.0};
    double amax[3] = {a_max ? a_max[0] : 0.0, a_max ? a_max[1] : 0.0, a_max ? a_max[2] : 0.0};
    double bmin[3] = {b_min ? b_min[0] : 0.0, b_min ? b_min[1] : 0.0, b_min ? b_min[2] : 0.0};
    double bmax[3] = {b_max ? b_max[0] : 0.0, b_max ? b_max[1] : 0.0, b_max ? b_max[2] : 0.0};
    scene3d_canonicalize_aabb_d(amin, amax);
    scene3d_canonicalize_aabb_d(bmin, bmax);
    return amin[0] <= bmax[0] && amax[0] >= bmin[0] && amin[1] <= bmax[1] && amax[1] >= bmin[1] &&
           amin[2] <= bmax[2] && amax[2] >= bmin[2];
}

/// @brief Whether a sphere overlaps an AABB, via closest-point squared distance vs radius².
/// @param aabb_min Borrowed box minimum corner.
/// @param aabb_max Borrowed box maximum corner.
/// @param center Borrowed sphere center.
/// @param radius Candidate non-negative sphere radius.
/// @return Nonzero when the sanitized closed shapes overlap or touch.
int scene3d_aabb_intersects_sphere(const double aabb_min[3],
                                   const double aabb_max[3],
                                   const double center[3],
                                   double radius) {
    double mn[3] = {
        aabb_min ? aabb_min[0] : 0.0, aabb_min ? aabb_min[1] : 0.0, aabb_min ? aabb_min[2] : 0.0};
    double mx[3] = {
        aabb_max ? aabb_max[0] : 0.0, aabb_max ? aabb_max[1] : 0.0, aabb_max ? aabb_max[2] : 0.0};
    double c[3] = {center ? center[0] : 0.0, center ? center[1] : 0.0, center ? center[2] : 0.0};
    double dist2 = 0.0;
    scene3d_canonicalize_aabb_d(mn, mx);
    scene3d_sanitize_vec3d(c, 0.0);
    radius = scene3d_distance_or_zero(radius);
    for (int i = 0; i < 3; ++i) {
        double v = c[i];
        if (v < mn[i]) {
            double d = mn[i] - v;
            dist2 += d * d;
        } else if (v > mx[i]) {
            double d = v - mx[i];
            dist2 += d * d;
        }
        if (!isfinite(dist2))
            return 0;
    }
    return dist2 <= radius * radius;
}

/// @brief Ray-vs-AABB slab test with a normalized direction and finite max distance.
/// @param origin Borrowed ray origin.
/// @param direction Borrowed non-degenerate ray direction, normalized internally.
/// @param aabb_min Borrowed box minimum corner.
/// @param aabb_max Borrowed box maximum corner.
/// @param max_distance Maximum accepted entry distance.
/// @param out_t Optional output receiving the non-negative entry distance.
/// @return Nonzero when the ray intersects within @p max_distance.
int scene3d_ray_intersects_aabb(const double origin[3],
                                const double direction[3],
                                const double aabb_min[3],
                                const double aabb_max[3],
                                double max_distance,
                                double *out_t) {
    double o[3] = {origin ? origin[0] : 0.0, origin ? origin[1] : 0.0, origin ? origin[2] : 0.0};
    double dvec[3] = {direction ? direction[0] : 0.0,
                      direction ? direction[1] : 0.0,
                      direction ? direction[2] : 0.0};
    double mn[3] = {
        aabb_min ? aabb_min[0] : 0.0, aabb_min ? aabb_min[1] : 0.0, aabb_min ? aabb_min[2] : 0.0};
    double mx[3] = {
        aabb_max ? aabb_max[0] : 0.0, aabb_max ? aabb_max[1] : 0.0, aabb_max ? aabb_max[2] : 0.0};
    double tmin = 0.0;
    double tmax;
    scene3d_sanitize_vec3d(o, 0.0);
    scene3d_sanitize_vec3d(dvec, 0.0);
    if (!scene3d_normalize_vec3d(dvec))
        return 0;
    scene3d_canonicalize_aabb_d(mn, mx);
    if (!isfinite(max_distance))
        max_distance = SCENE3D_ABS_MAX;
    if (max_distance < 0.0)
        return 0;
    max_distance = scene3d_distance_or_zero(max_distance);
    tmax = max_distance;
    for (int i = 0; i < 3; ++i) {
        double axis_origin = o[i];
        double d = dvec[i];
        if (fabs(d) <= 1e-12) {
            if (axis_origin < mn[i] || axis_origin > mx[i])
                return 0;
            continue;
        }
        double inv = 1.0 / d;
        double t1 = (mn[i] - axis_origin) * inv;
        double t2 = (mx[i] - axis_origin) * inv;
        if (!isfinite(t1) || !isfinite(t2))
            return 0;
        if (t1 > t2) {
            double tmp = t1;
            t1 = t2;
            t2 = tmp;
        }
        if (t1 > tmin)
            tmin = t1;
        if (t2 < tmax)
            tmax = t2;
        if (tmin > tmax)
            return 0;
    }
    if (out_t)
        *out_t = scene3d_distance_or_zero(tmin);
    return tmin <= max_distance;
}

/// @brief Visibility state used while testing nodes against authored zones and portals.
typedef struct {
    /// Owned byte mask indexed by zone.
    uint8_t *visible_zones;
    /// Number of valid mask entries.
    int32_t zone_count;
    /// Optional borrowed counter incremented on PVS rejection.
    int32_t *culled_count;
    /// Whether PVS filtering is active for the camera.
    int8_t active;
} scene3d_pvs_context_t;

/// @brief Push a (node, inherited-animator) frame onto the index-build traversal stack, growing it.
/// @param stack Address of the caller-owned traversal stack.
/// @param count In/out populated frame count.
/// @param capacity In/out allocated frame capacity.
/// @param node Borrowed node to validate and append.
/// @param inherited_animator Borrowed ancestor animator carried into the frame.
/// @return 1 on success (or NULL no-op), 0 on overflow/allocation failure.
int scene_index_build_stack_push(scene_index_build_stack_item_t **stack,
                                 size_t *count,
                                 size_t *capacity,
                                 rt_scene_node3d *node,
                                 void *inherited_animator) {
    rt_scene_node3d *checked;
    if (!stack || !count || !capacity || !node)
        return 1;
    checked = scene_node3d_checked(node);
    if (!checked)
        return 1;
    if (*count >= *capacity) {
        if (!scene3d_grow_stack_storage((void **)stack, capacity, sizeof(**stack)))
            return 0;
    }
    (*stack)[*count].node = checked;
    (*stack)[*count].inherited_animator = inherited_animator;
    (*count)++;
    return 1;
}

/// @brief Grow the scene's visibility-zone array to hold at least @p needed zones (doubling
/// growth).
/// @param scene Borrowed scene whose native zone array may grow.
/// @param needed Minimum required zone capacity.
/// @return 1 on success or when capacity already suffices; 0 on overflow or allocation failure.
static int scene3d_visibility_zone_ensure_capacity(rt_scene3d *scene, int32_t needed) {
    int32_t new_capacity;
    rt_scene3d_visibility_zone *grown;
    if (!scene || needed < 0)
        return 0;
    if (needed <= scene->visibility_zone_capacity)
        return 1;
    new_capacity = scene->visibility_zone_capacity < 8 ? 8 : scene->visibility_zone_capacity;
    while (new_capacity < needed) {
        if (new_capacity > INT32_MAX / 2)
            return 0;
        new_capacity *= 2;
    }
    if ((size_t)new_capacity > SIZE_MAX / sizeof(scene->visibility_zones[0]))
        return 0;
    grown = (rt_scene3d_visibility_zone *)realloc(
        scene->visibility_zones, (size_t)new_capacity * sizeof(scene->visibility_zones[0]));
    if (!grown)
        return 0;
    scene->visibility_zones = grown;
    scene->visibility_zone_capacity = new_capacity;
    return 1;
}

/// @brief Grow the scene's visibility-portal array to hold at least @p needed portals (doubling
/// growth).
/// @param scene Borrowed scene whose native portal array may grow.
/// @param needed Minimum required portal capacity.
/// @return 1 on success or when capacity already suffices; 0 on overflow or allocation failure.
static int scene3d_visibility_portal_ensure_capacity(rt_scene3d *scene, int32_t needed) {
    int32_t new_capacity;
    rt_scene3d_visibility_portal *grown;
    if (!scene || needed < 0)
        return 0;
    if (needed <= scene->visibility_portal_capacity)
        return 1;
    new_capacity = scene->visibility_portal_capacity < 8 ? 8 : scene->visibility_portal_capacity;
    while (new_capacity < needed) {
        if (new_capacity > INT32_MAX / 2)
            return 0;
        new_capacity *= 2;
    }
    if ((size_t)new_capacity > SIZE_MAX / sizeof(scene->visibility_portals[0]))
        return 0;
    grown = (rt_scene3d_visibility_portal *)realloc(
        scene->visibility_portals, (size_t)new_capacity * sizeof(scene->visibility_portals[0]));
    if (!grown)
        return 0;
    scene->visibility_portals = grown;
    scene->visibility_portal_capacity = new_capacity;
    return 1;
}

/// @brief Clamp a visibility-array count to initialized storage.
/// @param array Borrowed allocation pointer.
/// @param count Logical element count.
/// @param capacity Allocated element capacity.
/// @return Safe non-negative count no larger than capacity.
static int32_t scene3d_visibility_array_count_or_zero(const void *array,
                                                      int32_t count,
                                                      int32_t capacity) {
    if (!array || count <= 0 || capacity <= 0)
        return 0;
    return count < capacity ? count : capacity;
}

/// @brief Return the scene's safe initialized visibility-zone count.
/// @param scene Borrowed Scene3D payload.
/// @return Safe non-negative count bounded by allocated storage.
static int32_t scene3d_visibility_zone_count_safe(const rt_scene3d *scene) {
    return scene ? scene3d_visibility_array_count_or_zero(scene->visibility_zones,
                                                          scene->visibility_zone_count,
                                                          scene->visibility_zone_capacity)
                 : 0;
}

/// @brief Return the scene's safe initialized visibility-portal count.
/// @param scene Borrowed Scene3D payload.
/// @return Safe non-negative count bounded by allocated storage.
static int32_t scene3d_visibility_portal_count_safe(const rt_scene3d *scene) {
    return scene ? scene3d_visibility_array_count_or_zero(scene->visibility_portals,
                                                          scene->visibility_portal_count,
                                                          scene->visibility_portal_capacity)
                 : 0;
}

/// @brief Test whether @p point lies inside the zone's world-space AABB (inclusive bounds).
/// @param zone Borrowed authored visibility zone.
/// @param point Borrowed world-space point.
/// @return Nonzero when the point lies inside or on every zone boundary.
static int scene3d_visibility_zone_contains_point(const rt_scene3d_visibility_zone *zone,
                                                  const double point[3]) {
    return zone && point && point[0] >= zone->world_min[0] && point[0] <= zone->world_max[0] &&
           point[1] >= zone->world_min[1] && point[1] <= zone->world_max[1] &&
           point[2] >= zone->world_min[2] && point[2] <= zone->world_max[2];
}

/// @brief Find the visibility zone containing the camera eye (including its shake offset).
/// @param scene Borrowed scene containing authored zones.
/// @param cam Borrowed camera whose shaken eye is tested.
/// @return Index of the first containing zone, or -1 if the eye is non-finite or outside every
/// zone.
static int scene3d_visibility_find_camera_zone(const rt_scene3d *scene, const rt_camera3d *cam) {
    double eye[3];
    int32_t zone_count;
    if (!scene || !cam)
        return -1;
    zone_count = scene3d_visibility_zone_count_safe(scene);
    eye[0] = cam->eye[0] + cam->shake_offset[0];
    eye[1] = cam->eye[1] + cam->shake_offset[1];
    eye[2] = cam->eye[2] + cam->shake_offset[2];
    if (!isfinite(eye[0]) || !isfinite(eye[1]) || !isfinite(eye[2]))
        return -1;
    for (int32_t i = 0; i < zone_count; ++i) {
        if (scene3d_visibility_zone_contains_point(&scene->visibility_zones[i], eye))
            return i;
    }
    return -1;
}

/// @brief Legacy reachability flood-fill: mark every zone reachable through any portal.
/// @param scene Borrowed scene containing zones and directed portals.
/// @param out Borrowed active PVS context whose bitmap is populated.
/// @param camera_zone Valid starting zone index.
/// @param zone_count Safe zone count.
/// @param portal_count Safe directed portal count.
/// @param queue Caller-owned scratch queue with @p zone_count entries.
static void scene3d_pvs_flood_fill(rt_scene3d *scene,
                                   scene3d_pvs_context_t *out,
                                   int32_t camera_zone,
                                   int32_t zone_count,
                                   int32_t portal_count,
                                   int32_t *queue) {
    int32_t head = 0;
    int32_t tail = 0;
    out->visible_zones[camera_zone] = 1;
    queue[tail++] = camera_zone;
    while (head < tail) {
        int32_t zone = queue[head++];
        for (int32_t i = 0; i < portal_count; ++i) {
            const rt_scene3d_visibility_portal *portal = &scene->visibility_portals[i];
            if (portal->from_zone != zone || portal->to_zone < 0 || portal->to_zone >= zone_count)
                continue;
            if (!out->visible_zones[portal->to_zone]) {
                out->visible_zones[portal->to_zone] = 1;
                queue[tail++] = portal->to_zone;
            }
        }
    }
}

/// @brief Project a portal's world-space box through the culling VP into an NDC XY rect.
/// @details Portals are authored as zone links without geometry, so the "portal frame" is
///   the per-axis interval between the two zone AABBs — their intersection when they
///   overlap or touch, the gap slab when they are separated. Corners straddling the near
///   plane degrade to the full view rect (classic conservative portal handling).
/// @param vp Borrowed row-major culling view-projection matrix.
/// @param a Borrowed source visibility zone.
/// @param b Borrowed destination visibility zone.
/// @param out_rect Output receiving `[min_x, min_y, max_x, max_y]` in NDC.
/// @return 0 = portal entirely behind the camera (cull), 1 = @p out_rect valid,
///         2 = conservative full-view rect.
static int scene3d_portal_ndc_rect(const float *vp,
                                   const rt_scene3d_visibility_zone *a,
                                   const rt_scene3d_visibility_zone *b,
                                   double out_rect[4]) {
    double bmin[3];
    double bmax[3];
    double min_x = 1e30, min_y = 1e30, max_x = -1e30, max_y = -1e30;
    int behind = 0;
    int front = 0;
    for (int axis = 0; axis < 3; ++axis) {
        double lo = fmax(a->world_min[axis], b->world_min[axis]);
        double hi = fmin(a->world_max[axis], b->world_max[axis]);
        bmin[axis] = fmin(lo, hi);
        bmax[axis] = fmax(lo, hi);
    }
    for (int i = 0; i < 8; ++i) {
        double p0 = (i & 1) ? bmax[0] : bmin[0];
        double p1 = (i & 2) ? bmax[1] : bmin[1];
        double p2 = (i & 4) ? bmax[2] : bmin[2];
        double cw = vp[12] * p0 + vp[13] * p1 + vp[14] * p2 + vp[15];
        if (!isfinite(cw) || cw <= 1e-9) {
            behind = 1;
            continue;
        }
        front = 1;
        double inv = 1.0 / cw;
        double nx = (vp[0] * p0 + vp[1] * p1 + vp[2] * p2 + vp[3]) * inv;
        double ny = (vp[4] * p0 + vp[5] * p1 + vp[6] * p2 + vp[7]) * inv;
        if (!isfinite(nx) || !isfinite(ny))
            return 2;
        min_x = fmin(min_x, nx);
        min_y = fmin(min_y, ny);
        max_x = fmax(max_x, nx);
        max_y = fmax(max_y, ny);
    }
    if (!front)
        return 0;
    if (behind)
        return 2;
    out_rect[0] = fmax(min_x, -1.0);
    out_rect[1] = fmax(min_y, -1.0);
    out_rect[2] = fmin(max_x, 1.0);
    out_rect[3] = fmin(max_y, 1.0);
    if (out_rect[0] > out_rect[2] || out_rect[1] > out_rect[3])
        return 0;
    return 1;
}

/// @brief Portal-frustum PVS: propagate screen-space view windows through portals.
/// @details Each zone accumulates the union NDC rect through which it can be seen.
///   A portal only propagates when its projected rect intersects the source zone's
///   window; the target inherits the intersection. Runs to a fixpoint (a wider path
///   discovered later re-expands the target) with a hard expansion cap for safety.
///   Falls back to flood-fill mid-run only via the caller's toggle, never silently.
/// @param scene Borrowed scene containing zones and directed portals.
/// @param out Borrowed active PVS context whose bitmap is populated.
/// @param vp Borrowed culling view-projection matrix.
/// @param camera_zone Valid starting zone index.
/// @param zone_count Safe zone count.
/// @param portal_count Safe directed portal count.
/// @param queue Caller-owned circular queue scratch.
/// @param zone_rects Caller-owned four-double NDC rectangle per zone.
/// @param in_queue Caller-owned byte membership flags.
static void scene3d_pvs_portal_clip(rt_scene3d *scene,
                                    scene3d_pvs_context_t *out,
                                    const float *vp,
                                    int32_t camera_zone,
                                    int32_t zone_count,
                                    int32_t portal_count,
                                    int32_t *queue,
                                    double *zone_rects,
                                    uint8_t *in_queue) {
    int32_t head = 0;
    int32_t tail = 0;
    int32_t expansions = 0;
    const int32_t expansion_cap = 4096;
    for (int32_t i = 0; i < zone_count; ++i) {
        zone_rects[i * 4 + 0] = 1.0;
        zone_rects[i * 4 + 1] = 1.0;
        zone_rects[i * 4 + 2] = -1.0;
        zone_rects[i * 4 + 3] = -1.0; /* empty */
    }
    zone_rects[camera_zone * 4 + 0] = -1.0;
    zone_rects[camera_zone * 4 + 1] = -1.0;
    zone_rects[camera_zone * 4 + 2] = 1.0;
    zone_rects[camera_zone * 4 + 3] = 1.0;
    out->visible_zones[camera_zone] = 1;
    queue[tail] = camera_zone;
    in_queue[camera_zone] = 1;
    tail = (tail + 1) % zone_count;
    int32_t queued = 1;
    while (queued > 0 && expansions < expansion_cap) {
        int32_t zone = queue[head];
        head = (head + 1) % zone_count;
        in_queue[zone] = 0;
        queued--;
        const double *src = &zone_rects[zone * 4];
        for (int32_t i = 0; i < portal_count; ++i) {
            const rt_scene3d_visibility_portal *portal = &scene->visibility_portals[i];
            double rect[4];
            double clipped[4];
            int kind;
            if (portal->from_zone != zone || portal->to_zone < 0 || portal->to_zone >= zone_count ||
                portal->from_zone < 0 || portal->from_zone >= zone_count)
                continue;
            expansions++;
            kind = scene3d_portal_ndc_rect(vp,
                                           &scene->visibility_zones[portal->from_zone],
                                           &scene->visibility_zones[portal->to_zone],
                                           rect);
            if (kind == 0)
                continue;
            if (kind == 2) {
                rect[0] = -1.0;
                rect[1] = -1.0;
                rect[2] = 1.0;
                rect[3] = 1.0;
            }
            clipped[0] = fmax(rect[0], src[0]);
            clipped[1] = fmax(rect[1], src[1]);
            clipped[2] = fmin(rect[2], src[2]);
            clipped[3] = fmin(rect[3], src[3]);
            if (clipped[0] > clipped[2] || clipped[1] > clipped[3])
                continue;
            double *dst = &zone_rects[portal->to_zone * 4];
            double grown[4] = {fmin(dst[0], clipped[0]),
                               fmin(dst[1], clipped[1]),
                               fmax(dst[2], clipped[2]),
                               fmax(dst[3], clipped[3])};
            if (out->visible_zones[portal->to_zone] && grown[0] == dst[0] && grown[1] == dst[1] &&
                grown[2] == dst[2] && grown[3] == dst[3])
                continue;
            memcpy(dst, grown, sizeof(grown));
            out->visible_zones[portal->to_zone] = 1;
            if (!in_queue[portal->to_zone]) {
                queue[tail] = portal->to_zone;
                tail = (tail + 1) % zone_count;
                in_queue[portal->to_zone] = 1;
                queued++;
            }
        }
    }
    scene->last_portal_traversal_count = expansions;
}

/// @brief Build the potentially-visible-set for the camera's current zone.
/// @details Locates the zone containing the camera, then propagates visibility through
///          the authored portals: by default each portal narrows the view to its
///          projected screen window (portal-frustum culling); the legacy
///          reachability-only flood-fill remains available via SetPortalClipping(false)
///          and is used automatically when no culling VP is supplied. The result is a
///          per-zone visibility bitmap consumed by scene3d_pvs_allows_aabb. Left
///          inactive (zeroed) when the scene has no zones, the camera is outside all
///          zones, or allocation fails.
/// @param scene Borrowed scene containing visibility authoring.
/// @param cam Borrowed active camera.
/// @param vp Optional borrowed culling view-projection matrix.
/// @param out Output context, zeroed when no active PVS can be produced.
/// @return 1 if an active PVS context was produced; 0 otherwise (with @p out zeroed).
static int scene3d_build_pvs_context(rt_scene3d *scene,
                                     rt_camera3d *cam,
                                     const float *vp,
                                     scene3d_pvs_context_t *out) {
    int32_t zone_count;
    int32_t portal_count;
    int32_t camera_zone;
    int32_t *queue = NULL;
    if (!scene || !cam || !out)
        return 0;
    scene->last_portal_traversal_count = 0;
    zone_count = scene3d_visibility_zone_count_safe(scene);
    portal_count = scene3d_visibility_portal_count_safe(scene);
    if (zone_count <= 0)
        return 0;
    camera_zone = scene3d_visibility_find_camera_zone(scene, cam);
    if (camera_zone < 0)
        return 0;
    out->visible_zones = (uint8_t *)calloc((size_t)zone_count, 1);
    queue = (int32_t *)malloc((size_t)zone_count * sizeof(queue[0]));
    if (!out->visible_zones || !queue) {
        free(out->visible_zones);
        free(queue);
        memset(out, 0, sizeof(*out));
        return 0;
    }
    out->zone_count = zone_count;
    out->active = 1;
    if (vp && !scene->portal_clipping_disabled) {
        double *zone_rects = (double *)malloc((size_t)zone_count * 4u * sizeof(double));
        uint8_t *in_queue = (uint8_t *)calloc((size_t)zone_count, 1);
        if (zone_rects && in_queue) {
            scene3d_pvs_portal_clip(
                scene, out, vp, camera_zone, zone_count, portal_count, queue, zone_rects, in_queue);
            free(zone_rects);
            free(in_queue);
            free(queue);
            return 1;
        }
        free(zone_rects);
        free(in_queue);
    }
    scene3d_pvs_flood_fill(scene, out, camera_zone, zone_count, portal_count, queue);
    free(queue);
    return 1;
}

/// @brief Release a PVS context's zone bitmap and reset it to the inactive state. Safe on NULL.
/// @param pvs Borrowed context whose owned bitmap is freed.
static void scene3d_pvs_context_clear(scene3d_pvs_context_t *pvs) {
    if (!pvs)
        return;
    free(pvs->visible_zones);
    memset(pvs, 0, sizeof(*pvs));
}

/// @brief Test whether an AABB may be visible under the current PVS.
/// @details An AABB passes if it overlaps no authored zone (zones do not cover it) or overlaps at
///          least one zone flagged visible in @p pvs. Returns visible (1) whenever the PVS is
///          inactive, so visibility culling degrades safely to a no-op.
/// @param scene Borrowed scene containing authored zones.
/// @param pvs Borrowed current PVS context.
/// @param world_min Borrowed world-space AABB minimum.
/// @param world_max Borrowed world-space AABB maximum.
/// @return 1 if the AABB should be considered visible; 0 if every overlapping zone is culled.
static int scene3d_pvs_allows_aabb(const rt_scene3d *scene,
                                   const scene3d_pvs_context_t *pvs,
                                   const double world_min[3],
                                   const double world_max[3]) {
    int matched_zone = 0;
    int32_t zone_count;
    int32_t visible_zone_count;
    if (!scene || !pvs || !pvs->active || !pvs->visible_zones || !world_min || !world_max)
        return 1;
    zone_count = scene3d_visibility_zone_count_safe(scene);
    visible_zone_count = pvs->zone_count < zone_count ? pvs->zone_count : zone_count;
    for (int32_t i = 0; i < visible_zone_count; ++i) {
        const rt_scene3d_visibility_zone *zone = &scene->visibility_zones[i];
        if (!scene3d_aabb_intersects_aabb(world_min, world_max, zone->world_min, zone->world_max))
            continue;
        matched_zone = 1;
        if (pvs->visible_zones[i])
            return 1;
    }
    return matched_zone ? 0 : 1;
}

/// @brief Compute the AABB swept by a ray segment (origin to origin + dir·length) for broadphase
/// culling.
/// @param origin Borrowed ray origin.
/// @param direction Borrowed ray direction, normalized internally.
/// @param max_distance Non-negative sweep length.
/// @param out_min Output receiving swept AABB minimum.
/// @param out_max Output receiving swept AABB maximum.
/// @return Nonzero when finite canonical bounds were produced.
int scene3d_ray_sweep_bounds(const double origin[3],
                             const double direction[3],
                             double max_distance,
                             double out_min[3],
                             double out_max[3]) {
    double o[3] = {origin ? origin[0] : 0.0, origin ? origin[1] : 0.0, origin ? origin[2] : 0.0};
    double d[3] = {direction ? direction[0] : 0.0,
                   direction ? direction[1] : 0.0,
                   direction ? direction[2] : 0.0};
    double end[3];
    if (!origin || !direction || !out_min || !out_max)
        return 0;
    scene3d_sanitize_vec3d(o, 0.0);
    scene3d_sanitize_vec3d(d, 0.0);
    if (!scene3d_normalize_vec3d(d))
        return 0;
    if (!isfinite(max_distance) || max_distance > SCENE3D_ABS_MAX)
        max_distance = SCENE3D_ABS_MAX;
    if (max_distance < 0.0)
        return 0;
    for (int i = 0; i < 3; ++i) {
        double e = o[i] + d[i] * max_distance;
        if (!isfinite(e))
            return 0;
        end[i] = scene3d_clamp_abs_or(e, 0.0);
        out_min[i] = fmin(o[i], end[i]);
        out_max[i] = fmax(o[i], end[i]);
    }
    scene3d_canonicalize_aabb_d(out_min, out_max);
    return 1;
}

/// @brief Compute a world-space AABB enclosing the view frustum of a view-projection matrix.
/// @details Unprojects the eight clip-space corners through the inverse VP and bounds them, giving
/// a
///          coarse AABB the spatial index can use to reject off-screen nodes before exact culling.
/// @param vp Borrowed finite row-major view-projection matrix.
/// @param out_min Output receiving world-space frustum minimum.
/// @param out_max Output receiving world-space frustum maximum.
/// @return Nonzero when the inverse projection yields eight finite corners.
static int scene3d_frustum_bounds_from_vp(const float vp[16],
                                          double out_min[3],
                                          double out_max[3]) {
    double m[16];
    double inv[16];
    if (!vp || !out_min || !out_max)
        return 0;
    for (int i = 0; i < 16; ++i) {
        if (!isfinite(vp[i]))
            return 0;
        m[i] = (double)vp[i];
    }
    if (mat4d_invert(m, inv) != 0)
        return 0;
    scene_bounds_reset_d(out_min, out_max);
    for (int xi = 0; xi < 2; ++xi) {
        for (int yi = 0; yi < 2; ++yi) {
            for (int zi = 0; zi < 2; ++zi) {
                double x = xi ? 1.0 : -1.0;
                double y = yi ? 1.0 : -1.0;
                double z = zi ? 1.0 : -1.0;
                double wx = inv[0] * x + inv[1] * y + inv[2] * z + inv[3];
                double wy = inv[4] * x + inv[5] * y + inv[6] * z + inv[7];
                double wz = inv[8] * x + inv[9] * y + inv[10] * z + inv[11];
                double ww = inv[12] * x + inv[13] * y + inv[14] * z + inv[15];
                double point[3];
                if (!isfinite(ww) || fabs(ww) <= 1.0e-12)
                    return 0;
                wx /= ww;
                wy /= ww;
                wz /= ww;
                if (!isfinite(wx) || !isfinite(wy) || !isfinite(wz))
                    return 0;
                point[0] = wx;
                point[1] = wy;
                point[2] = wz;
                scene_bounds_include_point_d(out_min, out_max, point);
            }
        }
    }
    return out_min[0] <= out_max[0] && out_min[1] <= out_max[1] && out_min[2] <= out_max[2];
}
