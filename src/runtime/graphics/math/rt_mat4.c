//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/math/rt_mat4.c
// Purpose: 4x4 matrix mathematics for the Zanna.Mat4 class. Implements 3D
//   affine and projective transforms: translation, rotation (from quaternion or
//   axis-angle), scale, matrix multiplication, transpose, determinant, inverse,
//   perspective and orthographic projection, and Vec3 point/direction transformation.
//   Used by the 3D scene graph, camera, and skeletal animation systems.
//
// Key invariants:
//   - Elements are stored in row-major order: m[r*4+c] accesses row r, column c.
//   - Affine factory matrices use bottom row (0,0,0,1); New and arithmetic
//     operations can produce an arbitrary bottom row.
//   - Rotation basis vectors (X, Y, Z columns) represent the transformed axes;
//     translation is stored in column 3 (Tx, Ty, Tz).
//   - Mat4 objects are immutable after creation; all operations return new
//     objects allocated from the GC heap.
//   - Inverse is computed via cofactor expansion; a non-finite determinant or
//     |det| < 1e-15 traps ("singular matrix"), consistent with Mat3.Inverse
//     (VDOC-208) — it never returns identity as an ambiguous error sentinel.
//   - Perspective projection uses a right-handed coordinate system, depth range
//     [-1, 1] (OpenGL convention), with near/far clip planes.
//
// Ownership/Lifetime:
//   - All Mat4 objects are allocated via rt_obj_new_i64 (GC heap); no manual
//     free is required. The mat4_impl struct contains only a double[16] array.
//
// Links: src/runtime/graphics/math/rt_mat4.h (public API),
//        src/runtime/graphics/math/rt_vec3.h (Vec3 operand and result type),
//        src/runtime/graphics/math/rt_quat.h (quaternion-to-matrix conversion)
//
//===----------------------------------------------------------------------===//

#include "rt_mat4.h"

#include "rt_object.h"
#include "rt_trap.h"
#include "rt_vec3.h"

#include <math.h>
#include <stdlib.h>

//=============================================================================
// Thread-local free-list pool (mirrors Vec3 pool pattern)
//=============================================================================
#define MAT4_POOL_CAPACITY 32

static _Thread_local void *mat4_pool_buf_[MAT4_POOL_CAPACITY];
static _Thread_local int mat4_pool_top_ = 0;

/// @brief GC finalizer / pool-return hook for Mat4 objects.
/// @details When the GC is about to reclaim a Mat4 whose reference count has
///   dropped to zero, this function intercepts the reclamation by calling
///   rt_obj_resurrect to cancel the pending free, then re-registers itself as
///   the finalizer so the next release will call it again.  The revived object
///   is pushed onto the thread-local free-list (mat4_pool_buf_) up to
///   MAT4_POOL_CAPACITY entries.  Once the pool is full the function simply
///   returns without resurrecting, allowing the GC to complete the free.
///   This avoids repeated heap allocation for short-lived intermediate matrices
///   that arise in transform chains and animation blending.
/// @param p Mat4 payload whose finalizer is running.
static void mat4_pool_return(void *p) {
    if (mat4_pool_top_ < MAT4_POOL_CAPACITY) {
        rt_obj_resurrect(p);
        rt_obj_set_finalizer(p, mat4_pool_return);
        mat4_pool_buf_[mat4_pool_top_++] = p;
    }
}

//=============================================================================
// Internal Structure
//=============================================================================

/// @brief 4x4 matrix stored in row-major order.
typedef struct mat4_impl {
    double m[16]; ///< Elements in row-major order
} mat4_impl;

#define M(mat, r, c) ((mat)->m[(r) * 4 + (c)])

/// @brief Acquire storage for a Mat4 from the current thread's pool or the GC heap.
/// @details Reuses the most recently pooled payload when available. A newly allocated payload is
///          registered with @ref mat4_pool_return so it can enter the pool when reclaimed.
/// @return Writable Mat4 payload, or NULL if a required heap allocation fails.
static mat4_impl *mat4_alloc(void) {
    mat4_impl *mat;
    if (mat4_pool_top_ > 0) {
        mat = (mat4_impl *)mat4_pool_buf_[--mat4_pool_top_];
    } else {
        mat = (mat4_impl *)rt_obj_new_i64(RT_MAT4_CLASS_ID, sizeof(mat4_impl));
        if (!mat)
            return NULL;
        rt_obj_set_finalizer(mat, mat4_pool_return);
    }
    return mat;
}

/// @brief Safe-cast an opaque handle to mat4_impl.
/// @details With RT_MAT4_INTERNAL_ASSUME_STRUCT_HANDLE the pointer is trusted
///          directly (fast path); otherwise the object's class id is validated
///          against RT_MAT4_CLASS_ID, returning NULL on mismatch or NULL input.
/// @param m Candidate Mat4 runtime handle.
/// @return Typed Mat4 payload, or NULL when validation is enabled and @p m is incompatible.
static mat4_impl *mat4_checked(void *m) {
#ifdef RT_MAT4_INTERNAL_ASSUME_STRUCT_HANDLE
    return (mat4_impl *)m;
#else
    if (!rt_obj_is_instance(m, RT_MAT4_CLASS_ID, sizeof(mat4_impl)))
        return NULL;
    return (mat4_impl *)m;
#endif
}

/// @brief Compute a finite, overflow-resistant length for a 3D vector.
/// @details Matrix factory functions normalize axes and basis vectors before using them in
///          transform construction. Chained `hypot` avoids overflow in `sqrt(x*x + y*y + z*z)`;
///          non-finite input returns `INFINITY` so callers can fall back to identity.
/// @param x Vector x component.
/// @param y Vector y component.
/// @param z Vector z component.
/// @return Euclidean length, or positive infinity when any component is non-finite.
static double mat4_safe_len3(double x, double y, double z) {
    if (!isfinite(x) || !isfinite(y) || !isfinite(z))
        return INFINITY;
    return hypot(hypot(x, y), z);
}

//=============================================================================
// Construction
//=============================================================================

/// @brief Construct a 4x4 matrix from sixteen row-major scalar values.
/// @details Stores the supplied values without normalization or affine-layout validation. Storage
///          is acquired from the current thread's Mat4 pool when possible.
/// @param m00 Element at row 0, column 0.
/// @param m01 Element at row 0, column 1.
/// @param m02 Element at row 0, column 2.
/// @param m03 Element at row 0, column 3.
/// @param m10 Element at row 1, column 0.
/// @param m11 Element at row 1, column 1.
/// @param m12 Element at row 1, column 2.
/// @param m13 Element at row 1, column 3.
/// @param m20 Element at row 2, column 0.
/// @param m21 Element at row 2, column 1.
/// @param m22 Element at row 2, column 2.
/// @param m23 Element at row 2, column 3.
/// @param m30 Element at row 3, column 0.
/// @param m31 Element at row 3, column 1.
/// @param m32 Element at row 3, column 2.
/// @param m33 Element at row 3, column 3.
/// @return Newly acquired Mat4 handle, or NULL if allocation fails.
void *rt_mat4_new(double m00,
                  double m01,
                  double m02,
                  double m03,
                  double m10,
                  double m11,
                  double m12,
                  double m13,
                  double m20,
                  double m21,
                  double m22,
                  double m23,
                  double m30,
                  double m31,
                  double m32,
                  double m33) {
    mat4_impl *mat = mat4_alloc();
    if (!mat)
        return NULL;

    mat->m[0] = m00;
    mat->m[1] = m01;
    mat->m[2] = m02;
    mat->m[3] = m03;
    mat->m[4] = m10;
    mat->m[5] = m11;
    mat->m[6] = m12;
    mat->m[7] = m13;
    mat->m[8] = m20;
    mat->m[9] = m21;
    mat->m[10] = m22;
    mat->m[11] = m23;
    mat->m[12] = m30;
    mat->m[13] = m31;
    mat->m[14] = m32;
    mat->m[15] = m33;

    return mat;
}

/// @brief Create a 4x4 identity matrix.
/// @return Newly acquired identity Mat4 handle, or NULL if allocation fails.
void *rt_mat4_identity(void) {
    return rt_mat4_new(
        1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0);
}

/// @brief Create a 4x4 matrix whose elements are all zero.
/// @return Newly acquired zero Mat4 handle, or NULL if allocation fails.
void *rt_mat4_zero(void) {
    return rt_mat4_new(
        0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
}

//=============================================================================
// 3D Transformation Factories
//=============================================================================

/// @brief Create a homogeneous 3D translation matrix.
/// @param tx Translation applied along the x axis.
/// @param ty Translation applied along the y axis.
/// @param tz Translation applied along the z axis.
/// @return Newly acquired affine Mat4 handle, or NULL if allocation fails.
void *rt_mat4_translate(double tx, double ty, double tz) {
    return rt_mat4_new(1.0, 0.0, 0.0, tx, 0.0, 1.0, 0.0, ty, 0.0, 0.0, 1.0, tz, 0.0, 0.0, 0.0, 1.0);
}

/// @brief Create a homogeneous 3D non-uniform scaling matrix.
/// @param sx Scale factor applied along the x axis.
/// @param sy Scale factor applied along the y axis.
/// @param sz Scale factor applied along the z axis.
/// @return Newly acquired affine Mat4 handle, or NULL if allocation fails.
void *rt_mat4_scale(double sx, double sy, double sz) {
    return rt_mat4_new(sx, 0.0, 0.0, 0.0, 0.0, sy, 0.0, 0.0, 0.0, 0.0, sz, 0.0, 0.0, 0.0, 0.0, 1.0);
}

/// @brief Create a homogeneous 3D uniform scaling matrix.
/// @param s Scale factor applied along all three axes.
/// @return Newly acquired affine Mat4 handle, or NULL if allocation fails.
void *rt_mat4_scale_uniform(double s) {
    return rt_mat4_scale(s, s, s);
}

/// @brief Create a right-handed rotation matrix about the x axis.
/// @details A non-finite angle is treated as zero and produces an identity matrix.
/// @param angle Rotation angle in radians.
/// @return Newly acquired affine Mat4 handle, or NULL if allocation fails.
void *rt_mat4_rotate_x(double angle) {
    if (!isfinite(angle))
        return rt_mat4_identity();
    double c = cos(angle);
    double s = sin(angle);
    return rt_mat4_new(1.0, 0.0, 0.0, 0.0, 0.0, c, -s, 0.0, 0.0, s, c, 0.0, 0.0, 0.0, 0.0, 1.0);
}

/// @brief Create a right-handed rotation matrix about the y axis.
/// @details A non-finite angle is treated as zero and produces an identity matrix.
/// @param angle Rotation angle in radians.
/// @return Newly acquired affine Mat4 handle, or NULL if allocation fails.
void *rt_mat4_rotate_y(double angle) {
    if (!isfinite(angle))
        return rt_mat4_identity();
    double c = cos(angle);
    double s = sin(angle);
    return rt_mat4_new(c, 0.0, s, 0.0, 0.0, 1.0, 0.0, 0.0, -s, 0.0, c, 0.0, 0.0, 0.0, 0.0, 1.0);
}

/// @brief Create a right-handed rotation matrix about the z axis.
/// @details A non-finite angle is treated as zero and produces an identity matrix.
/// @param angle Rotation angle in radians.
/// @return Newly acquired affine Mat4 handle, or NULL if allocation fails.
void *rt_mat4_rotate_z(double angle) {
    if (!isfinite(angle))
        return rt_mat4_identity();
    double c = cos(angle);
    double s = sin(angle);
    return rt_mat4_new(c, -s, 0.0, 0.0, s, c, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0);
}

/// @brief Create a right-handed rotation matrix about an arbitrary axis.
/// @details Normalizes @p axis and applies Rodrigues' rotation formula. A NULL, non-finite, or
///          near-zero axis, or a non-finite angle, produces an identity matrix.
/// @param axis Vec3 rotation axis.
/// @param angle Rotation angle in radians.
/// @return Newly acquired rotation Mat4, or an identity Mat4 for invalid input.
void *rt_mat4_rotate_axis(void *axis, double angle) {
    if (!axis || !isfinite(angle))
        return rt_mat4_identity();

    double x = rt_vec3_x(axis);
    double y = rt_vec3_y(axis);
    double z = rt_vec3_z(axis);

    // Normalize axis
    double len = mat4_safe_len3(x, y, z);
    if (len < 1e-15 || !isfinite(len))
        return rt_mat4_identity();
    x /= len;
    y /= len;
    z /= len;

    double c = cos(angle);
    double s = sin(angle);
    double t = 1.0 - c;

    return rt_mat4_new(t * x * x + c,
                       t * x * y - s * z,
                       t * x * z + s * y,
                       0.0,
                       t * x * y + s * z,
                       t * y * y + c,
                       t * y * z - s * x,
                       0.0,
                       t * x * z - s * y,
                       t * y * z + s * x,
                       t * z * z + c,
                       0.0,
                       0.0,
                       0.0,
                       0.0,
                       1.0);
}

//=============================================================================
// Projection Matrices
//=============================================================================

/// @brief Create a right-handed perspective projection matrix.
/// @details Maps view-space depth to normalized device coordinates in [-1, 1]. All arguments must
///          be finite, @p fov must lie strictly between zero and pi, @p aspect and @p near_val must
///          be positive, and @p far_val must exceed @p near_val. Invalid input produces identity.
/// @param fov Vertical field of view in radians.
/// @param aspect Viewport width divided by viewport height.
/// @param near_val Positive distance to the near clipping plane.
/// @param far_val Distance to the far clipping plane, greater than @p near_val.
/// @return Newly acquired projection Mat4, or an identity Mat4 for invalid input.
void *rt_mat4_perspective(double fov, double aspect, double near_val, double far_val) {
    if (!isfinite(fov) || !isfinite(aspect) || !isfinite(near_val) || !isfinite(far_val) ||
        fov <= 0.0 || fov >= 3.14159265358979323846 || aspect <= 0.0 || near_val <= 0.0 ||
        near_val >= far_val)
        return rt_mat4_identity();

    double tanHalfFov = tan(fov / 2.0);
    if (!isfinite(tanHalfFov) || tanHalfFov == 0.0)
        return rt_mat4_identity();
    double f = 1.0 / tanHalfFov;
    double nf = 1.0 / (near_val - far_val);

    return rt_mat4_new(f / aspect,
                       0.0,
                       0.0,
                       0.0,
                       0.0,
                       f,
                       0.0,
                       0.0,
                       0.0,
                       0.0,
                       (far_val + near_val) * nf,
                       2.0 * far_val * near_val * nf,
                       0.0,
                       0.0,
                       -1.0,
                       0.0);
}

/// @brief Create an orthographic projection from a view-space box.
/// @details Maps the box defined by the supplied planes to normalized device coordinates. All
///          arguments must be finite and each opposing plane pair must differ; otherwise an
///          identity matrix is returned.
/// @param left View-space coordinate of the left clipping plane.
/// @param right View-space coordinate of the right clipping plane.
/// @param bottom View-space coordinate of the bottom clipping plane.
/// @param top View-space coordinate of the top clipping plane.
/// @param near_val View-space coordinate of the near clipping plane.
/// @param far_val View-space coordinate of the far clipping plane.
/// @return Newly acquired projection Mat4, or an identity Mat4 for invalid input.
void *rt_mat4_ortho(
    double left, double right, double bottom, double top, double near_val, double far_val) {
    if (!isfinite(left) || !isfinite(right) || !isfinite(bottom) || !isfinite(top) ||
        !isfinite(near_val) || !isfinite(far_val) || right == left || top == bottom ||
        far_val == near_val)
        return rt_mat4_identity();

    double rl = 1.0 / (right - left);
    double tb = 1.0 / (top - bottom);
    double fn = 1.0 / (far_val - near_val);

    return rt_mat4_new(2.0 * rl,
                       0.0,
                       0.0,
                       -(right + left) * rl,
                       0.0,
                       2.0 * tb,
                       0.0,
                       -(top + bottom) * tb,
                       0.0,
                       0.0,
                       -2.0 * fn,
                       -(far_val + near_val) * fn,
                       0.0,
                       0.0,
                       0.0,
                       1.0);
}

/// @brief Create a right-handed view matrix for a camera pose.
/// @details Builds an orthonormal basis from the eye-to-target direction and @p up. NULL vectors,
///          non-finite components, a coincident eye and target, or an up vector parallel to the
///          viewing direction produce an identity matrix.
/// @param eye Vec3 camera position in world space.
/// @param target Vec3 world-space point toward which the camera looks.
/// @param up Vec3 reference up direction.
/// @return Newly acquired view Mat4, or an identity Mat4 for invalid or degenerate input.
void *rt_mat4_look_at(void *eye, void *target, void *up) {
    if (!eye || !target || !up)
        return rt_mat4_identity();

    double eyeX = rt_vec3_x(eye);
    double eyeY = rt_vec3_y(eye);
    double eyeZ = rt_vec3_z(eye);

    double targetX = rt_vec3_x(target);
    double targetY = rt_vec3_y(target);
    double targetZ = rt_vec3_z(target);

    double upX = rt_vec3_x(up);
    double upY = rt_vec3_y(up);
    double upZ = rt_vec3_z(up);

    // Forward vector (from eye to target)
    double fX = targetX - eyeX;
    double fY = targetY - eyeY;
    double fZ = targetZ - eyeZ;
    double fLen = mat4_safe_len3(fX, fY, fZ);
    if (fLen < 1e-15 || !isfinite(fLen))
        return rt_mat4_identity();
    fX /= fLen;
    fY /= fLen;
    fZ /= fLen;

    // Right vector (cross product of forward and up)
    double rX = fY * upZ - fZ * upY;
    double rY = fZ * upX - fX * upZ;
    double rZ = fX * upY - fY * upX;
    double rLen = mat4_safe_len3(rX, rY, rZ);
    if (rLen < 1e-15 || !isfinite(rLen))
        return rt_mat4_identity();
    rX /= rLen;
    rY /= rLen;
    rZ /= rLen;

    // Recalculate up vector (cross product of right and forward)
    double uX = rY * fZ - rZ * fY;
    double uY = rZ * fX - rX * fZ;
    double uZ = rX * fY - rY * fX;

    return rt_mat4_new(rX,
                       rY,
                       rZ,
                       -(rX * eyeX + rY * eyeY + rZ * eyeZ),
                       uX,
                       uY,
                       uZ,
                       -(uX * eyeX + uY * eyeY + uZ * eyeZ),
                       -fX,
                       -fY,
                       -fZ,
                       fX * eyeX + fY * eyeY + fZ * eyeZ,
                       0.0,
                       0.0,
                       0.0,
                       1.0);
}

//=============================================================================
// Element Access
//=============================================================================

/// @brief Read one matrix element by zero-based row and column.
/// @param m Mat4 handle to inspect.
/// @param row Zero-based row index in the inclusive range [0, 3].
/// @param col Zero-based column index in the inclusive range [0, 3].
/// @return Selected element, or 0.0 for an invalid matrix or out-of-range coordinate.
double rt_mat4_get(void *m, int64_t row, int64_t col) {
    if (!m || row < 0 || row > 3 || col < 0 || col > 3)
        return 0.0;

    mat4_impl *mat = mat4_checked(m);
    if (!mat)
        return 0.0;
    return M(mat, row, col);
}

//=============================================================================
// Arithmetic
//=============================================================================

/// @brief Add two matrices element by element.
/// @param a Left-hand Mat4 operand.
/// @param b Right-hand Mat4 operand.
/// @return Newly acquired sum, or a zero Mat4 when either operand is invalid.
void *rt_mat4_add(void *a, void *b) {
    mat4_impl *ma = mat4_checked(a);
    mat4_impl *mb = mat4_checked(b);
    if (!ma || !mb)
        return rt_mat4_zero();

    double r[16];
    for (int i = 0; i < 16; i++)
        r[i] = ma->m[i] + mb->m[i];

    return rt_mat4_new(r[0],
                       r[1],
                       r[2],
                       r[3],
                       r[4],
                       r[5],
                       r[6],
                       r[7],
                       r[8],
                       r[9],
                       r[10],
                       r[11],
                       r[12],
                       r[13],
                       r[14],
                       r[15]);
}

/// @brief Subtract one matrix from another element by element.
/// @param a Mat4 minuend.
/// @param b Mat4 subtrahend.
/// @return Newly acquired difference, or a zero Mat4 when either operand is invalid.
void *rt_mat4_sub(void *a, void *b) {
    mat4_impl *ma = mat4_checked(a);
    mat4_impl *mb = mat4_checked(b);
    if (!ma || !mb)
        return rt_mat4_zero();

    double r[16];
    for (int i = 0; i < 16; i++)
        r[i] = ma->m[i] - mb->m[i];

    return rt_mat4_new(r[0],
                       r[1],
                       r[2],
                       r[3],
                       r[4],
                       r[5],
                       r[6],
                       r[7],
                       r[8],
                       r[9],
                       r[10],
                       r[11],
                       r[12],
                       r[13],
                       r[14],
                       r[15]);
}

/// @brief Multiply two matrices using standard row-by-column multiplication.
/// @details For column-vector transforms, `mul(A, B)` applies B first and A second. Invalid
///          operands produce an identity matrix.
/// @param a Left-hand Mat4 operand.
/// @param b Right-hand Mat4 operand.
/// @return Newly acquired product, or an identity Mat4 when either operand is invalid.
void *rt_mat4_mul(void *a, void *b) {
    mat4_impl *ma = mat4_checked(a);
    mat4_impl *mb = mat4_checked(b);
    if (!ma || !mb)
        return rt_mat4_identity();

    double r[16];
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            r[i * 4 + j] =
                ma->m[i * 4 + 0] * mb->m[0 * 4 + j] + ma->m[i * 4 + 1] * mb->m[1 * 4 + j] +
                ma->m[i * 4 + 2] * mb->m[2 * 4 + j] + ma->m[i * 4 + 3] * mb->m[3 * 4 + j];
        }
    }

    return rt_mat4_new(r[0],
                       r[1],
                       r[2],
                       r[3],
                       r[4],
                       r[5],
                       r[6],
                       r[7],
                       r[8],
                       r[9],
                       r[10],
                       r[11],
                       r[12],
                       r[13],
                       r[14],
                       r[15]);
}

/// @brief Multiply every matrix element by a scalar.
/// @param m Mat4 operand.
/// @param s Scalar multiplier.
/// @return Newly acquired scaled matrix, or a zero Mat4 for an invalid matrix.
void *rt_mat4_mul_scalar(void *m, double s) {
    mat4_impl *mat = mat4_checked(m);
    if (!mat)
        return rt_mat4_zero();

    double r[16];
    for (int i = 0; i < 16; i++)
        r[i] = mat->m[i] * s;

    return rt_mat4_new(r[0],
                       r[1],
                       r[2],
                       r[3],
                       r[4],
                       r[5],
                       r[6],
                       r[7],
                       r[8],
                       r[9],
                       r[10],
                       r[11],
                       r[12],
                       r[13],
                       r[14],
                       r[15]);
}

/// @brief Transform a 3D point and perform its homogeneous perspective divide.
/// @details Treats @p v as `(x, y, z, 1)`. When the resulting w differs from one, divides x, y,
///          and z by w. Invalid or non-finite input, non-finite output, or a result with
///          `|w| <= 1e-15` produces a zero vector.
/// @param m Mat4 transform.
/// @param v Vec3 point to transform.
/// @return Newly allocated transformed Vec3, or a zero Vec3 when transformation is invalid.
void *rt_mat4_transform_point(void *m, void *v) {
    if (!m || !v)
        return rt_vec3_zero();

    mat4_impl *mat = mat4_checked(m);
    if (!mat)
        return rt_vec3_zero();
    double x = rt_vec3_x(v);
    double y = rt_vec3_y(v);
    double z = rt_vec3_z(v);
    if (!isfinite(x) || !isfinite(y) || !isfinite(z))
        return rt_vec3_zero();

    // Transform as [x, y, z, 1]
    double rx = mat->m[0] * x + mat->m[1] * y + mat->m[2] * z + mat->m[3];
    double ry = mat->m[4] * x + mat->m[5] * y + mat->m[6] * z + mat->m[7];
    double rz = mat->m[8] * x + mat->m[9] * y + mat->m[10] * z + mat->m[11];
    double rw = mat->m[12] * x + mat->m[13] * y + mat->m[14] * z + mat->m[15];

    // Perspective divide
    if (!isfinite(rx) || !isfinite(ry) || !isfinite(rz) || !isfinite(rw) || fabs(rw) <= 1e-15)
        return rt_vec3_zero();
    if (fabs(rw - 1.0) > 1e-15) {
        rx /= rw;
        ry /= rw;
        rz /= rw;
        if (!isfinite(rx) || !isfinite(ry) || !isfinite(rz))
            return rt_vec3_zero();
    }

    return rt_vec3_new(rx, ry, rz);
}

/// @brief Transform a 3D direction using the upper-left 3x3 matrix block.
/// @details Treats @p v as `(x, y, z, 0)`, so translation and the bottom matrix row do not
///          contribute. NULL or incompatible inputs produce a zero vector.
/// @param m Mat4 transform.
/// @param v Vec3 direction to transform.
/// @return Newly allocated transformed Vec3, or a zero Vec3 for invalid input.
void *rt_mat4_transform_vec(void *m, void *v) {
    if (!m || !v)
        return rt_vec3_zero();

    mat4_impl *mat = mat4_checked(m);
    if (!mat)
        return rt_vec3_zero();
    double x = rt_vec3_x(v);
    double y = rt_vec3_y(v);
    double z = rt_vec3_z(v);

    // Transform as [x, y, z, 0] (ignores translation)
    double rx = mat->m[0] * x + mat->m[1] * y + mat->m[2] * z;
    double ry = mat->m[4] * x + mat->m[5] * y + mat->m[6] * z;
    double rz = mat->m[8] * x + mat->m[9] * y + mat->m[10] * z;

    return rt_vec3_new(rx, ry, rz);
}

//=============================================================================
// Matrix Operations
//=============================================================================

/// @brief Create the mathematical transpose of a matrix.
/// @param m Mat4 operand.
/// @return Newly acquired transpose, or an identity Mat4 for an invalid matrix.
void *rt_mat4_transpose(void *m) {
    mat4_impl *mat = mat4_checked(m);
    if (!mat)
        return rt_mat4_identity();

    return rt_mat4_new(mat->m[0],
                       mat->m[4],
                       mat->m[8],
                       mat->m[12],
                       mat->m[1],
                       mat->m[5],
                       mat->m[9],
                       mat->m[13],
                       mat->m[2],
                       mat->m[6],
                       mat->m[10],
                       mat->m[14],
                       mat->m[3],
                       mat->m[7],
                       mat->m[11],
                       mat->m[15]);
}

/// @brief Compute a 4x4 matrix determinant from paired 2x2 minors.
/// @param m Mat4 operand.
/// @return Determinant of @p m, or 0.0 for an invalid matrix.
double rt_mat4_det(void *m) {
    mat4_impl *mat = mat4_checked(m);
    if (!mat)
        return 0.0;

    double *a = mat->m;

    // Compute 2x2 determinants
    double s0 = a[0] * a[5] - a[1] * a[4];
    double s1 = a[0] * a[6] - a[2] * a[4];
    double s2 = a[0] * a[7] - a[3] * a[4];
    double s3 = a[1] * a[6] - a[2] * a[5];
    double s4 = a[1] * a[7] - a[3] * a[5];
    double s5 = a[2] * a[7] - a[3] * a[6];

    double c5 = a[10] * a[15] - a[11] * a[14];
    double c4 = a[9] * a[15] - a[11] * a[13];
    double c3 = a[9] * a[14] - a[10] * a[13];
    double c2 = a[8] * a[15] - a[11] * a[12];
    double c1 = a[8] * a[14] - a[10] * a[12];
    double c0 = a[8] * a[13] - a[9] * a[12];

    return s0 * c5 - s1 * c4 + s2 * c3 + s3 * c2 - s4 * c1 + s5 * c0;
}

/// @brief Compute the 4×4 inverse of `a` into a fresh Mat4, or return NULL when
///        the matrix is singular (non-finite determinant or `|det| < 1e-15`).
/// @details Shared core for the trapping public `rt_mat4_inverse` and the
///          non-trapping internal `rt_mat4_try_inverse` (VDOC-208). Never
///          fabricates an identity result on failure.
/// @param a Pointer to sixteen row-major matrix elements.
/// @return Newly acquired inverse Mat4, or NULL for a singular matrix or allocation failure.
static void *mat4_inverse_or_null(const double *a) {
    double inv[16];
    double det;
    double inv_det;

    inv[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] +
             a[9] * a[7] * a[14] + a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
    inv[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] -
             a[8] * a[7] * a[14] - a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
    inv[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] +
             a[8] * a[7] * a[13] + a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
    inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] -
              a[8] * a[6] * a[13] - a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
    inv[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] -
             a[9] * a[3] * a[14] - a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
    inv[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] +
             a[8] * a[3] * a[14] + a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
    inv[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] -
             a[8] * a[3] * a[13] - a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
    inv[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] +
              a[8] * a[2] * a[13] + a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
    inv[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] + a[5] * a[3] * a[14] +
             a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
    inv[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] -
             a[4] * a[3] * a[14] - a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
    inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] +
              a[4] * a[3] * a[13] + a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
    inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] -
              a[4] * a[2] * a[13] - a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
    inv[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] -
             a[5] * a[3] * a[10] - a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
    inv[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] + a[4] * a[3] * a[10] +
             a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
    inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] - a[4] * a[3] * a[9] -
              a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
    inv[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] + a[4] * a[2] * a[9] +
              a[8] * a[1] * a[6] - a[8] * a[2] * a[5];

    det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
    if (!isfinite(det) || fabs(det) < 1e-15)
        return NULL; // singular / non-invertible

    inv_det = 1.0 / det;
    for (int i = 0; i < 16; i++)
        inv[i] *= inv_det;

    return rt_mat4_new(inv[0],
                       inv[1],
                       inv[2],
                       inv[3],
                       inv[4],
                       inv[5],
                       inv[6],
                       inv[7],
                       inv[8],
                       inv[9],
                       inv[10],
                       inv[11],
                       inv[12],
                       inv[13],
                       inv[14],
                       inv[15]);
}

/// @brief Invert a matrix using the cofactor and adjugate formula.
/// @details A NULL or incompatible handle, or a matrix whose determinant is non-finite or has
///          magnitude below `1e-15`, raises a runtime trap. Failure never fabricates an identity
///          transform. An allocation failure in the shared inversion routine also yields NULL and
///          follows the singular-result trap path.
/// @param m Mat4 operand.
/// @return Newly acquired inverse Mat4, or NULL after trapping when inversion fails.
void *rt_mat4_inverse(void *m) {
    if (!m) {
        rt_trap("Mat4.Inverse: null matrix");
        return NULL;
    }
    mat4_impl *mat = mat4_checked(m);
    if (!mat) {
        rt_trap("Mat4.Inverse: invalid matrix");
        return NULL;
    }
    void *result = mat4_inverse_or_null(mat->m);
    if (!result) {
        rt_trap("Mat4.Inverse: singular matrix");
        return NULL;
    }
    return result;
}

/// @brief Non-trapping inverse for internal engine use: returns NULL for a
///        null/invalid/singular matrix instead of trapping, so callers can
///        degrade gracefully (e.g. treat an uninvertible parent transform as
///        the identity frame) rather than crashing (VDOC-208).
/// @param m Mat4 operand.
/// @return Newly acquired inverse Mat4, or NULL for invalid input, a singular matrix, or allocation
///         failure.
void *rt_mat4_try_inverse(void *m) {
    if (!m)
        return NULL;
    mat4_impl *mat = mat4_checked(m);
    if (!mat)
        return NULL;
    return mat4_inverse_or_null(mat->m);
}

/// @brief Negate every matrix element.
/// @param m Mat4 operand.
/// @return Newly acquired negated matrix, or a zero Mat4 for an invalid matrix.
void *rt_mat4_neg(void *m) {
    mat4_impl *mat = mat4_checked(m);
    if (!mat)
        return rt_mat4_zero();

    double r[16];
    for (int i = 0; i < 16; i++)
        r[i] = -mat->m[i];

    return rt_mat4_new(r[0],
                       r[1],
                       r[2],
                       r[3],
                       r[4],
                       r[5],
                       r[6],
                       r[7],
                       r[8],
                       r[9],
                       r[10],
                       r[11],
                       r[12],
                       r[13],
                       r[14],
                       r[15]);
}

//=============================================================================
// Comparison
//=============================================================================

/// @brief Compare two matrices using an absolute per-element tolerance.
/// @details Two NULL handles compare equal, while exactly one NULL handle compares unequal. A
///          non-positive or non-finite tolerance is replaced with `1e-9`. Any NaN element causes
///          the matrices to compare unequal, and incompatible non-NULL handles also compare
///          unequal.
/// @param a Left-hand Mat4 operand.
/// @param b Right-hand Mat4 operand.
/// @param epsilon Maximum permitted absolute difference for each corresponding element.
/// @return 1 when all sixteen elements compare within tolerance, otherwise 0.
int8_t rt_mat4_eq(void *a, void *b, double epsilon) {
    if (!a || !b)
        return (!a && !b) ? 1 : 0;

    // Normalize non-positive AND non-finite epsilon: a NaN epsilon slips past
    // `epsilon <= 0.0` and then `diff > NaN` is always false, making any two
    // matrices compare equal (VDOC-207).
    if (!(epsilon > 0.0) || !isfinite(epsilon))
        epsilon = 1e-9;

    mat4_impl *ma = mat4_checked(a);
    mat4_impl *mb = mat4_checked(b);
    if (!ma || !mb)
        return 0;

    for (int i = 0; i < 16; i++) {
        // Negated `<=` so a NaN component difference counts as NOT equal — a
        // matrix containing NaN is never equal to any matrix (VDOC-207).
        if (!(fabs(ma->m[i] - mb->m[i]) <= epsilon))
            return 0;
    }

    return 1;
}
